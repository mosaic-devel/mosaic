#include "core/adjustments.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

namespace mosaic::core {

namespace {

using enum AdjustmentParamType;

// The Grayscale method labels, index order = GrayscaleMethod = the stored bag value. Do NOT
// reorder this array -- the schema indexes it by the STORED enum value; the dropdown's grouped
// DISPLAY order lives in kGrayscaleOrder below. Names convey what each projection IS to a
// non-expert: "Luminance (linear)" is the linear-light photometric read, "Luma (gamma-weighted)"
// the classic gamma-encoded weights.
constexpr const char* kGrayscaleMethods[] = {
    "Luminance (linear)",    // 0 NoChrominance -- zero chroma, keep Rec.709 linear luminance
    "Luma (gamma-weighted)", // 1 Luma -- 0.30/0.59/0.11 on encoded values (the classic weights)
    "Red filter", "Green filter", "Blue filter", // 2-4 the photographer's mono colour filters
    "Dithered",            // 5 Floyd-Steinberg 1-bit
    "Max channel",         // 6 per-pixel max(R,G,B)
    "Adaptive threshold",  // 7 local-mean binarize
    "Min channel", // index 8 (appended -- see GrayscaleMethod::MinChannel; keeps the bag stable)
};

// The dropdown's DISPLAY order + group dividers (docs/adjustment-layers.md §2). Each entry is a
// STORED GrayscaleMethod index, arranged into three families -- luminance projections | colour
// filters & channel reads | halftone & threshold -- so the list reads sensibly WITHOUT touching
// the stored value (kGrayscaleMethods, the enum and the saved bag all stay in their original
// order). kGrayscaleDivider is indexed by DISPLAY position and draws a separator below each
// group's last row. The editor maps dropdown position <-> stored value through these.
constexpr int kGrayscaleOrder[] = {
    static_cast<int>(GrayscaleMethod::NoChrominance), // Luminance projections
    static_cast<int>(GrayscaleMethod::Luma),
    static_cast<int>(GrayscaleMethod::Red), // Colour filters + channel reads
    static_cast<int>(GrayscaleMethod::Green),
    static_cast<int>(GrayscaleMethod::Blue),
    static_cast<int>(GrayscaleMethod::MaxChannel),
    static_cast<int>(GrayscaleMethod::MinChannel),
    static_cast<int>(GrayscaleMethod::Dithered), // Halftone + threshold
    static_cast<int>(GrayscaleMethod::AdaptiveThreshold),
};
constexpr bool kGrayscaleDivider[] = {
    false, true,                       // after Luma: close the luminance-projection group
    false, false, false, false, true,  // after Min channel: close the filter/channel group
    false, false,
};

constexpr AdjustmentParamDesc kGrayscale[] = {
    {"method", "Method", 0.0, 8.0, static_cast<double>(GrayscaleMethod::Luma), 1.0, "", Choice,
     kGrayscaleMethods, 9, kGrayscaleOrder, kGrayscaleDivider},
    {"strength", "Strength", 0.0, 100.0, 100.0, 1.0, "%"},
    // The gray PALETTE size: how many grays represent the image -- 3? 60? 100? (user request).
    // 256 = the full 8-bit lattice = continuous, and the compositor skips quantization there,
    // so the default stays byte-identical to the pre-round-3 behavior.
    {"grays", "Grays", 2.0, 256.0, 256.0, 1.0, ""},
};

// Curves declares exactly ONE row: the channel the editor is on. The four curves themselves are
// indexed knots in the same bag (adjustments.hpp), deliberately NOT schema rows -- they have no
// scalar control and Reset erases them rather than re-seeding a default. The row exists so the
// schema is non-empty (an empty schema means "no editor", which is how the corner-panel arbiter
// decides whether to open the popover) and so the channel rides undo/redo and the document.
constexpr const char* kCurveChannels[] = {"RGB", "Red", "Green", "Blue"};
constexpr AdjustmentParamDesc kCurves[] = {
    {"channel", "Channel", 0.0, 3.0, static_cast<double>(CurveChannel::Composite), 1.0, "", Choice,
     kCurveChannels, kCurveChannelCount},
};

// Every table is identity-at-defaults unless the kind is inherently visible (Threshold cuts at
// mid-gray, Posterize quantizes) -- the compositor's identity early-out keys off these defaults,
// so a freshly seeded layer composites byte-identically to no layer at all.
constexpr AdjustmentParamDesc kBrightnessContrast[] = {
    {"brightness", "Brightness", -1.0, 1.0, 0.0, 0.01, ""},
    {"contrast", "Contrast", -1.0, 1.0, 0.0, 0.01, ""},
};

constexpr AdjustmentParamDesc kLevels[] = {
    {"in_black", "Input black", 0.0, 1.0, 0.0, 0.005, ""},
    {"in_white", "Input white", 0.0, 1.0, 1.0, 0.005, ""},
    {"gamma", "Gamma", 0.1, 10.0, 1.0, 0.01, ""},
    {"out_black", "Output black", 0.0, 1.0, 0.0, 0.005, ""},
    {"out_white", "Output white", 0.0, 1.0, 1.0, 0.005, ""},
};

constexpr AdjustmentParamDesc kExposure[] = {
    {"exposure", "Exposure", -5.0, 5.0, 0.0, 0.05, " EV"},
    {"offset", "Offset", -0.5, 0.5, 0.0, 0.005, ""},
    {"gamma", "Gamma", 0.1, 10.0, 1.0, 0.01, ""},
};

constexpr AdjustmentParamDesc kHueSaturation[] = {
    {"hue", "Hue", -180.0, 180.0, 0.0, 1.0, "\xC2\xB0"},
    {"saturation", "Saturation", -100.0, 100.0, 0.0, 1.0, ""},
    {"lightness", "Lightness", -100.0, 100.0, 0.0, 1.0, ""},
};

constexpr AdjustmentParamDesc kColorBalance[] = {
    {"shadows_cr", "Shadows: Cyan - Red", -100.0, 100.0, 0.0, 1.0, ""},
    {"shadows_mg", "Shadows: Magenta - Green", -100.0, 100.0, 0.0, 1.0, ""},
    {"shadows_yb", "Shadows: Yellow - Blue", -100.0, 100.0, 0.0, 1.0, ""},
    {"midtones_cr", "Midtones: Cyan - Red", -100.0, 100.0, 0.0, 1.0, ""},
    {"midtones_mg", "Midtones: Magenta - Green", -100.0, 100.0, 0.0, 1.0, ""},
    {"midtones_yb", "Midtones: Yellow - Blue", -100.0, 100.0, 0.0, 1.0, ""},
    {"highlights_cr", "Highlights: Cyan - Red", -100.0, 100.0, 0.0, 1.0, ""},
    {"highlights_mg", "Highlights: Magenta - Green", -100.0, 100.0, 0.0, 1.0, ""},
    {"highlights_yb", "Highlights: Yellow - Blue", -100.0, 100.0, 0.0, 1.0, ""},
    {"preserve_luminosity", "Preserve luminosity", 0.0, 1.0, 1.0, 1.0, "", Toggle},
};

constexpr AdjustmentParamDesc kThreshold[] = {
    {"level", "Level", 0.0, 1.0, 0.5, 0.005, ""},
};

constexpr AdjustmentParamDesc kPosterize[] = {
    {"levels", "Levels", 2.0, 32.0, 4.0, 1.0, ""},
};

// The S55 harmonization grade (docs/research-sky-estimate-from-layer.md §6.2): the keys
// texture::photometricMatchParams writes, exposed so the grade the estimator lands stays
// hand-tunable from the dock. mu_log and the night tint stay internal (the estimator's
// measurements, not knobs); the compositor's own defaults for them are unchanged.
constexpr AdjustmentParamDesc kPhotometricMatch[] = {
    {"gain_r", "Red gain", 0.25, 4.0, 1.0, 0.01, ""},
    {"gain_g", "Green gain", 0.25, 4.0, 1.0, 0.01, ""},
    {"gain_b", "Blue gain", 0.25, 4.0, 1.0, 0.01, ""},
    {"delta_ev", "Exposure shift", -5.0, 5.0, 0.0, 0.05, " EV"},
    {"sigma_ratio", "Contrast ratio", 0.5, 2.0, 1.0, 0.01, ""},
    {"gradient", "Vertical gradient", -0.25, 0.25, 0.0, 0.005, ""},
    {"rod", "Night vision mix", 0.0, 1.0, 0.0, 0.005, ""},
    {"night_gain", "Night gain", 0.0, 4.0, 1.0, 0.01, ""},
    {"saturation", "Saturation", 0.0, 2.0, 1.0, 0.01, ""},
};

// ---- The S33 blur family (docs/blur-filters.md §1) ------------------------------------------
// Deviation from the identity-at-defaults convention, on purpose: a freshly inserted blur is
// VISIBLE (the Threshold/Posterize "inherently visible" class) -- a Filter-menu item that
// inserts a do-nothing layer is the broken promise adjustmentImplemented exists to prevent.
// The compositor's no-op early-out keys off the *effective* params (radius/amount == 0), not
// default-equality. All px-dimensioned values are parent-space px (doc px at root); centers
// are parent-space px too -- NOT normalized -- so region crops and scaled previews see the
// same geometry (§2), and the menu insert seeds them to the document center.

constexpr AdjustmentParamDesc kGaussianBlur[] = {
    {"radius", "Radius", 0.0, 250.0, 10.0, 0.5, " px"},
};

constexpr AdjustmentParamDesc kBoxBlur[] = {
    {"radius", "Radius", 0.0, 250.0, 10.0, 1.0, " px"},
};

constexpr AdjustmentParamDesc kMotionBlur[] = {
    {"angle", "Angle", -180.0, 180.0, 0.0, 1.0, "\xC2\xB0", Angle},
    {"distance", "Distance", 0.0, 500.0, 20.0, 1.0, " px"},
};

// One Amount slider serves both modes: Spin reads it as degrees of arc, Zoom as the percentage
// of each pixel's center distance it streaks over.
constexpr const char* kRadialBlurModes[] = {"Spin", "Zoom"};
constexpr AdjustmentParamDesc kRadialBlur[] = {
    {"mode", "Mode", 0.0, 1.0, static_cast<double>(RadialBlurMode::Spin), 1.0, "", Choice,
     kRadialBlurModes, 2},
    {"amount", "Amount", 0.0, 100.0, 15.0, 1.0, ""},
    {"center_x", "Center X", -16384.0, 16384.0, 0.0, 1.0, " px"},
    {"center_y", "Center Y", -16384.0, 16384.0, 0.0, 1.0, " px"},
};

constexpr AdjustmentParamDesc kSurfaceBlur[] = {
    {"radius", "Radius", 1.0, 50.0, 8.0, 0.5, " px"},
    {"threshold", "Threshold", 1.0, 100.0, 15.0, 1.0, "%"},
};

// ⚠ INVARIANT: the highlight boost is a single LOWER threshold, applied as a global pre-pass on
// pixel values. It must never grow into an upper+lower "light range" pair, and never become a
// per-tap gather weight. One row on purpose -- do not widen it.
constexpr AdjustmentParamDesc kLensBlur[] = {
    {"radius", "Radius", 0.0, 60.0, 15.0, 0.5, " px"},
    {"blades", "Blades", 3.0, 8.0, 6.0, 1.0, ""},
    {"curvature", "Blade curvature", 0.0, 100.0, 50.0, 1.0, "%"},
    {"rotation", "Rotation", -180.0, 180.0, 0.0, 1.0, "\xC2\xB0", Angle},
    {"boost", "Highlight boost", 0.0, 100.0, 0.0, 1.0, ""},
    {"boost_threshold", "Highlight threshold", 0.0, 100.0, 80.0, 1.0, "%"},
};

// ⚠ INVARIANT: exactly ONE focus band per layer. A second band / pin / ellipse instance must
// become a second adjustment layer applying after this one -- never more geometry params here,
// and never two instances combined into one per-pixel field. A hard constraint on this schema,
// not an oversight.
constexpr const char* kDofBokeh[] = {"Soft", "Iris"};
constexpr AdjustmentParamDesc kDofBlur[] = {
    {"radius", "Blur radius", 0.0, 60.0, 15.0, 0.5, " px"},
    {"band", "Focus band", 0.0, 4000.0, 120.0, 1.0, " px"},
    {"feather", "Feather", 1.0, 4000.0, 200.0, 1.0, " px"},
    {"angle", "Angle", -180.0, 180.0, 0.0, 1.0, "\xC2\xB0", Angle},
    {"center_x", "Center X", -16384.0, 16384.0, 0.0, 1.0, " px"},
    {"center_y", "Center Y", -16384.0, 16384.0, 0.0, 1.0, " px"},
    {"bokeh", "Bokeh", 0.0, 1.0, static_cast<double>(DofBokeh::Soft), 1.0, "", Choice,
     kDofBokeh, 2},
};

// ---- The S34 photographic / compositing repairs (docs/adjustment-layers.md §2.2-§2.5) --------

// Shadows/Highlights. Both amounts default to 0, so a fresh layer is the identity (the §1 rule
// -- unlike the blur family this kind has no "obviously visible" default to fall back on: it is
// a repair, and which end needs repairing is the photograph's business). The two RANGE knobs say
// how much of the tonal axis counts as shadow / highlight; Radius is the local-background mask's
// blur, in the adjustment's parent-space px like every other px-dimensioned parameter (§2 of
// docs/blur-filters.md).
constexpr AdjustmentParamDesc kShadowsHighlights[] = {
    {"shadows", "Shadows", 0.0, 100.0, 0.0, 1.0, "%"},
    {"shadows_tone", "Shadow range", 5.0, 100.0, 50.0, 1.0, "%"},
    {"highlights", "Highlights", 0.0, 100.0, 0.0, 1.0, "%"},
    {"highlights_tone", "Highlight range", 5.0, 100.0, 50.0, 1.0, "%"},
    {"radius", "Radius", 1.0, 500.0, 30.0, 1.0, " px"},
};

// Defringe. Two independent jobs in one layer, both at identity by default: hue-band chroma
// suppression (purple / green, the two axial-fringe colours) and a lateral-CA radial rescale of
// the red and blue channels about the optical centre. The CA factors are PERCENTAGES of the
// pixel's distance from that centre -- real lateral CA is well under half a percent, hence the
// tight range and fine step. center_x/center_y are parent-space px (NOT normalized) exactly like
// the blur family's centers, which also earns them the Filter-menu insert's document-center
// seeding for free.
constexpr AdjustmentParamDesc kDefringe[] = {
    {"purple", "Purple fringe", 0.0, 100.0, 0.0, 1.0, "%"},
    {"green", "Green fringe", 0.0, 100.0, 0.0, 1.0, "%"},
    {"threshold", "Chroma threshold", 0.0, 100.0, 20.0, 1.0, "%"},
    {"ca_red", "Red scale", -1.0, 1.0, 0.0, 0.005, "%"},
    {"ca_blue", "Blue scale", -1.0, 1.0, 0.0, 0.005, "%"},
    {"center_x", "Center X", -16384.0, 16384.0, 0.0, 1.0, " px"},
    {"center_y", "Center Y", -16384.0, 16384.0, 0.0, 1.0, " px"},
};

// Matte Removal. An "inherently visible" kind in the Threshold/Posterize sense: every mode does
// real work the moment there is partial coverage below (on a fully opaque backdrop all four are
// the identity, which is the honest answer -- there is no matte to remove). No slash in a label:
// FLTK's menu add() splits paths on '/'.
constexpr const char* kMatteModes[] = {"Remove white matte", "Remove black matte",
                                       "Unpremultiply (divide by alpha)",
                                       "Premultiply (multiply by alpha)"};
constexpr AdjustmentParamDesc kMatteRemoval[] = {
    {"mode", "Mode", 0.0, 3.0, static_cast<double>(MatteMode::RemoveWhite), 1.0, "", Choice,
     kMatteModes, 4},
};

// Haze Removal. Amount 0 = identity. `airlight` is the assumed brightness of the haze itself and
// `tint` swings its colour warm (+) or blue (-) around neutral; `saturation` restores the chroma
// the atmosphere ate. ⚠ INVARIANT: the transmission is spatially CONSTANT. There is deliberately
// no radius and no per-pixel transmission knob, and nothing here may ever ESTIMATE transmission,
// depth or airlight from the image -- no dark channel, no patch minimum over channels, no
// local-contrast maximisation, no colour-line/haze-line prior, no "pick the brightest pixels"
// airlight guess. The density and the airlight are the user's, and that is the whole design.
constexpr AdjustmentParamDesc kHazeRemoval[] = {
    {"amount", "Amount", 0.0, 100.0, 0.0, 1.0, "%"},
    {"airlight", "Airlight", 10.0, 100.0, 95.0, 1.0, "%"},
    {"tint", "Airlight tint", -100.0, 100.0, 0.0, 1.0, ""},
    {"saturation", "Saturation", 0.0, 200.0, 100.0, 1.0, "%"},
};

// ---- The S35 artistic / stylize family (docs/filters-stylize.md §1) --------------------------
// Same sanctioned deviation from identity-at-defaults as the S33 blur family: a freshly inserted
// stylize filter is VISIBLE. The compositor's no-op early-out keys off the *effective* params
// (render::StylizeOp::effective), never off default-equality. Every px value is parent-space px
// (doc px at root); centres are parent-space px too and the menu insert seeds them (and the
// Vignette's radius) from the canvas, which the schema cannot know.

constexpr AdjustmentParamDesc kSharpen[] = {
    // 100% is exactly the textbook [[0,-1,0],[-1,5,-1],[0,-1,0]] convolution.
    {"amount", "Amount", 0.0, 300.0, 100.0, 1.0, "%"},
};

constexpr AdjustmentParamDesc kUnsharpMask[] = {
    {"radius", "Radius", 0.1, 250.0, 2.0, 0.1, " px"},
    {"amount", "Amount", 0.0, 500.0, 100.0, 1.0, "%"},
    // In 8-bit LEVELS -- the unit this control has been stated in since the darkroom crossed
    // over into software; the math divides by 255.
    {"threshold", "Threshold", 0.0, 255.0, 0.0, 1.0, ""},
};

constexpr const char* kNoiseDistributions[] = {"Gaussian", "Uniform"};
constexpr AdjustmentParamDesc kAddNoise[] = {
    {"amount", "Amount", 0.0, 100.0, 12.0, 0.5, "%"},
    {"distribution", "Distribution", 0.0, 1.0,
     static_cast<double>(NoiseDistribution::Gaussian), 1.0, "", Choice, kNoiseDistributions, 2},
    {"monochrome", "Monochromatic", 0.0, 1.0, 0.0, 1.0, "", Toggle},
    // The grain is a hash of (seed, parent-space pixel), never a running RNG -- this row is what
    // makes it both reproducible and re-rollable (docs/filters-stylize.md §3).
    {"seed", "Seed", 0.0, 9999.0, 1.0, 1.0, ""},
};

// Lee 1980 local-statistics MMSE, O(1) per pixel. Noise level 0 is the identity.
constexpr AdjustmentParamDesc kDenoise[] = {
    {"radius", "Radius", 1.0, 16.0, 3.0, 1.0, " px"},
    {"noise", "Noise level", 0.0, 50.0, 8.0, 0.5, "%"},
};

constexpr AdjustmentParamDesc kPixelate[] = {
    {"size", "Cell size", 1.0, 512.0, 12.0, 1.0, " px"},
};

constexpr AdjustmentParamDesc kEmboss[] = {
    {"angle", "Angle", -180.0, 180.0, 45.0, 1.0, "\xC2\xB0", Angle},
    {"height", "Height", 0.5, 32.0, 2.0, 0.5, " px"},
    {"amount", "Amount", 1.0, 500.0, 100.0, 1.0, "%"},
};

// The ORIGINAL 1976 four-quadrant Kuwahara. ⚠ INVARIANT: no structure-tensor, anisotropic or
// polynomial-weighted generalisation enters this kind -- the classic form is what "oil paint"
// means here, and the omission is deliberate.
constexpr AdjustmentParamDesc kOilPaint[] = {
    {"radius", "Radius", 1.0, 32.0, 4.0, 1.0, " px"},
};

constexpr const char* kWaveModes[] = {"Wave", "Ripple"};
constexpr AdjustmentParamDesc kWave[] = {
    {"mode", "Mode", 0.0, 1.0, static_cast<double>(WaveMode::Wave), 1.0, "", Choice, kWaveModes,
     2},
    {"amplitude", "Amplitude", 0.0, 500.0, 20.0, 0.5, " px"},
    {"wavelength", "Wavelength", 1.0, 4000.0, 80.0, 1.0, " px"},
    {"angle", "Angle", -180.0, 180.0, 0.0, 1.0, "\xC2\xB0", Angle},
    {"phase", "Phase", -180.0, 180.0, 0.0, 1.0, "\xC2\xB0", Angle},
    {"center_x", "Center X", -16384.0, 16384.0, 0.0, 1.0, " px"},
    {"center_y", "Center Y", -16384.0, 16384.0, 0.0, 1.0, " px"},
};

constexpr AdjustmentParamDesc kVignette[] = {
    {"exposure", "Exposure", -5.0, 5.0, -1.2, 0.05, " EV"},
    // Seeded at insert to 0.55 * half the canvas diagonal (app_window insertAdjustmentLayer): a
    // fixed px default cannot know the document any more than the centres could.
    {"radius", "Radius", 0.0, 16384.0, 300.0, 1.0, " px"},
    {"feather", "Feather", 0.0, 200.0, 60.0, 1.0, "%"},
    // Superellipse exponent knob: 0 -> circle, +100 -> squarer, -100 -> diamond.
    {"roundness", "Roundness", -100.0, 100.0, 0.0, 1.0, ""},
    {"center_x", "Center X", -16384.0, 16384.0, 0.0, 1.0, " px"},
    {"center_y", "Center Y", -16384.0, 16384.0, 0.0, 1.0, " px"},
};

// ---- The S34-a remainder of the galleries (docs/adjustment-layers.md §2.6-§2.9) --------------

// Gradient Map. The RAMP is not a schema row (it is indexed stops in the same bag -- see
// adjustments.hpp), so this table holds only the one honest knob. INHERENTLY VISIBLE: there is no
// identity ramp, so a fresh layer maps luma through black->white, which is real work -- the
// Threshold/Posterize/Matte Removal class, not the identity-at-defaults one.
constexpr AdjustmentParamDesc kGradientMap[] = {
    {"reverse", "Reverse", 0.0, 1.0, 0.0, 1.0, "", Toggle},
};

// Vibrance. ONE row on purpose: the LINEAR saturation scale already ships on Hue/Saturation, and
// what makes this kind worth having is precisely that its scale is weighted by the pixel's own
// saturation. Identity at 0 (the §1 rule) -- which end a photograph needs is its own business.
constexpr AdjustmentParamDesc kVibrance[] = {
    {"vibrance", "Vibrance", -100.0, 100.0, 0.0, 1.0, ""},
};

// Photo Filter. VISIBLE at defaults, the S33/S35 deviation: a warming filter at a quarter density
// is what every editor opens this control on, and a menu item that inserts a do-nothing layer is
// the broken promise adjustmentImplemented exists to prevent. The compositor's no-op early-out
// keys off the EFFECTIVE parameter (density == 0), never off default-equality.
// The three color_* rows are the CUSTOM preset's colour, in 8-bit levels: schema-declared so the
// clamp/seed/Reset machinery covers them, but owned by the editor's swatch rather than shown as
// three sliders (docs/adjustment-layers.md §2.8).
constexpr const char* kPhotoFilters[] = {
    "Warming (85)", "Warming (81)",  // the two standard warming densities
    "Cooling (80)", "Cooling (82)",  // ... and their cooling partners
    "Red", "Orange", "Yellow", "Green", "Cyan", "Blue", "Violet", "Magenta",
    "Sepia", "Underwater",
    "Custom",
};
constexpr bool kPhotoFilterDivider[] = {
    false, true,                                            // close the warming pair
    false, true,                                            // close the cooling pair
    false, false, false, false, false, false, false, true,  // close the colour filters
    false, true,                                            // close Sepia / Underwater
    false,
};
constexpr AdjustmentParamDesc kPhotoFilter[] = {
    {"filter", "Filter", 0.0, 14.0, static_cast<double>(PhotoFilterPreset::Warming85), 1.0, "",
     Choice, kPhotoFilters, kPhotoFilterPresetCount, nullptr, kPhotoFilterDivider},
    {"density", "Density", 0.0, 100.0, 25.0, 1.0, "%"},
    {"preserve_luminosity", "Preserve luminosity", 0.0, 1.0, 1.0, 1.0, "", Toggle},
    {"color_r", "Custom red", 0.0, 255.0, 236.0, 1.0, ""},
    {"color_g", "Custom green", 0.0, 255.0, 138.0, 1.0, ""},
    {"color_b", "Custom blue", 0.0, 255.0, 0.0, 1.0, ""},
};

// High Pass. VISIBLE at defaults for the same reason the blur family is (a zero-radius high pass
// is a flat grey field, not an identity), and its radius reads exactly like Gaussian Blur's and
// Unsharp Mask's: sigma = radius/2 (docs/filters-stylize.md §3).
constexpr AdjustmentParamDesc kHighPass[] = {
    {"radius", "Radius", 0.1, 250.0, 10.0, 0.1, " px"},
};

// The named photographic filter colours, index order = PhotoFilterPreset. Standard sRGB
// approximations of the classic gelatin filter designations; Custom's entry is the schema's own
// color_r/g/b default (the Warming 85 colour), so a caller that forgets to substitute the bag's
// custom colour still gets a filter rather than black.
constexpr common::Color8 kPhotoFilterColors[kPhotoFilterPresetCount] = {
    {0xEC, 0x8A, 0x00, 255}, // Warming (85)
    {0xEB, 0xB1, 0x13, 255}, // Warming (81)
    {0x00, 0x6D, 0xFF, 255}, // Cooling (80)
    {0x00, 0xB5, 0xFF, 255}, // Cooling (82)
    {0xEA, 0x1A, 0x1A, 255}, // Red
    {0xF0, 0x93, 0x00, 255}, // Orange
    {0xFF, 0xD4, 0x00, 255}, // Yellow
    {0x19, 0xFF, 0x19, 255}, // Green
    {0x00, 0xFF, 0xFF, 255}, // Cyan
    {0x00, 0x00, 0xFF, 255}, // Blue
    {0x9C, 0x1A, 0xFF, 255}, // Violet
    {0xFF, 0x00, 0xFF, 255}, // Magenta
    {0xAC, 0x7A, 0x33, 255}, // Sepia
    {0x00, 0xC1, 0xB1, 255}, // Underwater
    {0xEC, 0x8A, 0x00, 255}, // Custom (the color_* default)
};

// ---- Curve knots in the double bag (adjustments.hpp) -----------------------------------------

constexpr const char* kCurveChannelKeys[kCurveChannelCount] = {"curve_rgb", "curve_r", "curve_g",
                                                               "curve_b"};

// "<prefix>_<i>_<field>". The trailing '_' after the prefix is what keeps "curve_r" from being a
// prefix of "curve_rgb" when clearAdjustmentCurve sweeps a channel's keys.
[[nodiscard]] std::string curveKnotKey(CurveChannel ch, int i, char field) {
    std::string k(adjustmentCurveKey(ch));
    k += '_';
    k += std::to_string(i);
    k += '_';
    k += field;
    return k;
}

// ---- Gradient-map stops in the double bag (adjustments.hpp) -----------------------------------

constexpr const char* kGradientMapPrefix = "gm_";

// "gm_<i>_<field>". The field letters are t (offset), r/g/b/a (colour) and m (blend midpoint).
[[nodiscard]] std::string gradientStopKey(int i, char field) {
    std::string k(kGradientMapPrefix);
    k += std::to_string(i);
    k += '_';
    k += field;
    return k;
}

// One stop's field, or nullopt when it is absent or not a finite number (a hole: the whole stop
// is then dropped, exactly the way a curve knot with a missing coordinate is).
[[nodiscard]] std::optional<double> bagField(const std::map<std::string, double>& bag,
                                             const std::string& key) {
    const auto it = bag.find(key);
    if (it == bag.end() || !std::isfinite(it->second)) return std::nullopt;
    return it->second;
}

} // namespace

std::span<const AdjustmentParamDesc> adjustmentParamSchema(AdjustmentKind kind) {
    using enum AdjustmentKind;
    switch (kind) {
        case BrightnessContrast: return kBrightnessContrast;
        case Levels: return kLevels;
        case Curves: return kCurves;  // the channel picker; the knots are not schema rows
        case Exposure: return kExposure;
        case HueSaturation: return kHueSaturation;
        case ColorBalance: return kColorBalance;
        case Grayscale: return kGrayscale;
        case Invert: return {};
        case Threshold: return kThreshold;
        case Posterize: return kPosterize;
        case PhotometricMatch: return kPhotometricMatch;
        case GaussianBlur: return kGaussianBlur;
        case BoxBlur: return kBoxBlur;
        case MotionBlur: return kMotionBlur;
        case RadialBlur: return kRadialBlur;
        case SurfaceBlur: return kSurfaceBlur;
        case LensBlur: return kLensBlur;
        case DofBlur: return kDofBlur;
        case ShadowsHighlights: return kShadowsHighlights;
        case Defringe: return kDefringe;
        case MatteRemoval: return kMatteRemoval;
        case HazeRemoval: return kHazeRemoval;
        case Sharpen: return kSharpen;
        case UnsharpMask: return kUnsharpMask;
        case AddNoise: return kAddNoise;
        case Denoise: return kDenoise;
        case Pixelate: return kPixelate;
        case Emboss: return kEmboss;
        case OilPaint: return kOilPaint;
        case Wave: return kWave;
        case Vignette: return kVignette;
        case GradientMap: return kGradientMap;  // the ramp is stops in the bag, not a schema row
        case Vibrance: return kVibrance;
        case PhotoFilter: return kPhotoFilter;
        case HighPass: return kHighPass;
    }
    return {};
}

bool adjustmentIsSpatial(AdjustmentKind kind) {
    using enum AdjustmentKind;
    switch (kind) {
        case GaussianBlur:
        case BoxBlur:
        case MotionBlur:
        case RadialBlur:
        case SurfaceBlur:
        case LensBlur:
        case DofBlur:
        // S34: Shadows/Highlights reads a blurred local-background mask, and Defringe's
        // lateral-CA rescale resamples the red/blue channels off-pixel. Both report a finite
        // reach (render::blurAdjustmentReach) and read with clamp-to-edge, so the dirty-rect
        // path stays byte-identical to the full composite. Defringe is spatial by KIND rather
        // than per instance: with the CA sliders at zero its reach is 0, which costs nothing,
        // and one routing rule beats two.
        case ShadowsHighlights:
        case Defringe:
        // S35 (docs/filters-stylize.md §5): the seven windowed / resampling stylize kinds. They
        // must be declared spatial or the region + group-buffer machinery never asks for their
        // reach. AddNoise and Vignette are deliberately absent: both are per-pixel (reach 0) and
        // would only grow buffers they do not read from. Spatiality also opts a kind into the
        // editor's draft-scrub settle, which is what Unsharp's draft lane wants.
        case Sharpen:
        case UnsharpMask:
        case Denoise:
        case Pixelate:
        case Emboss:
        case OilPaint:
        case Wave:
        // S34-a: High Pass is the unsharp difference on its own, so it reads the same Gaussian
        // neighbourhood and reports the same 3-sigma reach. Gradient Map / Vibrance / Photo
        // Filter are deliberately absent: all three are pure per-pixel colour transfers.
        case HighPass: return true;
        default: return false;
    }
}

bool adjustmentIsSpatial(const AdjustmentLayer& layer) {
    const AdjustmentKind kind = layer.adjustmentKind();
    if (adjustmentIsSpatial(kind)) return true;
    // The one method-dependent case: Grayscale is spatial for Dithered / Adaptive threshold.
    if (kind == AdjustmentKind::Grayscale) {
        const AdjustmentParamDesc* d = adjustmentParamDesc(kind, "method");
        if (d == nullptr) return false;
        const auto m = static_cast<GrayscaleMethod>(std::lround(adjustmentParamValue(layer, *d)));
        return m == GrayscaleMethod::Dithered || m == GrayscaleMethod::AdaptiveThreshold;
    }
    return false;
}

const AdjustmentParamDesc* adjustmentParamDesc(AdjustmentKind kind, std::string_view key) {
    for (const AdjustmentParamDesc& d : adjustmentParamSchema(kind))
        if (key == d.key) return &d;
    return nullptr;
}

double adjustmentParamValue(const AdjustmentLayer& layer, const AdjustmentParamDesc& desc) {
    const auto& bag = layer.params();
    const auto it = bag.find(desc.key);
    if (it == bag.end()) return desc.def;
    return std::clamp(it->second, desc.min, desc.max);
}

void seedAdjustmentDefaults(AdjustmentLayer& layer) {
    for (const AdjustmentParamDesc& d : adjustmentParamSchema(layer.adjustmentKind()))
        layer.params()[d.key] = d.def;
}

ColorBalancePoint colorBalanceToPlane(const ColorBalanceTriple& v) {
    // p = (2/3) * sum(v_i * axis_i): the achromatic mean vanishes (the three unit axes sum to
    // zero), the 2/3 makes the projection below its exact inverse (sum(axis axis^T) = (3/2) I).
    constexpr double c120 = -0.5;
    const double s120 = std::sin(2.0 * std::numbers::pi / 3.0);
    return {(2.0 / 3.0) * (v.cr + c120 * (v.mg + v.yb)),
            (2.0 / 3.0) * (s120 * (v.mg - v.yb))};
}

ColorBalanceTriple colorBalanceFromPlane(const ColorBalancePoint& p, double mean) {
    constexpr double c120 = -0.5;
    const double s120 = std::sin(2.0 * std::numbers::pi / 3.0);
    return {mean + p.x,
            mean + c120 * p.x + s120 * p.y,
            mean + c120 * p.x - s120 * p.y};
}

bool adjustmentImplemented(AdjustmentKind kind) {
    using enum AdjustmentKind;
    switch (kind) {
        case Curves:  // S34 closed the one gap: every kind has real math now
        case BrightnessContrast:
        case Levels:
        case Exposure:
        case HueSaturation:
        case ColorBalance:
        case Grayscale:
        case Invert:
        case Threshold:
        case Posterize:
        case PhotometricMatch:
        case GaussianBlur:
        case BoxBlur:
        case MotionBlur:
        case RadialBlur:
        case SurfaceBlur:
        case LensBlur:
        case DofBlur:
        case ShadowsHighlights:
        case Defringe:
        case MatteRemoval:
        case HazeRemoval:
        case Sharpen:      // S35 artistic / stylize family
        case UnsharpMask:
        case AddNoise:
        case Denoise:
        case Pixelate:
        case Emboss:
        case OilPaint:
        case Wave:
        case Vignette:
        case GradientMap:  // S34-a: the remainder of the galleries
        case Vibrance:
        case PhotoFilter:
        case HighPass: return true;
    }
    return false;
}

// ---- Curve knots in the double bag (adjustments.hpp) -----------------------------------------

std::string_view adjustmentCurveKey(CurveChannel ch) {
    const int i = std::clamp(static_cast<int>(ch), 0, kCurveChannelCount - 1);
    return kCurveChannelKeys[i];
}

brush::Curve adjustmentCurve(const std::map<std::string, double>& bag, CurveChannel ch) {
    const auto n = bag.find(std::string(adjustmentCurveKey(ch)) + "_n");
    if (n == bag.end() || !std::isfinite(n->second)) return {}; // absent = the identity curve
    const double cap = static_cast<double>(kMaxCurvePoints);
    const int count = static_cast<int>(std::clamp(std::floor(n->second), 0.0, cap));
    std::vector<brush::CurvePoint> pts;
    pts.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const auto x = bag.find(curveKnotKey(ch, i, 'x'));
        const auto y = bag.find(curveKnotKey(ch, i, 'y'));
        if (x == bag.end() || y == bag.end()) continue;             // a hole: skip that knot
        if (!std::isfinite(x->second) || !std::isfinite(y->second)) continue;
        const auto c = bag.find(curveKnotKey(ch, i, 'c'));
        pts.push_back({std::clamp(x->second, 0.0, 1.0), std::clamp(y->second, 0.0, 1.0),
                       c != bag.end() && c->second >= 0.5});
    }
    // Fewer than two knots cannot span [0,1], which is the only domain a tone curve has; the
    // brush::Curve ctor would read it as a constant function, and "a corrupt file silently
    // flattens the image" is not an answer anyone wants.
    if (pts.size() < 2) return {};
    return brush::Curve(std::move(pts));
}

brush::Curve adjustmentCurve(const AdjustmentLayer& layer, CurveChannel ch) {
    return adjustmentCurve(layer.params(), ch);
}

void setAdjustmentCurve(std::map<std::string, double>& bag, CurveChannel ch,
                        const brush::Curve& c) {
    clearAdjustmentCurve(bag, ch); // a shorter curve must not leave the longer one's tail behind
    if (c.isIdentity()) return;    // absent IS the identity -- a reset leaves no residue behind
    const std::vector<brush::CurvePoint>& pts = c.points();
    const int n = static_cast<int>(
        std::min<std::size_t>(pts.size(), static_cast<std::size_t>(kMaxCurvePoints)));
    if (n < 2) return;
    bag[std::string(adjustmentCurveKey(ch)) + "_n"] = static_cast<double>(n);
    for (int i = 0; i < n; ++i) {
        const brush::CurvePoint& p = pts[static_cast<std::size_t>(i)];
        bag[curveKnotKey(ch, i, 'x')] = p.x;
        bag[curveKnotKey(ch, i, 'y')] = p.y;
        if (p.corner) bag[curveKnotKey(ch, i, 'c')] = 1.0; // smooth = absent, so bags stay small
    }
}

void clearAdjustmentCurve(std::map<std::string, double>& bag, CurveChannel ch) {
    const std::string prefix = std::string(adjustmentCurveKey(ch)) + "_";
    for (auto it = bag.begin(); it != bag.end();) {
        const bool mine = it->first.size() >= prefix.size() &&
                          it->first.compare(0, prefix.size(), prefix) == 0;
        it = mine ? bag.erase(it) : std::next(it);
    }
}

void clearAdjustmentCurves(std::map<std::string, double>& bag) {
    for (int i = 0; i < kCurveChannelCount; ++i)
        clearAdjustmentCurve(bag, static_cast<CurveChannel>(i));
}

bool adjustmentCurvesIdentity(const AdjustmentLayer& layer) {
    for (int i = 0; i < kCurveChannelCount; ++i)
        if (!adjustmentCurve(layer, static_cast<CurveChannel>(i)).isIdentity()) return false;
    return true;
}

// ---- Gradient-map stops in the double bag (adjustments.hpp) -----------------------------------

vec::Gradient defaultGradientMap() {
    vec::Gradient g;
    // Everything except the stops is FIXED: a gradient map has no geometry to place, and dither
    // is keyed to the destination pixel, which would break region == crop(full) (adjustments.hpp).
    g.type = vec::GradientType::Linear;
    g.transform = common::Affine2D::identity();
    g.spread = vec::SpreadMethod::Pad;
    g.dither = vec::DitherKind::None;
    g.stops = {{0.0, common::ColorF{0.0F, 0.0F, 0.0F, 1.0F}, 0.5},
               {1.0, common::ColorF{1.0F, 1.0F, 1.0F, 1.0F}, 0.5}};
    return g;
}

vec::Gradient adjustmentGradientMap(const std::map<std::string, double>& bag) {
    const std::optional<double> n = bagField(bag, std::string(kGradientMapPrefix) + "n");
    if (!n) return defaultGradientMap(); // absent = the default black-to-white ramp
    const int count = static_cast<int>(
        std::clamp(std::floor(*n), 0.0, static_cast<double>(kMaxGradientMapStops)));
    std::vector<vec::GradientStop> stops;
    stops.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const std::optional<double> t = bagField(bag, gradientStopKey(i, 't'));
        const std::optional<double> r = bagField(bag, gradientStopKey(i, 'r'));
        const std::optional<double> g = bagField(bag, gradientStopKey(i, 'g'));
        const std::optional<double> b = bagField(bag, gradientStopKey(i, 'b'));
        const std::optional<double> a = bagField(bag, gradientStopKey(i, 'a'));
        if (!t || !r || !g || !b || !a) continue; // a hole: skip that stop
        const std::optional<double> m = bagField(bag, gradientStopKey(i, 'm'));
        const auto f = [](double v) { return static_cast<float>(std::clamp(v, 0.0, 1.0)); };
        stops.push_back({std::clamp(*t, 0.0, 1.0),
                         common::ColorF{f(*r), f(*g), f(*b), f(*a)},
                         m ? std::clamp(*m, 0.0, 1.0) : 0.5});
    }
    // Fewer than two surviving stops cannot span the ramp; "a corrupt file silently flattens the
    // image to one colour" is no better an answer here than it was for Curves.
    if (stops.size() < 2) return defaultGradientMap();
    // sampleStops walks the list assuming it is ordered, so a hostile file's shuffled offsets
    // must not be handed to it. Stable, so equal offsets keep their stored order.
    std::stable_sort(stops.begin(), stops.end(),
                     [](const vec::GradientStop& a, const vec::GradientStop& b) {
                         return a.offset < b.offset;
                     });
    vec::Gradient out = defaultGradientMap();
    out.stops = std::move(stops);
    return out;
}

vec::Gradient adjustmentGradientMap(const AdjustmentLayer& layer) {
    return adjustmentGradientMap(layer.params());
}

void setAdjustmentGradientMap(std::map<std::string, double>& bag, const vec::Gradient& g) {
    clearAdjustmentGradientMap(bag); // a shorter ramp must not leave the longer one's tail behind
    if (g.stops == defaultGradientMap().stops) return; // absent IS the default ramp
    const int n = static_cast<int>(
        std::min<std::size_t>(g.stops.size(), static_cast<std::size_t>(kMaxGradientMapStops)));
    if (n < 2) return; // not a ramp: leave the keys absent, which reads as the default
    bag[std::string(kGradientMapPrefix) + "n"] = static_cast<double>(n);
    for (int i = 0; i < n; ++i) {
        const vec::GradientStop& s = g.stops[static_cast<std::size_t>(i)];
        bag[gradientStopKey(i, 't')] = s.offset;
        bag[gradientStopKey(i, 'r')] = s.color.r;
        bag[gradientStopKey(i, 'g')] = s.color.g;
        bag[gradientStopKey(i, 'b')] = s.color.b;
        bag[gradientStopKey(i, 'a')] = s.color.a;
        // A straight linear blend is the overwhelming case, and absent spells it -- bags stay
        // small and a document written before the blend curve existed reads back identically.
        if (s.midpoint != 0.5) bag[gradientStopKey(i, 'm')] = s.midpoint;
    }
}

void clearAdjustmentGradientMap(std::map<std::string, double>& bag) {
    const std::string prefix(kGradientMapPrefix);
    for (auto it = bag.begin(); it != bag.end();) {
        const bool mine = it->first.size() >= prefix.size() &&
                          it->first.compare(0, prefix.size(), prefix) == 0;
        it = mine ? bag.erase(it) : std::next(it);
    }
}

common::Color8 photoFilterPresetColor(PhotoFilterPreset p) {
    const int i = std::clamp(static_cast<int>(p), 0, kPhotoFilterPresetCount - 1);
    return kPhotoFilterColors[i];
}

} // namespace mosaic::core
