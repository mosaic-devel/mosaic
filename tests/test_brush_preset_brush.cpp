#include <doctest/doctest.h>

#include "common/image.hpp"
#include "core/brush/brush_tip.hpp"
#include "io/brush/preset_brush.hpp"

#include <algorithm>
#include <string>
#include <vector>

// PRESET -> BRUSH (docs/brushes.md §8): the last translation layer, and the first time a shipped
// preset's tip, spacing and option pipeline reach a stroke. The corpus case at the bottom paints all
// 117 of them.
namespace cb = mosaic::core::brush;

using mosaic::common::Image;
using mosaic::io::brush::LibraryPreset;
using mosaic::io::brush::presetBrushParams;
using mosaic::io::brush::PresetLibrary;
using mosaic::io::brush::TipXml;

namespace {

const PresetLibrary& shipped() {
    static const PresetLibrary lib = [] {
        PresetLibrary l;
        std::string error;
        const int n = l.addBundleFile(
            std::string(MOSAIC_SHIPPED_DATA_DIR) + "/brushes/Krita_4_Default_Resources.bundle",
            &error);
        REQUIRE_MESSAGE(n == 117, error);
        return l;
    }();
    return lib;
}

[[nodiscard]] const LibraryPreset* byName(std::string_view name) {
    for (const LibraryPreset& p : shipped().presets())
        if (p.preset.name == name)
            return &p;
    return nullptr;
}

// One option of a synthetic preset: `base`, at `strength`, with a live pressure sensor.
[[nodiscard]] cb::CurveOptionData opt(const char* base, double strength, bool checked = true) {
    cb::CurveOptionData d;
    d.name = base;
    d.checked = checked;
    d.checkable = !(std::string_view(base) == "Opacity" || std::string_view(base) == "Flow");
    d.strength = strength;
    d.sensors.sensors = {cb::Sensor::withDefaults(cb::SensorId::Pressure)};
    return d;
}

// A minimal auto-tip preset: a 20 px hard round tip, nothing driven.
[[nodiscard]] LibraryPreset plainPreset() {
    LibraryPreset lp;
    lp.preset.name = "synthetic";
    lp.preset.tip.kind = TipXml::Kind::Auto;
    lp.preset.tip.spacing = 0.1;
    lp.preset.tip.autoTip.generator.diameter = 20.0;
    lp.preset.tip.autoTip.generator.hFade = 1.0;
    lp.preset.tip.autoTip.generator.vFade = 1.0;
    lp.masterDiameter = 20.0;
    return lp;
}

[[nodiscard]] cb::StrokeInput at(double x, double y, double pressure = 1.0) {
    cb::StrokeInput in;
    in.pos = {x, y};
    in.pressure = pressure;
    return in;
}

// Paint one dab of `params` and hand back the alpha under its centre.
[[nodiscard]] int centreAlpha(cb::BrushParams p) {
    Image img(64, 64);
    p.color = mosaic::common::Color8{0, 0, 0, 255};
    cb::BrushEngine eng;
    eng.begin(64, 64, img, p, cb::BrushDynamics{}, at(32.0, 32.0));
    eng.end();
    eng.composite();
    return img.rgba[(32 * 64 + 32) * 4 + 3];
}

} // namespace

TEST_CASE("preset->brush: DRIVEN and HONOURED are the same set, base for base") {
    // ⚠ THE BICONDITIONAL, AND IT IS THE WHOLE POINT OF THE FILE.
    //
    //     drivenOption(base) != null   <==>   this mapping gives `base` a slot
    //
    // Left without right is the lie that cost 26 presets their honesty for two days: the importer's
    // `optionSupported()` (io/brush/mapper.cpp) reads kDrivenOptions to decide whether a preset
    // imports as EXACT, and a base it calls driven but this mapping quietly ignores is a preset
    // badged as faithful and painted as something else. Right without left is the same lie mirrored:
    // an option that paints while the badge says it was dropped.
    //
    // Neither side can be checked alone -- the census cannot see a missing SLOT (those presets are
    // Approximated for other reasons anyway, so the spread never moves), and this file cannot see a
    // missing LIST entry. Only the two, compared.
    for (const char* base :
         {"Opacity", "Flow", "Size", "Ratio", "Softness", "Rotation", "Sharpness",
          "LightnessStrength", "Scatter", "Darken", "Mix", "Rate", "Mirror", "Spacing", "h", "s", "v",
          "Texture/Strength/", "Density", "Line width", "Offset scale", "Curves opacity", "Angle",
          "Crosshatching", "Separation", "Thickness"}) {
        CAPTURE(base);
        LibraryPreset lp = plainPreset();
        lp.preset.options.push_back(opt(base, 1.0)); // a preset carrying this base ALONE
        const cb::BrushParams p = presetBrushParams(lp);

        const bool driven = cb::drivenOption(base) != nullptr;   // what the IMPORTER promises
        const bool honoured = p.options != nullptr;              // what a DAB actually gets
        CHECK(driven == honoured);
    }

    // And all of them, together, land in distinct slots -- not N-1 options and one overwritten.
    LibraryPreset all = plainPreset();
    for (const cb::BrushOptionSpec& spec : cb::kDrivenOptions)
        all.preset.options.push_back(opt(std::string(spec.base).c_str(), 1.0));
    const cb::BrushParams p = presetBrushParams(all);
    REQUIRE(p.options != nullptr);
    CHECK(p.options->size.has_value());
    CHECK(p.options->flow.has_value());
    CHECK(p.options->ratio.has_value());
    CHECK(p.options->rotation.has_value());
    CHECK(p.options->softness.has_value());
    CHECK(p.options->opacity.has_value());
    CHECK(p.options->scatter.has_value());
    CHECK(p.options->mirror.has_value());
    CHECK(p.options->spacing.has_value());
    CHECK(p.options->sharpness.has_value());
    CHECK(p.options->density.has_value());
    CHECK(p.options->lineWidth.has_value());
    CHECK(p.options->offsetScale.has_value());
    CHECK(p.options->curvesOpacity.has_value());
    CHECK(p.options->hatchAngle.has_value());
    CHECK(p.options->crosshatching.has_value());
    CHECK(p.options->separation.has_value());
    CHECK(p.options->thickness.has_value());
    // §6.6h's two: the texture strength and the airbrush rate. ⚠ `Texture/Strength/` is the one
    // base whose NAME contains the family separator -- slotFor matches the whole string, so a
    // prefix-matching mapping would put it in the wrong slot and this is where that shows.
    CHECK(p.options->textureStrength.has_value());
    CHECK(p.options->rate.has_value());
}

TEST_CASE("preset->brush: the ENGINE KIND is one list too, read by the importer and the mapping") {
    // ⚠ THE SECOND BICONDITIONAL (docs/brushes.md §6.6g), and it exists for the reason the option
    // one does:
    //
    //     painterKindForPaintop(op) != None   <==>   this mapping gives the stroke a painter
    //
    // Left without right is a preset the importer promised a real engine (it starts EXACT, with no
    // "Paintop '...' substituted" note) and that then paints as a plain pixel brush -- the fidelity
    // badge lying in exactly the direction §6.4 exists to prevent. Right without left is the mirror:
    // a preset painted by an engine the badge says it does not have.
    //
    // The corpus half of this lives in test_brush_library_census.cpp; this half can see a paintop
    // that the LIST names and the MAPPING forgets, which the corpus cannot (it would still paint,
    // just with the wrong engine, and every count would hold).
    for (const char* op : {"paintbrush", "eraser", "roundmarker", "colorsmudge", "smudge",
                           "spraybrush", "filter", "deformbrush", "duplicate", "gridbrush",
                           "hatchingbrush", "particlebrush", "curvebrush", "experimentbrush",
                           "tangentnormal", "sketchbrush", "hairybrush"}) {
        CAPTURE(op);
        LibraryPreset lp = plainPreset();
        lp.preset.painter.kind = cb::painterKindForPaintop(op); // what the MAPPER writes
        const cb::BrushParams p = presetBrushParams(lp);

        const bool listed = cb::painterKindForPaintop(op) != cb::StrokePainterKind::None;
        const bool painted = p.painter.kind != cb::StrokePainterKind::None;
        CHECK(listed == painted);
        CHECK(p.painter.kind == cb::painterKindForPaintop(op)); // and it is the SAME kind
    }
}

TEST_CASE("preset->brush: an option the preset never mentioned stays ABSENT") {
    // Not disabled, not defaulted: absent. A preset that drives nothing gets a NULL option table, and
    // the engine skips the pipeline entirely -- the path every golden in the suite was laid on.
    const cb::BrushParams none = presetBrushParams(plainPreset());
    CHECK(none.options == nullptr);

    LibraryPreset lp = plainPreset();
    // ⚠ This needs a base NO dab reads. `Rate` stood here until the airbrush landed (§6.6h) and
    // joined kDrivenOptions, at which point the case was asserting the opposite of its own name.
    // `LightnessStrength` is the last base still outside the list.
    lp.preset.options.push_back(opt("LightnessStrength", 1.0));
    CHECK(presetBrushParams(lp).options == nullptr);

    lp.preset.options.push_back(opt("Size", 1.0));
    const cb::BrushParams some = presetBrushParams(lp);
    REQUIRE(some.options != nullptr);
    CHECK(some.options->size.has_value());
    CHECK_FALSE(some.options->rotation.has_value()); // the five it never mentioned
    CHECK_FALSE(some.options->ratio.has_value());
    CHECK_FALSE(some.options->flow.has_value());
    CHECK_FALSE(some.options->softness.has_value());
    CHECK_FALSE(some.options->opacity.has_value());
}

TEST_CASE("preset->brush: the base flow stays at 1 -- the Flow option carries FlowValue, not both") {
    // ⚠ THE SQUARING TRAP. `FlowValue` is the always-on Flow option's own STRENGTH, and the dab
    // pipeline multiplies the base by it (`base.flow * sizeLikeValue`). Seed the base with the same
    // number as well and a preset authored at half flow paints at a QUARTER of it.
    LibraryPreset lp = plainPreset();
    lp.preset.options.push_back(opt("Flow", 0.5));

    const cb::BrushParams p = presetBrushParams(lp);
    CHECK(p.flow == 1.0);
    REQUIRE(p.options != nullptr);
    REQUIRE(p.options->flow.has_value());
    CHECK(p.options->flow->data().strength == 0.5);

    // And it lands as half a dab's worth of paint, not a quarter of one.
    CHECK(centreAlpha(p) == 128); // 0.5 -> 127.5, rounded up by the composite
    LibraryPreset full = plainPreset();
    full.preset.options.push_back(opt("Flow", 1.0));
    CHECK(centreAlpha(presetBrushParams(full)) == 255);
}

TEST_CASE("preset->brush: opacity is the Opacity option's static strength") {
    LibraryPreset lp = plainPreset();
    lp.preset.options.push_back(opt("Opacity", 0.4));
    const cb::BrushParams p = presetBrushParams(lp);
    CHECK(p.opacity == doctest::Approx(0.4));
    CHECK(centreAlpha(p) == 102); // 0.4 * 255
    // A preset that says nothing about opacity paints at full strength.
    CHECK(presetBrushParams(plainPreset()).opacity == 1.0);
}

TEST_CASE("preset->brush: a procedural tip keeps its ratio; a bitmap tip's is 1") {
    LibraryPreset lp = plainPreset();
    lp.preset.tip.autoTip.generator.ratio = 0.4;
    lp.preset.tip.angle = 0.75;
    const cb::BrushParams p = presetBrushParams(lp);
    CHECK(p.ratio == doctest::Approx(0.4));
    CHECK(p.angleRad == doctest::Approx(0.75)); // the file's `angle` is RADIANS
    REQUIRE(p.tip != nullptr);
    CHECK(p.tip->isProcedural());
    CHECK(p.tip->generator()->hFade == 1.0); // the authored falloff, carried whole

    // A bitmap tip's own aspect lives INSIDE the dab's envelope (bitmapDabShape), so the params'
    // ratio must stay 1 or the frame is squashed twice. `z)_Stamp_Leaves` is 1000x1000 anyway; the
    // hoses below are not square, which is what makes the double-squash visible at all.
    const LibraryPreset* stamp = byName("z)_Stamp_Leaves");
    REQUIRE(stamp != nullptr);
    const cb::BrushParams bmp = presetBrushParams(*stamp);
    REQUIRE(bmp.tip != nullptr);
    CHECK_FALSE(bmp.tip->isProcedural());
    CHECK(bmp.tip->bitmap() != nullptr);
    CHECK(bmp.ratio == 1.0);
    CHECK(bmp.diameter == doctest::Approx(stamp->masterDiameter));
}

TEST_CASE("preset->brush: the whole shipped set lays a stroke") {
    // The corpus, end to end: bundle -> kpp -> mapper -> library -> params -> a stroke on a canvas.
    // Every one of the 117 must CHANGE the canvas. A tip that decoded to nothing, a diameter that
    // resolved to zero, a spacing of 0 that hangs the walk, a NaN out of an option -- each ends here.
    //
    // The canvas starts OPAQUE, and the test measures what MOVED rather than the ink laid: three of
    // the 117 are erasers (`CompositeOp` "erase"), and an eraser over an EMPTY canvas correctly lays
    // nothing at all. Measuring the change catches paint and carving alike.
    //
    // ⚠ And the canvas is MID-GREY under a SATURATED colour, neither of which is an accident. Four of
    // the presets paint through a blend mode (`l)_Adjust_*`: Lighten, ColorDodge, Color, Overlay), and
    // every one of those is the IDENTITY for black over white -- they would have read as five presets
    // laying no ink at all, when in truth they were laying exactly what they should.
    const mosaic::common::Color8 base{128, 128, 128, 255};
    const mosaic::common::Color8 ink{220, 40, 40, 255};
    int painted = 0;
    int erased = 0;
    int shaped = 0;   // a tip that is not a plain circle: an authored ratio or angle
    int optioned = 0; // a preset the option pipeline actually drives
    for (const LibraryPreset& lp : shipped().presets()) {
        CAPTURE(lp.preset.name);
        cb::BrushParams p = presetBrushParams(lp);
        REQUIRE(p.tip != nullptr);
        CHECK(p.diameter > 0.0);
        CHECK(p.spacing > 0.0);
        if (p.ratio != 1.0 || p.angleRad != 0.0)
            ++shaped;
        if (p.options != nullptr)
            ++optioned;

        // At the preset's own size a 1000 px tip would rasterize a megapixel mask per dab, and the
        // tip's SHAPE is what this case is about -- it is scale-free. (The absolute sizes are pinned
        // by the library census, which derives them from the raw tip headers.)
        p.diameter = 24.0;
        p.color = ink;

        Image img(96, 96);
        for (std::size_t i = 0; i + 3 < img.rgba.size(); i += 4) {
            img.rgba[i] = base.r;
            img.rgba[i + 1] = base.g;
            img.rgba[i + 2] = base.b;
            img.rgba[i + 3] = base.a;
        }
        // A smudge preset can only move paint around, and on a UNIFORM canvas every patch equals
        // every other -- the six pure blenders (colour rate 0) would honestly change nothing. So
        // the smudge presets get an ink patch under the stroke's first half to drag across the
        // grey, which is the very thing the engine exists to show.
        if (p.smudge.enabled) {
            for (int y = 0; y < 96; ++y) {
                for (int x = 0; x < 48; ++x) {
                    const std::size_t q = (static_cast<std::size_t>(y) * 96 + x) * 4;
                    img.rgba[q] = ink.r;
                    img.rgba[q + 1] = ink.g;
                    img.rgba[q + 2] = ink.b;
                    img.rgba[q + 3] = ink.a;
                }
            }
        }
        // With the patch on the canvas "did it move" must compare against the PRE-STROKE image;
        // for everything else that image is the uniform base and the comparison is the same one
        // this case always made.
        const std::vector<std::uint8_t> before = img.rgba;
        // A CURVE, and a pressure ramp that visits BOTH ends of the range. A straight line at a
        // constant pressure exercises neither -- and the ramp has to reach LOW pressure, not just
        // leave it: `z)_Stamp_Shoujo_Bubbles` carries an INVERTED Size curve (`0,1;1,0;` -- press
        // harder, paint smaller), so at full pressure it resolves to a zero-diameter dab and lays
        // nothing at all. That is a faithful reading of the file, not a bug, and a corpus test that
        // only ever pressed hard would have called a working preset broken.
        cb::BrushEngine eng;
        eng.begin(96, 96, img, p, cb::BrushDynamics{}, at(16.0, 48.0, 0.15));
        eng.extendTo(at(48.0, 36.0, 0.6));
        eng.extendTo(at(80.0, 56.0, 1.0));
        eng.end();
        eng.composite();

        long moved = 0;
        long alpha = 0;
        for (std::size_t i = 0; i + 3 < img.rgba.size(); i += 4) {
            if (img.rgba[i] != before[i] || img.rgba[i + 1] != before[i + 1] ||
                img.rgba[i + 2] != before[i + 2] || img.rgba[i + 3] != before[i + 3])
                ++moved;
            alpha += img.rgba[i + 3];
        }
        CHECK_MESSAGE(moved > 0, "the preset left the canvas exactly as it found it");
        if (moved > 0)
            ++painted;

        // An eraser preset carves: it is the one family whose stroke takes alpha AWAY. That the
        // `CompositeOp` "erase" survives all the way to a StrokeMode is otherwise untested here.
        if (lp.preset.eraserMode) {
            CHECK(alpha < 96L * 96L * 255L);
            ++erased;
        }
    }
    CHECK(painted == 117);
    CHECK(erased == 3);    // the three a)_Eraser_* presets
    CHECK(optioned == 117); // every shipped preset drives at least Flow

    // ⚠ 22 OF THE 117 HAVE A TIP THAT IS NOT A CIRCLE, and until the tip landed in the engine every
    // one of them painted as one. Eleven are genuinely ELLIPTICAL -- the two knives at ratio 0.2, the
    // Watercolor Fringe at 0.17, three charcoals at 0.8, the Pen Rough and the Dry Marker at 0.5 --
    // and eleven more carry an authored ANGLE: the chisel markers and flat bristles at a quarter
    // turn, the Tilted Pencil at a half. (Two of those angles are a FULL turn, which is the identity;
    // they are counted because the file authored them, not because they bend a dab.)
    //
    // Do not confuse this with §3.10's "Ratio: 2 presets". That is the Ratio OPTION -- a ratio driven
    // by a sensor, per dab. This is the ratio the tip was drawn at.
    CHECK(shaped == 22);
}


TEST_CASE("preset->brush: Scatter and Mirror carry their axis gates into the wrapped slots") {
    // The gates ride BESIDE the curve option (BrushPreset's four bools), so a preset authored
    // with a one-axis scatter must reach the dab pipeline as one -- non-default values on every
    // gate, so a swapped or dropped field is visible.
    LibraryPreset lp = plainPreset();
    lp.preset.options.push_back(opt("Scatter", 2.0));
    lp.preset.options.push_back(opt("Mirror", 1.0));
    lp.preset.scatterAxisX = false;
    lp.preset.scatterAxisY = true;
    lp.preset.mirrorHorizontal = true;
    lp.preset.mirrorVertical = false;

    const cb::BrushParams p = presetBrushParams(lp);
    REQUIRE(p.options != nullptr);
    REQUIRE(p.options->scatter.has_value());
    REQUIRE(p.options->mirror.has_value());
    CHECK(!p.options->scatter->axisX);
    CHECK(p.options->scatter->axisY);
    CHECK(p.options->scatter->option.data().strength == 2.0);
    CHECK(p.options->mirror->horizontal);
    CHECK(!p.options->mirror->vertical);
}

TEST_CASE("preset->brush: a smudge preset DROPS Mirror and KEEPS Scatter -- the reference's split") {
    // The reference's colorsmudge applies its Scatter option to the dab position and never
    // constructs its Mirror option at all (only the pixel brush's dab executor wires mirror
    // postprocessing) -- so honouring Mirror under the smudge walk would be the UNFAITHFUL
    // choice. No shipped colorsmudge preset carries Mirror, so this rule lives here, not in the
    // census.
    LibraryPreset lp = plainPreset();
    lp.preset.smudge.enabled = true;
    lp.preset.options.push_back(opt("Scatter", 1.0));
    lp.preset.options.push_back(opt("Mirror", 1.0));
    lp.preset.mirrorHorizontal = true;
    lp.preset.mirrorVertical = true;

    const cb::BrushParams p = presetBrushParams(lp);
    REQUIRE(p.options != nullptr);
    CHECK(p.options->scatter.has_value());
    CHECK(!p.options->mirror.has_value());
}
