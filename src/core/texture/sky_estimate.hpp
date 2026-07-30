#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "common/image.hpp"
#include "core/selection.hpp"
#include "core/texture/solar.hpp"           // UtcTime
#include "core/texture/texture_params.hpp"  // SkyParams

// "Estimate from layer" (S55; docs/research-sky-estimate-from-layer.md): analyze a photograph
// (the active layer's doc-space pixels) and best-effort conform the sky generator's parameters
// to it -- horizon -> camera pitch/roll, sun position (or clock time via almanac inversion),
// turbidity, exposure, cloud coverage -- plus, on demand, a full-resolution sky Selection (S6)
// and a parametric photometric-harmonization parameter set (S7) for the PhotometricMatch
// adjustment kind. ⚠ Classical image processing only, no ML anywhere -- a standing constraint on
// this whole engine. Every stage emits (value, confidence) and a stage below its floor leaves its
// parameters UNTOUCHED and says so -- the degrade-to-no-change contract.
// The whole engine is pure core: FLTK-free, deterministic (fixed-seed RANSAC), headless-testable.
//
// Technique lineage (research doc §9): sky/mask = Luo & Etz, IEEE TIP 2002 (color-prior physics)
// + Zafarifar & de With, ACIVS 2006 (prior combination) + Lie/Lin/Lin/Hung, PRL 2005 (DP border)
// + Adams & Bischof, TPAMI 1994 (seeded growing); horizon line fit = Duda & Hart 1972 (Hough) +
// Fischler & Bolles 1981 (RANSAC); sun disc = Cozman & Krotkov, ICRA 1995; sky-model fitting =
// Lalonde/Narasimhan/Efros, IJCV 2010/2012 (we fit our OWN forward model); time = the Photo
// Sundial mechanism (Neurocomputing 2016) over Meeus almanac math (sky_almanac.hpp); relight =
// Reinhard 2001 log-luminance statistics + von Kries diagonal adaptation.
//
// ⚠ FOUR STANDING CONSTRUCTION CONSTRAINTS, honored by construction -- deliberate, and not to be
// "improved" away:
//   1. NO intensity histogram, bimodality test or global intensity threshold anywhere in horizon
//      detection or segmentation. The border is a color-prior + edge-energy DP polyline and the
//      line fit is RANSAC on border geometry;
//   2. the user's layer is never rotated, cropped or reprojected -- estimates only ever write
//      generator parameters;
//   3. horizon -> pitch/roll stays closed-form plus a fixed 2-parameter Gauss-Newton polish
//      against our OWN camera model;
//   4. time comes from the sun disc / sky state + almanac inversion only, never from shadows.
namespace mosaic::core::texture {

// Cross-thread progress / cancellation channel for one estimate (the TextureRenderProgress
// shape): `permille` climbs 0..1000 through the fixed stage fractions (proxy 100 / horizon 250 /
// segmentation 450 / sun 600 / probes 900 / invert 1000); cancellation is honored between
// stages and between probe renders -- a cancelled estimate returns `cancelled` set and must be
// discarded. Purely observational: plumbing a progress never changes an output value.
struct SkyEstimateProgress {
    std::atomic<std::uint32_t> permille{0};
    std::atomic<bool> cancel{false};
};

// One estimated quantity: the measured value, the stage's confidence in [0,1], whether it
// cleared its floor and was stamped onto the result params, and the human-readable honesty line
// for the dialog's info panel (empty when applied silently at high confidence).
struct EstimatedQuantity {
    double value = 0.0;
    double confidence = 0.0;
    bool applied = false;
    std::string note;
};

struct SkyEstimateOptions {
    SkyParams current{};  // the generator's params before the estimate (fallbacks + FOV source)
    // The dialog's "sky by date & place" latch: when set, an estimated sun elevation becomes a
    // CLOCK TIME via almanac inversion (S5) and the master clock stamps coherent sun + moon;
    // when clear, sunAzimuthDeg/sunElevationDeg are stamped manually and S5 is skipped.
    bool dateAndPlaceMode = false;
    // ---- EXIF-derived hints (phase 2): MEASUREMENTS, not estimates -- confidence 1.0. ----
    // The photo's horizontal FOV from FocalLengthIn35mmFilm (2*atan(18/f35), design §3.2). The
    // whole pipeline runs at THIS fov -- horizon inversion, probes, sun mapping -- and the
    // summary credits the lens metadata instead of the "unchanged" honesty line.
    std::optional<double> fovDegFromExif;
    // `current`'s observer date + place came from DateTimeOriginal + GPS (the dialog prefilled
    // them); only the summary changes -- the values already ride `current`.
    bool datePlaceFromExif = false;
};

// One chromaticity lobe of the sky color prior: a diagonal Gaussian over (r, b) chromaticity
// (r+g+b normalised, so two components carry the whole triple).
struct ChromaLobe {
    double meanR = 0.0;
    double meanB = 0.0;
    double sigmaR = 1.0;
    double sigmaB = 1.0;
};

// The re-fitted two-lobe sky color model (blue sky / bright neutral cloud), carried from the
// proxy estimate (S2's one EM-lite refit) to the full-resolution segmentation (S6) so both
// passes score color identically.
struct SkyColorModel {
    ChromaLobe blue{0.19, 0.50, 0.07, 0.11};   // canonical clear-sky seed
    ChromaLobe gray{0.333, 0.333, 0.05, 0.06}; // bright neutral cloud/overcast seed
    // The refit moved the second lobe well off neutral (a twilight/golden sky family): it now
    // represents the sky's own chroma, so the "bright neutral cloud" luminance gate no longer
    // applies to it.
    bool grayAdaptive = false;
    double meanLum = 0.18;  // scene mean linear luminance
    double lumNorm = 0.30;  // bright-region luminance (mean of the >= mean pixels) -- the Lum
                            // term's normaliser. Two mean passes: no percentile, no histogram
                            // (the conservative stand-in for the design's percentile -- see
                            // constraint 1 above).
};

// The estimate: per-quantity values + confidences + honesty notes, the conformed SkyParams
// (only cleared-floor quantities stamped; everything else untouched from options.current), and
// the proxy segmentation state S6 needs to build the full-resolution mask without re-analysis.
struct SkyEstimateResult {
    bool aborted = false;    // nothing usable found; params == options.current
    bool cancelled = false;  // the progress channel fired; discard the result
    std::string summary;     // newline-joined honesty lines for the info panel

    SkyParams params{};  // options.current with every cleared-floor quantity stamped

    EstimatedQuantity pitch;         // degrees (stamped with roll + shiftY reset as one unit)
    EstimatedQuantity roll;          // degrees, screen-clockwise positive
    EstimatedQuantity sunAzimuth;    // compass degrees (180 = frame centre)
    EstimatedQuantity sunElevation;  // degrees above the horizon
    EstimatedQuantity turbidity;     // 1..10
    EstimatedQuantity exposure;      // EV around the calibrated display mapping
    EstimatedQuantity cloudCoverage; // 0..1
    EstimatedQuantity timeUtc;       // fractional UTC hours (date-&-place mode only)
    EstimatedQuantity fov;           // degrees; applied only from lens metadata (confidence 1.0)

    // ---- proxy segmentation state (S2), consumed by skySelectionFromEstimate (S6) ----
    int proxyW = 0;
    int proxyH = 0;
    double proxyScale = 1.0;  // full-resolution pixels per proxy pixel
    long proxyOffX = 0;       // proxy origin in full-resolution pixels (opaque-content window)
    long proxyOffY = 0;
    std::vector<float> borderRows;  // per proxy column: sky/ground border row b(x), proxy px
    SkyColorModel colorModel{};
    double skyFraction = 0.0;        // fraction of the frame above the border
    double segConfidence = 0.0;      // the §5.3 gate score
    bool segmentationUsable = false; // cleared the §5.3 floor (the mask toggle's enable state)
    std::string segmentationNote;

    // ---- photo-side sky statistics (S4), consumed by photometricMatchParams (S7) ----
    double photoSkyMedianLum = 0.0;      // median linear luminance of the proxy sky pixels
    double photoElevationForMatch = 30.0;// elevation used for the photo side of k(el)
    double photoTurbidityForMatch = 2.5; // turbidity used for the photo side of k(el)
};

// The S0-S5 estimate. `photo` is the DOC-SPACE layer image (RGBA8, sRGB-encoded, straight
// alpha; transparent pixels are ignored). Deterministic; safe off-thread (pure function +
// renderTexture probe calls). Cancellation via `progress` between stages and probes.
[[nodiscard]] SkyEstimateResult estimateSkyFromLayer(const common::Image& photo,
                                                     const SkyEstimateOptions& options,
                                                     SkyEstimateProgress* progress = nullptr);

// ---- S5: almanac inversion (elevation -> clock time) -------------------------------------------
// Wraps sunEventsAtAltitude (sky_almanac.hpp): the ascending (morning) and descending
// (afternoon) crossings of `elevationDeg` on `date` at (lat +N, lon +E). Picks whichever valid
// solution is nearer `currentHourUtc` (ties break afternoon -- a photo cannot resolve the
// ambiguity, so the note carries the alternative for the dialog's swap link). Unreachable
// elevations clamp to solar noon; polar always-up/always-down days clamp with a polar note.
struct SkyTimeInversion {
    bool valid = false;
    double hourUtc = 0.0;             // the chosen solution (fractional UTC hours)
    bool hasAlternative = false;      // the other crossing exists
    double alternativeHourUtc = 0.0;
    std::string note;                 // "afternoon assumed -- morning equivalent 06:31" etc.
};
[[nodiscard]] SkyTimeInversion invertTimeFromElevation(const UtcTime& date, double latitudeDeg,
                                                       double longitudeDeg, double elevationDeg,
                                                       double currentHourUtc);

// ---- S6: full-resolution sky segmentation -------------------------------------------------------
// Consolidate the proxy border + color model into a document-space Selection over `photo` (the
// SAME doc-space image the estimate analyzed): border upsample, seeded color floods (the magic
// wand's metric + scanline flood, adaptive tolerance), prior reconcile, the holes policy
// (enclosed islands < 0.02% of the frame fill; detached sky specks < 0.1% drop), then
// Selection::smoothed + Selection::feathered -- ⚠ distance-transform and Gaussian ONLY, never a
// guided filter or edge-conforming matting, and never a Canny-contour-hull pipeline. Both of
// those exclusions are standing constraints on this stage, not gaps. Returns an empty Selection
// (with `noteOut` explaining) when the estimate's segmentation gates failed, the sky fraction
// leaves [2%, 98%], or the run was cancelled.
[[nodiscard]] Selection skySelectionFromEstimate(const common::Image& photo,
                                                 const SkyEstimateResult& estimate,
                                                 SkyEstimateProgress* progress = nullptr,
                                                 std::string* noteOut = nullptr);

// ---- S7: photometric harmonization parameters ---------------------------------------------------
// Compute the PhotometricMatch adjustment-layer parameter bag that grades the photo's
// FOREGROUND (everything outside `skySelection`) toward the GENERATED sky's illuminant. The
// harmonization target is PARAMETRIC -- derived from generator state (sun elevation ->
// atmospheric transmittance color, mean dome radiance from a small dome-only probe render) and
// ⚠ never from the pixel statistics of any style/reference image -- a standing constraint. The
// photo side uses classical color constancy on the foreground (gray-edge, van de Weijer 2007,
// blended with white-patch Retinex; gray-world fallback). All outputs are scalars:
//
//   gain_r/g/b     von Kries diagonal white-balance gains, clamp [0.6,1.6] ^ strength
//   mu_log         photo foreground mean log-luminance (natural log, linear space)
//   delta_ev       exposure shift: (k(el_target)+EV_target) - (k(el_photo)+EV_photo), [-6,+2]
//   sigma_ratio    log-luminance contrast ratio from the model's k-probes, clamp [0.7,1.3]
//   gradient       vertical luminance gradient amplitude a, [-0.12,+0.12] (0 below conf 0.5)
//   rod            scotopic rod fraction: smoothstep of target elevation -2 deg -> -14 deg
//   night_r/g/b    rod-signal tint (desaturated blue-gray, Thompson/Jensen day-for-night)
//   night_gain     rod-signal gain
//   saturation     post-mix saturation scale (1 - 0.5*rod)
//   strength       the caller's overall strength (recorded; gains already carry it)
//
// Deterministic; calls renderTexture for the dome probes (safe off-thread).
struct PhotometricMatchInput {
    SkyParams sky{};                    // the ACCEPTED generator state (target illuminant)
    double photoElevationDeg = 30.0;    // the photo's estimated sun elevation (S4/S3)
    double photoTurbidity = 2.5;        // the photo's estimated turbidity (S4)
    double photoSkyExposureEv = 0.0;    // the photo's estimated display exposure (S4)
    double strength = 1.0;              // overall harmonization strength, 0..1
    double confidence = 1.0;            // harmonization confidence (gates the gradient term)
};
[[nodiscard]] std::map<std::string, double> photometricMatchParams(
    const PhotometricMatchInput& input, const common::Image& photo,
    const Selection& skySelection);

}  // namespace mosaic::core::texture
