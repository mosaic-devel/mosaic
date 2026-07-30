#pragma once

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "core/brush/curve_option.hpp"
#include "core/brush/stroke_state.hpp"

#include <array>
#include <optional>
#include <string_view>

// One dab's resolved parameters, and the step that resolves them: the middle of docs/brushes.md §6.2.
//
//     sample -> StrokeState -> evaluate each CurveOption -> Dab -> DabMask -> stamp
//                              ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
//
// Everything on either side of that arrow already existed and was tested; nothing read it. This is
// what reads it.
//
// ⚠ §6.2 calls this block `DabPlacement`. It cannot be called that here: dab_mask.hpp has owned a
// `DabPlacement` since 2026-07-09, and it means something else entirely (WHERE a rasterized mask
// lands -- an integer corner and a quantized sub-pixel phase). This is what a dab IS; that is where
// it goes. The docs are corrected to match the code rather than the other way round.
//
// FLTK-, Vulkan- and platform-free.
namespace mosaic::core::brush {

// The dab, after the options have had their say. It is deliberately GEOMETRY AND ALPHA only.
//
// §6.2's field list also names `opacity` and `color`, and both are absent on purpose rather than
// forgotten: neither is a property of a dab's shape, and each changes the stroke's ACCUMULATION
// model rather than its placement. The per-dab opacity IS driven now (BrushOptions::opacity below)
// -- but as the engine's own per-dab state beside the dab, never as a Dab field: it is a per-dab
// CEILING the wash accumulation aims at, not a per-dab flow, and the stamp's geometry has no use
// for it. Colour rides the existing BrushDynamics::dabColor seam until the colour-dynamics options
// (h/s/v/Mix/Darken -- 2 presets of 82) land; it still needs its own goldens and its own commit.
struct Dab {
    common::Vec2 center{}; // document px
    double diameter = 24.0;
    double ratio = 1.0;     // height / width; 1 is a circle
    double angleRad = 0.0;  // the tip's rotation within the document
    double flow = 1.0;      // [0,1], paint deposited by this dab
    // The live Softness option, 1 = as authored (mask_generator.hpp). It reaches a PROCEDURAL tip and
    // nothing else: the option scales a mask generator's fade coefficients, and neither a decoded
    // bitmap raster nor the engine's built-in analytic falloff -- which is parameterized by hardness,
    // a different quantity -- has anything for it to scale.
    double softness = 1.0;
    // Which cell of an animated tip (a `.gih` hose) this dab stamps. `evaluateDab` does NOT set it,
    // and that is the one field it does not own: choosing a frame needs the tip, and a `Random` or
    // `Incremental` selection advances state -- so the engine picks it, exactly once per dab, beside
    // the option pipeline (brush_engine.cpp's resolveDab). 0 on every tip with one frame.
    int frame = 0;
    bool mirrorH = false;
    bool mirrorV = false;
};

// The preset's STATIC geometry: the dab a stroke would lay if no option were driving anything. Every
// option scales or offsets one of these, so a Dab with no options at all is exactly this at `center`.
struct DabBase {
    double diameter = 24.0;
    double ratio = 1.0;
    double angleRad = 0.0;
    double flow = 1.0;
    // 1 is "as authored" -- the preset's softness is already baked into the tip's fade coefficients
    // and its softness curve, so the base is the identity and the option is the only thing that moves
    // it. (A static softness slider in the editor would move THIS, by rebuilding the tip.)
    double softness = 1.0;
};

// The strength range each option's CONSUMER imposes (`readCurveOption`'s `strengthMin`/`strengthMax`,
// which are not preset properties -- §3.2), and whether it is checkable. One table, so the preset
// reader and the dab pipeline cannot end up disagreeing about what a strength of 1 means.
//
// Opacity and Flow are the two always-on options: their `Pressure{X}` bit is written to shipped files
// but the reader forces them on regardless, so "enabled" must never be inferred from the bit.
struct BrushOptionSpec {
    std::string_view base;
    bool checkable = true;
    double strengthMin = 0.0;
    double strengthMax = 1.0;
};
inline constexpr BrushOptionSpec kSizeOptionSpec{"Size", true, 0.0, 1.0};
inline constexpr BrushOptionSpec kFlowOptionSpec{"Flow", false, 0.0, 1.0};
inline constexpr BrushOptionSpec kRatioOptionSpec{"Ratio", true, 0.0, 1.0};
inline constexpr BrushOptionSpec kRotationOptionSpec{"Rotation", true, 0.0, 1.0};
// Softness alone has a strength FLOOR: 0.1, not 0. A tip whose fades are scaled to nothing is not a
// soft tip, it is an invisible one, and the format's own consumer refuses to author that.
inline constexpr BrushOptionSpec kSoftnessOptionSpec{"Softness", true, 0.1, 1.0};
// The second always-on option. Its SENSORS ride every dab as a per-dab CEILING through the Wash
// accumulation (brush_engine.hpp's washAlphaDarkenAlpha); its static STRENGTH is the whole stroke's
// ceiling (BrushParams::opacity) and never enters the per-dab value -- the reference's indirect
// painting splits it exactly there, and folding it in twice would square it.
//
// ⚠ UNDER THE SMUDGE ENGINE THE SPLIT INVERTS (brush_engine.hpp's SmudgeParams): colorsmudge is
// DIRECT painting in the reference, so there the per-dab opacity DOES include its strength
// (`useStrength=true`) and no stroke-level cap applies at all. One option, two consumers, and the
// consumer decides -- exactly as the reference's `useStrengthValue` flag does.
inline constexpr BrushOptionSpec kOpacityOptionSpec{"Opacity", false, 0.0, 1.0};
// The smudge trio (brush_engine.hpp's smudge walk; transcribed from the reference's colorsmudge --
// docs/brushes.md §6.6c). All three follow the reference's `computeSizeLikeValue` WITH strength,
// and all three have the UNCHECKED value the reference hard-codes rather than falling back to
// their static strength: an unchecked SmudgeRate is 1.0, an unchecked ColorRate is 0.0 (that is
// what makes a pure blender a blender), an unchecked SmudgeRadius is 0.0 (sample one pixel).
//
// ⚠ SmudgeRadius' range is [0,3] AFTER the reader's /100 version migration (SmudgeRadiusVersion
// < 2 stores percent; every shipped preset does) -- the migration belongs to the mapper, the range
// to this spec, and sizeLikeValue's clamp applies it at evaluation.
inline constexpr BrushOptionSpec kSmudgeLengthOptionSpec{"SmudgeRate", true, 0.0, 1.0};
inline constexpr BrushOptionSpec kColorRateOptionSpec{"ColorRate", true, 0.0, 1.0};
inline constexpr BrushOptionSpec kSmudgeRadiusOptionSpec{"SmudgeRadius", true, 0.0, 3.0};
// Scatter is the one option whose strength runs past 1: the reference's consumer imposes [0,5] --
// five dab-diameters of jitter at full strength -- and its jitter formula reads the value through
// the same sizeLikeValue clamp as everything else (§6.6d).
inline constexpr BrushOptionSpec kScatterOptionSpec{"Scatter", true, 0.0, 5.0};
inline constexpr BrushOptionSpec kMirrorOptionSpec{"Mirror", true, 0.0, 1.0};
// Spacing is the one CADENCE option, not a dab-shape one: the reference's KisSpacingOption::apply
// returns computeSizeLikeValue WITH strength over [0,1] and multiplies the WHOLE spacing interval --
// both axes, in every branch (auto/manual, isotropic/anisotropic) -- so a value below 1 tightens the
// stroke's dab density (docs/brushes.md §6.6e). Checkable; unchecked contributes exactly 1.0.
inline constexpr BrushOptionSpec kSpacingOptionSpec{"Spacing", true, 0.0, 1.0};
// Sharpness does two things off ONE per-dab value (`computeSizeLikeValue` WITH strength, [0,1]): it
// hardens the dab's mask by an alpha threshold (whenever CHECKED) and -- only when its `alignoutline`
// flag is set and its static strength is > 0 -- snaps the dab's centre to the pixel grid. §6.6e.
inline constexpr BrushOptionSpec kSharpnessOptionSpec{"Sharpness", true, 0.0, 1.0};
// Colour dynamics (h/s/v, §6.6f): the three HSV adjustments the reference applies to the paint
// COLOUR itself (not the tip), each a plain checkable curve option over [0,1]. Hue reads as a
// ROTATION-like value -- a half-turn hue rotation (`h += value*180` degrees); saturation and value
// read as SIZE-like values re-centred on 0.5 (the reference's KisHSVOption remap, applyColorDynamics).
// They ride the Colored accumulator's per-dab colour, so they are honoured only there -- NEVER under
// smudge, whose walk paints the stroke's own colour. The other two colour-dynamics bases, `Mix` and
// `Darken`, are deliberately absent: neither is transcribed yet (Mix needs the background colour and
// an alpha-weighted mix, Darken a Lab round-trip), and no shipped preset drives either.
inline constexpr BrushOptionSpec kHueOptionSpec{"h", true, 0.0, 1.0};
inline constexpr BrushOptionSpec kSaturationOptionSpec{"s", true, 0.0, 1.0};
inline constexpr BrushOptionSpec kValueOptionSpec{"v", true, 0.0, 1.0};
// The SKETCH engine's three own options (docs/brushes.md §6.6g; stroke_painter.hpp). They are plain
// checkable [0,1] curve options read through the reference's `KisStandardOption::apply` like every
// other one -- what makes them different is their CONSUMER: only the sketch painter reads them, the
// dab walk has nothing to do with any of the three. That is exactly the smudge trio's shape, and it
// carries the same caveat, owned by the mapper: an active one on any other paintop is a dropped
// option, badged, not silently honoured-by-list.
//
// ⚠ Two of the three base names CONTAIN A SPACE, and that is the file format, not a typo: the keys
// are `PressureLine width`, `Line widthSensor`, `Line widthValue`, ... exactly as `CurveOptionKeys::
// forBase` builds them.
inline constexpr BrushOptionSpec kDensityOptionSpec{"Density", true, 0.0, 1.0};
inline constexpr BrushOptionSpec kLineWidthOptionSpec{"Line width", true, 0.0, 1.0};
inline constexpr BrushOptionSpec kOffsetScaleOptionSpec{"Offset scale", true, 0.0, 1.0};
// The CURVE engine's own opacity option (§6.6g). ⚠ `Line width` is SHARED with the sketch engine,
// and upstream the two consumers impose DIFFERENT ranges -- [0,1] on the sketch paintop, [0.1,1] on
// the curve one. One base, one spec: Mosaic keeps the wider [0,1] and records the deviation. Neither
// shipped preset checks the option, so the floor is inert.
inline constexpr BrushOptionSpec kCurvesOpacityOptionSpec{"Curves opacity", true, 0.0, 1.0};
// The HATCHING engine's four (§6.6g). Not a painter's: hatching is a DAB engine whose dab content is
// a procedural lattice stencilled by the tip mask, so these ride resolveDab beside the smudge
// quartet. Same caveat as every other single-consumer family: honoured by the hatching stencil
// alone, badged on any other paintop.
inline constexpr BrushOptionSpec kAngleOptionSpec{"Angle", true, 0.0, 1.0};
inline constexpr BrushOptionSpec kCrosshatchingOptionSpec{"Crosshatching", true, 0.0, 1.0};
inline constexpr BrushOptionSpec kSeparationOptionSpec{"Separation", true, 0.0, 1.0};
inline constexpr BrushOptionSpec kThicknessOptionSpec{"Thickness", true, 0.0, 1.0};
// The TEXTURE option's strength (§6.6h). ⚠ THE BASE NAME CONTAINS SLASHES AND ENDS IN ONE -- the
// keys really are `PressureTexture/Strength/`, `Texture/Strength/Value`, ... -- and that is the
// format's, exactly as three of the sketch/curve bases contain a SPACE. Read through the reference's
// `KisStandardOption::apply` like every other one (checked -> size-like value WITH strength,
// unchecked -> exactly 1.0); its consumer is the texture composite and nothing else, so an active
// one on a preset whose texturing is off costs nothing and is badged nowhere -- the reference's own
// option is equally inert without a pattern.
inline constexpr BrushOptionSpec kTextureStrengthOptionSpec{"Texture/Strength/", true, 0.0, 1.0};
// The AIRBRUSH rate option (§6.6h). It scales the stroke's TIMED dab interval rather than any
// property of a dab: the reference divides the interval `1000/rate` by this value, so a value below
// 1 lays the timed dabs further apart and a value of 0 stops them entirely. Like Spacing it is a
// CADENCE option, and like Spacing it is read WITH strength over [0,1].
inline constexpr BrushOptionSpec kRateOptionSpec{"Rate", true, 0.0, 1.0};

// ⚠ THE ONE LIST OF WHAT A DAB ACTUALLY READS. The importer's honesty contract (io/brush/mapper.cpp's
// `optionSupported` -- which decides whether a preset imports as Exact or gets badged) and the
// preset -> engine mapping (io/brush/preset_brush.cpp -- which decides what a stroke is laid with)
// BOTH read it, so an option cannot be driven without being honoured, or honoured without being
// driven.
//
// It is a list and not a plan. It named Scatter, Mirror and Spacing once, in a place the engine could
// not see, and 26 presets imported as Exact and painted without them for two days.
//
// ⚠ `Opacity` is read in BOTH accumulation modes, and they are two different mechanisms wearing one
// option (§6.6i): under WASH -- the reference's indirect painting -- the per-dab value is a per-dab
// CEILING the accumulation strives toward (washAlphaDarkenAlpha); under BUILDUP -- direct painting,
// with no stroke temp to strive toward -- it is the per-dab SHARE of the deposit (buildCap). Both
// are transcribed, so the option carries no mode caveat any more.
//
// ⚠ The smudge trio carries a caveat of a different KIND: the engine reads
// SmudgeRate/ColorRate/SmudgeRadius only when the SMUDGE walk is active (SmudgeParams::enabled) --
// and so does the reference, whose pixel brush constructs no smudge option at all. So one of the
// three active on a preset that maps to any other engine is dropped WITHOUT a badge: the stroke
// matches, which is what a badge measures. (It was badged until 2026-07-28, and five Krita-2-era
// presets that merely carry the stale key were Approximated for it -- §6.6i.)
//
// ⚠ `Mirror` carries the inverse caveat, owned by preset_brush.cpp's mapping: the reference's
// colorsmudge engine never reads its Mirror option (the settings widget offers one, the paintop
// constructs none -- only the pixel brush's dab executor wires mirror postprocessing), so a Mirror
// on a colorsmudge preset is DROPPED there without a badge: the stroke matches the reference
// exactly, because the reference ignores it too. Scatter, by contrast, rides both walks -- the
// reference's colorsmudge applies it to the dab position like the pixel brush does.
// ⚠ The sketch trio carries the smudge trio's caveat with a different consumer: the SKETCH PAINTER
// (stroke_painter.hpp) is the only thing that reads Density / Line width / Offset scale, so one of
// them active on a preset that maps to any other engine is badged, not honoured-by-list.
inline constexpr std::array<BrushOptionSpec, 26> kDrivenOptions{
    kSizeOptionSpec,     kFlowOptionSpec,         kRatioOptionSpec,
    kRotationOptionSpec, kSoftnessOptionSpec,     kOpacityOptionSpec,
    kSmudgeLengthOptionSpec, kColorRateOptionSpec, kSmudgeRadiusOptionSpec,
    kScatterOptionSpec,  kMirrorOptionSpec,       kSpacingOptionSpec,
    kSharpnessOptionSpec, kHueOptionSpec,         kSaturationOptionSpec,
    kValueOptionSpec,    kDensityOptionSpec,      kLineWidthOptionSpec,
    kOffsetScaleOptionSpec, kCurvesOpacityOptionSpec, kAngleOptionSpec,
    kCrosshatchingOptionSpec, kSeparationOptionSpec, kThicknessOptionSpec,
    kTextureStrengthOptionSpec, kRateOptionSpec,
};

// The spec for `base`, or null when the dab pipeline does not read that option at all.
[[nodiscard]] const BrushOptionSpec* drivenOption(std::string_view base) noexcept;

// Scatter and Mirror each carry data BESIDE their curve option -- axis gates that are plain preset
// properties, not sensor-driven values -- so their BrushOptions slots wrap the option rather than
// being one. Both transcribed from the reference (docs/brushes.md §6.6d).

// `Scattering/AxisX`/`AxisY`, default true. With both gates off the option is inert whatever its
// sensors say -- the reference returns the unjittered position before ever drawing a random number,
// and so does applyScatter, which is a property its stream discipline depends on.
struct ScatterOption {
    CurveOption option;
    bool axisX = true;
    bool axisY = true;
};

// `HorizontalMirrorEnabled`/`VerticalMirrorEnabled`, default false. Same inertness rule: no axis,
// no sensor draw.
struct MirrorOption {
    CurveOption option;
    bool horizontal = false;
    bool vertical = false;
};

// `Sharpness/alignoutline` (default false) and `Sharpness/softness` (int [0,100], default 0), the
// two static properties beside Sharpness's curve option (§6.6e) -- like the positional gates. The
// option has TWO effects on ONE per-dab value: applyThreshold hardens the dab's alpha (whenever the
// option is CHECKED), and the coordinate snap aligns the dab's centre to the pixel grid (only when
// `alignOutline` is set AND the static strength is > 0 -- the reference's own gate). `softness` is
// applyThreshold's soft band: at softness 0 a pixel is either opaque or transparent, nothing kept.
struct SharpnessOption {
    CurveOption option;
    bool alignOutline = false;
    int softness = 0;
};

// The options a preset drives its dabs with.
//
// An ABSENT option is not a disabled one: it is an option the preset never mentioned, and it
// contributes exactly the identity. That is what keeps a stroke with no options byte-for-byte the
// stroke the engine laid before any of this existed -- the hard rule of §6.2.
//
// Eleven of the format's bases are here (six shape/alpha ones, the smudge trio, and the two
// positional ones -- Scatter jitters where a dab lands, Mirror flips its tip). The count is not
// the point; the pipeline is, and these are the ones whose mapping is settled in the tree or in
// the docs. What is deliberately still out, and why -- each needs a fact this build does not have:
//
//   - The colour bases `Mix` and `Darken`: `h`/`s`/`v` landed 2026-07-18 (the HSV colour
//     dynamics -- they resolve the Colored accumulator's per-dab colour, see applyColorDynamics
//     below); Mix and Darken are the two that still wait (untranscribed, undriven by any preset).
//   - `LightnessStrength`: a later tier. It scales how hard a LIGHTNESS-MAP tip's own lightness
//     drives the deposit, and no shipped preset carries a lightness-map tip at all.
//     (`Texture/Strength/` and `Rate` landed 2026-07-28 -- §6.6h.)
//
// (`Scatter` -- 8 presets -- and `Mirror` -- 11 -- sat on that list from the day it was written
// until 2026-07-14, on the honest ground that the reference's formulas had never been read. They
// have been now -- §6.6d is the transcription record -- and both ride resolveDab: Scatter offsets
// the dab's centre AFTER the frame selection, because its jitter amplitude is the rotated tip's
// own extents; Mirror sets the Dab's mirrorH/mirrorV, which the mask pipeline has carried since
// before either option could drive them.)
//
// (`Softness` -- 3 presets -- used to be on that list, because the engine had no mask generator to
// scale: it walked an analytic falloff parameterized by HARDNESS, a different quantity, and wiring
// the option meant inventing the map between the two. The tip lands in the engine now (brush_tip.hpp),
// so the option scales the thing it was written for and the map is not needed. `Opacity` -- all 82 --
// waited longer still, because a per-dab opacity is a per-dab CEILING, not a per-dab flow, and the
// single wash coverage channel could not hold both until the accumulation itself was transcribed:
// see `opacity` below and brush_engine.hpp's washAlphaDarkenAlpha.)
struct BrushOptions {
    std::optional<CurveOption> size;     // scales the diameter
    std::optional<CurveOption> flow;     // scales the flow
    std::optional<CurveOption> ratio;    // scales the tip's height/width
    std::optional<CurveOption> rotation; // ADDS to the tip's authored angle
    std::optional<CurveOption> softness; // scales a PROCEDURAL tip's softness; nothing else reads it
    // The per-dab CEILING (docs/brushes.md §6.2). Deliberately NOT a Dab field -- it changes the
    // stroke's ACCUMULATION, not the dab's shape -- so the engine evaluates it beside the dab
    // (resolveDab), always WITHOUT its strength: the strength is the whole stroke's ceiling
    // (BrushParams::opacity), and the reference's indirect painting is what draws that line.
    // (Under the SMUDGE engine the same option evaluates WITH its strength and no stroke cap
    // exists -- colorsmudge is direct painting; see kOpacityOptionSpec.)
    std::optional<CurveOption> opacity;
    // The smudge trio (kSmudge*/kColorRate* specs above; docs/brushes.md §6.6c). Like `opacity`,
    // none is a Dab field: they parameterize the smudge ACCUMULATION, evaluated beside the dab in
    // resolveDab, once per dab in order. Read only when SmudgeParams::enabled.
    std::optional<CurveOption> smudgeRate;
    std::optional<CurveOption> colorRate;
    std::optional<CurveOption> smudgeRadius;
    // The two positional options (§6.6d). Evaluated in resolveDab AFTER everything above -- new
    // draws append to the per-dab stream, they never reorder the draws the goldens pinned -- and
    // after the frame selection, because Scatter's amplitude is the extents of the frame this dab
    // actually stamps. Scatter rides both walks; `mirror` is never filled on a smudge preset
    // (preset_brush.cpp -- the reference's colorsmudge ignores its Mirror option, so honouring it
    // there would be the unfaithful choice).
    std::optional<ScatterOption> scatter;
    std::optional<MirrorOption> mirror;
    // The Spacing cadence option (§6.6e). NOT a Dab field and NOT positional: it scales the stroke's
    // dab INTERVAL, not the dab -- the engine evaluates it once per dab (resolveDab) and multiplies
    // the spacing ellipse by it (brush_engine.cpp's dabSpacingEllipse), WITH strength, over [0,1].
    // Rides both walks, like Scatter -- the reference's colorsmudge spaces its dabs through the same
    // KisSpacingOption. Absent or unchecked it is exactly 1.0, and `interval * 1.0 == interval`, so
    // every spacing golden holds.
    std::optional<CurveOption> spacing;
    // The Sharpness option (§6.6e): threshold + pixel-grid snap, carrying `alignOutline` + `softness`
    // beside the curve, so it wraps like Scatter/Mirror. Evaluated once per dab in resolveDab (the
    // threshold value AND the snap read the one draw). ⚠ NEVER filled on a smudge preset -- the
    // reference's colorsmudge installs no sharpness option at all, so dropping it there is the
    // faithful stroke, badge-free (preset_brush.cpp), exactly like Mirror.
    std::optional<SharpnessOption> sharpness;
    // Colour dynamics (h/s/v, §6.6f): the three HSV adjustments to the paint COLOUR. Plain
    // CurveOption slots (no gates beside them, like Spacing) -- the engine resolves them together
    // in resolveDab into the dab's per-dab colour (applyColorDynamics), read only on the Colored
    // accumulator. Absent or unchecked, each contributes a zero adjustment and draws nothing.
    std::optional<CurveOption> hue;        // `h`: a half-turn hue rotation
    std::optional<CurveOption> saturation; // `s`: scales chroma, re-centred on 0.5
    std::optional<CurveOption> value;      // `v`: scales value/brightness, re-centred on 0.5
    // The sketch engine's three (§6.6g). Read ONLY by the sketch StrokePainter -- `evaluateDab`
    // never touches them, exactly as the smudge trio is read only by the smudge walk. Each scales
    // one of the three things the web is made of: how often a candidate connection is drawn
    // (`density`), how wide each connection is (`lineWidth`), and how far its two ends are pulled
    // along the line joining them (`offsetScale`).
    std::optional<CurveOption> density;     // `Density`
    std::optional<CurveOption> lineWidth;   // `Line width` -- the space is the format
    std::optional<CurveOption> offsetScale; // `Offset scale`
    // The curve engine's own (§6.6g); `lineWidth` above is shared with the sketch engine.
    std::optional<CurveOption> curvesOpacity; // `Curves opacity`
    // The hatching engine's four (§6.6g). Read ONLY when HatchingParams::enabled, exactly as the
    // smudge trio is read only under SmudgeParams::enabled -- and read by the DAB walk, not by a
    // painter: hatching stencils the tip mask with a procedural lattice.
    std::optional<CurveOption> hatchAngle;         // `Angle`
    std::optional<CurveOption> crosshatching;      // `Crosshatching`
    std::optional<CurveOption> separation;         // `Separation`
    std::optional<CurveOption> thickness;          // `Thickness`
    // The texture option's strength and the airbrush's rate (§6.6h). Neither is a Dab field: the
    // first parameterizes a COMPOSITE over the dab's mask (brush_engine.cpp's texture stencil), the
    // second the stroke's TIMED dab interval (walkSpan's second cadence) -- so both are evaluated
    // beside the dab in resolveDab, like the opacity pair and the smudge quartet.
    std::optional<CurveOption> textureStrength; // `Texture/Strength/` -- the slashes are the format
    std::optional<CurveOption> rate;            // `Rate`
};

// A rotation-like value is in HALF TURNS -- `rotationLikeValue` doubles the canvas angle into its
// [-1,1) space (a normalized base angle of 0.25, a quarter turn, reads as 0.5) -- so this, not a full
// turn, is what carries it into radians. An ellipse has a period of pi, so a pressure sweep at full
// strength turning the dab a half turn each way is the whole of its range, not a fraction of it.
inline constexpr double kRotationTurnRad = 3.14159265358979323846;

// The §6.2 step: sensors -> curves -> one Dab, at `center`, against the stroke as it stands.
//
// `state` is non-const and that is not an oversight: evaluating an option draws from the stroke's
// random stream (`fuzzy`) and latches its drawing angle (`lockedAngleMode`). Evaluating a dab CHANGES
// the stroke, which is why the walk must evaluate each dab exactly once and in order.
//
// ⚠ The caller must have put `state` at the DAB's own point on the stroke first (StrokeState::
// rewindTo). The dab walk lags the sample stream by one sample, so the live state belongs to a sample
// the dab has not reached.
[[nodiscard]] Dab evaluateDab(const BrushOptions& options, const DabBase& base, common::Vec2 center,
                              StrokeState& state);

// The §6.6d Scatter step: jitter `dab.center` by up to `sensorValue` times the dab's larger extent,
// per axis. `extentW`/`extentH` are the AXIS-ALIGNED extents of the rotated tip as this dab stamps
// it (dabExtent of the mask pipeline's DabShape) -- the reference measures its jitter in mask-raster
// dimensions, and that box is what a mask raster is. The transcribed formula, per the reference's
// scatter option:
//
//     jitter = (2*rand01 - 1) * max(extentW, extentH) * sizeLikeValue
//
// Both axes on: two INDEPENDENT draws, X first, then Y -- the draw order is part of the stroke's
// replay contract. One axis on: ONE draw, laid along the stroke's drawing angle (X) or its normal
// (Y). Unchecked, or both axes off: NO draws at all -- the reference returns before its random
// source, and a stroke must replay the same whether an inert scatter is present or absent.
//
// The stamp itself is untouched: Scatter moves a dab, never the walk. The spacing cadence, the
// sample stream and every other option read the stroke's own path.
void applyScatter(const ScatterOption& sc, double extentW, double extentH, StrokeState& state,
                  Dab& dab);

// The §6.6d Mirror step: one size-like draw per dab, and `>= 0.5` flips every ENABLED axis --
// so a `fuzzy`-driven Mirror is a fair per-dab coin, and a pressure-driven one flips exactly while
// the pen presses past half. Sets `dab.mirrorH`/`mirrorV`; the mask pipeline does the rest.
//
// ⚠ The dab's ANGLE passes through unchanged, and that is the transcription, not a drop: the
// reference renders a single-axis flip as "negate the rotation, rasterize, then flip the raster"
// -- flip∘R(-θ) -- while this pipeline's TipTransform flips the tip in its own frame BEFORE
// rotating -- R(θ)∘flip -- and a flip conjugates a rotation into its inverse, so the two compose
// to the SAME map, exactly. (Both axes flipped is a half-turn for both forms, and the reference
// negates nothing then either.) The AABB extents agree the same way, so placement, spacing and
// the cache key all see one geometry. Inert (unchecked, or no axis enabled): no draw.
void applyMirror(const MirrorOption& mo, StrokeState& state, Dab& dab);

// The §6.6e Sharpness coordinate snap: shift `dab.center` so its mask's TOP-LEFT lands toward the
// pixel grid. The reference snaps `pt = center - halfExtent` via `sharpness*round(pt) + (1 -
// sharpness)*pt`; the centre delta is the same `sharpness*(round(pt) - pt)` on each axis, so at
// sharpness 1 the top-left snaps to an integer (zero sub-pixel phase -- pixel-perfect) and at 0 it
// does not move. `extentW`/`extentH` are the rotated tip's axis-aligned extents, exactly as placeDab
// measures them (and as applyScatter reads them). `sharpness` is the per-dab value in [0,1], drawn
// once by the engine and shared with the threshold; this takes no draw of its own. Pure geometry ->
// unit-tested with exact-equality predictions.
void applySharpnessSnap(double sharpness, double extentW, double extentH, Dab& dab);

// The §6.6f colour-dynamics step: adjust the paint `base` colour by the HSV options (h/s/v),
// returning the per-dab colour the Colored accumulator deposits. Transcribed from the reference's
// brush-dynamics colour path (its hsv_adjustment in the NON-compatibility HSV mode -- the
// KisHSVOption sets type=HSV, colorize off and compatibilityMode OFF, selecting the reference's
// `HSVTransform<HSVPolicy>` branch rather than the legacy RGB<->HSV one).
//
// The three channels are evaluated in the reference's order -- hue, saturation, value -- and each
// DRAWS from the stroke's random streams only when checked (the reference's apply() returns before
// computing anything on an unchecked option). Hue is a `rotationLikeValue` (a half-turn hue
// rotation: `h += value*180` degrees); saturation and value are size-like values put through the
// reference's remap `val = 2*(rawWithStrength*strength + (0.5 - 0.5*strength)) - 1`, so each lands
// in [-strength, +strength] and is zero at the sensor's midpoint. An absent or all-unchecked set
// draws NOTHING and returns `base` unchanged -- the inert contract Scatter/Mirror keep.
[[nodiscard]] common::Color8 applyColorDynamics(const BrushOptions& options, common::Color8 base,
                                                StrokeState& state);

// The pure HSV pixel adjustment (§6.6f), factored out so the transcribed sextant math can be pinned
// on its own. `dh`/`ds`/`dv` are the reference's `m_adj_h`/`m_adj_s`/`m_adj_v` in [-1,1]: `dh` a
// half-turn hue rotation, `ds`/`dv` the chroma/value scales. Straight (non-premultiplied) 8-bit
// RGB in and out; alpha passes through. The math runs in `float`, exactly as the reference does,
// and the result is round-to-nearest 8-bit (KoColorSpaceMaths<float,quint8>). Free + pure.
[[nodiscard]] common::Color8 hsvAdjust(common::Color8 base, double dh, double ds, double dv);

// The angle a dab is laid at: the tip's authored angle, plus whatever `Rotation` adds. Factored out
// of evaluateDab because the RETICLE has to draw the tip the next dab will lay, and two copies of
// this rule would drift -- which is not hypothetical. The ring showed a knife's authored slant while
// the knife itself turned to follow the stroke, and the user reported it.
[[nodiscard]] double dabAngle(const BrushOptions& options, double baseAngleRad, StrokeState& state);

// The same angle, for a UI that must draw the tip BEFORE any dab exists (ui/brush_reticle.hpp).
//
// ⚠ THE RULE, and it is a ruling, not a heuristic: THE RETICLE FOLLOWS DIRECTION AND IGNORES
// MAGNITUDE. It turns with the stroke's heading, the pen's tilt BEARING and the barrel's twist; it
// does not move with pressure, speed, tilt ANGLE, fade or distance. §6.3 already settled the second
// half ("a ring that breathed with the pen would be unusable as a size gauge, which is the one job
// it has") -- and the same argument says nothing at all about an ORIENTATION, which is precisely a
// statement about where the paint is going to land.
//
// The split costs no judgement call, because the sensor CLASSIFICATION already draws it
// (sensors.cpp): `drawingangle` is the sole AbsoluteRotation sensor; `rotation` (barrel) and
// `ascension` (tilt bearing) are Additive; and every magnitude -- pressure, speed, `declination`
// (tilt angle), fade, distance, time -- is Scaling. So this is `rotationLikeValue` with the scaling
// part switched off, which is a lever the arithmetic already had.
//
// A RANDOM rotation (`fuzzy`/`fuzzystroke`) is not a direction, so it is not shown: the tip keeps its
// authored angle. A ring that jittered on every motion event would be telling the user something
// true and useless.
//
// `pen` supplies tilt and barrel (a mouse leaves them at rest, which is correct -- a mouse has none).
// A non-finite heading means "no direction yet" and yields the authored angle.
[[nodiscard]] double reticleDabAngle(const BrushOptions& options, double baseAngleRad,
                                     const StrokeInput& pen, double headingRad);

} // namespace mosaic::core::brush
