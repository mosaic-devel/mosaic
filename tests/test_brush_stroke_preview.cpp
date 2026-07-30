#include <doctest/doctest.h>

#include "common/image.hpp"
#include "core/brush/stroke_preview.hpp"
#include "io/brush/preset_brush.hpp"
#include "io/io.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

// THE STROKE PREVIEW (docs/brushes.md §8.2/§8.3): one brush, one representative stroke, rendered
// through the REAL engine into a plain image. The corpus case at the bottom is the payoff -- it is
// the one that can see a whole family of presets previewing as a blank card.
namespace cb = mosaic::core::brush;

using mosaic::common::Color8;
using mosaic::common::Image;
using mosaic::io::brush::LibraryPreset;
using mosaic::io::brush::presetBrushParams;
using mosaic::io::brush::PresetLibrary;

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

// The card the dock actually draws (a long strip), and a diameter ceiling that suits it.
constexpr int kW = 196;
constexpr int kH = 58;
constexpr double kCap = 28.0;

// How many pixels of `img` differ from the preview's paper -- i.e. how much of the stroke landed.
// ⚠ It counts an ERASED pixel too (alpha moved), which is the whole point: three shipped presets can
// only ever take paper AWAY, and a test that looked for ink would call all three of them broken.
[[nodiscard]] int marked(const Image& img, const cb::StrokePreviewStyle& style = {}) {
    int n = 0;
    for (std::size_t i = 0; i + 3 < img.rgba.size(); i += 4) {
        const bool same = img.rgba[i] == style.paper.r && img.rgba[i + 1] == style.paper.g &&
                          img.rgba[i + 2] == style.paper.b && img.rgba[i + 3] == style.paper.a;
        n += static_cast<int>(!same);
    }
    return n;
}

} // namespace

// ---- The path ---------------------------------------------------------------------------------

TEST_CASE("the preview path is a CURVE, and its pressure visits both ends") {
    const std::vector<cb::StrokeInput> path = cb::strokePreviewPath(kW, kH);
    REQUIRE(path.size() >= 4); // 3+ samples or the engine's curve cannot bend at all

    // ⚠ A STRAIGHT stroke cannot show a per-dab HEADING -- on a straight span the curve's tangent
    // and the chord direction are bit-identical, so all 14 heading-following presets would preview
    // exactly as if they ignored the stroke. The preview path must genuinely bend.
    const mosaic::common::Vec2 a = path.front().pos;
    const mosaic::common::Vec2 b = path.back().pos;
    const double len = std::hypot(b.x - a.x, b.y - a.y);
    REQUIRE(len > 1.0);
    double maxDev = 0.0;
    for (const cb::StrokeInput& s : path) { // perpendicular distance from the straight chord
        const double d = std::abs(((b.x - a.x) * (a.y - s.pos.y)) - ((a.x - s.pos.x) * (b.y - a.y)));
        maxDev = std::max(maxDev, d / len);
    }
    CHECK(maxDev > kH * 0.15); // a real S, not a line with a wobble

    // Both curvature directions: the chord is crossed, so the deviation changes sign.
    int sides = 0;
    for (int want : {-1, 1}) {
        for (const cb::StrokeInput& s : path) {
            const double cross =
                ((b.x - a.x) * (s.pos.y - a.y)) - ((b.y - a.y) * (s.pos.x - a.x));
            if ((want < 0 && cross < -1.0) || (want > 0 && cross > 1.0)) {
                ++sides;
                break;
            }
        }
    }
    CHECK(sides == 2);

    // ⚠ THE PRESSURE MUST REACH BOTH ENDS. `z)_Stamp_Shoujo_Bubbles` has an INVERTED size curve
    // (press harder -> paint smaller, to nothing at full pressure): a ramp that only ever pressed
    // hard would render that working preset as an empty card. And without the hard end there is no
    // taper to see.
    CHECK(path.front().pressure == doctest::Approx(0.0));
    CHECK(path.back().pressure == doctest::Approx(1.0));
    for (std::size_t i = 1; i < path.size(); ++i)
        CHECK(path[i].pressure >= path[i - 1].pressure);
}

TEST_CASE("the preview path ramps speed and tilt, so speed- and tilt-driven presets preview") {
    const std::vector<cb::StrokeInput> path = cb::strokePreviewPath(kW, kH);
    REQUIRE(path.size() >= 4);

    // Time runs forward, and the pen ACCELERATES: the gaps shrink. A `speed` sensor reads distance
    // over time, so a path drawn at one cadence shows every speed-driven preset at a single speed --
    // which is not showing what the option does.
    for (std::size_t i = 1; i < path.size(); ++i)
        CHECK(path[i].timeUs > path[i - 1].timeUs);
    const std::uint64_t firstGap = path[1].timeUs - path[0].timeUs;
    const std::uint64_t lastGap = path.back().timeUs - path[path.size() - 2].timeUs;
    CHECK(lastGap < firstGap);

    // The pen starts upright and leans over. `declination` (the tilt ANGLE) is this vector's
    // magnitude, and it is what most tilt-driven presets actually read.
    const double tilt0 = std::hypot(path.front().xTilt, path.front().yTilt);
    const double tilt1 = std::hypot(path.back().xTilt, path.back().yTilt);
    CHECK(tilt0 == doctest::Approx(0.0));
    CHECK(tilt1 > 30.0);
}

TEST_CASE("the path insets itself by the brush's RADIUS, so the stroke stays inside its box") {
    // ⚠ A USER-REPORTED BUG: "the stroke previews like to go outside the bounds of their box". The
    // path runs through the dabs' CENTRES, so a curve laid out against the full box hangs HALF A NIB
    // over each of its edges -- and the wider the brush, the further over.
    constexpr double r = 14.0; // the card's ceiling radius

    for (const cb::StrokeInput& s : cb::strokePreviewPath(kW, kH, r)) {
        CHECK(s.pos.x >= r - 1e-9);
        CHECK(s.pos.x <= kW - r + 1e-9);
        CHECK(s.pos.y >= r - 1e-9);
        CHECK(s.pos.y <= kH - r + 1e-9);
    }

    // ... and WITHOUT the inset it does not. This is the bug, and it is what the inset is for.
    bool spills = false;
    for (const cb::StrokeInput& s : cb::strokePreviewPath(kW, kH, 0.0))
        spills = spills || s.pos.x < r || s.pos.x > kW - r || s.pos.y < r || s.pos.y > kH - r;
    CHECK(spills);

    // ⚠ A brush WIDER THAN ITS BOX must not turn the path inside out (a negative box would mirror the
    // curve). It collapses to a line down the middle and fills the strip -- which is the honest
    // picture of a brush wider than its strip.
    const std::vector<cb::StrokeInput> huge = cb::strokePreviewPath(kW, kH, 500.0);
    REQUIRE(huge.size() >= 2);
    for (const cb::StrokeInput& s : huge) {
        CHECK(s.pos.x >= 0.0);
        CHECK(s.pos.x <= kW);
        CHECK(std::abs(s.pos.y - kH / 2.0) <= 1.0); // down the middle
    }

    // ⚠⚠ AND THE RENDERER MUST ACTUALLY PASS THE RADIUS. Everything above tests the PATH; none of it
    // can see a renderer that computes a beautiful inset path and then asks for an un-inset one. The
    // mutant that hard-coded the inset to 0 inside renderStrokePreview SAILED THROUGH every check
    // above -- the unit was pinned and the WIRING was not.
    //
    // So: paint a wide round nib and demand the box's own border survives it. 40 px of nib on a 58 px
    // strip spills 3 px past the top and bottom edges without the inset, and sits 5 px inside them
    // with it.
    cb::BrushParams wide;
    wide.diameter = 40.0;
    const Image img = cb::renderStrokePreview(wide, kW, kH, {});
    const cb::StrokePreviewStyle style;
    const auto atPx = [&](int x, int y) {
        const std::size_t i = ((static_cast<std::size_t>(y) * kW) + x) * 4;
        return Color8{img.rgba[i], img.rgba[i + 1], img.rgba[i + 2], img.rgba[i + 3]};
    };
    for (int x = 0; x < kW; ++x) {
        CHECK(atPx(x, 0) == style.paper);
        CHECK(atPx(x, kH - 1) == style.paper);
    }
    for (int y = 0; y < kH; ++y) {
        CHECK(atPx(0, y) == style.paper);
        CHECK(atPx(kW - 1, y) == style.paper);
    }
    CHECK(marked(img, style) > 500); // ... and it is a real stroke, not an empty box
}

// ---- The render -------------------------------------------------------------------------------

TEST_CASE("a preview lays a stroke, deterministically") {
    cb::BrushParams p;
    p.diameter = 12.0;

    const Image a = cb::renderStrokePreview(p, kW, kH);
    REQUIRE(a.width == static_cast<std::uint32_t>(kW));
    REQUIRE(a.height == static_cast<std::uint32_t>(kH));
    CHECK(marked(a) > 200); // it painted

    // ⚠ The seed is PINNED, so the same brush previews to the same picture forever. A card whose
    // `fuzzy` dabs reshuffled on every repaint would shimmer in the dock.
    const Image b = cb::renderStrokePreview(p, kW, kH);
    CHECK(a.rgba == b.rgba);
}

TEST_CASE("the diameter ceiling caps a huge brush and leaves every smaller one at TRUE SCALE") {
    // §8.3's ruling is that a preview renders at the brush's real size and lets it overflow the box,
    // rather than squeezing every brush into one window that makes the big ones lie. The ceiling
    // must therefore be exactly that -- a ceiling -- and must not touch a brush below it.
    cb::BrushParams small;
    small.diameter = 9.0;
    cb::StrokePreviewStyle capped;
    capped.maxDiameter = kCap;

    CHECK(cb::renderStrokePreview(small, kW, kH, {}).rgba ==
          cb::renderStrokePreview(small, kW, kH, capped).rgba); // untouched, byte for byte

    cb::BrushParams huge;
    huge.diameter = 600.0;
    const Image uncapped = cb::renderStrokePreview(huge, kW, kH, {});
    const Image ceiling = cb::renderStrokePreview(huge, kW, kH, capped);
    CHECK(uncapped.rgba != ceiling.rgba);
    CHECK(marked(uncapped) > marked(ceiling)); // 600 px of nib floods a 64 px strip
}

TEST_CASE("the ceiling scales the MASKING brush with the primary, or it is a different brush") {
    // ⚠⚠ The masking tip's size is authored as a COEFFICIENT of the master size and only resolved to
    // an absolute at load. So a ceiling that shrinks the primary and leaves the mask alone does not
    // draw the same brush smaller -- it draws a brush wearing a mask several times too big for it.
    //
    // ⚠ ASSERTED AS A RATIO, NOT AS A PIXEL COUNT. The first cut of this case compared how many
    // pixels each variant marked -- and a pixel count measures the mask's FEROCITY, not its
    // FAITHFULNESS, so the moment the path geometry changed the comparison pointed the other way
    // while the rule itself had not moved an inch. What the rule says is that the authored ratio
    // survives the ceiling, so that is what the case says.
    const LibraryPreset* lp = byName("g)_Dry_Bristles_Eroded");
    REQUIRE(lp != nullptr);
    const cb::BrushParams p = presetBrushParams(*lp);
    REQUIRE(p.masking.enabled);
    REQUIRE(p.diameter == doctest::Approx(120.0));
    REQUIRE(p.masking.diameter == doctest::Approx(120.0)); // the authored 1:1 ratio

    const cb::BrushParams capped = cb::previewCapped(p, 30.0);
    CHECK(capped.diameter == doctest::Approx(30.0));
    CHECK(capped.masking.diameter == doctest::Approx(30.0)); // the ratio SURVIVED
    CHECK(capped.masking.diameter / capped.diameter ==
          doctest::Approx(p.masking.diameter / p.diameter));

    // A brush UNDER the ceiling is not touched at all: the ceiling is a ceiling, not a window.
    const cb::BrushParams small = cb::previewCapped(p, 500.0);
    CHECK(small.diameter == doctest::Approx(120.0));
    CHECK(small.masking.diameter == doctest::Approx(120.0));
    CHECK(cb::previewCapped(p, 0.0).diameter == doctest::Approx(120.0)); // 0 = no ceiling at all

    // ... and the ratio is what keeps the mask ERODING the stroke instead of deleting it: leave the
    // mask at 120 over a 30 px nib and the two pictures are not the same picture.
    cb::StrokePreviewStyle style;
    style.maxDiameter = 30.0;
    cb::BrushParams broken = p; // what a ceiling that forgot the mask would hand the engine
    broken.diameter = 30.0;
    CHECK(cb::renderStrokePreview(p, kW, kH, style).rgba !=
          cb::renderStrokePreview(broken, kW, kH, {}).rgba);
}

TEST_CASE("an ERASER previews as a carve -- which is why the paper is opaque") {
    // Three shipped presets carry CompositeOp=erase. An eraser lays NOTHING on an empty canvas; it
    // can only take paper away. On a transparent preview canvas all three would render blank cards
    // and read as broken imports.
    const LibraryPreset* lp = byName("a)_Eraser_Circle");
    REQUIRE(lp != nullptr);
    REQUIRE(lp->preset.eraserMode);

    cb::BrushParams p = presetBrushParams(*lp);
    p.diameter = 20.0;
    REQUIRE(p.strokeMode == cb::StrokeMode::Erase);

    const cb::StrokePreviewStyle style;
    const Image img = cb::renderStrokePreview(p, kW, kH, style);
    const auto at = [&](int x, int y) {
        const std::size_t i = ((static_cast<std::size_t>(y) * kW) + x) * 4;
        return Color8{img.rgba[i], img.rgba[i + 1], img.rgba[i + 2], img.rgba[i + 3]};
    };

    // ⚠⚠ THE PREMISE, AND WITHOUT IT THIS CASE PROVES NOTHING. On a TRANSPARENT canvas every pixel
    // already differs from the paper and every alpha is already below 255 -- so both assertions below
    // would pass with flying colours while the eraser did absolutely nothing. That is not a
    // hypothesis: the mutant that filled the canvas with transparent black SURVIVED the first cut of
    // this very test. The instrument was in on the lie.
    //
    // So prove the canvas is opaque paper FIRST, at two corners the S-curve never reaches. Then, and
    // only then, does "something went transparent" mean the eraser did it.
    CHECK(at(0, 0) == style.paper);
    CHECK(at(kW - 1, kH - 1) == style.paper);

    CHECK(marked(img, style) > 200);

    // What it did is take alpha AWAY: the paper went opaque -> transparent along the stroke.
    int carved = 0;
    for (std::size_t i = 3; i < img.rgba.size(); i += 4)
        carved += static_cast<int>(img.rgba[i] < 255);
    CHECK(carved > 200);
}

TEST_CASE("a brush that cannot mark WHITE falls back to grey -- the five that otherwise blank") {
    // ⚠⚠ MEASURED OVER THE WHOLE CORPUS: exactly FIVE shipped presets leave white paper EXACTLY as
    // they found it when painting black -- `l)_Adjust_Lighten`, `_Dodge`, `_Color`, `_Overlay_Burn`
    // and `y)_Texture_Starfield`. Every one of them paints through a BLEND MODE, and Lighten /
    // ColorDodge / Color / Overlay / Screen are all the identity for black over white. They are not
    // broken; white is a canvas they physically cannot move. Left alone they would be five blank
    // cards in the dock, looking like five bugs.
    const cb::StrokePreviewStyle style; // black on white, as shipped
    const auto corner = [&](const Image& img) {
        return Color8{img.rgba[0], img.rgba[1], img.rgba[2], img.rgba[3]}; // never under the stroke
    };

    const LibraryPreset* blind = byName("l)_Adjust_Lighten");
    REQUIRE(blind != nullptr);
    cb::BrushParams p = presetBrushParams(*blind);
    p.diameter = 20.0;

    const Image img = cb::renderStrokePreview(p, kW, kH, style);
    CHECK(corner(img) == style.fallbackPaper); // it fell back...

    cb::StrokePreviewStyle asLaid; // ... and on the fallback PAIR it actually marks
    asLaid.paper = style.fallbackPaper;
    asLaid.ink = style.fallbackInk;
    CHECK(marked(img, asLaid) > 100);

    // ⚠⚠ AND IT IS THE PAIR, NOT THE PAPER. Black is darker than every paper in EVERY channel, so
    // `Lighten` is the identity against any of them -- `max(x, 0) == x`. A fallback that swapped the
    // paper to grey and kept the black ink would leave all five exactly as blank as it found them.
    // (That is not a hypothesis: it is what the first cut of this fallback did.)
    cb::StrokePreviewStyle greyPaperBlackInk;
    greyPaperBlackInk.paper = style.fallbackPaper;
    greyPaperBlackInk.ink = Color8{0, 0, 0, 255};
    greyPaperBlackInk.fallbackPaper = greyPaperBlackInk.paper; // no escape hatch: measure the pair
    greyPaperBlackInk.fallbackInk = greyPaperBlackInk.ink;
    CHECK(marked(cb::renderStrokePreview(p, kW, kH, greyPaperBlackInk), greyPaperBlackInk) == 0);

    // ⚠ An ordinary brush is NOT dragged onto the fallback: white stays white, which is the whole
    // point of the ruling. Without this the case would pass on code that simply painted every card
    // grey.
    cb::BrushParams plain;
    plain.diameter = 12.0;
    const Image ordinary = cb::renderStrokePreview(plain, kW, kH, style);
    CHECK(corner(ordinary) == style.paper);
    CHECK(marked(ordinary, style) > 200);

    // ⚠ And the trigger is the RESULT, not a list of names: hand the SAME blend-blind preset a pair
    // its blend mode CAN move and it must stay on it. A hard-coded list of the five would not.
    cb::StrokePreviewStyle movable;
    movable.paper = style.fallbackPaper;
    movable.ink = style.fallbackInk;
    movable.fallbackPaper = Color8{7, 8, 9, 255}; // a colour nothing else here uses
    const Image onMovable = cb::renderStrokePreview(p, kW, kH, movable);
    CHECK(corner(onMovable) == movable.paper);
}

// ---- The corpus -------------------------------------------------------------------------------

// The one-time masking-tip gap, now CLOSED -- kept as a positive pin. `g)_Dry_Bristles_Eroded` is a
// 120 px nib under a 120 px SUBTRACT masking brush whose authored tip is an ERODED BITMAP TEXTURE
// (grain and holes, which is what "eroded" means). While the masking walk stamped a round analytic
// disc instead of that tip, the mask subtracted a solid disc covering the whole nib and the stroke
// previewed at 74 marked pixels of 14,080 -- a named exception case sat here, built to FAIL the day
// the walk learned to stamp a real tip. It did, and this is what replaced it: the preset must keep
// painting a REAL stroke, and the mask must keep BITING it (it is a subtract mask -- a mask that
// costs nothing is not being stamped).
TEST_CASE("g)_Dry_Bristles_Eroded paints through its eroded mask -- the masking tip is real") {
    const LibraryPreset* lp = byName("g)_Dry_Bristles_Eroded");
    REQUIRE(lp != nullptr);
    cb::StrokePreviewStyle style;
    style.maxDiameter = kCap;

    cb::BrushParams p = presetBrushParams(*lp);
    REQUIRE(p.masking.tip != nullptr); // the authored bitmap, resolved -- not the analytic disc
    const int withMask = marked(cb::renderStrokePreview(p, kW, kH, style), style);

    cb::BrushParams unmasked = p;
    unmasked.masking.enabled = false;
    const int withoutMask = marked(cb::renderStrokePreview(unmasked, kW, kH, style), style);

    CHECK(withMask > 2000);          // a real stroke survives (2,722 when this landed; was 74)
    CHECK(withMask < withoutMask);   // ... and the subtract mask still carves grain out of it
}

TEST_CASE("every shipped preset previews something -- not one blank card in the library") {
    cb::StrokePreviewStyle style;
    style.maxDiameter = kCap;

    std::vector<std::string> blank;
    for (const LibraryPreset& lp : shipped().presets()) {
        cb::BrushParams p = presetBrushParams(lp);
        // The preset's OWN diameter, ceilinged -- not a fixed size. A preview that resized every
        // brush to the same nib would be a picture of the tip, not of the brush.
        const Image img = cb::renderStrokePreview(p, kW, kH, style);
        if (marked(img, style) < 20)
            blank.push_back(lp.preset.name);
    }
    INFO("blank preview cards: " << [&] {
        std::string s;
        for (const std::string& n : blank)
            s += n + " ";
        return s;
    }());
    CHECK(blank.empty());
}


// A contact sheet of the whole library, and the clock on it. Not an assertion -- a way to LOOK at
// 117 previews at once (MOSAIC_PREVIEW_SHOT=<dir>) and to know what a card costs before deciding how
// hard the dock has to work to avoid re-rendering one.
TEST_CASE("preview contact sheet + cost" * doctest::skip(true)) {
    cb::StrokePreviewStyle style;
    style.maxDiameter = kCap;

    const auto& presets = shipped().presets();
    std::vector<Image> shots;
    shots.reserve(presets.size());

    const auto t0 = std::chrono::steady_clock::now();
    for (const LibraryPreset& lp : presets) {
        cb::BrushParams p = presetBrushParams(lp);
        shots.push_back(cb::renderStrokePreview(p, kW, kH, style));
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    MESSAGE("rendered " << shots.size() << " previews in " << ms << " ms ("
                        << ms / static_cast<double>(shots.size()) << " ms each)");

    const char* dir = std::getenv("MOSAIC_PREVIEW_SHOT");
    if (dir == nullptr)
        return;

    constexpr int kCols = 4;
    const int rows = (static_cast<int>(shots.size()) + kCols - 1) / kCols;
    Image sheet(static_cast<std::uint32_t>(kCols * (kW + 6) + 6),
                static_cast<std::uint32_t>(rows * (kH + 6) + 6));
    sheet.fill(Color8{30, 32, 40, 255});
    for (std::size_t i = 0; i < shots.size(); ++i) {
        const int ox = 6 + static_cast<int>(i % kCols) * (kW + 6);
        const int oy = 6 + static_cast<int>(i / kCols) * (kH + 6);
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x) {
                const std::uint8_t* s = &shots[i].rgba[((static_cast<std::size_t>(y) * kW) + x) * 4];
                std::uint8_t* d =
                    &sheet.rgba[((static_cast<std::size_t>(oy + y) * sheet.width) + ox + x) * 4];
                const double a = s[3] / 255.0; // over the sheet's own ground: an eraser shows a hole
                for (int c = 0; c < 3; ++c)
                    d[c] = static_cast<std::uint8_t>(std::lround(s[c] * a + d[c] * (1.0 - a)));
                d[3] = 255;
            }
    }
    std::string err;
    if (!mosaic::io::savePng(sheet, std::string(dir) + "/preview-sheet.png", {}, &err))
        MESSAGE("savePng: " << err);
}

