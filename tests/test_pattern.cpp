#include "core/vector/pattern.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <memory>

#include "common/image.hpp"
#include "core/layer_effects.hpp"
#include "core/vector/paint.hpp"
#include "render/layer_effects_render.hpp"

using namespace mosaic::core;
using mosaic::common::ColorF;
using mosaic::common::Vec2;
using K = vec::ProceduralPattern::Kind;

namespace {

// A procedural pattern with opaque black fg on transparent bg (the default), overridable.
vec::ProceduralPattern pat(K kind, float scale = 32.0f, float weight = 0.5f) {
    vec::ProceduralPattern p;
    p.kind = kind;
    p.scale = scale;
    p.weight = weight;
    return p;
}

bool isFg(const ColorF& c) { return c.a > 0.9f; }  // opaque -> the fg feature
bool isBg(const ColorF& c) { return c.a < 0.1f; }  // transparent -> the bg

}  // namespace

TEST_CASE("pattern: every kind has a name") {
    for (int i = 0; i < vec::ProceduralPattern::kKindCount; ++i) {
        const char* n = vec::patternKindName(static_cast<K>(i));
        CHECK(n != nullptr);
        CHECK(std::string(n).size() > 0);
    }
    CHECK(std::string(vec::patternKindName(K::Herringbone)) == "Herringbone");
    CHECK(std::string(vec::patternKindName(K::Chainmail)) == "Chainmail");
}

TEST_CASE("pattern: Dots -- fg at cell centre, bg at cell corner") {
    const vec::Pattern p = pat(K::Dots, 32.0f, 0.6f);
    // cell period = scale (32 px); a dot sits at the cell centre (16,16).
    CHECK(isFg(vec::samplePattern(p, {16.0, 16.0})));
    CHECK(isBg(vec::samplePattern(p, {0.5, 0.5})));  // near the corner, outside the dot
    // tiling: the next cell's centre is identical.
    const ColorF a = vec::samplePattern(p, {16.0, 16.0});
    const ColorF b = vec::samplePattern(p, {16.0 + 32.0, 16.0});
    CHECK(a.a == doctest::Approx(b.a));
}

TEST_CASE("pattern: Checker -- parity flips fg/bg every cell") {
    const vec::Pattern p = pat(K::Checker, 16.0f);
    // The first full square begins at the tile origin (u==0) with only a sub-pixel inward nudge (so the
    // shape's top/left edge clears the boundary AA -- no 1px seam). Cell (0,0) is fg; (1,0) bg; (1,1) fg.
    CHECK(isFg(vec::samplePattern(p, {2.0, 2.0})));    // interior of cell (0,0)
    CHECK(isBg(vec::samplePattern(p, {18.0, 2.0})));   // interior of cell (1,0)
    CHECK(isFg(vec::samplePattern(p, {18.0, 18.0})));  // interior of cell (1,1) -> fg again
}

TEST_CASE("pattern: Lines -- duty cycle sets the fg band") {
    const vec::Pattern p = pat(K::Lines, 20.0f, 0.5f);  // band [0,0.5) of each 20px period
    CHECK(isFg(vec::samplePattern(p, {5.0, 10.0})));   // u.x = 0.25 -> in band
    CHECK(isBg(vec::samplePattern(p, {15.0, 10.0})));  // u.x = 0.75 -> out of band
}

TEST_CASE("pattern: fg / bg colours are used") {
    vec::ProceduralPattern pp = pat(K::Lines, 20.0f, 0.5f);
    pp.fg = ColorF{1.0f, 0.0f, 0.0f, 1.0f};  // red
    pp.bg = ColorF{0.0f, 0.0f, 1.0f, 1.0f};  // opaque blue (so bg is visible too)
    const vec::Pattern p = pp;
    const ColorF fg = vec::samplePattern(p, {5.0, 10.0});
    const ColorF bg = vec::samplePattern(p, {15.0, 10.0});
    CHECK(fg.r == doctest::Approx(1.0f).epsilon(0.02));
    CHECK(fg.b == doctest::Approx(0.0f).epsilon(0.02));
    CHECK(bg.b == doctest::Approx(1.0f).epsilon(0.02));
    CHECK(bg.r == doctest::Approx(0.0f).epsilon(0.02));
}

TEST_CASE("pattern: angle rotates the field") {
    // Lines at 0deg vary along x (constant along y); at 90deg they vary along y (constant along x).
    vec::ProceduralPattern v = pat(K::Lines, 20.0f, 0.5f);
    v.angleDeg = 90.0f;
    const vec::Pattern p = v;
    // constant along x at 90deg: two points with the same y but different x agree.
    const ColorF a = vec::samplePattern(p, {5.0, 3.0});
    const ColorF b = vec::samplePattern(p, {105.0, 3.0});
    CHECK(a.a == doctest::Approx(b.a));
    // ...but they differ as y crosses the band edge.
    const ColorF lo = vec::samplePattern(p, {50.0, 5.0});   // u.y = 0.25 -> band
    const ColorF hi = vec::samplePattern(p, {50.0, 15.0});  // u.y = 0.75 -> gap
    CHECK(isFg(lo));
    CHECK(isBg(hi));
}

TEST_CASE("pattern: Parquet tiles by its brick lattice") {
    const vec::Pattern p = pat(K::Parquet, 24.0f);
    // Lattice periods (unit space) are V1=(1,-3), V2=(1,1); in px = scale*V.
    for (Vec2 base : {Vec2{40, 55}, Vec2{7, 90}, Vec2{130, 22}}) {
        const ColorF a = vec::samplePattern(p, base);
        const ColorF b = vec::samplePattern(p, {base.x + 24.0 * 1, base.y + 24.0 * -3});
        const ColorF c = vec::samplePattern(p, {base.x + 24.0 * 1, base.y + 24.0 * 1});
        CHECK(a.a == doctest::Approx(b.a).epsilon(0.001));
        CHECK(a.a == doctest::Approx(c.a).epsilon(0.001));
    }
}

TEST_CASE("pattern: Herringbone/Parquet have no shaved-corner holes") {
    // The chipped-corner bug left small transparent notches mid-brick. Sample a dense grid; interior
    // coverage must be almost entirely fg-or-bg (thin mortar only), never a scatter of odd holes.
    for (K kind : {K::Herringbone, K::Parquet}) {
        const vec::Pattern p = pat(kind, 20.0f, 0.5f);
        int ambiguous = 0, total = 0;
        for (int y = 0; y < 200; ++y)
            for (int x = 0; x < 200; ++x) {
                const ColorF c = vec::samplePattern(p, {static_cast<double>(x) + 0.5,
                                                        static_cast<double>(y) + 0.5});
                ++total;
                if (c.a > 0.15f && c.a < 0.85f) ++ambiguous;  // an AA edge pixel
            }
        // Only mortar-edge pixels are ambiguous; for a 20px feature that's a small minority.
        CHECK(ambiguous < total / 4);
    }
}

TEST_CASE("pattern: weight vs spacing control classification") {
    // Kinds with a thickness/radius/duty knob use weight; the fixed-shape lattice motifs use
    // spacing; the gapless tessellations use neither. Exactly one (or zero) applies -- never both.
    for (int i = 0; i < vec::ProceduralPattern::kKindCount; ++i) {
        const K k = static_cast<K>(i);
        const bool both = vec::patternUsesWeight(k) && vec::patternUsesSpacing(k);
        CHECK_FALSE(both);
    }
    CHECK(vec::patternUsesWeight(K::Dots));
    CHECK(vec::patternUsesWeight(K::Lines));
    CHECK_FALSE(vec::patternUsesWeight(K::Hearts));
    CHECK(vec::patternUsesSpacing(K::Hearts));
    CHECK(vec::patternUsesSpacing(K::Stars));
    CHECK(vec::patternUsesSpacing(K::StarAnise));
    // Gapless tessellations take neither.
    for (K k : {K::Checker, K::Triangles, K::Sawtooth, K::Harlequin}) {
        CHECK_FALSE(vec::patternUsesWeight(k));
        CHECK_FALSE(vec::patternUsesSpacing(k));
    }
}

TEST_CASE("pattern: spacing opens the gap between motif elements") {
    // More spacing shrinks each heart within its cell -> fewer fg pixels over a fixed field.
    const auto fgCount = [](float spacing) {
        vec::ProceduralPattern pp;
        pp.kind = K::Hearts;
        pp.scale = 24.0f;
        pp.spacing = spacing;
        const vec::Pattern p = pp;
        int fg = 0;
        for (int y = 0; y < 120; ++y)
            for (int x = 0; x < 120; ++x)
                if (isFg(vec::samplePattern(p, {x + 0.5, y + 0.5}))) ++fg;
        return fg;
    };
    CHECK(fgCount(0.0f) > fgCount(1.0f));
}

TEST_CASE("pattern: antialias=false hardens the edge to 0/1") {
    const vec::Pattern p = pat(K::Dots, 32.0f, 0.6f);
    // Crisp: sweep a dot edge -- coverage is only ever fully fg or fully bg, never a ramp pixel.
    int ambiguousCrisp = 0, ambiguousSoft = 0;
    for (int t = 0; t < 400; ++t) {
        const double x = t * 0.08;  // fine sweep across several cells
        const float ca = vec::samplePattern(p, {x, 16.0}, /*antialias=*/false).a;
        const float aa = vec::samplePattern(p, {x, 16.0}, /*antialias=*/true).a;
        if (ca > 0.02f && ca < 0.98f) ++ambiguousCrisp;
        if (aa > 0.02f && aa < 0.98f) ++ambiguousSoft;
    }
    CHECK(ambiguousCrisp == 0);  // no partial coverage when AA is off
    CHECK(ambiguousSoft > 0);    // the soft edge still ramps
}

TEST_CASE("pattern: Grain follows the AA setting like every other kind") {
    const vec::Pattern p = pat(K::Grain, 8.0f, 0.5f);
    int partialAA = 0, partialCrisp = 0;
    for (int t = 0; t < 2000; ++t) {
        const double x = t * 0.37, y = t * 0.19;
        if (const float aa = vec::samplePattern(p, {x, y}, /*antialias=*/true).a;
            aa > 0.02f && aa < 0.98f)
            ++partialAA;
        if (const float cr = vec::samplePattern(p, {x, y}, /*antialias=*/false).a;
            cr > 0.02f && cr < 0.98f)
            ++partialCrisp;
    }
    CHECK(partialCrisp == 0);  // AA off -> crisp speckle
    CHECK(partialAA > 0);      // AA on -> Grain now anti-aliases its noise edges too
}

TEST_CASE("pattern: offset shifts the tiling phase (1.0 == a full tile == no shift)") {
    vec::ProceduralPattern base = pat(K::Lines, 20.0f, 0.5f);
    const auto at = [](vec::ProceduralPattern pp, double x, double y) {
        return vec::samplePattern(vec::Pattern{pp}, {x, y}).a;
    };
    // A half-tile offset flips a Lines band edge: a point in the band at offset 0 lands in the gap.
    base.offset = 0.0f;
    const float in0 = at(base, 5.0, 10.0);   // u.x = 0.25 -> band (fg)
    base.offset = 0.5f;
    const float in5 = at(base, 5.0, 10.0);   // shifted half a tile -> gap (bg)
    CHECK(in0 > 0.9f);
    CHECK(in5 < 0.1f);
    // A full-tile offset (1.0) wraps back to the unshifted pattern.
    base.offset = 1.0f;
    CHECK(at(base, 5.0, 10.0) == doctest::Approx(in0));
}

TEST_CASE("pattern: image pattern reads transparent until LE-d2") {
    vec::ImagePattern img;  // null tile
    const vec::Pattern p = img;
    CHECK(isBg(vec::samplePattern(p, {10.0, 10.0})));
}

TEST_CASE("pattern: reachable through vec::sampleAt (the shared Paint evaluator)") {
    const vec::Paint paint = pat(K::Dots, 32.0f, 0.6f);
    CHECK(isFg(vec::sampleAt(paint, {16.0, 16.0})));
    CHECK(isBg(vec::sampleAt(paint, {0.5, 0.5})));
}

// ---- ImagePattern (LE-d2) ---------------------------------------------------------------------
namespace {

using mosaic::common::Color8;
using mosaic::common::Image;

// Build a w x h RGBA tile from row-major {r,g,b,a} bytes (missing pixels stay zero/transparent).
std::shared_ptr<const Image> tileOf(std::uint32_t w, std::uint32_t h,
                                    std::initializer_list<std::array<std::uint8_t, 4>> px) {
    auto img = std::make_shared<Image>(w, h);
    std::size_t i = 0;
    for (const auto& p : px) {
        img->rgba[i * 4 + 0] = p[0];
        img->rgba[i * 4 + 1] = p[1];
        img->rgba[i * 4 + 2] = p[2];
        img->rgba[i * 4 + 3] = p[3];
        ++i;
    }
    return img;
}

// The 2x2 reference tile: (0,0)=red (1,0)=green / (0,1)=blue (1,1)=white, all opaque.
std::shared_ptr<const Image> rgbwTile() {
    return tileOf(2, 2, {{{255, 0, 0, 255}}, {{0, 255, 0, 255}}, {{0, 0, 255, 255}}, {{255, 255, 255, 255}}});
}
bool approx(const ColorF& c, float r, float g, float b, float a) {
    return c.r == doctest::Approx(r).epsilon(0.02) && c.g == doctest::Approx(g).epsilon(0.02) &&
           c.b == doctest::Approx(b).epsilon(0.02) && c.a == doctest::Approx(a).epsilon(0.02);
}

}  // namespace

TEST_CASE("image pattern: null / empty tile reads transparent") {
    CHECK(isBg(vec::samplePattern(vec::Pattern{vec::ImagePattern{}}, {10.0, 10.0})));  // null tile
    vec::ImagePattern empty;
    empty.tile = std::make_shared<const Image>();  // 0x0 image
    CHECK(isBg(vec::samplePattern(vec::Pattern{empty}, {3.0, 7.0})));
}

TEST_CASE("image pattern: nearest sampling tiles and wraps in layer px") {
    vec::ImagePattern ip;
    ip.tile = rgbwTile();  // 2x2, scale 1 -> one native px per layer px
    const vec::Pattern p = ip;
    // Sample cell centres (antialias off = nearest).
    CHECK(approx(vec::samplePattern(p, {0.5, 0.5}, false), 1, 0, 0, 1));  // (0,0) red
    CHECK(approx(vec::samplePattern(p, {1.5, 0.5}, false), 0, 1, 0, 1));  // (1,0) green
    CHECK(approx(vec::samplePattern(p, {0.5, 1.5}, false), 0, 0, 1, 1));  // (0,1) blue
    CHECK(approx(vec::samplePattern(p, {1.5, 1.5}, false), 1, 1, 1, 1));  // (1,1) white
    // Tiling wrap-around: +2 px in either axis returns to the same texel.
    CHECK(approx(vec::samplePattern(p, {2.5, 0.5}, false), 1, 0, 0, 1));  // wraps to red
    CHECK(approx(vec::samplePattern(p, {3.5, 2.5}, false), 0, 1, 0, 1));  // wraps to green
    // Negative coordinates wrap too (no clamp / no mirror).
    CHECK(approx(vec::samplePattern(p, {-0.5, 0.5}, false), 0, 1, 0, 1));  // -1 -> col 1 green
}

TEST_CASE("image pattern: scale magnifies the tile (multiplier of native px)") {
    vec::ImagePattern ip;
    ip.tile = rgbwTile();
    ip.scale = 4.0f;  // each native texel now spans 4 layer px
    const vec::Pattern p = ip;
    // Texel (0,0) covers layer px [0,4): a point at layer (2,2) still lands in it.
    CHECK(approx(vec::samplePattern(p, {2.0, 2.0}, false), 1, 0, 0, 1));  // red
    CHECK(approx(vec::samplePattern(p, {5.0, 2.0}, false), 0, 1, 0, 1));  // native 1.25 -> green
    CHECK(approx(vec::samplePattern(p, {2.0, 6.0}, false), 0, 0, 1, 1));  // native y 1.5 -> blue
}

TEST_CASE("image pattern: bilinear blends between texels; nearest does not") {
    vec::ImagePattern ip;
    ip.tile = tileOf(2, 1, {{{0, 0, 0, 255}}, {{255, 255, 255, 255}}});  // black | white
    const vec::Pattern p = ip;
    // Layer x=1.0 is exactly halfway between the two texel centres (0.5 and 1.5).
    CHECK(approx(vec::samplePattern(p, {1.0, 0.5}, /*antialias=*/true), 0.5f, 0.5f, 0.5f, 1));
    // Nearest snaps to one texel (white here), never a mid grey.
    CHECK(approx(vec::samplePattern(p, {1.0, 0.5}, /*antialias=*/false), 1, 1, 1, 1));
}

TEST_CASE("image pattern: bilinear interpolates in premultiplied alpha (no colour bleed)") {
    // Opaque red beside a fully transparent texel: the midpoint should stay RED at half alpha, not
    // fade toward black (which straight-alpha interpolation would produce).
    vec::ImagePattern ip;
    ip.tile = tileOf(2, 1, {{{255, 0, 0, 255}}, {{0, 0, 0, 0}}});
    const ColorF mid = vec::samplePattern(vec::Pattern{ip}, {1.0, 0.5}, /*antialias=*/true);
    CHECK(mid.r == doctest::Approx(1.0f).epsilon(0.02));  // pure red preserved
    CHECK(mid.g == doctest::Approx(0.0f).epsilon(0.02));
    CHECK(mid.a == doctest::Approx(0.5f).epsilon(0.02));  // alpha halfway
}

TEST_CASE("image pattern: angleDeg rotates the tiling") {
    vec::ImagePattern ip;
    ip.tile = tileOf(2, 1, {{{0, 0, 0, 255}}, {{255, 255, 255, 255}}});  // varies along x
    ip.angleDeg = 90.0f;
    const vec::Pattern p = ip;
    // At 90deg the horizontal-varying tile now varies along Y and is constant along X.
    const ColorF a = vec::samplePattern(p, {0.5, 0.5}, false);
    const ColorF b = vec::samplePattern(p, {0.5, 1.5}, false);
    CHECK(a.r == doctest::Approx(0.0f).epsilon(0.02));   // black
    CHECK(b.r == doctest::Approx(1.0f).epsilon(0.02));   // white -> differs along y
    const ColorF c = vec::samplePattern(p, {9.5, 0.5}, false);
    CHECK(a.r == doctest::Approx(c.r).epsilon(0.02));    // constant along x
}

TEST_CASE("image pattern: offset shifts the tiling phase (in whole tiles per axis)") {
    vec::ImagePattern ip;
    ip.tile = tileOf(2, 1, {{{0, 0, 0, 255}}, {{255, 255, 255, 255}}});  // black | white
    // No offset: layer (0.5,0.5) reads texel 0 (black).
    CHECK(vec::samplePattern(vec::Pattern{ip}, {0.5, 0.5}, false).r == doctest::Approx(0.0f));
    // Half a tile (offset.x=0.5, tile width 2 -> +1 native px): now reads texel 1 (white).
    ip.offset.x = 0.5;
    CHECK(vec::samplePattern(vec::Pattern{ip}, {0.5, 0.5}, false).r == doctest::Approx(1.0f));
    // A full tile wraps back to the unshifted phase.
    ip.offset.x = 1.0;
    CHECK(vec::samplePattern(vec::Pattern{ip}, {0.5, 0.5}, false).r == doctest::Approx(0.0f));
}

TEST_CASE("image pattern: reachable through vec::sampleAt (the shared Paint evaluator)") {
    vec::ImagePattern ip;
    ip.tile = rgbwTile();
    const vec::Paint paint = vec::Pattern{ip};
    CHECK(approx(vec::sampleAt(paint, {0.5, 0.5}, false), 1, 0, 0, 1));  // red texel
}

TEST_CASE("image pattern: makeImagePattern from a whole image holds the tile self-contained") {
    Image src(2, 2);
    for (std::uint32_t i = 0; i < 4; ++i) src.rgba[i * 4 + 0] = 200;  // red-ish, opaque? set alpha too
    for (std::uint32_t i = 0; i < 4; ++i) src.rgba[i * 4 + 3] = 255;
    const vec::ImagePattern ip = vec::makeImagePattern(src);
    REQUIRE(ip.tile != nullptr);
    const ColorF c = vec::samplePattern(vec::Pattern{ip}, {0.5, 0.5}, false);
    CHECK(c.r == doctest::Approx(200.0f / 255.0f).epsilon(0.02));
    CHECK(c.a == doctest::Approx(1.0f));
    // An empty source yields a null tile that reads transparent.
    CHECK(vec::makeImagePattern(Image{}).tile == nullptr);
    CHECK(isBg(vec::samplePattern(vec::Pattern{vec::makeImagePattern(Image{})}, {1.0, 1.0})));
}

TEST_CASE("image pattern: makeImagePattern from a region crops the selection's bounding box") {
    // A 4x4 source where each pixel's red channel encodes x+16*y, so the crop is identifiable.
    Image src(4, 4);
    for (std::uint32_t y = 0; y < 4; ++y)
        for (std::uint32_t x = 0; x < 4; ++x) {
            const std::size_t o = (static_cast<std::size_t>(y) * 4 + x) * 4;
            src.rgba[o + 0] = static_cast<std::uint8_t>(x * 16 + y);  // distinct per pixel
            src.rgba[o + 3] = 255;
        }
    const vec::ImagePattern ip = vec::makeImagePattern(src, 1, 1, 2, 2);  // crop the middle 2x2
    REQUIRE(ip.tile != nullptr);
    CHECK(ip.tile->width == 2);
    CHECK(ip.tile->height == 2);
    // Tile (0,0) == source (1,1): red = 1*16 + 1 = 17.
    const ColorF c = vec::samplePattern(vec::Pattern{ip}, {0.5, 0.5}, false);
    CHECK(c.r == doctest::Approx(17.0f / 255.0f).epsilon(0.01));
    // A zero-area region yields a null tile.
    CHECK(vec::makeImagePattern(src, 0, 0, 0, 3).tile == nullptr);
}

TEST_CASE("image pattern: overlay renders through applyEffects (real-px, layer tiled, clipped)") {
    // Green opaque square; an image-tile pattern overlay (2x2 red/green/blue/white) must tile it in
    // LAYER px and stay clipped to the silhouette (nothing leaks outside).
    mosaic::common::ImageF io(40, 40);
    for (std::uint32_t y = 8; y < 32; ++y)
        for (std::uint32_t x = 8; x < 32; ++x) io.set(x, y, {0.0f, 1.0f, 0.0f, 1.0f});

    mosaic::core::LayerEffects fx;
    fx.patternOverlay.enabled = true;
    vec::ImagePattern ip;
    ip.tile = rgbwTile();
    ip.scale = 4.0f;
    fx.patternOverlay.paint = vec::Pattern{ip};
    mosaic::render::applyEffects(io, fx);

    int painted = 0, leaked = 0;
    for (std::uint32_t y = 0; y < 40; ++y)
        for (std::uint32_t x = 0; x < 40; ++x) {
            const ColorF c = io.at(x, y);
            const bool inSquare = x >= 8 && x < 32 && y >= 8 && y < 32;
            if (inSquare) {
                if (c.a > 0.5f) ++painted;  // the opaque tile covers the square
            } else if (c.a > 0.05f) {
                ++leaked;
            }
        }
    CHECK(painted > 0);
    CHECK(leaked == 0);
}

TEST_CASE("pattern: a Pattern Overlay renders through applyEffects (real-px, box-anchored)") {
    // A green opaque square in the middle of a transparent buffer, with a red-dots pattern overlay
    // (bg transparent). The overlay must tile the shape in LAYER px (paintAtNorm's pattern branch):
    // red dots appear over green gaps, clipped to the square (nothing leaks outside it).
    mosaic::common::ImageF io(40, 40);
    for (std::uint32_t y = 8; y < 32; ++y)
        for (std::uint32_t x = 8; x < 32; ++x) io.set(x, y, {0.0f, 1.0f, 0.0f, 1.0f});  // green

    mosaic::core::LayerEffects fx;
    fx.patternOverlay.enabled = true;
    mosaic::core::vec::ProceduralPattern pp;
    pp.kind = K::Dots;
    pp.fg = ColorF{1.0f, 0.0f, 0.0f, 1.0f};  // red dots
    pp.bg = ColorF{0.0f, 0.0f, 0.0f, 0.0f};  // transparent gaps -> the green shows through
    pp.scale = 8.0f;
    pp.weight = 0.6f;
    fx.patternOverlay.paint = mosaic::core::vec::Pattern{pp};

    mosaic::render::applyEffects(io, fx);

    int red = 0, green = 0, leaked = 0;
    for (std::uint32_t y = 0; y < 40; ++y)
        for (std::uint32_t x = 0; x < 40; ++x) {
            const ColorF c = io.at(x, y);
            const bool inSquare = x >= 8 && x < 32 && y >= 8 && y < 32;
            if (inSquare) {
                if (c.r > 0.6f && c.g < 0.4f) ++red;
                if (c.g > 0.6f && c.r < 0.4f) ++green;
            } else if (c.a > 0.05f) {
                ++leaked;  // the overlay must be clipped to the shape
            }
        }
    CHECK(red > 0);     // dots painted
    CHECK(green > 0);   // gaps kept the layer colour (bg was transparent)
    CHECK(leaked == 0); // nothing outside the silhouette
}
