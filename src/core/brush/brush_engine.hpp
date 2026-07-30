#pragma once

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "core/blend_mode.hpp"
#include "core/brush/brush_tip.hpp"   // BrushTip -- the procedural/bitmap tip a dab stamps
#include "core/brush/dab.hpp"         // Dab, DabBase, BrushOptions -- the §6.2 option pipeline
#include "core/brush/dab_cache.hpp"   // DabMaskCache -- one rasterized dab, memoized
#include "core/brush/hatching.hpp"    // HatchingParams -- the procedural-lattice dab (§6.6g)
#include "core/brush/stroke_painter.hpp" // StrokePainter -- the SECOND engine kind (§6.6b/§6.6g)
#include "core/brush/stroke_state.hpp" // StrokeInput -- the engine's sample; StrokeState
#include "core/stroke_confinement.hpp" // StrokeConfinement -- the selection, on this stroke's grid
#include "core/brush/texture.hpp"      // TextureParams -- the document-locked grain (§6.6h)

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

// The CPU brush stamping engine (S19-a) -- the shared base every brush-family tool paints through
// (Brush now; Eraser/Smudge S23, Heal S38, the Inpaint brush S39). It is deliberately FLTK- and
// Vulkan-free so it is unit-tested headlessly and runs unchanged in a future CPU-only mode; the
// canvas (ui::VulkanCanvas) drives it from pointer events. GPU stamping (the S60-a active-layer
// residency pull-forward) can later replace the inner stamping loop *behind this same interface*.
//
// Model (the standard flow-vs-opacity split): dabs are stamped along the stroke into a per-pixel
// coverage buffer with "over" accumulation gated by `flow`, so heavily overlapped passes within ONE
// stroke build toward 1 -- and the whole accumulated coverage is composited over the untouched base
// once at `opacity`, so a single stroke never exceeds its opacity even where it crosses itself.
// A preset driving a DYNAMIC Opacity option adds the per-dab ceiling on top (§6.2): the coverage
// then accumulates by the transcribed washAlphaDarkenAlpha step, striving toward each dab's own
// sensor-driven ceiling instead of 1, while the static `opacity` stays the whole stroke's cap.
//
// That is `PaintMode::Wash`, the default, and it is unchanged. Beside it sit `PaintMode::Buildup`
// (overlapping dabs climb past `opacity`), `StrokeMode::Erase` (destination-out), a `BlendMode`
// applied at composite, and the colour axis `StrokeAccumulator {Uniform, Colored}` -- docs/
// brushes.md §6.1. All combinations are reachable, and the coverage buffer is maintained in every
// one of them because the Inpaint brush (S39) reads it as a mask.
//
// `Uniform x Wash x Normal` is byte-for-byte what it was before those axes existed, and that is a
// test: tests/test_brush_wash_golden.cpp. Do not "unify" its composite arithmetic with the blend
// path; the float round-trip through an identity blend moves pixels.
namespace mosaic::core::brush {

// How the stroke's dabs accumulate toward the target (docs/brushes.md §6.1).
enum class PaintMode {
    // The model above: the coverage buffer builds toward 1, and the whole of it is composited once
    // at `opacity`, so a stroke can never exceed its opacity however often it crosses itself.
    Wash,
    // Each dab carries its own share of the opacity, and those shares accumulate -- so overlapping
    // dabs push a pixel PAST `opacity`, toward fully opaque. The coverage buffer is still
    // maintained (the Inpaint brush reads it as a hole mask), it just no longer sets the alpha.
    Buildup,
};

// Whether the stroke adds paint or takes it away.
enum class StrokeMode {
    Paint,
    // Destination-out against the pristine base: the colour under the stroke is left alone and its
    // alpha is carved back by the stroke's alpha. `color` and `blendMode` are not consulted.
    Erase,
};

// The colour axis (docs/brushes.md §6.1): what a dab deposits besides its alpha.
enum class StrokeAccumulator {
    // One colour for the whole stroke, applied once at composite. The fast path -- a single float
    // of coverage per pixel -- and the auto-selected default: every preset in the shipped set is a
    // grayscale alpha mask, so it runs entirely on this.
    Uniform,
    // Each dab also deposits its own colour, premultiplied by the dab's alpha, into an RGBA
    // accumulation beside the coverage; composite() normalizes that accumulation back to a colour
    // per pixel. What this buys is per-dab colour -- colour dynamics (`h`/`s`/`v`/`Mix`/`Darken`)
    // and image-stamp/lightness-map tips -- which a single coverage channel cannot express.
    Colored,
};

// How the masking stroke's value modifies the paint stroke's accumulated alpha (docs/brushes.md
// §6.2): `alpha' = maskingOp(mask, alpha)`, per pixel, before the Wash opacity ceiling. These three
// are the format's default (`multiply`) plus everything the shipped preset set uses (`subtract` x3,
// `linear_dodge` x3). The format admits seven more ids; they wait for the importer's oracle pass,
// and an unknown id maps to Multiply there -- the reference's own fallback.
enum class MaskingOp : std::uint8_t {
    Multiply,
    Subtract,
    LinearDodge,
};

// Pure; exposed for direct unit testing. `mask` and `alpha` in [0,1].
[[nodiscard]] double maskingOp(MaskingOp op, double mask, double alpha) noexcept;

// The masking brush (docs/brushes.md §6.2): a SECOND dab walk along the same stroke path -- its own
// tip geometry, spacing cadence and pressure response -- accumulating a stroke-scoped grayscale
// mask beside the coverage. composite() then modifies the paint stroke's accumulated alpha through
// `op`. The mask never paints alone: pixels the paint stroke itself never touched stay pristine
// whatever the mask laid there.
//
// `diameter` is absolute document px: the format's UseMasterSize/MasterSizeCoeff coupling is
// resolved by the preset reader at load, not here.
struct MaskingParams {
    bool enabled = false;
    MaskingOp op = MaskingOp::Multiply;
    double diameter = 24.0;
    double hardness = 0.8;
    double flow = 1.0;
    double spacing = 0.10;
    // The nested preset carries its own PressureSize/PressureFlow gates and its own spacing mode,
    // independent of the primary's -- the two walks stay symmetric in mechanism.
    bool sizeFromPressure = false;
    bool flowFromPressure = false;
    bool useAutoSpacing = false;
    double autoSpacingCoeff = 1.0;
    // The masking brush's own tip -- the nested brush_definition's SHAPE, resolved at load exactly
    // like the primary's (a procedural generator, or a decoded bitmap). NULL is the analytic round
    // disc parameterized by `hardness` above, which is what the masking walk stamped before it
    // could carry a tip, and what an unresolvable masking tip file falls back to. This is not a
    // nicety: a mask AUTHORED as an eroded texture (grain and holes) that stamps as a solid disc
    // subtracts the whole nib and deletes the stroke it was meant to texture.
    std::shared_ptr<const BrushTip> tip;
    // The tip's authored geometry, exactly as BrushParams carries the primary's: `ratio` is the
    // procedural generator's (a bitmap tip's is 1 -- its frame's aspect lives inside the dab's
    // envelope), `angleRad` the nib's authored slant. The masking walk drives no options, so
    // neither varies per dab.
    double ratio = 1.0;
    double angleRad = 0.0;
};

// The smudge engine (docs/brushes.md §6.6c): the stroke reads the canvas and drags it. Transcribed
// from the reference's colorsmudge paintop in its LEGACY parameterization -- the one every shipped
// colorsmudge preset runs (grayscale alpha-mask tips, `SmudgeRateUseNewEngine` absent): the smear
// op is COPY at rate 1, the dulling rate is 1, the colour rate is the lerp below, and the final
// per-dab blt is a COPY through the dab mask at `smudgeRate x opacity` -- alpha included, so a
// smudge can LOWER alpha, which is what lets it eat paint at a stroke's trailing edge.
//
// ⚠⚠ THE SMUDGE WALK NEVER READS `m_target`. The reference reads and writes the live layer device
// per dab -- the smear CHAIN (each dab reading what the previous dabs wrote) is the mechanism --
// but reading the live target would make the mark depend on composite() cadence and break replay.
// The deterministic equivalent is a stroke-local STATE buffer: premultiplied float RGBA, seeded
// from the pristine base snapshot (the §6.6b DabSource snapshot), read and written by the dabs,
// copied into the target by composite(). Same chain, no cadence dependence.
//
// ⚠ `enabled` SUPERSEDES the accumulation axes: begin() normalizes its params copy to Wash /
// Uniform / no masking / Normal blend (the reference's colorsmudge has none of those axes; every
// shipped preset authors `normal`, and the mapper badges anything else). It also requires a REAL
// tip and Paint mode -- an Erase stroke or a tipless brush paints plainly, smudge ignored.
//
// ⚠ NO PAINT-LOAD CHANNEL, by §5's gate: this is destination-sample-and-blend ONLY. The
// reference's PaintThickness option is dropped at import, never mapped.
struct SmudgeParams {
    bool enabled = false;
    // SmudgeRateMode: false = smearing (copy the patch under the previous dab), true = dulling
    // (flood the dab with a sampled average colour). The reference's default is smearing.
    bool dulling = false;
    // `SmudgeRateSmearAlpha` (default true, and every shipped preset leaves it there): the final
    // COPY lerps alpha down as well as up. False would make the final blt a source-OVER -- kept as
    // data so the mapper can badge it, but the engine implements the true (COPY) branch only.
    bool smearAlpha = true;
    // The SmudgeRate option's STATIC strength -- read regardless of the option's checkbox, exactly
    // as the reference reads `strengthValue()` -- which bounds how much paint the colour rate may
    // add: the colour-rate ceiling is `max(1 - maxSmudgeRate, 0.2)` (smudgeColorRateOpacity).
    double maxSmudgeRate = 1.0;
};

// THE AIRBRUSH (docs/brushes.md §6.6h), transcribed from the reference's airbrush option and the
// TIMED half of its dab cadence.
//
// ⚠⚠ IT IS A SECOND CADENCE, NOT A SECOND CLOCK. The reference's walk asks two questions between
// two samples -- "has the brush travelled a spacing?" and "has a timed interval elapsed?" -- and
// lays a dab at whichever comes first. The second question is answered from the SAMPLES' OWN
// TIMESTAMPS (`StrokeInput::timeUs`, which `StrokeSnapshot::elapsedMs` carries), never from a clock
// read inside the walk: the mark stays a pure function of the sample stream, so goldens, undo
// replay and the stroke preview all reproduce exactly. That is the same contract the `time`,
// `speed` and `fade`-over-time sensors have always run under, and the airbrush adds no new class
// of nondeterminism to it.
//
// ⚠ What the reference does with a WALL clock is generate INPUT: while the pointer is held still,
// its tool synthesizes a fresh sample at the previous position with the current stroke time, and
// feeds it through the ordinary paint path. That belongs to whatever drives the engine (Mosaic's
// canvas), exactly as it belongs to the tool upstream -- the engine needs no change for it, and
// without it an airbrush preset simply lays no dabs while nothing moves.
struct AirbrushParams {
    bool enabled = false;
    // `PaintOpSettings/rate`, in dabs per second: the timed interval is `1000 / rate` ms, scaled by
    // the per-dab `Rate` option, and finally floored at the reference's own 0.5 ms. The reference's
    // own slider spans [1, 1000], so **rate 1000 means a dab every millisecond** and is a real
    // authored value -- which is why the importer reads only the keys the reference reads (the
    // Krita-2-era `AirbrushOption/*` spelling is dead there, and honouring it was a hang; §6.6h).
    double rate = 20.0;
    // `PaintOpSettings/ignoreSpacing`: switch the DISTANCE cadence off entirely, leaving the timed
    // one alone to place dabs. The reference reads it only when the airbrush is on.
    bool ignoreSpacing = false;
};

// The reference's floor on a timed interval, in milliseconds (`MIN_TIMED_INTERVAL`). A rate high
// enough to ask for less than this lays dabs every half millisecond and no faster.
inline constexpr double kMinTimedIntervalMs = 0.5;

// ⚠⚠ THE SPAN'S TIME BUDGET, in milliseconds -- the most elapsed time one span may spend on timed
// dabs (*fixed 2026-07-28, a USER-REPORTED HANG*).
//
// The timed cadence lays `elapsed / interval` dabs, and BOTH of those are things the engine does not
// control: `elapsed` is a delta of the caller's monotonic clock, and the shipped airbrush presets
// author rate 1000, i.e. an interval of ONE MILLISECOND. So a two-second stall between two samples
// asks for 2000 dabs inside a single `extendTo` -- each a full option evaluation, tip rasterization
// and blit of a large soft brush -- and a twenty-second one asks for 20,000. That is not a corner:
// a window drag, a debugger break, a swap-in or a driver stutter all produce it, and it is what a
// user experiences as "the program froze".
//
// So the budget SATURATES. Beyond it the paint a stall "earned" is deliberately dropped rather than
// dumped into one event. It is a clamp on an INPUT, not a clock read, so the mark stays a pure
// function of the sample stream and replay is still exact. 250 ms is a quarter second of the
// fastest cadence the shipped set authors -- far more than any real sample interval, far less than
// any stall a user would notice paint for.
inline constexpr double kMaxSpanBudgetMs = 250.0;

// The stroke's effective random seed, from the caller's `BrushParams::seed` and the stroke's own
// first sample (§6.6i). Pure -- no clock, no global state -- so the same sample stream always
// derives the same seed and a recorded stroke replays byte-for-byte; but two taps that differ in
// position, pressure, tilt or timestamp derive different ones, which is what makes a random hose
// cell, a `fuzzy` rotation and a scatter jitter differ between two strokes the way they do upstream.
//
// ⚠ The timestamp is what separates two taps at the SAME point: `StrokeInput::timeUs` is the
// engine's own monotonic ingest clock (docs/tablet.md §5), so it always moves between two presses.
// A caller that feeds a stream with no timestamps and no movement gets the same seed twice, and
// that is honest rather than hidden -- it is the same stroke.
//
// Free + pure -> unit-tested.
[[nodiscard]] std::uint64_t strokeSeedFor(std::uint64_t base, const StrokeInput& first) noexcept;

// One stroke's timed dab interval in milliseconds: `1000 / rate`, divided by the per-dab `Rate`
// option value, floored at kMinTimedIntervalMs. A non-positive rate scale means "never" -- the
// reference substitutes a length of time no stroke will last -- and so does a non-positive rate.
// Free + pure -> unit-tested.
[[nodiscard]] double airbrushIntervalMs(double rate, double rateScale) noexcept;

// The colour-rate opacity of one smudge dab (the legacy strategy's colorRateOpacity, transcribed):
// how strongly the paint colour is folded into the smear patch / dulling fill before the blt.
//
//     clamp01( lerp(0, max(1 - maxSmudgeRate, 0.2), colorRate * opacity) )
//
// A brush that smears at full rate keeps a 0.2 floor for its paint -- the reference's own floor,
// which is what keeps a Wet paint wet instead of paint-free. Free + pure -> unit-tested.
[[nodiscard]] double smudgeColorRateOpacity(double colorRate, double opacity,
                                            double maxSmudgeRate) noexcept;

// The reference's integer Halton generator (base 2 for x, 3 for y), transcribed: the deterministic
// low-discrepancy point stream the dulling sampler walks its rectangle with. Deliberately NOT the
// stroke's random streams: the sample pattern is a property of the rectangle, replays exactly, and
// draws nothing a rewind would have to preserve. Free + pure -> unit-tested.
class HaltonSequence {
public:
    explicit HaltonSequence(int base) noexcept : m_base(base) {}
    // Advance, then read the current point scaled onto [0, maxRange] (integer rounding, exactly
    // the reference's `(n * maxRange + d/2) / d`).
    [[nodiscard]] int generate(int maxRange) noexcept;

private:
    int m_n = 0;
    int m_d = 1;
    int m_base;
};

// An integer rectangle in document px (x, y, width, height -- closed, like the reference's QRect),
// only used by the smudge sampler below.
struct SmudgeRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

// The dulling sample rectangle (transcribed): `radius <= 0` samples the single pixel at src's
// centre; otherwise src blown outward by `0.5 * (radius - 1)` per the reference's blowRect (which
// TRUNCATES `extent * coeff` toward zero -- radius < 1 shrinks), unioned with that centre pixel so
// over-shrinking can never yield nothing. (The reference reaches the union through Qt rect
// semantics whose invalid-rect corners are not reproduced here; for a colour-averaging heuristic
// the bounding-box reading is the whole of the meaning.) Free + pure -> unit-tested.
[[nodiscard]] SmudgeRect smudgeSampleRect(const SmudgeRect& src, double radius) noexcept;

// (`TipApplication` used to be forward-declared here, to keep the tip machinery -- and its
// `brush::detail` namespace, which shadows blend_math's inside this namespace -- out of the engine
// header. The engine STAMPS through a tip now, so it needs the real thing: brush_tip.hpp above. The
// shadowing rule stands for anything written inside `namespace mosaic::core::brush`: qualify
// `core::detail::` in full, as brush_engine.cpp's blend path does.)

// §6.1's auto-choice: a preset needs `Colored` iff its tip stamps colour (any application other
// than `AlphaMask`), or a colour-dynamics option is active (`h`/`s`/`v`/`Mix`/`Darken` -- the caller
// resolves which options those are; this function only owns the rule), or its StrokePainter deposits
// a colour of its own per mark (`painterVariesColor`, §6.6g -- the sketch engine's `randomRGB`).
// Everything else runs on the `Uniform` fast path. The choice is per preset, not per stroke, and the
// editor reports it. The third input is defaulted so the two-input callers keep reading as the rule
// they were written against.
[[nodiscard]] StrokeAccumulator chooseAccumulator(TipApplication application,
                                                  bool colorDynamicsActive,
                                                  bool painterVariesColor = false);

// Static settings for one stroke (read once at begin()).
struct BrushParams {
    double diameter = 24.0; // tip diameter, document px (clamped >= ~0.1)
    // The tip's SHAPE. `ratio` is height/width, so 1.0 is the circle the engine stamped before it had
    // a shape at all, and `angleRad` is the nib's authored slant. A circular, unrotated tip walks the
    // very same arithmetic it always did -- cos(0) is exactly 1, sin(0) exactly 0, and x * 1.0 is
    // exactly x -- so it lays bit-identical dabs and every golden holds. That identity is a test.
    double ratio = 1.0;
    double angleRad = 0.0;
    // The tip's FALLOFF -- what fills the ellipse above. NULL is not "no falloff": it is the engine's
    // built-in analytic circle, parameterized by `hardness`, which every brush-family tool that is
    // not driving a preset still paints with (and which every golden in the suite was laid by). A
    // preset supplies a real tip here -- one of the six procedural generators, or a decoded bitmap --
    // and `hardness` then means nothing, because a tip carries its own edge.
    //
    // SHARED, not copied: a tip outlives its strokes, its raster is memoized by `id` in a cache that
    // outlives them too, and a bitmap tip's mip chains are far too big to duplicate per stroke.
    std::shared_ptr<const BrushTip> tip;
    double hardness = 0.8;  // 0 = soft cone from the centre .. 1 = hard edge (still AA'd). NO TIP only.
    double flow = 1.0;      // 0..1 paint deposited per dab
    double opacity = 1.0;   // 0..1 cap the whole stroke can build to
    // The dab interval, as a fraction of the dab's own extents.
    //
    // ⚠ SPACING IS AN ELLIPSE, NOT A SCALAR (docs/brushes.md §6.2). The interval is a fraction of
    // BOTH of the dab's extents -- `spacing * width` along the tip's own x, `spacing * height`
    // along its y -- and the step actually taken is that ellipse's radius in the DIRECTION OF
    // TRAVEL. A 75x15 knife at spacing 0.08 steps 6 px along its blade and 1.2 px across it; keying
    // the cadence off the width alone stamps it FIVE TIMES too sparsely across the thin axis, which
    // is exactly what a shaped tip looked like before this landed. A ROUND tip is the degenerate
    // case where the two are equal, which is why nothing noticed for as long as every tip was round.
    double spacing = 0.10;
    // The format's auto-spacing (`useAutoSpacing`/`autoSpacingCoeff`, XML defaults "0"/1.0): the
    // interval becomes an ABSOLUTE step of `coeff * sqrt(extent)` px for extents >= 1 px (sub-px
    // ones step linearly at `coeff * extent`), so a bigger brush lays relatively denser dabs. It is
    // applied PER AXIS, so it is an ellipse too. When on, `spacing` above is ignored.
    bool useAutoSpacing = false;
    double autoSpacingCoeff = 1.0;
    // `Spacing/Isotropic` (§3.2, XML default false; 2 shipped presets set it -- both bitmap chalks).
    // The author's opt-out of the ellipse: the cadence keys off the LARGER extent alone and is the
    // same in every direction, so a broad textured tip lays evenly however it is dragged.
    bool isotropicSpacing = false;
    common::Color8 color{0, 0, 0, 255}; // paint colour (the editor's active foreground)
    PaintMode paintMode = PaintMode::Wash;
    StrokeMode strokeMode = StrokeMode::Paint;
    BlendMode blendMode = BlendMode::Normal; // applied at composite, against the pristine base
    // Normally chooseAccumulator()'s verdict on the preset. Erase strokes have no colour to
    // accumulate, so they run as `Uniform` whatever this says.
    StrokeAccumulator accumulator = StrokeAccumulator::Uniform;
    // Whether an h/s/v colour-dynamics option is active (§6.6f -- chooseAccumulator's second input,
    // persisted). The engine resolves the per-dab colour through applyColorDynamics only when this
    // is set AND the accumulator is Colored, so a preset whose Colored verdict came from a
    // colour-stamping TIP rather than from colour dynamics leaves its colour to the tip/dabColor seam.
    bool colorDynamicsActive = false;
    MaskingParams masking; // the masking brush; disabled by default
    SmudgeParams smudge;   // the smudge engine; disabled by default, supersedes the axes above
    // The SECOND ENGINE KIND (docs/brushes.md §6.6b Tier 4 / §6.6g). `kind == None` -- the default,
    // and every pixel-brush, eraser and smudge preset -- is the dab walk this engine has always
    // been. Anything else replaces the dab walk with a `StrokePainter`, which draws its own geometry
    // per span into these very buffers. A SPEC and not a live painter, deliberately: BrushParams is
    // copied per stroke and shared const, a painter has per-stroke state, and a stroke that is
    // described wholly by its params is a stroke that replays.
    //
    // ⚠ SMUDGE WINS. The two are mutually exclusive in every preset format that exists (a paintop
    // is one thing), and begin() resolves the impossible combination in the smudge engine's favour
    // rather than half-running both.
    StrokePainterParams painter;
    // The hatching engine (§6.6g): a DAB engine, not a painter -- the dab walk, the spacing cadence
    // and the dab cache all run untouched, and the only thing that changes is what fills the dab.
    // `enabled` gates it; it needs a REAL tip (the lattice is stencilled by the tip's mask) and it
    // never runs beside the smudge walk or a painter.
    HatchingParams hatching;
    // The TEXTURE option (§6.6h): a document-locked pattern composited into every dab's own alpha.
    // Like hatching it needs a REAL tip and it rides the dab walk -- but unlike hatching it ALSO
    // rides the smudge walk, because the reference installs its texture option on the brush-based
    // paintop base that colorsmudge derives from, and the smudge dab's mask goes through the very
    // same post-processing step. `enabled` without a baked pattern is inert.
    TextureParams texture;
    // The AIRBRUSH (§6.6h): the stroke's second, TIME-driven dab cadence. Off, walkSpan runs the
    // distance cadence alone and is byte-for-byte the walk it always was.
    AirbrushParams airbrush;
    // The preset's dynamic options (dab.hpp): sensors -> curves -> one Dab, evaluated per dab against
    // the stroke. SHARED, not copied: a preset outlives its strokes and a stroke must not pay to
    // duplicate its option table. NULL is the whole point of the pointer -- a stroke with no options
    // resolves to the preset's static geometry through `x * 1.0`, which is exactly the dab the engine
    // laid before options existed. That is what keeps a mouse stroke and Uniform x Wash byte-identical.
    std::shared_ptr<const BrushOptions> options;
    // Fixes the stroke's random streams (`fuzzy` / `fuzzystroke`, §6.2). A PARAMETER, never a clock
    // read: a stroke must replay to the same dabs for goldens, the editor's preview and undo/redo.
    std::uint64_t seed = 0;
    // ⚠⚠ ... AND ON ITS OWN THAT MAKES EVERY STROKE OF A PRESET IDENTICAL (§6.6i, a USER-REPORTED
    // bug: "single dabs don't take the next shape"). A random hose cell, a `fuzzy` rotation, a
    // scatter jitter and a mirror flip are all drawn from the stream `seed` fixes, so a stroke laid
    // with a fixed seed draws the SAME first value every time -- and a single-dab tap is exactly one
    // first value. The reference draws a fresh random seed per stroke, so tapping a 23-cell hose
    // there walks the cells and tapping here stamped one cell forever.
    //
    // With this set, `begin()` derives the stroke's effective seed from `seed` AND the stroke's own
    // FIRST SAMPLE (`strokeSeedFor`). That keeps the whole contract above -- the mark stays a pure
    // function of (params, samples), so goldens, undo replay and the preview all still reproduce
    // exactly -- while giving two taps at different places or times different randomness, because
    // their first samples differ. Deriving it here rather than making every caller read a clock is
    // deliberate: a clock-read seed would have to be RECORDED for replay, and the sample stream
    // already carries this one.
    //
    // Default FALSE, and the two answers are not a preference: a live canvas stroke must vary
    // (`ui::BrushPresetStore`), and a repaint of the dock's preview card must NOT (a card that
    // shimmered on every expose would be a different brush every frame -- stroke_preview.cpp pins
    // both this and `seed`).
    bool seedFromFirstSample = false;

    // SELECTION CONFINEMENT (core/stroke_confinement.hpp): the active selection resampled onto THIS
    // stroke's target grid, multiplied into the stroke's alpha at composite(). NULL is "no
    // selection" -- and a null field is NOT "all 255": the engine never evaluates it, so an
    // unconfined stroke is byte-for-byte the stroke laid before this existed (a test, and every
    // brush golden in the suite rests on it).
    //
    // ⚠ NOT the masking brush. `masking` above is a second DAB WALK accumulating a stroke-scoped
    // value through a preset-chosen op; this is a STATIC document-space field. They ride side by
    // side and compose -- the masking op shapes the stroke, the selection then bounds it.
    //
    // SHARED, not copied: built once per stroke and immutable for its lifetime, so the mark stays a
    // pure function of the stroke's inputs and undo/replay reproduces it exactly.
    std::shared_ptr<const core::StrokeConfinement> confine;
};

// Pressure/tilt-ready dynamics. S19-a shipped the hooks only; both flags default OFF, which is what
// keeps a mouse stroke (pressure = 1) byte-identical however they are set. Since S19 Arc C the canvas
// The dab interval along a dab's OWN two axes, and the angle those axes sit at within the document
// (docs/brushes.md §6.2). Spacing is an ellipse: `sx` is the interval along the tip's x, `sy` along
// its y, and both are already floored at the half-pixel minimum.
struct SpacingEllipse {
    double sx = 0.5;
    double sy = 0.5;
    double rot = 0.0;
};

// The step from one dab to the next when travelling along `headingRad` (a document-frame bearing, as
// `StrokeState::drawingAngle` reports it): the ellipse's radius in the direction of travel, in
// document px.
//
// A knife 75 px along its blade and 15 px across it, at spacing 0.08, has `sx = 6`, `sy = 1.2`. Drag
// it along the blade and it steps 6 px; drag it across and it steps 1.2 px. Keying the cadence off
// one number cannot express that, and a shaped tip stamped five times too sparsely across its thin
// axis is what sent this back for a fix.
//
// ⚠ THE ROUND CASE IS A DELIBERATE BRANCH AND NOT A FAST PATH. `1/sqrt(cos^2 + sin^2)` is not
// exactly 1.0 for an arbitrary angle, so putting a round tip through the ellipse arithmetic would
// move its dabs by an ulp -- and EVERY GOLDEN IN THE SUITE WAS LAID BY A ROUND TIP. Note this is the
// exact opposite of the tip-frame map in `stamp()`, where the general arithmetic IS the bit-exact
// identity at ratio 1 and a branch would be an unkillable mutant. Here the branch is load-bearing,
// and an equality test pins it.
[[nodiscard]] double spacingStepAlong(const SpacingEllipse& e, double headingRad) noexcept;

// turns both ON for the Brush and the Eraser, so a real tablet's pressure finally reaches the dab
// walk (the Inpaint brush keeps them off -- see ui::VulkanCanvas::currentBrushDynamics). When a flag
// is on, that channel scales linearly with the sample's pressure. The §6.2 option pipeline (sensors
// -> curves -> DabPlacement) supersedes these two booleans in Arc D; the StrokeState the engine now
// keeps is the state it will evaluate against.
struct BrushDynamics {
    bool sizeFromPressure = false;
    bool flowFromPressure = false;
    // Per-dab colour -- the HOST seam (third-party colour packs, RGBA hose cells), and one reason
    // `Colored` exists. Consulted once per dab, only when the accumulator is `Colored` and the
    // stroke paints; absent, every dab uses `BrushParams::color`. ⚠ The engine's own h/s/v colour
    // dynamics (§6.6f, applyColorDynamics) OVERRIDE this: on a colour-dynamics preset the per-dab
    // colour is the HSV-adjusted paint colour, resolved from the stroke's sensors, not this callback.
    // The `dab` argument counts every dab the stroke lays, including ones clipped off the
    // document -- the sequence is a property of the stroke's geometry, so a stroke near an edge
    // keeps the same colours it would have anywhere else. The returned alpha is IGNORED: the
    // stroke's ceiling froze at begin() from `color.a`, and a per-dab alpha would have to flow
    // into the coverage buffer, which must stay the Inpaint brush's plain geometric mask.
    std::function<common::Color8(std::size_t dab, double pressure)> dabColor;
};

// One sampled point along a stroke is a `core::brush::StrokeInput` (stroke_state.hpp): position in
// document px, plus pressure/tilt/rotation/tangential and OUR timestamp. It is the core mirror of
// platform::TabletSample, so the tablet wiring (docs/tablet.md §10 step 5) converts once, applies
// TabletPolicy, and hands the WHOLE sample here -- an engine that took only (pos, pressure) would
// have thrown away tilt and the clock at the door, and §6.2's sensors need both. A mouse stroke
// passes pressure 1 and leaves the rest at rest.

// Anti-aliased dab coverage at distance `d` (px) from the dab centre, for a dab of radius `R` and
// `hardness` in [0,1]. 1 in the solid core, a smoothstep shoulder out to `R`, 0 beyond. A hard dab
// (hardness 1) keeps a ~0.75 px anti-aliased rim rather than a jagged step. Free + pure →
// unit-tested.
[[nodiscard]] double dabCoverage(double d, double R, double hardness);

// The stroke's running per-dab-opacity average (docs/brushes.md §6.2, transcribed from the
// reference's painter): a ONE-SIDED EMA that rises to a louder dab INSTANTLY and decays toward a
// quieter one at 0.1 per dab. It is what keeps a fading stroke from carving a lower ceiling into
// paint it already laid: where the accumulated alpha already stands above the current dab's own
// ceiling, the accumulation aims at THIS average instead. Starts at 0 (the first dab's own opacity
// becomes the average). Free + pure -> unit-tested.
[[nodiscard]] double blendAverageOpacity(double opacity, double average) noexcept;

// One wash-accumulation step under a DYNAMIC per-dab opacity: the new accumulated alpha for a pixel
// at `dst`, covered by this dab at `cov` (the mask value in [0,1]), with the dab's `flow`, per-dab
// `opacity` and the stroke's running `averageOpacity` (above). Transcribed from the reference's
// indirect-painting composite in its default (creamy) parameterization -- the per-dab opacity is
// the TARGET the alpha strives toward, the mask is how hard this dab pulls it there, and the flow
// interpolates the whole step from "no change":
//
//     src            = cov * opacity
//     full           = averageOpacity > opacity
//                        ? (averageOpacity > dst ? lerp(src, averageOpacity, dst / averageOpacity)
//                                                : dst)
//                        : (opacity > dst        ? lerp(dst, opacity, cov) : dst)
//     result         = flow == 1 ? full : lerp(dst, full, flow)
//
// ⚠ At opacity == 1 (and average <= 1) this is dst + flow*cov*(1 - dst) in REAL arithmetic -- the
// very accumulation the static wash path runs -- but NOT in floats: the grouping differs by an ulp.
// That is why the engine takes this path only when a preset actually drives a dynamic Opacity
// (optionIsDynamic), and why the static path's bytes cannot move. An absent option is not a
// disabled one; here that rule is also what keeps every golden alive. Free + pure -> unit-tested.
[[nodiscard]] double washAlphaDarkenAlpha(double dst, double cov, double flow, double opacity,
                                          double averageOpacity) noexcept;

// One pixel of the Sharpness threshold (§6.6e), transcribed from KisSharpnessOption::applyThreshold
// on the 8-bit mask value `v`. `threshold` is the per-dab value in [0,1]; `softness` is the option's
// integer soft band in [0,100]:
//     tolerance = (uint32)(255 - threshold*255)
//     v > tolerance                       -> 255 (opaque)
//     v <= (100 - softness)*tolerance/100 -> 0   (transparent, INTEGER division)
//     otherwise                           -> v   (kept)
// At threshold 1 the band collapses (tolerance 0): every non-zero pixel goes opaque and zero goes
// transparent -- the hard 1-bit edge the Pixel_Art presets rely on. Free + pure -> unit-tested.
[[nodiscard]] std::uint8_t sharpnessThreshold(std::uint8_t v, double threshold,
                                              int softness) noexcept;

class BrushEngine {
public:
    // Begin a stroke that paints directly onto `target` (the live `width`x`height` layer image).
    // The engine keeps a BOUNDED snapshot of target's pristine pixels over the stroke's growing
    // footprint -- it never allocates document-sized buffers, so begin() costs ~one dab and the
    // stroke costs its own area rather than the whole canvas (S60-c). `target` must outlive the
    // stroke (until end()/restore()); composite() writes the dabs into it. The first dab lands at
    // `first`.
    void begin(std::uint32_t width, std::uint32_t height, common::Image& target,
               const BrushParams& params, const BrushDynamics& dynamics, StrokeInput first);

    // Extend the stroke to `sample`, stamping dabs every (spacing*diameter) px along the path from
    // the previous sample and carrying the sub-interval remainder across calls (so dab density is
    // independent of how finely the pointer is sampled). Call ONCE PER SAMPLE -- the stroke state
    // (distance, elapsed time, the speed EMA, the drawing angle) folds in here, so feeding the same
    // sample twice would double-count its travel.
    //
    // ⚠ THE PATH IS A CURVE THROUGH THE SAMPLES, NOT A CHORD BETWEEN THEM (stroke_path.hpp). A mouse
    // delivers one position per motion event, coalesced to the frame, so chords made a 60 Hz stroke a
    // literal 60-gon. The curve is INTERPOLATING -- it passes exactly through every sample the user
    // made -- which is what keeps it clear of the gated stabilizer. Interpolate; do not filter.
    //
    // ⚠ CONSEQUENCE: THE DAB WALK LAGS THE SAMPLE STREAM BY ONE SAMPLE. Fitting a curve *through* a
    // point needs to know where the path goes next, so the span ending at the newest sample is not
    // stamped until its successor arrives. **flush() lays the tail**, and end() calls it -- a stroke
    // that is never flushed is missing its last span. (The lag is one sample: ~5 ms on a 200 Hz
    // tablet, one frame on a mouse.)
    void extendTo(StrokeInput sample);

    // Lay the tail span -- the one still waiting for a lookahead sample that will never come, because
    // the stroke is over. The last sample stands in for its own successor (the same duplication
    // begin() uses at the head of the stroke). Idempotent, and a no-op on a stroke with nothing
    // pending, so calling it twice is safe.
    //
    // ⚠ Anything that reads the stroke's PIXELS must flush FIRST and composite AFTER, or it sees a
    // stroke with its last span missing. end() flushes for exactly this reason -- so that no consumer
    // can silently truncate a stroke by forgetting to.
    void flush();

    // The stroke's derived state (docs/brushes.md §6.2): distance, elapsed time, the smoothed speed,
    // the drawing angle, the dab counter and the two random streams. begin()/extendTo() drive it from
    // the sample stream; the §6.2 option pipeline evaluates its sensors against it in Arc D. Exposed
    // for the tests and the Settings->Tablet test area, which reads the live speed.
    [[nodiscard]] const StrokeState& strokeState() const noexcept { return m_stroke; }

    // Calibrate the `speed` sensor's EMA (docs/tablet.md §7 makes both a user setting -- Settings ->
    // Tablet -> Speed smoothing). Set before begin(); begin() resets the EMA's value, never its
    // params, so a stroke always starts from a still pen at the calibration the user chose.
    void setSpeedParams(const SpeedParams& p) noexcept { m_stroke.setSpeedParams(p); }

    // Composite the dabs stamped SINCE THE LAST composite() into the target, then clear that
    // pending region. Each composite() refreshes only the freshly-painted pixels, so a live stroke
    // costs work proportional to the new dabs rather than the whole stroke each frame. Returns the
    // rect it rewrote (integer px; empty when nothing new was stamped). A pixel whose coverage grew
    // this batch is always inside the pending region, so the incremental refresh stays exact.
    common::Rect composite();

    // Revert the target to its pre-stroke pixels (drops the whole in-flight stroke). For cancel and
    // for the inpaint brush dropping its red overlay; the paint tool reads the painted region out
    // first. Cheap -- only the stamped pixels are restored from the bounded base snapshot.
    void restore();

    // Ends the stroke. Flushes the tail span first (see flush()): the walk lags the sample stream by
    // one sample, and a stroke whose last span was never laid is a stroke with a missing end.
    void end();
    [[nodiscard]] bool active() const noexcept { return m_active; }

    // Integer-pixel bounds touched by the stroke so far (empty if none) -- the region the canvas
    // recomposites and the final command needs to cover.
    [[nodiscard]] common::Rect dirtyBounds() const;

    // The accumulated per-pixel stroke coverage [0,1], over the BOUNDED working rectangle
    // (coverageOriginX/Y + coverageWidth/Height), row-major. The Inpaint brush (S39) reads this as
    // the hole mask -- the stroke paints a red overlay AND records the region in one pass. The
    // buffer is the stroke's footprint, not the document, so map index i -> layer pixel
    // (originX + i%coverageWidth, originY + i/coverageWidth). Empty until the first dab.
    [[nodiscard]] const std::vector<float>& coverage() const noexcept { return m_coverage; }
    [[nodiscard]] std::uint32_t coverageWidth() const noexcept { return m_cw; }
    [[nodiscard]] std::uint32_t coverageHeight() const noexcept { return m_ch; }
    [[nodiscard]] std::int32_t coverageOriginX() const noexcept { return m_ox; }
    [[nodiscard]] std::int32_t coverageOriginY() const noexcept { return m_oy; }
    [[nodiscard]] std::uint32_t width() const noexcept { return m_w; } // layer dims
    [[nodiscard]] std::uint32_t height() const noexcept { return m_h; }

    // How far the flattened polyline may sit from the true curve. Far below a pixel: the dab spacing
    // can be a fraction of a pixel on a small tip, and a flattening error must never be visible in
    // the placement. It is also the threshold at which a curve counts as "straight" and the walk
    // falls back to the single chord -- so it must be tight enough that no visible bend slips through
    // as a straight line.
    static constexpr double kFlattenTolerancePx = 0.05;

private:
    // Lay the dabs of ONE span -- the curve from s1 to s2, with s0 and s3 supplying its tangents.
    // Flattens the curve, then walks the resulting polyline on the primary cadence and (if masking)
    // on the masking one.
    //
    // ⚠ When the curve lies within the flattening tolerance of the straight chord, the flattener
    // emits NO interior points and the polyline is exactly [s1, s2] -- one edge. walkSpan then runs
    // the identical arithmetic the old straight-chord walk ran, so a straight stroke lays BIT-
    // IDENTICAL dabs. That is not an accident and it is not decoration: it is what keeps `Uniform x
    // Wash` and the mouse goldens honest across this change.
    void stampSpan(const StrokeSnapshot& s0, const StrokeSnapshot& s1, const StrokeSnapshot& s2,
                   const StrokeSnapshot& s3);

    // The painter's analog of walkSpan (docs/brushes.md §6.6g): hand the flattened polyline to the
    // stroke painter ONE EDGE AT A TIME, with the derived state interpolated to each edge's ends by
    // arc length exactly as the dab walk interpolates it. That is not a convenience -- the reference
    // subdivides a curved segment into flat pieces and calls its `paintLine()` on each, so an edge
    // IS the unit its painters are written against. On a straight span the flattener emits nothing
    // and the painter gets the span's own two endpoints, once.
    void paintSpanEdges(const StrokeSnapshot& a, const StrokeSnapshot& b);

    // Advance the once-per-mark stroke state before a painter's span: the dab counter (`fade` reads
    // it), the dab index, and the dynamic-opacity pair. Exactly what resolveDab advances for a dab,
    // and for the same reason -- the reference evaluates its opacity option once per segment.
    void beginPainterSegment();

    // The write seam a StrokePainter paints through: one integer pixel straight into the engine's
    // own accumulation, so a painter's mark rides Wash/Buildup/Erase, the blend path, the masking
    // brush, the coverage the Inpaint brush reads, restore() and undo replay exactly as a dab does.
    // Constructed on the stack per span; it holds nothing.
    class PainterCanvas final : public StrokeCanvas {
    public:
        explicit PainterCanvas(BrushEngine& engine) noexcept : m_engine(engine) {}
        [[nodiscard]] int width() const noexcept override;
        [[nodiscard]] int height() const noexcept override;
        void plot(int x, int y, double alpha, common::Color8 color) override;

    private:
        BrushEngine& m_engine;
    };

    // Walk one polyline, laying dabs at the spacing cadence and carrying the remainder. `mask`
    // selects which of the two independent cadences (and which stamp) this walk drives. The dab's
    // whole STATE -- pressure, tilt, distance, speed, heading -- interpolates from `a` to `b` by
    // ARC-LENGTH fraction along the polyline, which on a single-edge polyline is the chord fraction
    // the old walk used, to the bit.
    void walkSpan(bool mask, const StrokeSnapshot& a, const StrokeSnapshot& b);

    // The airbrush's stationary half (§6.6h): lay the TIMED dabs of a span whose two ends are the
    // same point. The distance cadence has nothing to measure there and the walk has always
    // stopped -- but the clock has not, and dabs continuing to lay where the pointer rests is what
    // an airbrush is. Called only from walkSpan's zero-travel branch, and only when the airbrush is
    // live, so with it off the walk is exactly the walk it always was.
    void pumpStationarySpan(const StrokeSnapshot& a, const StrokeSnapshot& b);

    // Resolve one dab: put the stroke state at the dab's own point, advance the dab counter, and run
    // the option pipeline (dab.hpp). Call EXACTLY ONCE per dab and in order -- evaluating an option
    // draws from the stroke's random stream, so a second call is a different dab.
    [[nodiscard]] Dab resolveDab(common::Vec2 center, double pressure);

    // The per-dab constants every pixel of one dab shares: its flow, its per-dab opacity pair (when
    // a dynamic Opacity option is driving), and -- on the Colored accumulator -- the colour it
    // deposits, resolved ONCE per dab rather than per pixel.
    struct DabDeposit {
        double flow = 1.0;
        // The dynamic-opacity path (washAlphaDarkenAlpha). `dynOpacity` false is the static path,
        // whose accumulation expression must stay bit-for-bit what it always was.
        bool dynOpacity = false;
        // BUILDUP's own half of the same option (§6.6i): direct painting composites each dab at its
        // OWN opacity instead of striving toward a ceiling, so the Buildup accumulation's per-dab
        // share becomes `opacity * cap` instead of `cap`. Exclusive with `dynOpacity` by
        // construction -- one is the Wash gate and the other the Buildup gate on one option.
        bool buildOpacity = false;
        double opacity = 1.0;    // this dab's own ceiling: sensors only, never the static strength
        double avgOpacity = 0.0; // the stroke's running average, as of this dab
        bool color = false;
        double r = 0.0;
        double g = 0.0;
        double b = 0.0;
    };
    [[nodiscard]] DabDeposit beginDeposit(const Dab& dab, std::size_t dabNo, double pressure) const;

    void stamp(const Dab& dab, double pressure);
    // One dab of a REAL tip (BrushParams::tip): rasterize through the dab cache and blit. The
    // analytic path and this one differ in where a pixel's coverage comes from and in NOTHING else,
    // which is why both end in `deposit()` instead of each carrying its own copy of the accumulation
    // -- three accumulators, a first-touch base snapshot and two bboxes are not a thing to keep two
    // copies of.
    void stampTipDab(const Dab& dab, double R, std::size_t dabNo, double pressure);
    // BUILDUP's per-dab share of the stroke's ceiling (§6.6i): `m_cap`, or this dab's own opacity
    // times it when the Buildup gate is open. Read by the Buildup accumulator AND by the Colored
    // buffer's deposit weight, which must agree (§6.1).
    [[nodiscard]] double buildCap(const DabDeposit& dep) const noexcept;
    // Accumulate one pixel's coverage into every live buffer. Assumes ensureCovers() has already
    // grown the working rect over (x, y).
    void deposit(int x, int y, double cov, const DabDeposit& dep);
    // One SMUDGE dab (docs/brushes.md §6.6c): read the stroke-state buffer, blend, lerp the state
    // toward the blend through the dab's mask. Mirrors stampTipDab's shape/quantize/cache steps --
    // with the sub-pixel phase DISABLED in smearing mode, as the reference does (its bug 327235:
    // the smear must copy ALIGNED areas, or the patch resamples and the chain blurs) -- and then
    // takes the smudge deposit instead of deposit(). The FIRST dab of a stroke paints nothing: it
    // records the anchor the second dab's source patch is read at, exactly the reference's
    // m_firstRun. The anchor advances for clipped dabs too (a property of the stroke's geometry).
    void stampSmudgeDab(const Dab& dab, double R, double pressure);
    // The §6.6b DabSource snapshot, bulk form: fill m_base from the still-pristine m_target over
    // the given working-rect-clamped box, wherever it has not been filled yet (m_baseFilled), and
    // seed the smudge state buffer from those pixels. composite() never writes a pixel whose
    // coverage is 0, so an unfilled pixel's target bytes ARE pristine; a filled pixel's may not
    // be, which is why the flag -- not the coverage -- is the gate here.
    void seedSmudge(int bx0, int by0, int bx1, int by1);
    // One masking dab into m_mask: the masking walk's analog of stamp(). No base snapshot, no
    // coverage -- but it does mark the pending region, because a masking dab landing on an
    // already-composited pixel changes that pixel's alpha and it must be recomposited. A real
    // masking tip routes to stampMaskTipDab; a null one lays the analytic disc, bit for bit as it
    // always did.
    void stampMask(common::Vec2 center, double pressure);
    // One masking dab of a REAL masking tip: rasterize through the same dab cache the primary uses
    // (the masking tip has its own raster id, so the two can never trade masks) and blit into
    // m_mask. The masking walk's analog of stampTipDab, ending in depositMask exactly as the
    // analytic path does. Frame 0, no mirror, softness as authored: the masking walk drives no
    // options.
    void stampMaskTipDab(common::Vec2 center, double diameter, double flow);
    // Accumulate one masking value at `a` into m_mask (wash-style) and mark the pixel pending.
    // Assumes ensureCovers() has already grown the working rect over (x, y).
    void depositMask(int x, int y, double a);
    // Grow the bounded working rect -- EVERY live per-pixel buffer: coverage, base snapshot, the
    // Buildup/Colored accumulations, and the masking value -- to cover the integer box
    // [bx0,bx1) x [by0,by1) (already clamped to the layer), tile-aligned so growth is chunky.
    void ensureCovers(int bx0, int by0, int bx1, int by1);
    // The ellipse a dab sets the cadence with. It reads the dab's RESOLVED (option- and
    // pressure-scaled) geometry and is re-resolved after every dab, not per segment -- so it reads
    // what the dab was actually laid at rather than re-deriving it, which would evaluate the options
    // a second time and draw a second `fuzzy` for the same dab.
    //
    // ⚠ It asks the TIP for the dab's extents, exactly as the stamp does. A bitmap tip's `ratio` is
    // 1 and its frame's aspect lives inside the envelope, so a dab's two extents are NOT
    // `(diameter, diameter * ratio)` in general and the tip is the only thing that knows them.
    [[nodiscard]] SpacingEllipse dabSpacingEllipse(const Dab& dab) const;
    // The masking tip's pressure-scaled diameter (its own PressureSize gate), shared by stampMask's
    // radius and the masking walk's cadence so the two can never disagree.
    [[nodiscard]] double maskingDiameter(double pressure) const;
    // The masking dab's spacing ellipse: the masking walk's analog of dabSpacingEllipse, asking the
    // masking TIP for the dab's two extents. With no tip both extents are the one diameter, the
    // ellipse is round, and spacingStepAlong takes its scalar branch -- bit for bit the step the
    // analytic masking walk always took. With a tip the extents are the tip's own (the shipped
    // eroded-debris masking tip is a 153x64 frame -- NOT square), so the cadence tightens across
    // the thin axis exactly as the primary's does.
    [[nodiscard]] SpacingEllipse maskingSpacingEllipse(double pressure) const;
    // The highest alpha this stroke may reach: `opacity`, times the paint colour's own alpha when
    // painting. An Erase stroke has no colour, so its ceiling is the opacity alone.
    [[nodiscard]] double strokeAlphaCap() const;
    [[nodiscard]] bool buildup() const noexcept { return m_params.paintMode == PaintMode::Buildup; }
    // The colour axis is live: an Erase stroke never accumulates colour (destination-out reads
    // none), so it stays on the Uniform path however the params are set.
    [[nodiscard]] bool colored() const noexcept {
        return m_params.accumulator == StrokeAccumulator::Colored &&
               m_params.strokeMode == StrokeMode::Paint;
    }
    [[nodiscard]] bool maskingActive() const noexcept { return m_params.masking.enabled; }
    // Frozen at begin() (m_painterActive): the params carry a painter kind, the painter built, and
    // the smudge walk is NOT running. When true the dab walk does not run at all -- no dabs, no
    // spacing cadence, no dab cache, no hose -- and every mark comes from `paintSpanEdges`.
    [[nodiscard]] bool painterActive() const noexcept { return m_painterActive; }
    // Frozen at begin() (m_smudgeActive): SmudgeParams::enabled, a REAL tip, Paint mode. When
    // true, the whole accumulation is the smudge state buffer's -- begin() normalized the params
    // copy so buildup()/colored()/maskingActive() above all read false.
    [[nodiscard]] bool smudgeActive() const noexcept { return m_smudgeActive; }

    std::uint32_t m_w = 0; // layer dimensions
    std::uint32_t m_h = 0;
    common::Image* m_target = nullptr; // the live layer image the stroke paints onto
    BrushParams m_params;
    BrushDynamics m_dyn;

    // The bounded working rectangle: coverage + a pristine base snapshot, both sized m_cw x m_ch
    // and placed at layer-local origin (m_ox, m_oy), tile-aligned and grown lazily as the stroke
    // moves.
    std::int32_t m_ox = 0;
    std::int32_t m_oy = 0;
    std::uint32_t m_cw = 0;
    std::uint32_t m_ch = 0;
    std::vector<float> m_coverage; // accumulated stroke alpha [0,1] over the working rect
    // Buildup only (empty in Wash): the per-dab-capped accumulation composite() reads instead of
    // `m_coverage * cap`. Kept beside the coverage rather than replacing it, because coverage stays
    // the Inpaint brush's hole mask in every mode.
    std::vector<float> m_build;
    // Colored only (empty in Uniform): premultiplied RGBA, 4 floats per pixel, the dabs' colours
    // stacked source-over. Its deposits use the same per-dab alpha as the mode's own accumulator
    // (`a` beside the Wash coverage, `a*cap` beside the Buildup one), so normalizing by its own
    // alpha channel recovers exactly the colour that mode's compositing implies.
    std::vector<float> m_colored;
    // Masking only (empty otherwise): the masking stroke's accumulated grayscale value [0,1].
    // Allocated by ensureCovers alongside the others, so wherever a pending pixel exists, the mask
    // buffer exists too -- composite() relies on that.
    std::vector<float> m_mask;
    common::Image m_base; // pristine pre-stroke pixels over the working rect (lazy)
    // Smudge only (both empty otherwise). The STATE buffer: premultiplied float RGBA over the
    // working rect -- what the layer under this stroke LOOKS LIKE right now, seeded from m_base
    // (seedSmudge) and evolved by the dabs. composite() copies it to the target wherever coverage
    // is > 0; restore() still restores from m_base. Premultiplied because the reference's COPY
    // composite (channels lerped premultiplied, then unpremultiplied by the lerped alpha) is a
    // plain componentwise lerp in this form, and the dulling average is a plain componentwise mean.
    std::vector<float> m_smudgeState;
    // Which working-rect cells m_base (and, under smudge, m_smudgeState) hold real pixels. The
    // per-pixel first-touch snapshot in deposit() keys off coverage == 0; the smudge walk reads
    // pixels it never deposits into, so it needs an explicit flag. Allocated only under smudge.
    std::vector<std::uint8_t> m_baseFilled;
    std::vector<float> m_smudgeScratch; // one dab's source patch (the smear read; overlap-safe)
    double m_cap = 1.0;   // strokeAlphaCap(), frozen at begin() with the rest of the params
    // BrushParams::confine, cached at begin() (the shared_ptr in m_params owns it for the stroke).
    // Null is the point: every confinement site is guarded on it, so an unconfined stroke pays
    // nothing at all.
    const core::StrokeConfinement* m_confine = nullptr;

    // The spline's window: the samples still needed as control points, oldest first. At most four --
    // a span needs the sample before it and the sample after it to know its tangents. begin() seeds
    // it with the first sample TWICE (a stroke has no sample before its first), and flush() ends it
    // the same way.
    //
    // Each entry carries the sample AND the derived state as it stood when that sample arrived. That
    // is what closes the lookahead gap: the walk is one sample behind the stream, so a span can only
    // be evaluated against the state it is stamped with if that state was kept.
    std::vector<StrokeSnapshot> m_path;
    // Scratch for the walk, kept as members purely so a 200 Hz sample stream does not allocate.
    std::vector<common::Vec2> m_flat; // the flattener's interior points
    std::vector<common::Vec2> m_poly; // p1 -> interior... -> p2, the polyline the walk actually walks
    std::vector<double> m_edge;       // its per-edge lengths

    // The derived stroke state (§6.2). It is fed once per SAMPLE by begin()/extendTo() -- never per
    // dab, which is a different cadence -- so between spans it holds the LIVE state, describing the
    // pointer rather than the dab walk. strokeState() hands that out, and the Settings->Tablet test
    // area reads the live speed from it.
    //
    // ⚠ THE WALK LAGS THE SAMPLE STREAM BY ONE SAMPLE (extendTo), so when the span [p1,p2] is finally
    // stamped, this has ALREADY advanced past p2 to the lookahead sample p3. Evaluating a dab against
    // it as it stands would read every speed/fade/heading sensor one sample INTO THE FUTURE. So the
    // walk brackets each span: it rewinds the state to the dab's own point on the stroke (m_path
    // keeps a StrokeSnapshot beside every sample; lerpSnapshot lands between two of them), stamps,
    // and puts the live state back. The LAG ITSELF STAYS -- it is what makes the curve interpolate.
    //
    // A rewind moves only the derived state. The random streams, the dab counter and the latched
    // drawing angle keep running across it (stroke_state.hpp), or `fuzzy` would repeat itself once
    // per span.
    StrokeState m_stroke;
    double m_carry = 0.0;      // distance travelled since the last dab (the spacing remainder)
    double m_maskCarry = 0.0;  // the masking walk's own remainder -- the cadences are independent
    // The size each walk's most recent dab was laid at: the step to the NEXT dab is resolved from
    // the LAST dab's own size (a segment's samples only bound the interpolation). The primary walk
    // stores the diameter the option pipeline RESOLVED, rather than the pressure it would have to
    // re-resolve it from -- re-running the options for the cadence would draw a second `fuzzy` for a
    // dab that has already been laid. The masking walk has no options, so its pressure still suffices.
    // Invariant: each carry stays below its walk's current spacing, so the next dab is never overdue.
    //
    // The primary walk stores the whole ELLIPSE and not just a diameter, because the step it yields
    // depends on the direction of travel as well as on the size (BrushParams::spacing).
    SpacingEllipse m_lastSpacing;
    double m_lastMaskPressure = 1.0;
    std::size_t m_dabIndex = 0; // dabs laid so far (feeds BrushDynamics::dabColor)

    // The dynamic-opacity state (§6.2). `m_dynOpacity` is frozen at begin(): Wash mode AND a preset
    // Opacity option that is genuinely dynamic (optionIsDynamic -- the same predicate the importer's
    // honesty contract reads, so "honoured" and "driven" cannot drift apart). The per-dab value and
    // the running average advance in resolveDab -- once per dab, in order, clipped and zero-flow
    // dabs included, exactly like the dab counter: they are properties of the stroke's geometry.
    // Like the two random streams, they belong to the STROKE and run across a rewind.
    bool m_dynOpacity = false;
    // BUILDUP's own gate on the SAME option (§6.6i), frozen beside it and mutually exclusive with
    // it: Buildup is the reference's DIRECT painting, where the opacity option is applied to each
    // dab's own composite rather than to a stroke temp. `m_dabOpacity` carries the value for both
    // (one evaluation, in one place -- the reference reads the option identically in both modes and
    // differs only in whether the strength rides along, which is exactly Mosaic's ceiling/sensor
    // split from the other end).
    bool m_buildOpacity = false;
    double m_dabOpacity = 1.0; // this dab's ceiling: sensors WITHOUT the static strength
    double m_avgOpacity = 0.0; // blendAverageOpacity's running value (0 = no dab yet)

    // The smudge state (§6.6c). The gate freezes at begin(); the per-dab values advance in
    // resolveDab -- once per dab, in order, clipped dabs included, after the wash-opacity pair
    // above and in the fixed order rate -> colour rate -> radius -> opacity, because any of the
    // four can draw from the stroke's random streams and the draw order is part of the replay
    // contract. The unchecked fallbacks are the reference's own: rate 1 (smear fully), colour
    // rate 0 (deposit nothing), radius 0 (sample one pixel).
    bool m_smudgeActive = false;
    double m_dabSmudgeRate = 1.0;
    double m_dabColorRate = 0.0;
    double m_dabSmudgeRadius = 0.0;
    double m_dabSmudgeOpacity = 1.0; // WITH strength: colorsmudge is direct painting (dab.hpp)
    // The Spacing option's per-dab cadence scale (§6.6e), resolved once per dab in resolveDab (LAST
    // among the draws) and multiplied into the spacing ellipse by dabSpacingEllipse. 1.0 is the
    // no-option identity, and `interval * 1.0 == interval` keeps every spacing golden byte-identical.
    // Rides both walks; the masking walk carries no options and passes 1.0 explicitly.
    double m_dabSpacingScale = 1.0;
    // Sharpness (§6.6e), frozen at begin(): `m_sharpnessActive` gates the alpha threshold (the option
    // is checked -- and NEVER under smudge, whose colorsmudge installs no sharpness), `m_sharpnessSnap`
    // the pixel-grid coordinate snap (also alignOutline + static strength > 0). `m_sharpnessSoftness`
    // is the threshold's soft band. The per-dab value drives BOTH the threshold and the snap and is
    // drawn once in resolveDab; it belongs to the stroke and runs across a rewind like the others.
    bool m_sharpnessActive = false;
    bool m_sharpnessSnap = false;
    int m_sharpnessSoftness = 0;
    double m_dabSharpness = 1.0;
    // Colour dynamics (§6.6f), frozen at begin(): `m_colorDynamicsActive` is set only on the Colored
    // accumulator (never under smudge, whose normalization forced Uniform) and only when an h/s/v
    // option is actually checked. When set, resolveDab resolves the HSV-adjusted paint colour into
    // `m_dabDynColor` (once per dab, LAST among the draws -- appended after the spacing scale so every
    // prior golden's stream is byte-identical), and beginDeposit deposits it in place of the flat
    // colour. It belongs to the stroke and runs across a rewind like the other per-dab draws.
    bool m_colorDynamicsActive = false;
    common::Color8 m_dabDynColor{}; // this dab's HSV-adjusted paint colour (colour dynamics only)
    // The hatching engine (§6.6g), frozen at begin(): HatchingParams::enabled, a REAL tip, and
    // neither the smudge walk nor a painter running. The three CHECKED gates are frozen with it
    // because the reference's own pass selection reads them; the four per-dab values advance in
    // resolveDab, LAST among the draws (appended after the colour dynamics, so every prior golden's
    // random stream is byte-identical), in the reference's assignment order angle -> crosshatching
    // -> separation -> thickness. `m_hatchStencil` is one dab's lattice, reused across dabs.
    bool m_hatchActive = false;
    HatchingDabValues m_dabHatch;
    std::vector<std::uint8_t> m_hatchStencil;
    // The texture option (§6.6h), frozen at begin(): TextureParams::enabled, a baked pattern, and a
    // REAL tip (the pattern composites into the tip's mask, and a tipless brush has none). The two
    // effective offsets are drawn ONCE PER STROKE here -- from the stroke's keyed per-stroke random
    // constant when the preset asks for a random offset -- exactly as the reference reads its
    // per-stroke random source, so they cost no per-dab draw and cannot disturb any pinned order.
    // The per-dab strength advances in resolveDab.
    bool m_textureActive = false;
    int m_textureOffX = 0;
    int m_textureOffY = 0;
    double m_dabTextureStrength = 1.0;
    // The airbrush (§6.6h), frozen at begin(): AirbrushParams::enabled, and never beside a painter
    // (a painter's mark is a segment, not a dab on a cadence). `m_timeCarry` is the walk's second
    // remainder -- milliseconds accumulated since the last dab, carried across spans exactly as
    // `m_carry` carries the distance remainder. `m_dabRateScale` is the per-dab `Rate` value,
    // resolved in resolveDab beside the spacing scale and for the same reason: the reference
    // re-computes its timing AFTER laying a dab, to size the interval to the NEXT one.
    bool m_airbrushActive = false;
    double m_dabRateScale = 1.0;
    double m_timeCarry = 0.0;
    // The second engine kind (§6.6g), frozen at begin(). `m_painter` owns the stroke's painter for
    // exactly the stroke's lifetime; it is rebuilt from the params on every begin(), so two strokes
    // of the same preset never share a point history, a bristle field or a random cursor.
    bool m_painterActive = false;
    std::unique_ptr<StrokePainter> m_painter;
    // The previous dab's rendered-rect centre -- where this dab's source patch is read (the smear
    // offset is the integer-rounded difference, exactly the reference's srcDabRect translation).
    // Valid from the first dab on; that first dab paints nothing and only plants this.
    common::Vec2 m_smudgeAnchor{};
    bool m_haveSmudgeAnchor = false;

    // The tip path's two pieces of state (null tip: both inert).
    //
    // The cache OUTLIVES the stroke, and that is the whole point: a stroke at constant size and angle
    // has only `subPixelSteps^2` distinct masks however many dabs it lays, and the next stroke with
    // the same brush reuses every one of them. It is keyed on the tip's raster `id`, so two tips can
    // never collide -- and it is exactly transparent (dab_cache.hpp), so evicting from it changes
    // performance and nothing else.
    DabMaskCache m_dabCache;
    // Which cell of an animated tip the next dab stamps. Restarts at 0 per stroke, because the dab
    // counter does -- an `Incremental` hose that carried across strokes would start each stroke on a
    // different cell.
    HoseState m_hose;

    // Two integer-px bboxes, half-open [min, max): the TOTAL stroke extent (for dirtyBounds()) and
    // the PENDING region stamped since the last composite() (cleared by composite()).
    struct Box {
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        bool valid = false;
        void add(int x, int y);
        [[nodiscard]] common::Rect rect() const;
    };
    Box m_total;
    Box m_pending;
    bool m_active = false;
};

} // namespace mosaic::core::brush
