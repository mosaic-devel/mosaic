#pragma once

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "core/brush/curve.hpp"
#include "core/brush/dab.hpp"
#include "core/brush/stroke_state.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

// THE SECOND ENGINE KIND (docs/brushes.md §6.6b Tier 4, §6.6g).
//
// `BrushEngine` walks a path stamping a tip mask at a spacing cadence. Six shipped presets are not
// dab engines at all: their mark is a function of the stroke's HISTORY or of a persistent STATE,
// and upstream they express that by overriding `paintLine()` instead of `paintAt()`. A sketch brush
// draws lines back to earlier points of its own stroke; a bristle brush turns every opaque pixel of
// its tip into a bristle and drags each one along the segment. Neither is expressible as "stamp this
// mask here", however the mask is chosen.
//
// So: a `StrokePainter` is handed one SPAN -- the two ends of one flattened edge of the stroke's
// path, with the derived state at each end -- and draws whatever it likes, one pixel at a time,
// through a `StrokeCanvas`.
//
// ⚠ IT WRITES INTO THE ENGINE'S EXISTING ACCUMULATION, NOT A PARALLEL ONE. `StrokeCanvas::plot`
// lands in the very buffers a dab's `deposit()` lands in, so Wash/Buildup, Erase, the blend path,
// the masking brush, the coverage the Inpaint brush reads, `restore()`, `composite()`'s incremental
// pending region and undo replay all keep working with no second copy of any of them. A painter
// that needed its own buffer would need its own everything.
//
// ⚠ ONE CALL PER FLATTENED EDGE, NOT PER SAMPLE -- and that is the transcription, not a shortcut.
// The reference subdivides a curved segment into flat pieces and calls `paintLine()` on each with
// interpolated paint information, so a painter that receives the engine's own flattened polyline
// receives exactly what the reference's painter receives. On a straight span the flattener emits no
// interior point at all and the painter gets the span's own two endpoints, once.
//
// ⚠ DETERMINISM IS THE SAME CONTRACT A DAB HAS. A painter draws its randomness from the stroke's
// seeded streams (`StrokeState::nextRandom`), never from a clock, so a stroke replays to the same
// pixels for goldens, undo/redo and the editor's preview. Anything a painter needs that is a
// property of the BRUSH rather than of the stroke (the bristle layout) is drawn from its own fixed
// stream, exactly as the reference's is.
//
// FLTK-, Vulkan- and platform-free, like the rest of the engine core.
namespace mosaic::core::brush {

struct BrushTip;

// ------------------------------------------------------------------------------------------------
// The write seam.

// One integer pixel of paint. The engine's implementation clips to the document, grows the bounded
// working rect and runs the same accumulation a dab's coverage runs -- so a painter never learns
// where the working rect is, what the paint mode is, or whether the stroke erases.
class StrokeCanvas {
public:
    StrokeCanvas() = default;
    StrokeCanvas(const StrokeCanvas&) = delete;
    StrokeCanvas& operator=(const StrokeCanvas&) = delete;
    virtual ~StrokeCanvas() = default;

    [[nodiscard]] virtual int width() const noexcept = 0;
    [[nodiscard]] virtual int height() const noexcept = 0;

    // Lay `alpha` (clamped to [0,1]) of `color` at (x, y). Out-of-document pixels are dropped, and
    // an alpha of 0 lays nothing at all. `color` reaches the accumulation only on the `Colored`
    // axis; on `Uniform` the stroke's one colour is applied at composite and this is ignored.
    virtual void plot(int x, int y, double alpha, common::Color8 color) = 0;
};

// ------------------------------------------------------------------------------------------------
// Shared rasterizers. Free + pure -> unit-tested on their own, which is the point of the split: a
// painter's mark is its geometry decisions PLUS one of these, and only the first half is novel.

// A rasterized pixel: an integer position and the alpha WEIGHT the rasterizer gives it. The weight
// is 1 everywhere except on an antialiased thick line's outer rim.
struct LinePixel {
    int x = 0;
    int y = 0;
    double weight = 1.0;
};

// The half-open integer box a rasterizer may emit into. Rasterizers still WALK their whole line
// (the walk is what puts the next pixel in the right place); they just do not emit outside this.
struct LineClip {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
};

// The backstop on a single rasterized line. Nothing in a preset file bounds a stroke's coordinates,
// and both walks below are `while (x != x2)` loops in the reference -- so a hostile or a NaN-poisoned
// endpoint must cost a bounded amount of work, not a hung stroke. Well past any real line.
inline constexpr std::size_t kMaxLinePixels = 1u << 20;

// The reference's integer DDA line (its painter's `drawDDALine`), transcribed: floor both endpoints,
// step one pixel along the major axis and round the minor one. Every pixel weighs 1 -- this is the
// hard-edged 1 px line, and the sketch engine's default connection.
//
// ⚠ Its `lockAxis` flag is not a tidy-up: a purely horizontal line leaves the gradient at 0 AND the
// flag set, and a purely vertical one sets the gradient to 2 so the walk takes the y branch with a
// zeroed step. Collapse the two and a vertical line skews.
void rasterizeDdaLine(common::Vec2 start, common::Vec2 end, const LineClip& clip,
                      std::vector<LinePixel>& out);

// The reference's thick line (its painter's `drawLine(start, end, width, antialias)`), transcribed:
// a distance field over the line's bounding box rather than a walk. A pixel is in when its distance
// to the SEGMENT (perpendicular inside the projection range, to the nearer endpoint outside it) is
// within `halfWidth = width*0.5 + subPixel`, where `subPixel` is the fractional part of the start
// point along the line's major axis. With `antialias` the outer 1 px of that band fades linearly.
//
// ⚠ A line whose two endpoints FLOOR to the same pixel draws nothing -- the reference returns before
// its loop, and a zero-length connection is a real case (a sketch brush connects a point to itself).
void rasterizeThickLine(common::Vec2 start, common::Vec2 end, double width, bool antialias,
                        const LineClip& clip, std::vector<LinePixel>& out);

// The reference's hairy `Trajectory::getLinearTrajectory`, transcribed: the list of points one
// bristle deposits ink at, from `start` to `end`. It is NOT the DDA line above -- it emits the exact
// `start`, then one point per integer step of the major axis carrying the FRACTIONAL minor
// coordinate, then the exact `end`. A degenerate line yields exactly two points (both the endpoint),
// which is what makes a bristle that did not move lay a single splat.
void linearTrajectory(common::Vec2 start, common::Vec2 end, std::vector<common::Vec2>& out);

// A quadratic / cubic Bezier as a polyline, appended to `out` WITHOUT its first point (so a path
// can be built by chaining segments). The step count is estimated from the control polygon's length
// and capped; the reference reaches these curves through Qt's path rasterizer, which is not
// reproduced -- the flattening tolerance is Mosaic's and it is recorded as such in §6.6g.
void flattenQuadratic(common::Vec2 p0, common::Vec2 c, common::Vec2 p1,
                      std::vector<common::Vec2>& out);
void flattenCubic(common::Vec2 p0, common::Vec2 c1, common::Vec2 c2, common::Vec2 p1,
                  std::vector<common::Vec2>& out);

// ------------------------------------------------------------------------------------------------
// The per-segment scratch.
//
// ⚠ IT IS THE TRANSCRIPTION, NOT AN OPTIMIZATION. Three of the five painters composite a segment's
// marks into a temporary device and blit the RESULT over the layer -- so within one segment the
// marks combine by that device's own law (ADD-saturating for the bristle and particle engines,
// which is measurably heavier than "over" wherever two marks touch; a MASK for a stroked path,
// which must not double-darken at its own joins) and only the segment total goes over what came
// before. The temporary is the PAINTER's, exactly as it is the paintop's upstream; the ENGINE's
// accumulation is still the one and only one.
//
// 8-bit, like the device it stands in for, so the saturating add saturates where the reference's
// does. Colour is not carried: every painter that uses this deposits one colour per segment.
enum class ScratchMode : std::uint8_t {
    Add, // saturating 8-bit sum -- the reference's antialiased particle copy
    Over, // source-over -- its compositing branch
    Max,  // keep the more opaque -- its `darkenPixel`, and a path mask laid once
};

// The backstop on one segment's scratch. Clamped to the document either way; this bounds the
// pathological case where a huge brush on a huge canvas would ask for the whole layer per segment.
inline constexpr std::size_t kMaxScratchCells = 1u << 24;

class SegmentScratch {
public:
    // Size to the (half-open) box intersected with the canvas, and clear. False when nothing of it
    // is on the document -- the caller must still run its mark loop, because the marks' STATE is a
    // property of the stroke (see the painters), it just lands nowhere.
    bool reset(int x0, int y0, int x1, int y1, const StrokeCanvas& canvas);
    void plot(int x, int y, int value, ScratchMode mode);
    // Deposit every non-zero cell, scaled by `opacity` -- the reference blits its temporary at the
    // painter's opacity, so the scale belongs to the BLIT and not to the marks that built it.
    void flush(StrokeCanvas& canvas, common::Color8 color, double opacity = 1.0) const;

    [[nodiscard]] int originX() const noexcept { return m_x; }
    [[nodiscard]] int originY() const noexcept { return m_y; }
    [[nodiscard]] int width() const noexcept { return m_w; }
    [[nodiscard]] int height() const noexcept { return m_h; }

private:
    std::vector<std::uint8_t> m_cells;
    int m_x = 0;
    int m_y = 0;
    int m_w = 0;
    int m_h = 0;
};

// Fill a closed polygon into `out` at full value, by the winding (non-zero) or the alternate
// (even-odd) rule. `antialias` supersamples the rule 4x4 per pixel; otherwise a pixel is in when
// its centre is. The reference fills through Qt's rasterizer -- not reproduced, and the difference
// is confined to the edge ramp (§6.6g).
void fillPolygon(const std::vector<common::Vec2>& points, bool windingFill, bool antialias,
                 SegmentScratch& out);

// ------------------------------------------------------------------------------------------------
// The painters, as DATA. A `StrokePainter` has per-stroke mutable state, so it cannot live on
// `BrushParams` (which is copied per stroke and shared const); the SPEC does, and `begin()` builds
// the painter from it. That is also what keeps a stroke replayable: the params describe it wholly.

enum class StrokePainterKind : std::uint8_t {
    None, // the dab walk -- every pixel-brush, smudge, hatching and eraser preset
    Sketch,
    Hairy,
    Curve,
    Particle,
    Experiment,
};

// The `Sketch/*` property block, transcribed name for name with the reference reader's defaults.
struct SketchPainterParams {
    double probability = 0.5;     // Sketch/probability
    double offset = 30.0;         // Sketch/offset -- a PERCENT; the painter multiplies by 0.01
    int lineWidth = 1;            // Sketch/lineWidth, px
    bool simpleMode = false;      // Sketch/simpleMode: the radius test instead of the mask test
    bool makeConnection = true;   // Sketch/makeConnection: draw the segment itself, not only the web
    bool magnetify = true;        // Sketch/magnetify
    bool randomRgb = false;       // Sketch/randomRGB
    bool randomOpacity = false;   // Sketch/randomOpacity
    bool distanceOpacity = false; // Sketch/distanceOpacity
    bool distanceDensity = true;  // Sketch/distanceDensity
    bool antiAliasing = false;    // Sketch/antiAliasing
};

// The `HairyBristle/*` + `HairyInk/*` property blocks (the Sumi-e engine), transcribed name for name
// with the reference reader's defaults. The three percent weights are divided by 100 at load exactly
// as the reference does -- and `inkDepletionWeight` is NOT, which is the reference's own asymmetry
// and is reproduced rather than tidied.
struct HairyPainterParams {
    bool useMousePressure = false; // HairyBristle/useMousePressure
    double shearFactor = 0.0;      // HairyBristle/shear
    double randomFactor = 2.0;     // HairyBristle/random
    double scaleFactor = 2.0;      // HairyBristle/scale
    double densityFactor = 100.0;  // HairyBristle/density -- a PERCENT of the tip's opaque pixels
    bool threshold = false;        // HairyBristle/threshold
    bool antialias = false;        // HairyBristle/antialias
    bool useCompositing = false;   // HairyBristle/useCompositing
    bool connectedPath = false;    // HairyBristle/isConnected

    bool inkDepletionEnabled = false; // HairyInk/enabled
    int inkAmount = 1024;             // HairyInk/inkAmount -- the depletion LUT's size
    Curve inkDepletionCurve;          // HairyInk/inkDepletionCurve
    bool useOpacity = true;           // HairyInk/useOpacity
    bool useWeights = false;          // HairyInk/useWeights
    double pressureWeight = 0.5;           // HairyInk/pressureWeights        / 100
    double bristleLengthWeight = 0.5;      // HairyInk/bristleLengthWeights   / 100
    double bristleInkAmountWeight = 0.5;   // HairyInk/bristleInkAmountWeight / 100
    double inkDepletionWeight = 50.0;      // HairyInk/inkDepletionWeight     -- ⚠ NOT / 100
};

// The `Curve/*` property block. ⚠ The reference reader passes NO defaults to its own getters, so an
// absent key reads as false / 0 / 0.0 -- including the history size, which then makes its path build
// from an empty list. The defaults below are its STRUCT's, which is what a preset written by its own
// UI always carries; the history size is additionally floored at 1 by the painter, because a zero
// there is not a shape, it is a crash.
struct CurvePainterParams {
    bool makeConnection = false; // Curve/makeConnection
    bool smoothing = false;      // Curve/smoothing: a quadratic through the history, not a cubic
    int strokeHistorySize = 30;  // Curve/strokeHistorySize
    int lineWidth = 1;           // Curve/lineWidth, px
    double curvesOpacity = 1.0;  // Curve/curvesOpacity
};

// The `Particle/*` property block, name for name with the reference reader's defaults.
struct ParticlePainterParams {
    int count = 50;       // Particle/count
    int iterations = 10;  // Particle/iterations
    double gravity = 0.989; // Particle/gravity
    double weight = 0.2;    // Particle/weight
    double scaleX = 0.3;    // Particle/scaleX
    double scaleY = 0.3;    // Particle/scaleY
};

// The `Experiment/*` property block. Only the two that shape the FILL are transcribed; displacement,
// speed correction and path smoothing are badged at import (see the painter).
struct ExperimentPainterParams {
    bool windingFill = false; // Experiment/windingFill: non-zero rather than even-odd
    bool hardEdge = false;    // Experiment/hardEdge: no antialiasing on the fill
};

// Which painter a preset paints through, and its settings. `kind == None` is every dab preset.
struct StrokePainterParams {
    StrokePainterKind kind = StrokePainterKind::None;
    SketchPainterParams sketch;
    HairyPainterParams hairy;
    CurvePainterParams curve;
    ParticlePainterParams particle;
    ExperimentPainterParams experiment;
};

// ⚠ THE ONE LIST OF WHICH PAINTOPS HAVE A REAL ENGINE OF THIS KIND. The importer's fidelity floor
// (io/brush/mapper.cpp -- does this preset start Exact, or is it substituted with a pixel brush?)
// and the preset -> engine mapping (io/brush/preset_brush.cpp -- does this preset get a painter?)
// BOTH read it, for exactly the reason `kDrivenOptions` (dab.hpp) is one list: a paintop cannot be
// promised an engine it does not get, or get one the badge says it does not have. The biconditional
// is a test (tests/test_brush_preset_brush.cpp).
[[nodiscard]] StrokePainterKind painterKindForPaintop(std::string_view paintopId) noexcept;

// Does this painter deposit a colour of its OWN, mark by mark? Then the stroke needs the `Colored`
// accumulator, exactly as a colour-dynamics option or a colour-stamping tip does -- one coverage
// channel cannot carry two colours. (§6.1's rule, third input.)
[[nodiscard]] bool painterVariesColor(const StrokePainterParams& params) noexcept;

// The stroke-scoped facts a painter is built against, handed over once by `BrushEngine::begin()`.
struct StrokePainterContext {
    const BrushOptions* options = nullptr; // the preset's option table; NULL means none at all
    const BrushTip* tip = nullptr;         // the preset's tip; NULL is the analytic circle
    double diameter = 24.0;                // the tip's authored master size, document px
    double ratio = 1.0;                    // ... and its authored aspect (a bitmap tip's is 1)
    double angleRad = 0.0;                 // the nib's authored slant
    common::Color8 color{0, 0, 0, 255};    // the stroke's paint colour
    StrokeInput first{};                   // the press sample: a painter's stroke starts HERE
};

class StrokePainter {
public:
    StrokePainter() = default;
    StrokePainter(const StrokePainter&) = delete;
    StrokePainter& operator=(const StrokePainter&) = delete;
    virtual ~StrokePainter() = default;

    // Once, at the press. Nothing is painted here: a painter's mark is made per span.
    virtual void begin(const StrokePainterContext& ctx) = 0;

    // One flattened edge of the stroke's path, from `a` to `b`.
    //
    // ⚠ `state` HAS ALREADY BEEN REWOUND TO `b`, and the span counter has already advanced -- the
    // reference evaluates its options against the segment's END sample, and the engine puts the
    // stroke there before calling. Reading an option or a random number here therefore reads the
    // right point of the stroke, and every such read is part of the replay contract: draw the same
    // things in the same order on every replay, or a golden moves.
    virtual void paintSpan(StrokeCanvas& canvas, const StrokeSnapshot& a, const StrokeSnapshot& b,
                           StrokeState& state) = 0;

    // At `end()`, after the tail span. The seam a WHOLE-STROKE painter needs (§6.6b's
    // `experimentbrush` accumulates the entire stroke and fills it as one polygon on release);
    // neither painter built so far has anything to do here.
    virtual void finish(StrokeCanvas& canvas) { (void)canvas; }
};

// Build the painter a spec describes. Null for `kind == None`.
[[nodiscard]] std::unique_ptr<StrokePainter> makeStrokePainter(const StrokePainterParams& params);

// The reference's `KisStandardOption::apply`, verbatim: a CHECKED option contributes its size-like
// value WITH strength, an unchecked or absent one contributes exactly 1.0. Every painter option --
// Size, Density, Line width, Offset scale -- reads through this, so none of them can disagree about
// what "off" means. (The dab pipeline's own `sizeLikeOr` is the same rule; it lives in dab.cpp
// because that is where its callers are.)
[[nodiscard]] double standardOptionValue(const std::optional<CurveOption>& opt, StrokeState& state);

} // namespace mosaic::core::brush
