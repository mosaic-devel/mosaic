#pragma once

#include <cstdint>
#include <variant>
#include <vector>

#include "common/image.hpp"

// The Texture Generator's parameter model (S55-a; docs/texture-generator.md §3.1). A
// TextureLayer stores the WHOLE value: pixels are a regenerated cache, parameters are the
// document content (undo swaps values, docio serialises them, the modal edits them). Each
// generator's params are a variant arm so they never collide and the dialog reflects only the
// active arm; the structs below are the S55-a starter sets that the real renderers
// (S55-b sky/camera/cloud-catalogue, S55-d paper kinds, S55-e grass blades) GROW -- fields are
// added per session, never repurposed.
namespace mosaic::core::texture {

using common::ColorF;

enum class Generator { Sky, Paper, Grass, Wood, Marble, Stone, Canvas, Metal };
inline constexpr int kGeneratorCount = 8;

// Stable ASCII generator name (auto layer names, tests, logging). Never localised through here.
[[nodiscard]] const char* generatorName(Generator g);

// The §4.3 cloud type catalogue (S55-b). Each type is a preset over the shared knobs -- natural
// altitude, feature size, coverage response, cellularity, wind shear, shading -- rendered by the
// 2D layered lane (sky_render.cpp); Cumulus/Cumulonimbus gain the volumetric lane in S55-c.
// Contrails/mammatus/lenticular are parameter PRESETS over these types (S55-f), not types; fog
// is the haze element's business.
enum class CloudType {
    Cirrus,         // high wind-sheared wisps
    Cirrocumulus,   // high fine cellular "mackerel" ripples
    Cirrostratus,   // high thin uniform veil
    Altocumulus,    // mid cellular rolls/patches
    Altostratus,    // mid featureless grey sheet
    Stratocumulus,  // low lumpy broken rolls
    Stratus,        // low flat overcast
    Nimbostratus,   // low thick dark rain sheet
    Cumulus,        // low rounded heaps, lit top / shaded base
    Cumulonimbus,   // deep towers
};
inline constexpr int kCloudTypeCount = 10;

// Stable ASCII cloud-type name (docio tokens, tests, logging). Never localised through here.
[[nodiscard]] const char* cloudTypeName(CloudType t);

// One cloud deck on its own §4.5 altitude plane, projected independently through the camera --
// low decks race to the horizon while high cirrus barely moves (real parallax). The per-layer
// knobs BIAS the sky-level master dials so one Coverage slider still drives the whole sky.
struct CloudLayerParams {
    bool enabled = true;
    CloudType type = CloudType::Cumulus;
    double coverageBias = 1.0;  // multiplier on SkyParams::cloudCoverage for this deck
    double scaleBias = 1.0;     // multiplier on the type's natural feature size
    double altitudeM = 0.0;     // 0 = the type's natural altitude, else explicit metres
    bool operator==(const CloudLayerParams&) const = default;
};

// Sky (S55-b: Hosek-Wilkie float dome + perspective camera + the 2D cloud catalogue + solar
// tinting; S55-c adds the volumetric cloud lane). Every element toggles independently -- a
// dome-off render carries alpha only where clouds/sun write coverage (§3.4: that IS the
// "create a transparent layer" affordance). ⚠ GROWTH RULE: fields are only ever ADDED (docio
// reads absent ones as these defaults); repurposing one breaks .mosaic backward-reads.
struct SkyParams {
    bool enableDome = true;
    bool enableSun = true;
    bool enableClouds = true;
    bool enableHaze = true;
    double sunAzimuthDeg = 180.0;   // compass degrees, clockwise from north; 180 = frame centre
    double sunElevationDeg = 28.0;  // 0 = on the horizon, 90 = zenith (28 sits in the default frame)
    double turbidity = 2.2;         // atmosphere/haze thickness, 1 (crystalline) .. 10 (murky)
    double cloudCoverage = 0.4;     // the master dial, 0 (clear) .. 1 (overcast)
    double cloudScale = 1.0;        // master cloud feature-size multiplier
    // -- S55-b growth --
    double groundAlbedo = 0.2;    // Hosek-Wilkie ground-bounce input, 0..1
    double exposure = 0.0;        // EV around the calibrated display mapping (§4.4)
    double sunDiscScale = 1.0;    // multiplier on the physical 0.255° solar disc radius
    double fovDeg = 62.0;         // camera horizontal field of view (§4.5)
    double pitchDeg = 18.0;       // camera pitch above the horizon; sets where the horizon sits
    double rollDeg = 0.0;         // horizon tilt (hand-held matching)
    double shiftY = 0.0;          // tilt-shift: principal-point offset as a fraction of frame
                                  // height; positive moves the horizon DOWN the frame
    double windDirectionDeg = 25.0;  // compass direction the wind blows TOWARD (shear axis)
    double windStrength = 0.5;       // 0..1: octave shear / wisp elongation
    // -- S55-c growth --
    bool volumetricClouds = true;  // heap/tower types (Cumulus/Cumulonimbus) ray-march the §4.3
                                   // lane-B implicit field instead of the flat 2D projection;
                                   // false forces every deck onto the fast 2D lane (§9.1)
    // -- S55-f night growth (user 2026-07-15: the sky needs its other half) -- sunElevationDeg
    // below 0 now renders twilight into night (the day path at elevation >= 0 is untouched);
    // stars fade in past civil twilight; the moon is an element like the sun, its PHASE computed
    // from the real sun-moon geometry (full when opposite the sun), visible faintly by day too.
    bool enableMoon = false;         // off by default so pre-night documents keep their look
    double moonAzimuthDeg = 210.0;   // compass, like the sun (210 = right of frame centre)
    double moonElevationDeg = 35.0;
    double moonScale = 1.0;          // multiplier on the physical 0.259 deg lunar disc radius
    // -- moon PHASE control (user 2026-07-15: it was "full moon every day" -- a night scene puts
    // the sun opposite the moon, so scene-sun lighting is always near-full, and there was no phase
    // knob). Mode 0 keeps the legacy scene-sun lighting (pre-phase-control docs unchanged); mode 1
    // is a manual illuminated fraction; mode 2 derives the real phase from the observer clock. The
    // dialog latches ephemeris-vs-manual first-set-wins (phase 5).
    int moonPhaseMode = 0;                 // 0 = scene sun (legacy), 1 = manual, 2 = ephemeris
    double moonIlluminatedFraction = 1.0;  // manual phase (mode 1): 0 new .. 0.5 half .. 1 full
    double starsAmount = 0.5;        // star density/brightness master, 0 (none) .. 1
    // -- S55 night overhaul (user 2026-07-15): the observer's civil-UTC clock + location. It
    // orients the REAL star field (Yale BSC projected by true position, lunar.hpp) so genuine
    // constellations appear; the dialog (phase 5) also binds the sun & moon ephemerides to it
    // ("moon phase from date/place, or the manual value -- whichever was set first"). Only visible
    // at night, so the daytime default document is unaffected.
    int obsYear = 2000;               // civil UTC date...
    int obsMonth = 1;                 // 1..12
    int obsDay = 1;                   // 1..31
    double obsHourUtc = 4.0;          // fractional hours 0..24 (default = a clear winter night)
    double obsLatitudeDeg = 40.0;     // observer latitude, +N
    double obsLongitudeDeg = -74.0;   // observer longitude, +E
    // -- lens-flare growth (screen-space photographic flare off the SUN; sky_render.cpp). Off by
    // default so every pre-flare document keeps its exact bytes (the golden pins rely on it).
    bool enableLensFlare = false;  // ghost train + halo + starburst along the sun-centre axis
    double flareStrength = 0.5;    // 0..1 energy master (renderer clamps hostile values)
    std::vector<CloudLayerParams> cloudLayers{
        CloudLayerParams{},  // a low cumulus deck...
        CloudLayerParams{true, CloudType::Cirrus, 0.7, 1.0, 0.0},  // ...under high wisps (§4.5)
    };
    bool operator==(const SkyParams&) const = default;
};

// The §5.2 paper "kind" -- the structure of the height field. WOVE is an isotropic fBm tooth
// (modern machine paper: copy stock, cardstock); LAID carries the two ruled-line systems of
// mould-made paper -- fine parallel "laid" lines (~1 mm) plus the sparse perpendicular "chain"
// lines (~25 mm) -- over a wove base (antique writing/watercolour stock); FELT is the coarse,
// cloudy relief of the mould's felt side (vs the fine wire side). Chain lines are a COMPONENT of
// LAID, not a separate kind (physically the two line systems always co-occur on laid paper).
enum class PaperKind { Wove, Laid, Felt };
inline constexpr int kPaperKindCount = 3;

// Stable ASCII paper-kind name (docio tokens, tests, logging). Never localised through here.
[[nodiscard]] const char* paperKindName(PaperKind k);

// Paper (S55-d: fibre/grain/tooth height field per §5.1 with the laid/chain/wove/felt kinds ->
// single-pass Sobel normal -> Oren-Nayar raked-light + optional coated sheen -> tint; optional
// deckle-edge alpha and blue-noise print tooth. Baseline was S55-a's fBm tooth + Lambert shade.
// ⚠ GROWTH RULE (as SkyParams): fields are only ever ADDED -- docio reads an absent field as this
// default (schema stays 1), so a .mosaic written before S55-d still loads; never repurpose one).
struct PaperParams {
    ColorF tint{0.93f, 0.90f, 0.84f, 1.0f};  // warm cardstock white
    double roughness = 0.5;                  // tooth relief amplitude, 0..1
    double grainAngleDeg = 0.0;              // fiber/grain axis
    double grainAnisotropy = 0.35;           // 0 isotropic (wove) .. 1 strongly directional
    double lightAzimuthDeg = 315.0;          // raking light from the upper-left, the tactile default
    double lightElevationDeg = 25.0;         // grazing angle: low = relief throws long micro-shadow
    // -- S55-d growth --
    PaperKind kind = PaperKind::Wove;  // §5.2 height-field structure
    double fiber = 0.5;                // spectral-fibre (Gabor) streak strength along the grain, 0..1
    double laidSpacing = 5.0;          // laid-line pitch in px at Scale 1 (Laid kind)
    double chainSpacing = 90.0;        // chain-line pitch in px at Scale 1 (Laid kind, ⟂ laid)
    double laidDepth = 0.6;            // prominence of the laid/chain ruling, 0..1 (Laid kind)
    double matte = 0.7;                // Oren-Nayar roughness sigma proxy, 0 (Lambert) .. 1 (very matte)
    double sheen = 0.0;                // coated-stock Blinn-Phong specular sheen, 0..1 (0 = uncoated)
    bool deckleEdge = false;           // §5.4 torn/deckle boundary -> transparent fringe (alpha carry)
    double deckleAmount = 0.5;         // fringe irregularity/depth, 0..1
    double deckleInset = 0.06;         // fringe band width as a fraction of min(w, h)
    bool printTooth = false;           // §5.4 blue-noise print speckle in the tooth valleys
    double printAmount = 0.35;         // speckle darkening strength, 0..1
    bool operator==(const PaperParams&) const = default;
};

// Grass (S55-e: the distance-graded hybrid of §6 -- a ground-plane homography camera recedes the
// lawn to a horizon; a procedural turf base carries the far field between/behind blades; a
// single-class jittered scatter roots Bezier blades whose count per image area falls with depth
// (the cost lever); Kajiya-Kay tangent lighting + wrap-translucency + root-AO shade each blade;
// back-to-front Porter-Duff composites them over the turf. The S55-a baseline was the turf field
// alone -- Worley clump tint + Perlin jitter + wear -- which survives as the grass base pass.
// ⚠ GROWTH RULE (as SkyParams/PaperParams): fields are only ever ADDED -- docio reads an absent
// field as this default (schema stays 1), so a .mosaic written before S55-e still loads.
struct GrassParams {
    ColorF baseColor{0.16f, 0.30f, 0.09f, 1.0f};  // shaded blade green (root)
    ColorF tipColor{0.46f, 0.66f, 0.24f, 1.0f};   // lit blade green (tip)
    double clumpScale = 1.0;                      // clump cell size multiplier
    double patchiness = 0.5;                      // 0 uniform lawn .. 1 strongly patchy/worn
    // -- S55-e growth --
    ColorF soilColor{0.10f, 0.11f, 0.05f, 1.0f};  // earth/thatch showing between & under blades
    ColorF dryColor{0.60f, 0.56f, 0.26f, 1.0f};   // dead/straw blade tint (dryAmount mixes it in)
    bool enableTurf = true;    // §7.2 element toggle: off => transparent ground, blades only (§3.4)
    bool enableBlades = true;  // §7.2 element toggle: off => the turf base alone (the S55-a look)
    double density = 0.85;     // blade coverage, 0 (bald) .. 1 (dense sward)
    double bladeHeight = 1.0;  // blade length multiplier (world height x this x Scale)
    double bladeWidth = 1.0;   // blade width multiplier
    double curvature = 0.5;    // blade arc/droop, 0 (stiff upright) .. 1 (drooping)
    double windDirectionDeg = 30.0;  // the direction blades lean toward (ground compass)
    double windStrength = 0.35;      // 0 (upright) .. 1 (flattened), the global lean amount
    double fovDeg = 55.0;            // camera horizontal field of view (§6.1 homography)
    double pitchDeg = 16.0;          // camera down-tilt from level; sets where the horizon sits
    double lightAzimuthDeg = 135.0;  // sun compass direction (Kajiya-Kay tangent light)
    double lightElevationDeg = 40.0;  // sun elevation; low = long raking sheen/backlight
    double dryAmount = 0.1;           // fraction of dead/straw blades, 0..1
    bool operator==(const GrassParams&) const = default;
};

// ---- S55-g follow-on materials ----------------------------------------------------------------
// Five §5-engine materials (docs/texture-generator.md §1.1 follow-ons): each is a height-field
// recipe + tint defaults over the SAME cleared pipeline as paper -- noise-kit height ->
// single-pass Sobel normal -> Oren-Nayar raked light (+ optional Blinn-Phong sheen) -> tint
// (material_render.cpp). All render the 8-bit lane and carry paper's Scale semantics: features
// are sized in document px at Scale 1 (the registry's pixelScaledFeatures trait).
// ⚠ GROWTH RULE (as SkyParams/PaperParams): fields are only ever ADDED -- docio reads an absent
// grown field as its default (schema stays 1); never repurpose one.

// Wood: Peachey-style solid-texture growth rings (distance across the grain + turbulence
// perturbation), sparse Worley-seeded knots that bend the ring field locally, along-grain
// spectral-fibre streaks, an earlywood/latewood colour ramp.
struct WoodParams {
    ColorF earlyColor{0.78f, 0.60f, 0.38f, 1.0f};  // earlywood (the pale band)
    ColorF lateColor{0.48f, 0.30f, 0.16f, 1.0f};   // latewood (the dark dense band)
    double ringSpacing = 24.0;   // ring pitch in px at Scale 1
    double ringContrast = 0.65;  // early/late separation, 0..1
    double waviness = 0.4;       // turbulence perturbation of the ring field, 0..1
    double knots = 0.25;         // knot density, 0 (clear lumber) .. 1
    double fiber = 0.5;          // along-grain Gabor streaks, 0..1
    double grainAngleDeg = 0.0;  // the grain axis (rings band across it)
    double roughness = 0.45;     // open-grain relief amplitude, 0..1
    double matte = 0.6;          // Oren-Nayar roughness proxy, 0..1
    double sheen = 0.15;         // satin-finish Blinn-Phong sheen, 0..1
    double lightAzimuthDeg = 315.0;
    double lightElevationDeg = 35.0;
    bool operator==(const WoodParams&) const = default;
};

// Marble: Perlin-1985 veining -- sin(k*x + turbulence) sharpened into vein bands through a
// base/vein colour ramp, two fracture families, a tight polished sheen.
struct MarbleParams {
    ColorF baseColor{0.91f, 0.91f, 0.93f, 1.0f};  // the polished ground
    ColorF veinColor{0.42f, 0.44f, 0.52f, 1.0f};  // the mineral veins
    double veinSpacing = 64.0;  // primary vein pitch in px at Scale 1
    double turbulence = 0.65;   // vein meander, 0 (ruled) .. 1 (wild)
    double contrast = 0.7;      // vein strength, 0..1
    double veinAngleDeg = 25.0; // the primary fracture direction
    double roughness = 0.12;    // polished: only a hair of relief
    double matte = 0.3;
    double sheen = 0.55;        // the polish
    double lightAzimuthDeg = 315.0;
    double lightElevationDeg = 50.0;
    bool operator==(const MarbleParams&) const = default;
};

// Stone: Worley cellular aggregate -- the F2-F1 crack network between rounded cells, per-cell
// decorrelated fBm relief, per-cell albedo variation.
struct StoneParams {
    ColorF baseColor{0.62f, 0.60f, 0.56f, 1.0f};
    double cellSize = 48.0;   // aggregate cell size in px at Scale 1
    double crackDepth = 0.7;  // crack channel depth (and darkening), 0..1
    double roughness = 0.55;  // per-cell fBm relief, 0..1
    double variation = 0.45;  // per-cell albedo variation, 0..1
    double matte = 0.85;
    double sheen = 0.0;
    double lightAzimuthDeg = 315.0;
    double lightElevationDeg = 30.0;
    bool operator==(const StoneParams&) const = default;
};

// Canvas: a true woven surface -- two perpendicular sinusoid thread systems with over/under
// checker parity (the §5.2 laid/chain machinery generalized to a weave), thread wobble +
// thickness jitter, gap darkening.
struct CanvasParams {
    ColorF tint{0.87f, 0.84f, 0.76f, 1.0f};  // natural cotton duck
    double threadPitch = 7.0;   // thread spacing in px at Scale 1
    double irregularity = 0.35; // thread wobble + thickness jitter, 0..1
    double weaveDepth = 0.7;    // over/under relief prominence, 0..1
    double weaveAngleDeg = 0.0; // warp direction
    double fuzz = 0.25;         // stray-fibre micro relief, 0..1
    double matte = 0.8;
    double sheen = 0.0;
    double lightAzimuthDeg = 315.0;
    double lightElevationDeg = 30.0;
    bool operator==(const CanvasParams&) const = default;
};

// Metal: brushed sheet -- fBm stretched hard along the brush axis, a broad Blinn-Phong lobe,
// and a plain vertical reflection-ramp tint (top brighter, as a sheet reflecting sky reads).
struct MetalParams {
    ColorF tint{0.72f, 0.73f, 0.76f, 1.0f};  // brushed steel
    double brushAngleDeg = 0.0;  // the brushing direction (streaks run along it)
    double roughness = 0.35;     // streak relief amplitude, 0..1
    double sheen = 0.75;         // the specular lobe strength, 0..1
    double gradient = 0.4;       // vertical reflection-ramp tint, 0..1
    double matte = 0.25;
    double lightAzimuthDeg = 315.0;
    double lightElevationDeg = 45.0;
    bool operator==(const MetalParams&) const = default;
};

struct TextureParams {
    Generator generator = Generator::Sky;
    std::uint64_t seed = 0;  // determinism (§8.3); the dialog's "Randomize" reseeds
    double scale = 1.0;      // the mandatory Scale slider -- generator feature size in document units
    std::variant<SkyParams, PaperParams, GrassParams, WoodParams, MarbleParams, StoneParams,
                 CanvasParams, MetalParams>
        spec;
    bool operator==(const TextureParams&) const = default;  // the cache-validity comparison
};

// A fresh generator's defaults: the right variant arm seeded for `g` (the modal's rail switch,
// Document::makeTexture callers, tests).
[[nodiscard]] TextureParams defaultTextureParams(Generator g);

}  // namespace mosaic::core::texture
