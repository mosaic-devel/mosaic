#include <doctest/doctest.h>

#include "common/image.hpp"
#include "core/brush/brush_engine.hpp"
#include "core/brush/curve_painter.hpp"
#include "core/brush/experiment_painter.hpp"
#include "core/brush/hairy_painter.hpp"
#include "core/brush/hatching.hpp"
#include "core/brush/particle_painter.hpp"
#include "core/brush/sketch_painter.hpp"
#include "core/brush/stroke_painter.hpp"
#include "io/brush/mapper.hpp"
#include "io/brush/preset_xml.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// THE SECOND ENGINE KIND (docs/brushes.md §6.6g): the StrokePainter scaffold, its two shared
// rasterizers, the sketch engine and the hairy/Sumi-e engine.
//
// ⚠ WHAT EACH CASE CAN ACTUALLY SEE is stated in its own comment, because the repeated lesson of
// this arc is that a metric which measures the wrong quantity passes while asserting nothing. The
// rasterizer cases compare EXACT pixel sets; the painter cases record every plot through a fake
// canvas and check values that only one implementation can produce; the engine cases compare two
// live strokes against each other rather than against a blessed constant, so they pin behaviour
// without a golden this session cannot bless.
namespace cb = mosaic::core::brush;

using mosaic::common::Color8;
using mosaic::common::Image;
using mosaic::common::Vec2;

namespace {

constexpr cb::LineClip kBigClip{-1000, -1000, 1000, 1000};

[[nodiscard]] std::set<std::pair<int, int>> pixelSet(const std::vector<cb::LinePixel>& px) {
    std::set<std::pair<int, int>> out;
    for (const cb::LinePixel& p : px)
        out.insert({p.x, p.y});
    return out;
}

// Everything a painter drew, kept: the seam that lets a painter be tested without an engine, a
// document or a composite.
class RecordingCanvas final : public cb::StrokeCanvas {
public:
    RecordingCanvas(int w, int h) : m_w(w), m_h(h) {}

    [[nodiscard]] int width() const noexcept override { return m_w; }
    [[nodiscard]] int height() const noexcept override { return m_h; }

    void plot(int x, int y, double alpha, Color8 color) override {
        plots.push_back(Plot{x, y, alpha, color});
    }

    struct Plot {
        int x = 0;
        int y = 0;
        double alpha = 0.0;
        Color8 color{};
    };
    std::vector<Plot> plots;

    [[nodiscard]] double alphaAt(int x, int y) const {
        for (const Plot& p : plots)
            if (p.x == x && p.y == y)
                return p.alpha;
        return 0.0;
    }
    [[nodiscard]] double totalAlpha() const {
        double t = 0.0;
        for (const Plot& p : plots)
            t += p.alpha;
        return t;
    }

private:
    int m_w;
    int m_h;
};

[[nodiscard]] cb::StrokeInput at(double x, double y, double pressure = 1.0) {
    cb::StrokeInput in;
    in.pos = {x, y};
    in.pressure = pressure;
    return in;
}

// A snapshot standing at `pos`. The engine builds these by interpolating a span; a direct painter
// test builds them by hand, which is the whole reason StrokeSnapshot is a value type.
[[nodiscard]] cb::StrokeSnapshot snapAt(Vec2 pos, double pressure = 1.0) {
    cb::StrokeSnapshot s;
    s.sample.pos = pos;
    s.sample.pressure = pressure;
    s.maxPressure = pressure;
    return s;
}

[[nodiscard]] Image greyCanvas(int w, int h) {
    Image img(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
    for (std::size_t i = 0; i + 3 < img.rgba.size(); i += 4) {
        img.rgba[i] = 128;
        img.rgba[i + 1] = 128;
        img.rgba[i + 2] = 128;
        img.rgba[i + 3] = 255;
    }
    return img;
}

// A minimal sketch preset: a 40 px reach, the web on, nothing random.
[[nodiscard]] cb::StrokePainterParams sketchParams() {
    cb::StrokePainterParams p;
    p.kind = cb::StrokePainterKind::Sketch;
    p.sketch.simpleMode = true;
    p.sketch.distanceDensity = false;
    p.sketch.probability = 1.0;
    p.sketch.offset = 0.0;
    p.sketch.lineWidth = 1;
    p.sketch.makeConnection = true;
    p.sketch.magnetify = true;
    return p;
}

// A minimal hairy preset: one hair (no tip -> the single-bristle fallback), no jitter, no shear.
[[nodiscard]] cb::HairyPainterParams oneHair() {
    cb::HairyPainterParams h;
    h.randomFactor = 0.0;
    h.shearFactor = 0.0;
    h.scaleFactor = 1.0;
    h.antialias = true;
    h.useCompositing = false;
    h.connectedPath = false;
    h.densityFactor = 100.0;
    return h;
}

// A minimal but complete tip, so a synthetic preset is not badged for the lack of one.
constexpr const char* kDef =
    R"(<param name="brush_definition" type="string"><![CDATA[<Brush type="auto_brush" )"
    R"(BrushVersion="2" spacing="0.1"><MaskGenerator diameter="10"/></Brush>]]></param>)";

[[nodiscard]] mosaic::io::brush::BrushPreset mapXml(std::string xml) {
    xml.insert(xml.rfind("</Preset>"), kDef);
    const auto parsed = mosaic::io::brush::parsePresetXml(xml);
    REQUIRE(parsed.has_value());
    return mosaic::io::brush::mapPreset(*parsed, "kpp");
}

[[nodiscard]] bool hasDrop(const mosaic::io::brush::BrushPreset& p, std::string_view needle) {
    for (const std::string& d : p.provenance.droppedOptions)
        if (d.find(needle) != std::string::npos)
            return true;
    return false;
}

[[nodiscard]] cb::StrokePainterContext ctxFor(Color8 color = Color8{0, 0, 0, 255}) {
    cb::StrokePainterContext ctx;
    ctx.diameter = 40.0;
    ctx.color = color;
    ctx.first = at(0.0, 0.0);
    return ctx;
}

} // namespace

// ================================================================================================
// The shared rasterizers.

TEST_CASE("rasterizer: the DDA line walks its major axis, one pixel per step") {
    // SEES: the exact pixel set. A missing seed plot loses the first pixel; a walk that steps the
    // wrong axis produces a different set entirely; a rounding change moves interior pixels.
    std::vector<cb::LinePixel> px;

    cb::rasterizeDdaLine({2.5, 3.5}, {8.5, 3.5}, kBigClip, px);
    std::set<std::pair<int, int>> want;
    for (int x = 2; x <= 8; ++x)
        want.insert({x, 3});
    CHECK(pixelSet(px) == want);
    for (const cb::LinePixel& p : px)
        CHECK(p.weight == 1.0); // the DDA line is hard-edged: no partial coverage anywhere

    // ⚠ THE VERTICAL LINE IS THE ONE THAT KILLS THE `lockAxis` MUTANT. With the gradient left at 0
    // for a zero dx the walk takes the x branch, the `x != x2` loop never runs, and the whole line
    // collapses to its first pixel.
    cb::rasterizeDdaLine({3.5, 2.5}, {3.5, 8.5}, kBigClip, px);
    want.clear();
    for (int y = 2; y <= 8; ++y)
        want.insert({3, y});
    CHECK(pixelSet(px) == want);

    cb::rasterizeDdaLine({0.5, 0.5}, {5.5, 5.5}, kBigClip, px);
    want.clear();
    for (int i = 0; i <= 5; ++i)
        want.insert({i, i});
    CHECK(pixelSet(px) == want);

    // A connection from a point to itself is a real case -- the sketch engine's history always
    // contains the point it is connecting FROM -- and the DDA line lays exactly one pixel for it.
    cb::rasterizeDdaLine({4.2, 9.9}, {4.8, 9.1}, kBigClip, px);
    CHECK(px.size() == 1);
    CHECK(px[0].x == 4);
    CHECK(px[0].y == 9);
}

TEST_CASE("rasterizer: the DDA line emits nothing outside its clip") {
    // SEES: that the clip is applied at the EMIT and not by shortening the walk -- the far half of
    // a line that starts inside must still be dropped rather than ending the loop early, and the
    // near half must survive in full.
    std::vector<cb::LinePixel> px;
    const cb::LineClip clip{0, 0, 5, 5};
    cb::rasterizeDdaLine({0.5, 0.5}, {19.5, 0.5}, clip, px);
    CHECK(px.size() == 5);
    for (const cb::LinePixel& p : px)
        CHECK(p.x < 5);
}

TEST_CASE("rasterizer: a thick line whose ends share a pixel draws NOTHING") {
    // SEES: the reference's early return, which is not a micro-optimization -- the sketch engine
    // asks for exactly this line on every single span (the history contains the new point itself,
    // at distance zero), and without the return that degenerate ask becomes a blob at the cursor.
    std::vector<cb::LinePixel> px;
    cb::rasterizeThickLine({10.2, 10.2}, {10.8, 10.9}, 4.0, false, kBigClip, px);
    CHECK(px.empty());
}

TEST_CASE("rasterizer: the thick line is a band around the segment") {
    // SEES: the band's WIDTH and its full-weight interior. A distance field measured to the line
    // rather than to the SEGMENT would round the ends differently, and a half-width that dropped
    // its sub-pixel term would give a measurably thinner band.
    std::vector<cb::LinePixel> px;
    cb::rasterizeThickLine({10.0, 20.0}, {30.0, 20.0}, 5.0, false, kBigClip, px);
    REQUIRE(!px.empty());

    // Every pixel of a non-antialiased thick line is full weight.
    for (const cb::LinePixel& p : px)
        CHECK(p.weight == 1.0);

    // The column at mid-span is a contiguous run centred on the line.
    std::vector<int> column;
    for (const cb::LinePixel& p : px)
        if (p.x == 20)
            column.push_back(p.y);
    std::sort(column.begin(), column.end());
    REQUIRE(column.size() >= 5);
    CHECK(column.front() <= 18);
    CHECK(column.back() >= 22);
    CHECK(column.back() - column.front() + 1 == static_cast<int>(column.size())); // contiguous

    // ... and the band does not reach beyond the half width plus its one-pixel margin.
    for (const cb::LinePixel& p : px) {
        CHECK(p.y >= 20 - 4);
        CHECK(p.y <= 20 + 4);
    }
}

TEST_CASE("rasterizer: an antialiased thick line fades its outer rim and only its rim") {
    // SEES: that the fade exists AND is confined -- a mutant that multiplied every pixel by the
    // ramp would leave no full-weight interior, and one that dropped the ramp would leave no
    // partial pixel at all. Both are visible here; neither is visible from a pixel COUNT.
    std::vector<cb::LinePixel> hard;
    std::vector<cb::LinePixel> soft;
    cb::rasterizeThickLine({10.0, 20.0}, {30.0, 20.0}, 5.0, false, kBigClip, hard);
    cb::rasterizeThickLine({10.0, 20.0}, {30.0, 20.0}, 5.0, true, kBigClip, soft);
    CHECK(hard.size() == soft.size()); // the BAND is the same; only the weights differ

    int partial = 0;
    int full = 0;
    for (const cb::LinePixel& p : soft) {
        CHECK(p.weight <= 1.0);
        CHECK(p.weight >= 0.0);
        if (p.weight > 0.0 && p.weight < 1.0)
            ++partial;
        if (p.weight == 1.0)
            ++full;
    }
    CHECK(partial > 0);
    CHECK(full > 0);
}

TEST_CASE("rasterizer: the bristle trajectory keeps its exact endpoints") {
    // SEES: the point COUNT and the two exact endpoints. The count is what the hairy engine's
    // "drop the last point when antialiasing" arithmetic rests on, and a degenerate trajectory
    // yielding two coincident points is what makes a bristle that did not move lay exactly one
    // splat rather than none or two.
    std::vector<Vec2> path;

    cb::linearTrajectory({10.5, 10.5}, {10.5, 10.5}, path);
    REQUIRE(path.size() == 2);
    CHECK(path[0].x == 10.5);
    CHECK(path[1].y == 10.5);

    cb::linearTrajectory({0.0, 0.0}, {4.0, 0.0}, path);
    // start + one point per integer step + the exact end.
    REQUIRE(path.size() == 6);
    CHECK(path.front().x == 0.0);
    CHECK(path.back().x == 4.0);

    // The steep branch: the walk switches axes, so the point count follows dy rather than dx.
    cb::linearTrajectory({0.0, 0.0}, {1.0, 5.0}, path);
    REQUIRE(path.size() == 7);
    CHECK(path.back().y == 5.0);
    // ... and the minor coordinate is FRACTIONAL along the way, which is what gives a bristle its
    // sub-pixel path (an integer-only walk would splat on whole pixels and lose the antialiasing).
    bool anyFractional = false;
    for (const Vec2& p : path)
        if (p.x != std::floor(p.x))
            anyFractional = true;
    CHECK(anyFractional);
}

// ================================================================================================
// The engine-kind rules.

TEST_CASE("painter: the paintop -> engine-kind list is the ONE list, and it is closed") {
    CHECK(cb::painterKindForPaintop("sketchbrush") == cb::StrokePainterKind::Sketch);
    CHECK(cb::painterKindForPaintop("hairybrush") == cb::StrokePainterKind::Hairy);
    CHECK(cb::painterKindForPaintop("curvebrush") == cb::StrokePainterKind::Curve);
    CHECK(cb::painterKindForPaintop("particlebrush") == cb::StrokePainterKind::Particle);
    CHECK(cb::painterKindForPaintop("experimentbrush") == cb::StrokePainterKind::Experiment);
    // Everything else in the shipped corpus is either a dab engine or still has no engine at all.
    // ⚠ `hatchingbrush` belongs to the FIRST of those, not the second, and that is a claim about
    // the algorithm rather than about a gate: it derives from the reference's brush-based base and
    // overrides `paintAt`, so it rides the dab walk with a procedural lattice for a dab. The case
    // below pins that it really does get an engine, so this line cannot be read as "unbuilt".
    for (const char* op : {"paintbrush", "eraser", "roundmarker", "colorsmudge", "smudge",
                           "spraybrush", "filter", "deformbrush", "duplicate", "gridbrush",
                           "hatchingbrush", "tangentnormal", ""}) {
        CAPTURE(op);
        CHECK(cb::painterKindForPaintop(op) == cb::StrokePainterKind::None);
    }
}

TEST_CASE("mapper: hatching is a DAB engine, and the mapping says so on both sides") {
    // SEES: that a hatching preset gets NO painter and DOES get the lattice -- the two halves of
    // "it is a dab engine". A future session that mistook it for a painter would break one or the
    // other, and the corpus alone could not tell (it would still paint, just through the wrong
    // engine, and every count would hold).
    const auto hatch =
        mapXml(R"(<Preset name="h" paintopid="hatchingbrush">)"
               R"(<param name="Hatching/angle" type="internal">45</param>)"
               R"(<param name="Hatching/separation" type="internal">18</param>)"
               R"(<param name="Hatching/thickness" type="internal">9</param>)"
               R"(<param name="Hatching/bool_nocrosshatching" type="internal">false</param>)"
               R"(<param name="Hatching/bool_moirepattern" type="internal">true</param>)"
               R"(<param name="Hatching/bool_subpixelprecision" type="internal">true</param>)"
               R"(</Preset>)");
    CHECK(hatch.painter.kind == cb::StrokePainterKind::None);
    CHECK(hatch.hatching.enabled);
    CHECK(hatch.hatching.angle == doctest::Approx(45.0));
    CHECK(hatch.hatching.separation == doctest::Approx(18.0));
    CHECK(hatch.hatching.thickness == doctest::Approx(9.0));
    CHECK(hatch.hatching.style == cb::CrosshatchingStyle::Moire);
    CHECK(hatch.hatching.subpixelPrecision);
    CHECK(hatch.provenance.fidelity == mosaic::io::brush::PresetFidelity::Exact);

    // ⚠ THE STYLE IS A PRIORITY CHAIN whose FIRST key defaults TRUE: a file that mentions none of
    // the five hatches once, not zero times. A preset that only sets `bool_moirepattern` and leaves
    // `bool_nocrosshatching` absent is therefore NOT a moiré preset -- which is exactly the trap a
    // reader that treated the five as an enum would fall into.
    const auto defaulted =
        mapXml(R"(<Preset name="h" paintopid="hatchingbrush">)"
               R"(<param name="Hatching/bool_moirepattern" type="internal">true</param>)"
               R"(</Preset>)");
    CHECK(defaulted.hatching.style == cb::CrosshatchingStyle::None);

    // And a plain pixel brush gets neither.
    const auto pixel = mapXml(R"(<Preset name="p" paintopid="paintbrush"></Preset>)");
    CHECK_FALSE(pixel.hatching.enabled);
    CHECK(pixel.painter.kind == cb::StrokePainterKind::None);
}

TEST_CASE("painter: the factory builds exactly the kinds the list names") {
    cb::StrokePainterParams none;
    CHECK(cb::makeStrokePainter(none) == nullptr);
    for (const cb::StrokePainterKind kind :
         {cb::StrokePainterKind::Sketch, cb::StrokePainterKind::Hairy, cb::StrokePainterKind::Curve,
          cb::StrokePainterKind::Particle, cb::StrokePainterKind::Experiment}) {
        cb::StrokePainterParams p;
        p.kind = kind;
        CHECK(cb::makeStrokePainter(p) != nullptr);
    }
}

TEST_CASE("mapper: the smudge trio outside the smudge family is a BADGE-FREE drop") {
    // ⚠⚠ IT WAS BADGED UNTIL 2026-07-28, AND FIVE SHIPPED PRESETS PAID FOR IT (§6.6i). The
    // reference's own pixel brush constructs no smudge option AT ALL -- its option list is
    // size / ratio / rate / softness / lightness / spacing / scatter / sharpness / rotation /
    // opacity, plus mirror and precision -- so `SmudgeRate` on a `paintbrush` is read by nothing
    // there, Mosaic's engine reads it only on the smudge walk, and the two strokes are the same
    // stroke. A badge measures the DIFFERENCE, so there is nothing to badge; this is the Mirror
    // -on-colorsmudge shape (dab.hpp), not a kindness.
    using mosaic::io::brush::PresetFidelity;
    for (const char* base : {"SmudgeRate", "ColorRate", "SmudgeRadius"}) {
        CAPTURE(base);
        const std::string on = std::string(R"(<param name="Pressure)") + base +
                               R"(" type="internal">true</param>)";
        const auto pixel =
            mapXml(R"(<Preset name="p" paintopid="paintbrush">)" + on + R"(</Preset>)");
        CHECK_FALSE(hasDrop(pixel, base));
        CHECK(pixel.provenance.fidelity == PresetFidelity::Exact);
        // ... and the option is still IMPORTED, so a preset that is later re-saved keeps it and a
        // smudge engine would still find it. Dropping it from the table would be a real loss.
        REQUIRE(pixel.option(base) != nullptr);
        CHECK(pixel.option(base)->checked);

        // On the smudge family the very same key is honoured by the walk, which is what makes the
        // case above a statement about the CONSUMER rather than about the option. (Only the drop is
        // asserted here: a synthetic colorsmudge preset can pick up the smudge family's OWN badges,
        // which is a different rule with its own case.)
        const auto smudge =
            mapXml(R"(<Preset name="s" paintopid="colorsmudge">)" + on + R"(</Preset>)");
        CHECK_FALSE(hasDrop(smudge, base));
    }
}

TEST_CASE("mapper: the sketch trio is honoured by the sketch painter and badged everywhere else") {
    // ⚠ The smudge trio's caveat WITHOUT its 2026-07-28 correction, and deliberately so: the same
    // reading (the reference's other paintops construct no Density option either) very likely
    // applies, but no shipped preset trips this rule, so there is no corpus evidence to move it on
    // and a badge that overstates a loss is the safe direction. `Density`,
    // `Line width` and `Offset scale` are in `kDrivenOptions` -- so `optionSupported()` says yes and
    // the generic drop does NOT fire -- but ONLY the sketch painter reads them. Without this rule a
    // paintbrush preset that drives Line width would import as Exact and paint without it, which is
    // precisely the failure that cost 26 presets their honesty in the 16th session.
    using mosaic::io::brush::PresetFidelity;
    for (const char* base : {"Density", "Line width", "Offset scale"}) {
        CAPTURE(base);
        const std::string on = std::string(R"(<param name="Pressure)") + base +
                               R"(" type="internal">true</param>)";
        const auto pixel =
            mapXml(R"(<Preset name="p" paintopid="paintbrush">)" + on + R"(</Preset>)");
        CHECK(pixel.provenance.fidelity == PresetFidelity::Approximated);
        CHECK(hasDrop(pixel, base));

        const auto sketch =
            mapXml(R"(<Preset name="s" paintopid="sketchbrush"><param name="Sketch/simpleMode" )"
                   R"(type="internal">true</param>)" +
                   on + R"(</Preset>)");
        CHECK_FALSE(hasDrop(sketch, base)); // the painter reads it: no badge, no fidelity cost
        CHECK(sketch.provenance.fidelity == PresetFidelity::Exact);
        CHECK(sketch.painter.kind == cb::StrokePainterKind::Sketch);
    }
}

TEST_CASE("mapper: a painter preset is badged for exactly what the transcription leaves out") {
    // SEES: that the two sketch badges and the two hairy badges each fire on their own key and on
    // nothing else -- an over-badged preset understates fidelity (§8.1 fixed that once already) and
    // an under-badged one is the lie §6.4 exists to prevent.
    using mosaic::io::brush::PresetFidelity;

    // Simple mode ON, anti-aliasing OFF: nothing the painter cannot do -> Exact, no notes.
    const auto clean =
        mapXml(R"(<Preset name="s" paintopid="sketchbrush">)"
               R"(<param name="Sketch/simpleMode" type="internal">true</param></Preset>)");
    CHECK(clean.provenance.fidelity == PresetFidelity::Exact);
    CHECK(clean.provenance.droppedOptions.empty());

    // The mask connection test and the Wu 1 px line are the two the transcription leaves out.
    const auto masked = mapXml(R"(<Preset name="s" paintopid="sketchbrush"></Preset>)");
    CHECK(hasDrop(masked, "Sketch mask mode")); // simpleMode's reader default is FALSE
    const auto aa =
        mapXml(R"(<Preset name="s" paintopid="sketchbrush">)"
               R"(<param name="Sketch/simpleMode" type="internal">true</param>)"
               R"(<param name="Sketch/antiAliasing" type="internal">true</param></Preset>)");
    CHECK(hasDrop(aa, "anti-aliasing"));

    // The hairy engine's two: saturation depletion (an HSL transform) and soaked ink.
    const auto hairy = mapXml(R"(<Preset name="h" paintopid="hairybrush"></Preset>)");
    CHECK(hairy.provenance.fidelity == PresetFidelity::Exact);
    CHECK(hairy.provenance.droppedOptions.empty());
    CHECK(hairy.painter.kind == cb::StrokePainterKind::Hairy);
    const auto sat =
        mapXml(R"(<Preset name="h" paintopid="hairybrush">)"
               R"(<param name="HairyInk/useSaturation" type="internal">true</param></Preset>)");
    CHECK(hasDrop(sat, "saturation depletion"));
    const auto soak = mapXml(R"(<Preset name="h" paintopid="hairybrush">)"
                             R"(<param name="HairyInk/soak" type="internal">true</param></Preset>)");
    CHECK(hasDrop(soak, "soak"));
}

TEST_CASE("painter: a per-mark colour forces the Colored accumulator") {
    // SEES: §6.1's rule with its third input. One coverage channel cannot carry two colours, so a
    // sketch preset that randomizes its connection colour has to land on Colored -- and one that
    // does not must NOT, or every sketch stroke would pay for a colour buffer it never fills.
    cb::StrokePainterParams p = sketchParams();
    CHECK_FALSE(cb::painterVariesColor(p));
    p.sketch.randomRgb = true;
    CHECK(cb::painterVariesColor(p));

    cb::StrokePainterParams hairy;
    hairy.kind = cb::StrokePainterKind::Hairy;
    CHECK_FALSE(cb::painterVariesColor(hairy)); // every bristle deposits the stroke's own colour

    using cb::StrokeAccumulator;
    using cb::TipApplication;
    CHECK(cb::chooseAccumulator(TipApplication::AlphaMask, false, false) ==
          StrokeAccumulator::Uniform);
    CHECK(cb::chooseAccumulator(TipApplication::AlphaMask, false, true) ==
          StrokeAccumulator::Colored);
}

// ================================================================================================
// The sketch engine.

TEST_CASE("sketch: the history grows one point per span, seeded with the press") {
    // SEES: the history's SIZE and its first element. A painter that appended both ends of a span
    // would double every point; one that never seeded would lose the press point, which is the
    // anchor every early connection is made to.
    cb::SketchPainter painter(sketchParams().sketch);
    painter.begin(ctxFor());
    REQUIRE(painter.points().size() == 1);
    CHECK(painter.points()[0].x == 0.0);

    RecordingCanvas canvas(64, 64);
    cb::StrokeState state;
    state.begin(at(10.0, 10.0), /*seed=*/7);
    painter.paintSpan(canvas, snapAt({10.0, 10.0}), snapAt({20.0, 10.0}), state);
    painter.paintSpan(canvas, snapAt({20.0, 10.0}), snapAt({30.0, 10.0}), state);
    REQUIRE(painter.points().size() == 3);
    CHECK(painter.points()[1].x == 20.0);
    CHECK(painter.points()[2].x == 30.0);
}

TEST_CASE("sketch: the WEB is the mark -- connections land where no segment ever went") {
    // ⚠ THE CASE THE WHOLE ENGINE EXISTS FOR, and it is two-sided on purpose. With the segment line
    // switched off, EVERY pixel drawn is a connection between two history points; a pixel on the
    // chord from the newest point back to the FIRST one is therefore paint that no dab walk and no
    // segment could have laid. Turning the probability off must take that same pixel away -- which
    // is what rules out "it painted there for some other reason".
    //
    // The three points make an L: (10,10) -> (30,30) -> (10,30). The chord from the last point back
    // to the first is the vertical x = 10, and (10,20) sits on it and on neither segment.
    cb::StrokePainterParams params = sketchParams();
    params.sketch.makeConnection = false; // no segment lines at all: only the web
    params.sketch.offset = 0.0;

    const auto run = [&]() {
        cb::SketchPainter painter(params.sketch);
        cb::StrokePainterContext ctx = ctxFor();
        ctx.diameter = 200.0; // a reach that covers the whole figure
        ctx.first = at(10.0, 10.0);
        painter.begin(ctx);
        RecordingCanvas canvas(64, 64);
        cb::StrokeState state;
        state.begin(at(10.0, 10.0), /*seed=*/3);
        painter.paintSpan(canvas, snapAt({10.0, 10.0}), snapAt({30.0, 30.0}), state);
        painter.paintSpan(canvas, snapAt({30.0, 30.0}), snapAt({10.0, 30.0}), state);
        return canvas.alphaAt(10, 20);
    };

    params.sketch.probability = 1.0;
    CHECK(run() > 0.0);

    // Probability 0 makes the draw threshold 1.0, and a uniform draw on [0,1) is never >= 1 -- so
    // no connection is made, and the only thing that could still paint (the segment line) is off.
    params.sketch.probability = 0.0;
    CHECK(run() == 0.0);
}

TEST_CASE("sketch: makeConnection alone draws the segment and nothing else") {
    // SEES: the exact pixel set of one span. With the web's probability at 0 the only mark left is
    // the segment line, so its raster must be EXACTLY the DDA line between the two points -- which
    // is also what pins that the painter passes the right two endpoints in the right order.
    cb::StrokePainterParams params = sketchParams();
    params.sketch.probability = 0.0;
    params.sketch.makeConnection = true;

    cb::SketchPainter painter(params.sketch);
    cb::StrokePainterContext ctx = ctxFor();
    ctx.first = at(10.0, 10.0);
    painter.begin(ctx);

    RecordingCanvas canvas(64, 64);
    cb::StrokeState state;
    state.begin(at(10.0, 10.0), /*seed=*/1);
    painter.paintSpan(canvas, snapAt({10.0, 10.0}), snapAt({20.0, 10.0}), state);

    std::vector<cb::LinePixel> want;
    cb::rasterizeDdaLine({10.0, 10.0}, {20.0, 10.0}, cb::LineClip{0, 0, 64, 64}, want);
    std::set<std::pair<int, int>> got;
    for (const RecordingCanvas::Plot& p : canvas.plots)
        got.insert({p.x, p.y});
    CHECK(got == pixelSet(want));
}

TEST_CASE("sketch: randomRGB varies the connection colour, and nothing else does") {
    // SEES: the SET of colours plotted. Off, every plot carries the stroke's colour exactly -- so a
    // painter that leaked a random colour into the non-random path is visible. On, at least two
    // distinct colours appear; surviving that would need every one of the many draws across the
    // whole web to round back to the same byte triple.
    const Color8 ink{200, 120, 60, 255};

    const auto colours = [&](bool randomRgb) {
        cb::StrokePainterParams params = sketchParams();
        params.sketch.randomRgb = randomRgb;
        cb::SketchPainter painter(params.sketch);
        cb::StrokePainterContext ctx = ctxFor(ink);
        ctx.diameter = 200.0;
        ctx.first = at(10.0, 10.0);
        painter.begin(ctx);
        RecordingCanvas canvas(64, 64);
        cb::StrokeState state;
        state.begin(at(10.0, 10.0), /*seed=*/11);
        painter.paintSpan(canvas, snapAt({10.0, 10.0}), snapAt({20.0, 20.0}), state);
        painter.paintSpan(canvas, snapAt({20.0, 20.0}), snapAt({30.0, 12.0}), state);
        painter.paintSpan(canvas, snapAt({30.0, 12.0}), snapAt({18.0, 26.0}), state);
        std::set<std::uint32_t> out;
        for (const RecordingCanvas::Plot& p : canvas.plots)
            out.insert((static_cast<std::uint32_t>(p.color.r) << 16) |
                       (static_cast<std::uint32_t>(p.color.g) << 8) | p.color.b);
        return out;
    };

    const std::set<std::uint32_t> plain = colours(false);
    REQUIRE(plain.size() == 1);
    CHECK(*plain.begin() ==
          ((static_cast<std::uint32_t>(ink.r) << 16) | (static_cast<std::uint32_t>(ink.g) << 8) |
           ink.b));
    CHECK(colours(true).size() > 1);
}

// ================================================================================================
// The hairy / Sumi-e engine.

TEST_CASE("hairy: every opaque pixel of the tip becomes a bristle") {
    // SEES: the bristle COUNT against an independently rasterized mask. This is the claim the whole
    // engine rests on, and it is checked against the tip machinery's own output rather than against
    // a number typed here -- a change in the tip rasterizer moves both sides together, while a
    // change in the bristle loop (a threshold instead of `!= 0`, a row skipped, the density draw
    // taken at full density) moves only one.
    cb::MaskGeneratorParams gen;
    gen.shape = cb::MaskShape::Circle;
    gen.diameter = 21.0;
    gen.ratio = 1.0;
    gen.hFade = 1.0;
    gen.vFade = 1.0;
    const std::shared_ptr<const cb::BrushTip> tip = cb::makeTip(gen);

    cb::StrokePainterContext ctx = ctxFor();
    ctx.tip = tip.get();
    ctx.diameter = 21.0;
    ctx.ratio = 1.0;

    const cb::DabShape shape = cb::tipDabShape(*tip, 0, 21.0, 1.0, 0.0, false, false);
    const cb::DabMask mask = cb::renderTipMask(*tip, 0, shape, 1.0, 0.0, 0.0);
    std::size_t opaque = 0;
    for (std::uint32_t y = 0; y < mask.height; ++y)
        for (std::uint32_t x = 0; x < mask.width; ++x)
            if (mask.at(x, y) != 0)
                ++opaque;
    REQUIRE(opaque > 0);

    cb::HairyPainter full(oneHair());
    full.begin(ctx);
    CHECK(full.bristleCount() == opaque);

    // ... and the density percentage keeps a strict subset of them.
    cb::HairyPainterParams half = oneHair();
    half.densityFactor = 40.0;
    cb::HairyPainter thin(half);
    thin.begin(ctx);
    CHECK(thin.bristleCount() > 0);
    CHECK(thin.bristleCount() < opaque);

    // ⚠ THE BRISTLE LAYOUT IS A PROPERTY OF THE BRUSH, NOT OF A STROKE: the reference draws its
    // density subset from a fixed-seed source, so the same preset must give the same field every
    // time it is picked up. A painter that seeded this from the stroke would differ here.
    cb::HairyPainter thinAgain(half);
    thinAgain.begin(ctx);
    CHECK(thinAgain.bristleCount() == thin.bristleCount());
}

TEST_CASE("hairy: a segment's marks ADD inside the segment, they do not composite over") {
    // ⚠ THE PER-SEGMENT SCRATCH, PINNED IN NUMBERS. A single hair walking one pixel lays two 2x2
    // splats one pixel apart, so two cells receive a contribution from BOTH. At full opacity each
    // contribution is round(0.25 * 255) = 64, and the three combination laws the reference can take
    // give three different answers at the shared cell:
    //     add-saturate (antialias, no compositing) -> 128
    //     source-over  (compositing)               -> 64 + round(64 * 191/255) = 112
    //     max          (no antialias)              -> 64
    // Depositing each splat straight into the stroke's coverage would give the middle one, which is
    // exactly the deviation the scratch exists to avoid.
    const cb::HairyPainterParams params = oneHair();

    const auto shared = [&](bool useCompositing) {
        cb::HairyPainterParams p = params;
        p.useCompositing = useCompositing;
        cb::HairyPainter painter(p);
        cb::StrokePainterContext ctx = ctxFor();
        ctx.tip = nullptr; // the single-hair fallback: one bristle at the centre, length 1
        painter.begin(ctx);
        RecordingCanvas canvas(64, 64);
        cb::StrokeState st;
        st.begin(at(10.5, 10.5), /*seed=*/5);
        painter.paintSpan(canvas, snapAt({10.5, 10.5}), snapAt({11.5, 10.5}), st);
        return canvas.alphaAt(11, 10);
    };

    CHECK(shared(false) == doctest::Approx(128.0 / 255.0));
    CHECK(shared(true) == doctest::Approx(112.0 / 255.0));
}

TEST_CASE("hairy: the ink counter ages the mark through the transfer curve") {
    // ⚠ THE FIRST MARK OF A DEPLETING BRUSH LAYS NOTHING, and that is the reference's own
    // arithmetic, not a bug in this transcription: a bristle's stored ink amount starts at ZERO and
    // is only written after a mark is laid, while the opacity of a mark is `length x inkAmount`. So
    // the first mark is transparent, the second is full, and every one after that steps down the
    // depletion curve.
    //
    // SEES: three successive spans of a single hair that does not move (one mark each, because the
    // antialiased path drops the trajectory's last point). A painter that read the curve at the
    // wrong index, never advanced the counter, or wrote the ink amount BEFORE the mark would break
    // one of the three.
    cb::HairyPainterParams params = oneHair();
    params.inkDepletionEnabled = true;
    params.useOpacity = true;
    params.useWeights = false;
    params.inkAmount = 4; // a 4-entry transfer: 0, 1/3, 2/3, 1

    cb::HairyPainter painter(params);
    cb::StrokePainterContext ctx = ctxFor();
    ctx.tip = nullptr;
    painter.begin(ctx);

    cb::StrokeState state;
    state.begin(at(10.5, 10.5), /*seed=*/2);

    RecordingCanvas first(64, 64);
    painter.paintSpan(first, snapAt({10.5, 10.5}), snapAt({10.5, 10.5}), state);
    CHECK(first.plots.empty()); // ink amount 0 -> a fully transparent mark -> nothing plotted

    RecordingCanvas second(64, 64);
    painter.paintSpan(second, snapAt({10.5, 10.5}), snapAt({10.5, 10.5}), state);
    CHECK(second.plots.size() == 4); // the 2x2 splat, at ink amount 1
    const double full = second.totalAlpha();
    CHECK(full > 0.0);

    RecordingCanvas third(64, 64);
    painter.paintSpan(third, snapAt({10.5, 10.5}), snapAt({10.5, 10.5}), state);
    CHECK(third.totalAlpha() < full); // the curve has moved on: the stroke is drying out
    CHECK(third.totalAlpha() > 0.0);
}

TEST_CASE("hairy: the bristle field jitters, and it jitters from the STROKE's stream") {
    // SEES: that the random factor reaches the transform (the same hair lands somewhere else) and
    // that the draw comes from the seeded stroke state (the same seed replays the same landing).
    // Without the second half a painter that read a clock would pass the first half every time.
    cb::HairyPainterParams params = oneHair();
    params.randomFactor = 6.0;

    // A checksum over WHERE and HOW HARD every mark of four spans landed, rather than one cell: two
    // different seeds could put one splat in the same pixel by chance, but not four spans' worth.
    const auto signature = [&](std::uint64_t seed) {
        cb::HairyPainter painter(params);
        cb::StrokePainterContext ctx = ctxFor();
        ctx.tip = nullptr;
        painter.begin(ctx);
        RecordingCanvas canvas(64, 64);
        cb::StrokeState state;
        state.begin(at(32.0, 32.0), seed);
        for (int i = 0; i < 4; ++i)
            painter.paintSpan(canvas, snapAt({32.0, 32.0}), snapAt({32.0, 32.0}), state);
        REQUIRE(!canvas.plots.empty());
        double sum = 0.0;
        for (const RecordingCanvas::Plot& p : canvas.plots)
            sum += p.x * 7919.0 + p.y * 104729.0 + p.alpha;
        return sum;
    };

    CHECK(signature(1) == signature(1));
    CHECK(signature(1) != signature(9));
}

// ================================================================================================
// The curve, particle and experiment engines.

TEST_CASE("curve: nothing is drawn until the history window is FULL") {
    // SEES: the window's gate and its size. The reference paints its curve only once the deque has
    // reached `strokeHistorySize`, so a short stroke lays only its connection lines -- and with the
    // connection off it lays NOTHING AT ALL. A painter that curved through a partial window would
    // paint on the first span; one that never filled would paint never. Both are visible here.
    cb::CurvePainterParams params;
    params.makeConnection = false;
    params.smoothing = true;
    params.strokeHistorySize = 4;
    params.lineWidth = 3;

    cb::CurvePainter painter(params);
    painter.begin(ctxFor());
    RecordingCanvas canvas(64, 64);
    cb::StrokeState state;
    state.begin(at(10.0, 10.0), /*seed=*/1);

    painter.paintSpan(canvas, snapAt({10.0, 10.0}), snapAt({20.0, 14.0}), state);
    CHECK(painter.historySize() == 1);
    CHECK(canvas.plots.empty());
    painter.paintSpan(canvas, snapAt({20.0, 14.0}), snapAt({30.0, 22.0}), state);
    painter.paintSpan(canvas, snapAt({30.0, 22.0}), snapAt({38.0, 34.0}), state);
    CHECK(canvas.plots.empty()); // three points is not a full window of four

    painter.paintSpan(canvas, snapAt({38.0, 34.0}), snapAt({42.0, 46.0}), state);
    CHECK(painter.historySize() == 4);
    CHECK_FALSE(canvas.plots.empty()); // ... and the fourth fills it

    // ⚠ The window SLIDES rather than growing: a fifth span must leave it at four.
    painter.paintSpan(canvas, snapAt({42.0, 46.0}), snapAt({40.0, 56.0}), state);
    CHECK(painter.historySize() == 4);
}

TEST_CASE("curve: the stroked path is ONE mask, not overlapping bands") {
    // ⚠ THE JOIN TEST. The reference hands its whole path to one pen, so a path that doubles back
    // over itself is one mark at one opacity. Chaining thick lines and depositing each straight
    // would double-darken every join; the painter combines them in its scratch by MAX first and
    // deposits once, so no plot may exceed the path's own opacity.
    cb::CurvePainterParams params;
    params.makeConnection = true;
    params.strokeHistorySize = 2;
    params.lineWidth = 5;
    params.curvesOpacity = 0.5;

    cb::CurvePainter painter(params);
    painter.begin(ctxFor());
    RecordingCanvas canvas(64, 64);
    cb::StrokeState state;
    state.begin(at(10.0, 30.0), /*seed=*/1);
    // A hairpin: out and straight back, so the two halves of the path share every pixel.
    painter.paintSpan(canvas, snapAt({10.0, 30.0}), snapAt({40.0, 30.0}), state);
    painter.paintSpan(canvas, snapAt({40.0, 30.0}), snapAt({10.0, 30.0}), state);
    REQUIRE(!canvas.plots.empty());
    for (const RecordingCanvas::Plot& p : canvas.plots)
        CHECK(p.alpha <= 1.0 + 1e-9);
    // ... and the curve pass really does deposit at ITS opacity, not at full.
    double lowest = 1.0;
    for (const RecordingCanvas::Plot& p : canvas.plots)
        lowest = std::min(lowest, p.alpha);
    CHECK(lowest < 0.9);
}

TEST_CASE("particle: the cloud is planted once and then CHASES the cursor") {
    // SEES: that the particles are persistent state and not geometry. Planted at the press they sit
    // on top of one another, so the first segment's marks are tight; after several segments toward a
    // moving target the cloud has spread AND moved toward it. A painter that recomputed positions
    // per segment (or reset them) would not spread at all.
    cb::ParticlePainterParams params;
    params.count = 24;
    params.iterations = 8;
    params.scaleX = 1.0;
    params.scaleY = 1.0;
    params.gravity = 0.906;
    params.weight = 0.5;

    cb::ParticlePainter painter(params);
    cb::StrokePainterContext ctx = ctxFor();
    ctx.first = at(20.0, 40.0);
    painter.begin(ctx);
    CHECK(painter.particleCount() == 24);

    cb::StrokeState state;
    state.begin(at(20.0, 40.0), /*seed=*/1);

    const auto spread = [](const RecordingCanvas& c) {
        int lo = 1 << 20;
        int hi = -(1 << 20);
        for (const RecordingCanvas::Plot& p : c.plots) {
            lo = std::min(lo, p.x);
            hi = std::max(hi, p.x);
        }
        return c.plots.empty() ? 0 : hi - lo;
    };

    RecordingCanvas first(96, 96);
    painter.paintSpan(first, snapAt({20.0, 40.0}), snapAt({30.0, 40.0}), state);
    const int firstSpread = spread(first);

    RecordingCanvas later(96, 96);
    for (int i = 0; i < 6; ++i) {
        const double x = 30.0 + 6.0 * i;
        painter.paintSpan(later, snapAt({x, 40.0}), snapAt({x + 6.0, 40.0}), state);
    }
    CHECK(!later.plots.empty());
    CHECK(spread(later) > firstSpread); // the cloud opened up as it was dragged

    // ⚠ THIS ENGINE DRAWS NOTHING RANDOM. It is a simulation with a deterministic acceleration
    // ramp, so the same path under a different SEED must be pixel-identical -- which is what says
    // the spread above came from the simulation and not from a jitter.
    const auto run = [&](std::uint64_t seed) {
        cb::ParticlePainter p2(params);
        p2.begin(ctx);
        RecordingCanvas c(96, 96);
        cb::StrokeState st;
        st.begin(at(20.0, 40.0), seed);
        p2.paintSpan(c, snapAt({20.0, 40.0}), snapAt({30.0, 40.0}), st);
        double sum = 0.0;
        for (const RecordingCanvas::Plot& p : c.plots)
            sum += p.x * 31.0 + p.y * 131.0 + p.alpha;
        return sum;
    };
    CHECK(run(1) == run(999));
}

TEST_CASE("experiment: nothing until release, then the WHOLE stroke as one polygon") {
    // ⚠ THE `finish()` SEAM, and the one engine whose output is not a function of any prefix of the
    // stroke. SEES: that paintSpan lays nothing at all, that finish fills, and that the two fill
    // RULES disagree -- which they must, because the stroke is a self-crossing scribble and the
    // rules differ at exactly its crossings.
    // A StrokeCanvas is deliberately non-copyable (it is the engine's write seam), so the canvas is
    // filled in place rather than returned by value.
    const auto fill = [](RecordingCanvas& canvas, bool winding) {
        cb::ExperimentPainterParams params;
        params.windingFill = winding;
        params.hardEdge = true; // a crisp test: every plot is 1.0, so counts are areas
        cb::ExperimentPainter painter(params);
        painter.begin(ctxFor());
        cb::StrokeState state;
        state.begin(at(10.0, 10.0), /*seed=*/1);
        // A five-pointed star traced as one path: its centre is wound twice, so the winding rule
        // fills it and the alternate rule punches it out.
        const std::vector<Vec2> star{{40.0, 8.0},  {52.0, 62.0}, {8.0, 26.0},
                                     {72.0, 26.0}, {28.0, 62.0}, {40.0, 8.0}};
        for (std::size_t i = 0; i + 1 < star.size(); ++i)
            painter.paintSpan(canvas, snapAt(star[i]), snapAt(star[i + 1]), state);
        CHECK(canvas.plots.empty()); // NOTHING is laid while the pointer moves
        painter.finish(canvas);
    };

    RecordingCanvas wound(80, 80);
    RecordingCanvas alternate(80, 80);
    fill(wound, true);
    fill(alternate, false);
    CHECK(!wound.plots.empty());
    CHECK(!alternate.plots.empty());
    // The winding rule fills the pentagon at the star's centre; the alternate rule does not.
    CHECK(wound.alphaAt(40, 36) > 0.0);
    CHECK(alternate.alphaAt(40, 36) == 0.0);
    CHECK(wound.plots.size() > alternate.plots.size());
    // A point outside the star is in neither.
    CHECK(wound.alphaAt(4, 4) == 0.0);
}

TEST_CASE("experiment: a hard edge is 1-bit, an antialiased one is not") {
    // SEES: that `hardEdge` really inverts the antialiasing (the reference's own
    // `setAntiAliasPolygonFill(!m_hardEdge)`) -- a partial-coverage plot exists in one and cannot
    // exist in the other.
    const auto run = [](bool hardEdge) {
        cb::ExperimentPainterParams params;
        params.windingFill = true;
        params.hardEdge = hardEdge;
        cb::ExperimentPainter painter(params);
        painter.begin(ctxFor());
        RecordingCanvas canvas(64, 64);
        cb::StrokeState state;
        state.begin(at(0.0, 0.0), /*seed=*/1);
        const std::vector<Vec2> tri{{10.3, 10.7}, {50.6, 20.2}, {20.1, 50.9}, {10.3, 10.7}};
        for (std::size_t i = 0; i + 1 < tri.size(); ++i)
            painter.paintSpan(canvas, snapAt(tri[i]), snapAt(tri[i + 1]), state);
        painter.finish(canvas);
        int partial = 0;
        for (const RecordingCanvas::Plot& p : canvas.plots)
            if (p.alpha > 0.0 && p.alpha < 1.0)
                ++partial;
        return partial;
    };
    CHECK(run(true) == 0);
    CHECK(run(false) > 0);
}

// ================================================================================================
// The hatching lattice (a DAB engine, not a painter).

TEST_CASE("hatching: separation steps by whole powers of two around the middle bucket") {
    // SEES: the exact factors and the pass-through. Hand-computed from the transcribed rule: with
    // 2 intervals the base factor is 2/2 - 1 = 0, so the first bucket is x1 and the second x1/2.
    CHECK(cb::hatchSeparationForParameter(0.25, 8.0, 2) == doctest::Approx(8.0));
    CHECK(cb::hatchSeparationForParameter(0.75, 8.0, 2) == doctest::Approx(4.0));
    // With 3 intervals the base factor is 1: x2, x1, x1/2.
    CHECK(cb::hatchSeparationForParameter(0.1, 8.0, 3) == doctest::Approx(16.0));
    CHECK(cb::hatchSeparationForParameter(0.5, 8.0, 3) == doctest::Approx(8.0));
    CHECK(cb::hatchSeparationForParameter(0.9, 8.0, 3) == doctest::Approx(4.0));
    // ⚠ Outside 2..7 the reference complains and returns the separation UNCHANGED.
    CHECK(cb::hatchSeparationForParameter(0.5, 8.0, 1) == doctest::Approx(8.0));
    CHECK(cb::hatchSeparationForParameter(0.5, 8.0, 9) == doctest::Approx(8.0));
}

TEST_CASE("hatching: spinAngle folds into (-90, 90] keeping the UNFOLDED sign") {
    // SEES: the sign rule, which is the part that is easy to get wrong -- the sign comes from the
    // sum BEFORE folding, so a small negative sum stays negative even where the fold is a no-op.
    CHECK(cb::hatchSpinAngle(45.0, 0.0) == doctest::Approx(45.0));
    CHECK(cb::hatchSpinAngle(45.0, 90.0) == doctest::Approx(-45.0)); // 135 folds to -(180-135)
    CHECK(cb::hatchSpinAngle(-45.0, 0.0) == doctest::Approx(-45.0));
    CHECK(cb::hatchSpinAngle(0.0, 180.0) == doctest::Approx(0.0));
    // ⚠ On the FOLDED branch the sign factor still applies, so it meets the fold's own negation and
    // the two cancel: a sum of -105 lands at +75, not -75. The function is ODD -- f(-x) == -f(x) --
    // and that is the cleanest way to state the rule, so pin it as the pair.
    CHECK(cb::hatchSpinAngle(60.0, 45.0) == doctest::Approx(-(180.0 - 105.0)));
    CHECK(cb::hatchSpinAngle(-60.0, -45.0) == doctest::Approx(180.0 - 105.0));
}

TEST_CASE("hatching: the lattice is PHASE-LOCKED to the document, not to the dab") {
    // ⚠ THE ONE PROPERTY THE WHOLE ENGINE RESTS ON, and the one a naive implementation loses: two
    // overlapping dabs must continue each other's lines. SEES it directly -- the stencil of a dab at
    // (0,0) and the stencil of a dab at (0, separation) must agree on their overlap, because the
    // second dab is exactly one lattice period down.
    cb::HatchingParams p;
    p.enabled = true;
    p.angle = 0.0; // horizontal lines: the lattice period is exactly `separation` in y
    p.separation = 8.0;
    p.thickness = 1.0;
    p.originX = 0.0;
    p.originY = 0.0;
    p.subpixelPrecision = true;
    p.style = cb::CrosshatchingStyle::None;

    cb::HatchingDabValues v;
    std::vector<std::uint8_t> a;
    std::vector<std::uint8_t> b;
    constexpr int kW = 24;
    constexpr int kH = 24;
    cb::hatchStencil(p, v, 0.0, 0.0, kW, kH, a);
    cb::hatchStencil(p, v, 0.0, 8.0, kW, kH, b);
    REQUIRE(a.size() == static_cast<std::size_t>(kW) * kH);
    REQUIRE(b.size() == a.size());

    // Something was drawn at all -- a lattice that produced an empty stencil would pass every
    // "they agree" test vacuously, which is exactly the trap this arc keeps re-learning.
    int inked = 0;
    for (const std::uint8_t s : a)
        if (s != 0)
            ++inked;
    CHECK(inked > 0);
    CHECK(inked < kW * kH); // ... and it is a lattice, not a flood

    // Shifted by one period, the two stencils are the same pattern.
    CHECK(a == b);

    // ⚠ And shifted by HALF a period they are NOT -- otherwise "phase-locked" would be untestable,
    // because a stencil that ignored the dab position entirely would also pass the line above.
    std::vector<std::uint8_t> half;
    cb::hatchStencil(p, v, 0.0, 4.0, kW, kH, half);
    CHECK(a != half);
}

TEST_CASE("hatching: the moiré style lays ONE sensor-driven pass, not a fixed cross") {
    // SEES: the pass-selection branch that the shipped `y)_Screentone_Moire` preset actually takes.
    // With Crosshatching CHECKED and the moiré style, the reference lays exactly one hatch at
    // `spinAngle(value * 360)` -- and skips its own base pass, which is what makes the pattern beat
    // against itself as the sensor moves. Two different sensor values must give different stencils;
    // a mutant that fell through to the base pass would give the same one twice.
    cb::HatchingParams p;
    p.enabled = true;
    p.angle = 45.0;
    p.separation = 18.0;
    p.thickness = 9.0;
    p.originX = 0.0;
    p.originY = 0.0;
    p.subpixelPrecision = true;
    p.style = cb::CrosshatchingStyle::Moire;

    // ⚠ THE SENSOR VALUES CANNOT DIFFER BY A MULTIPLE OF 0.5. The pass angle is
    // `spinAngle(value * 360)` and the fold's period is 180 deg -- which is exactly 0.5 in sensor
    // units -- so 0.1 and 0.6 both land on +81 deg and the stencils are IDENTICAL for a perfectly
    // correct engine. That pairing made this case assert a falsehood, not a property.
    // 0.1 -> 36 -> +81; 0.35 -> 126 -> -9.
    cb::HatchingDabValues lo;
    lo.crosshatchingChecked = true;
    lo.crosshatching = 0.1;
    cb::HatchingDabValues hi = lo;
    hi.crosshatching = 0.35;

    std::vector<std::uint8_t> a;
    std::vector<std::uint8_t> b;
    cb::hatchStencil(p, lo, 0.0, 0.0, 40, 40, a);
    cb::hatchStencil(p, hi, 0.0, 0.0, 40, 40, b);
    int inkedA = 0;
    for (const std::uint8_t s : a)
        if (s != 0)
            ++inkedA;
    CHECK(inkedA > 0);
    CHECK(a != b);
}

TEST_CASE("hatching: the stencil CLIPS the tip, and an off preset changes nothing") {
    // SEES: the engine integration, both ways. On, a hatched stroke must paint strictly LESS than
    // the same stroke unhatched (the lattice can only take coverage away, never add it) and must
    // still paint something. Off, the stroke must be byte-identical to one laid by a preset that
    // has no hatching block at all -- the frozen gate keeps the dab loop as it was.
    const auto stroke = [](bool hatching) {
        cb::MaskGeneratorParams gen;
        gen.shape = cb::MaskShape::Circle;
        gen.diameter = 40.0;
        gen.hFade = 1.0;
        gen.vFade = 1.0;
        cb::BrushParams p;
        p.diameter = 40.0;
        p.tip = cb::makeTip(gen);
        p.color = Color8{0, 0, 0, 255};
        p.spacing = 0.2;
        if (hatching) {
            p.hatching.enabled = true;
            p.hatching.angle = 30.0;
            p.hatching.separation = 7.0;
            p.hatching.thickness = 2.0;
            p.hatching.subpixelPrecision = true;
            p.hatching.style = cb::CrosshatchingStyle::None;
        }
        Image img = greyCanvas(96, 96);
        cb::BrushEngine eng;
        eng.begin(96, 96, img, p, cb::BrushDynamics{}, at(20.0, 48.0));
        eng.extendTo(at(48.0, 40.0));
        eng.extendTo(at(76.0, 52.0));
        eng.end();
        eng.composite();
        long ink = 0;
        for (std::size_t i = 0; i + 3 < img.rgba.size(); i += 4)
            ink += 255 - img.rgba[i]; // black on grey: how dark it got
        return std::pair<long, std::vector<std::uint8_t>>{ink, img.rgba};
    };

    const auto plain = stroke(false);
    const auto hatched = stroke(true);
    CHECK(hatched.first > 0);              // it painted
    CHECK(hatched.first < plain.first);    // ... and the lattice took coverage away
    CHECK(hatched.second != plain.second);
}

// ================================================================================================
// The engine integration.

TEST_CASE("engine: the painter data changes NOTHING for a dab stroke") {
    // ⚠ THE PRIMARY SAFETY CHECK OF THE WHOLE CHUNK, and it is deliberately an equality between two
    // LIVE strokes rather than a blessed constant -- a constant could only be produced by running
    // the very code it is meant to police. A dab preset that also carries a fully populated painter
    // spec (kind None) and all three of the new sketch OPTIONS must lay the identical image: the
    // dab walk never reads any of them, and evaluating one would shift every later draw from the
    // stroke's random stream.
    const auto strokeImage = [](bool carryPainterData) {
        cb::BrushParams p;
        p.diameter = 12.0;
        p.spacing = 0.1;
        p.color = Color8{20, 40, 200, 255};
        auto options = std::make_shared<cb::BrushOptions>();
        cb::CurveOptionData size;
        size.name = "Size";
        size.checked = true;
        size.sensors.sensors = {cb::Sensor::withDefaults(cb::SensorId::Fuzzy)};
        options->size.emplace(size);
        if (carryPainterData) {
            // The three new bases, CHECKED and driven by the same random sensor -- so a dab walk
            // that evaluated any of them would consume draws and move every dab after the first.
            for (const char* base : {"Density", "Line width", "Offset scale"}) {
                cb::CurveOptionData d;
                d.name = base;
                d.checked = true;
                d.sensors.sensors = {cb::Sensor::withDefaults(cb::SensorId::Fuzzy)};
                if (std::string_view(base) == "Density")
                    options->density.emplace(d);
                else if (std::string_view(base) == "Line width")
                    options->lineWidth.emplace(d);
                else
                    options->offsetScale.emplace(d);
            }
            p.painter.sketch.probability = 0.25; // populated, but kind stays None
            p.painter.hairy.randomFactor = 3.0;
        }
        p.options = options;
        p.seed = 4242;

        Image img = greyCanvas(64, 64);
        cb::BrushEngine eng;
        eng.begin(64, 64, img, p, cb::BrushDynamics{}, at(8.0, 8.0));
        eng.extendTo(at(30.0, 20.0));
        eng.extendTo(at(52.0, 44.0));
        eng.end();
        eng.composite();
        return img.rgba;
    };

    CHECK(strokeImage(false) == strokeImage(true));
}

TEST_CASE("engine: a painter stroke rides the real accumulation -- coverage, restore, cadence") {
    // SEES: that the painter went THROUGH deposit() rather than around it. The coverage buffer is
    // the Inpaint brush's hole mask and the base snapshot is what restore() answers from, so a
    // painter that wrote the target directly would leave the coverage empty and restore() would
    // leave the stroke on the canvas. The cadence half sees a painter that read the live target:
    // compositing after every sample would then differ from compositing once at the end.
    cb::BrushParams p;
    p.diameter = 30.0;
    p.color = Color8{10, 10, 10, 255};
    p.painter = sketchParams();
    p.seed = 99;

    Image incremental = greyCanvas(80, 80);
    const std::vector<std::uint8_t> pristine = incremental.rgba;
    cb::BrushEngine eng;
    eng.begin(80, 80, incremental, p, cb::BrushDynamics{}, at(12.0, 12.0));
    eng.composite();
    eng.extendTo(at(40.0, 30.0));
    eng.composite();
    eng.extendTo(at(20.0, 55.0));
    eng.composite();
    eng.extendTo(at(60.0, 60.0));
    eng.end();
    eng.composite();

    CHECK(incremental.rgba != pristine);            // it painted
    CHECK(!eng.coverage().empty());                 // ... into the coverage buffer
    CHECK(eng.dirtyBounds().w > 0.0);
    double maxCoverage = 0.0;
    for (const float c : eng.coverage())
        maxCoverage = std::max(maxCoverage, static_cast<double>(c));
    CHECK(maxCoverage > 0.0);

    // ... and every pixel it touched came from the bounded base snapshot, so it all goes back.
    eng.restore();
    CHECK(incremental.rgba == pristine);

    Image once = greyCanvas(80, 80);
    cb::BrushEngine eng2;
    eng2.begin(80, 80, once, p, cb::BrushDynamics{}, at(12.0, 12.0));
    eng2.extendTo(at(40.0, 30.0));
    eng2.extendTo(at(20.0, 55.0));
    eng2.extendTo(at(60.0, 60.0));
    eng2.end();
    eng2.composite();

    Image again = greyCanvas(80, 80);
    cb::BrushEngine eng3;
    eng3.begin(80, 80, again, p, cb::BrushDynamics{}, at(12.0, 12.0));
    eng3.composite();
    eng3.extendTo(at(40.0, 30.0));
    eng3.composite();
    eng3.extendTo(at(20.0, 55.0));
    eng3.composite();
    eng3.extendTo(at(60.0, 60.0));
    eng3.end();
    eng3.composite();
    CHECK(once.rgba == again.rgba); // composite cadence is not part of the mark
}

TEST_CASE("engine: a painter stroke replays, and its seed is what makes it replay") {
    // SEES: determinism (the undo/golden contract) AND that randomness is genuinely in play -- a
    // painter that drew nothing random would pass the first check and fail the second, which is
    // the failure mode a determinism test alone cannot see.
    const auto run = [](std::uint64_t seed) {
        cb::BrushParams p;
        p.diameter = 30.0;
        p.color = Color8{0, 0, 0, 255};
        p.painter = sketchParams();
        p.painter.sketch.randomOpacity = true; // a draw per connection
        p.seed = seed;
        Image img = greyCanvas(80, 80);
        cb::BrushEngine eng;
        eng.begin(80, 80, img, p, cb::BrushDynamics{}, at(15.0, 15.0));
        eng.extendTo(at(45.0, 30.0));
        eng.extendTo(at(25.0, 55.0));
        eng.end();
        eng.composite();
        return img.rgba;
    };
    CHECK(run(7) == run(7));
    CHECK(run(7) != run(8));
}

TEST_CASE("engine: a painter stroke erases, and it never touches the dab walk's state") {
    // SEES: that StrokeMode::Erase reaches a painter's marks (they go through the same deposit and
    // the same composite as a dab's), and that a painter stroke leaves the smudge engine alone --
    // the two are mutually exclusive and begin() resolves that in the smudge engine's favour.
    cb::BrushParams p;
    p.diameter = 30.0;
    p.painter = sketchParams();
    p.strokeMode = cb::StrokeMode::Erase;
    p.seed = 5;

    Image img = greyCanvas(80, 80);
    long before = 0;
    for (std::size_t i = 3; i < img.rgba.size(); i += 4)
        before += img.rgba[i];

    cb::BrushEngine eng;
    eng.begin(80, 80, img, p, cb::BrushDynamics{}, at(15.0, 15.0));
    eng.extendTo(at(45.0, 30.0));
    eng.extendTo(at(25.0, 55.0));
    eng.end();
    eng.composite();

    long after = 0;
    for (std::size_t i = 3; i < img.rgba.size(); i += 4)
        after += img.rgba[i];
    CHECK(after < before); // an eraser CARVES; the painter's coverage is what it carves with
}
