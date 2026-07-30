#include "core/retarget/importance_map.hpp"
#include "core/retarget/inpaint_fill.hpp"
#include "core/retarget/keep_regions.hpp"
#include "core/retarget/recompose.hpp"
#include "core/retarget/smart_crop.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>

// Smart Resize core (S16-f, docs/smart-resize-research.md §8): the importance map and the
// crop-window search. All assertions target the *decision* (the chosen rect) and the map's
// semantics, not golden pixels, plus byte-for-byte determinism.
namespace {

using mosaic::common::Image;
using mosaic::common::Rect;
using mosaic::core::retarget::buildImportanceMap;
using mosaic::core::retarget::chooseCropWindow;
using mosaic::core::retarget::extractKeepRegions;
using mosaic::core::retarget::ImportanceMap;
using mosaic::core::retarget::ImportanceOptions;
using mosaic::core::retarget::KeepRegion;
using mosaic::core::retarget::KeepRegionOptions;
using mosaic::core::retarget::recompose;
using mosaic::core::retarget::RecomposeOptions;
using mosaic::core::retarget::RecomposeResult;
using mosaic::core::retarget::SmartCropOptions;
using mosaic::core::retarget::solvePlacements;

// A solid-color opaque image.
Image solid(std::uint32_t w, std::uint32_t h, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    Image img(w, h);
    img.fill({r, g, b, 255});
    return img;
}

// Paint the axis-aligned block [x0,x1) x [y0,y1) of `img` with an opaque gray level.
void paintBlock(Image& img, std::uint32_t x0, std::uint32_t y0, std::uint32_t x1,
                std::uint32_t y1, std::uint8_t v) {
    for (std::uint32_t y = y0; y < y1; ++y)
        for (std::uint32_t x = x0; x < x1; ++x) {
            std::uint8_t* p = img.rgba.data() + (static_cast<std::size_t>(y) * img.width + x) * 4;
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
}

// Total map mass inside the map-space block [x0,x1) x [y0,y1).
double massIn(const ImportanceMap& m, std::uint32_t x0, std::uint32_t y0, std::uint32_t x1,
              std::uint32_t y1) {
    double s = 0.0;
    for (std::uint32_t y = y0; y < y1; ++y)
        for (std::uint32_t x = x0; x < x1; ++x)
            s += m.at(x, y);
    return s;
}

} // namespace

TEST_CASE("importance map: empty input / degenerate options yield an empty map") {
    CHECK(buildImportanceMap(Image{}).empty());
    ImportanceOptions opts;
    opts.maxDim = 0;
    CHECK(buildImportanceMap(solid(8, 8, 100, 100, 100), opts).empty());
}

TEST_CASE("importance map: working resolution honors maxDim, keeps aspect, never upsamples") {
    ImportanceOptions opts;
    opts.maxDim = 320;
    const ImportanceMap m = buildImportanceMap(solid(800, 600, 50, 50, 50), opts);
    CHECK(m.width == 320);
    CHECK(m.height == 240);
    CHECK(m.sourceW == 800);
    CHECK(m.sourceH == 600);
    // Smaller than maxDim: used as-is, no upsampling.
    const ImportanceMap s = buildImportanceMap(solid(100, 50, 50, 50, 50), opts);
    CHECK(s.width == 100);
    CHECK(s.height == 50);
}

TEST_CASE("importance map: a flat image is pure composition prior — center beats corners") {
    const ImportanceMap m = buildImportanceMap(solid(200, 200, 128, 128, 128));
    REQUIRE_FALSE(m.empty());
    const float center = m.at(m.width / 2, m.height / 2);
    const float corner = m.at(0, 0);
    CHECK(center > corner);
    // No structure anywhere: every value is the (positive) prior, nothing else.
    CHECK(corner > 0.0f);
    float mx = 0.0f;
    for (const float v : m.w)
        mx = std::max(mx, v);
    CHECK(mx == m.at(m.width / 2, m.height / 2)); // the prior peaks at the center
}

TEST_CASE("importance map: structure attracts importance — textured quadrant outweighs flat") {
    // 400x400 mid-gray with a checkered block in the top-left quadrant.
    Image img = solid(400, 400, 128, 128, 128);
    for (std::uint32_t y = 20; y < 180; y += 8)
        for (std::uint32_t x = 20; x < 180; x += 8)
            paintBlock(img, x, y, x + 4, y + 4, 255);
    const ImportanceMap m = buildImportanceMap(img);
    REQUIRE_FALSE(m.empty());
    const std::uint32_t hw = m.width / 2, hh = m.height / 2;
    const double topLeft = massIn(m, 0, 0, hw, hh);
    const double bottomRight = massIn(m, hw, hh, m.width, m.height);
    CHECK(topLeft > 2.0 * bottomRight);
}

TEST_CASE("importance map: a step edge scores higher than the flat field around it") {
    // Left half black, right half white: the seam of the step carries the gradient energy.
    Image img = solid(320, 320, 0, 0, 0);
    paintBlock(img, 160, 0, 320, 320, 255);
    const ImportanceMap m = buildImportanceMap(img);
    REQUIRE_FALSE(m.empty());
    const std::uint32_t edgeX = m.width / 2;
    const std::uint32_t y = m.height / 2;
    // Compare at equal distance from the center so the composition prior cancels.
    const float atEdge = std::max(m.at(edgeX - 1, y), m.at(edgeX, y));
    const float flat = m.at(edgeX - m.width / 4, y);
    CHECK(atEdge > flat);
}

TEST_CASE("importance map: transparent regions carry no structure") {
    // Checkered texture everywhere, but the right half is fully transparent.
    Image img = solid(320, 320, 128, 128, 128);
    for (std::uint32_t y = 0; y < 320; y += 8)
        for (std::uint32_t x = 0; x < 320; x += 8)
            paintBlock(img, x, y, std::min(x + 4, 320u), std::min(y + 4, 320u), 255);
    for (std::uint32_t y = 0; y < 320; ++y)
        for (std::uint32_t x = 170; x < 320; ++x)
            img.rgba[(static_cast<std::size_t>(y) * 320 + x) * 4 + 3] = 0;
    const ImportanceMap m = buildImportanceMap(img);
    REQUIRE_FALSE(m.empty());
    // Interior of the opaque textured half vs interior of the transparent half (both clear of
    // the opaque/transparent boundary, which legitimately carries edge energy).
    const double opaque = massIn(m, 0, 0, m.width * 2 / 5, m.height);
    const double transparent = massIn(m, m.width * 3 / 5, 0, m.width, m.height);
    CHECK(opaque > 2.0 * transparent);
}

TEST_CASE("importance map: deterministic — two runs are byte-for-byte identical") {
    Image img = solid(300, 200, 40, 90, 160);
    paintBlock(img, 60, 40, 140, 120, 230);
    const ImportanceMap a = buildImportanceMap(img);
    const ImportanceMap b = buildImportanceMap(img);
    REQUIRE(a.w.size() == b.w.size());
    CHECK(a.w == b.w);
}

// ---- crop-window search (§4.3) -----------------------------------------------------------

namespace {

// A hand-built map (1:1 map <-> document pixels unless sizes differ) with a small uniform base
// so the total mass is never zero.
ImportanceMap makeMap(std::uint32_t w, std::uint32_t h, float base = 0.001f) {
    ImportanceMap m;
    m.width = w;
    m.height = h;
    m.sourceW = w;
    m.sourceH = h;
    m.w.assign(static_cast<std::size_t>(w) * h, base);
    return m;
}

void addMass(ImportanceMap& m, std::uint32_t x0, std::uint32_t y0, std::uint32_t x1,
             std::uint32_t y1, float v) {
    for (std::uint32_t y = y0; y < y1; ++y)
        for (std::uint32_t x = x0; x < x1; ++x)
            m.w[static_cast<std::size_t>(y) * m.width + x] += v;
}

// Whether doc-space rect `inner` lies inside `outer` (with a small tolerance for the map's
// working-resolution quantization).
bool containsRect(const Rect& outer, const Rect& inner, double eps) {
    return inner.x >= outer.x - eps && inner.y >= outer.y - eps &&
           inner.right() <= outer.right() + eps && inner.bottom() <= outer.bottom() + eps;
}

} // namespace

TEST_CASE("crop search: a uniform map yields the full source (free = no cheap edges to trim)") {
    // Free aspect = smart trim: on a uniform map every edge strip sits AT the mean density, so
    // nothing is 'cheap' and the frame stays full. An empty map degenerates to an empty rect.
    ImportanceMap m = makeMap(100, 80);
    const Rect full{0, 0, 100, 80};
    CHECK(chooseCropWindow(m, 0.0) == full);
    CHECK(chooseCropWindow(m, -1.0) == full);
    CHECK(chooseCropWindow(ImportanceMap{}, 1.0) == Rect{});
}

TEST_CASE("crop search: free aspect trims flat margins down to the content") {
    ImportanceMap m = makeMap(200, 100);
    addMass(m, 20, 20, 60, 60, 1.0f); // the only interesting content
    const Rect r = chooseCropWindow(m, 0.0);
    CHECK(containsRect(r, Rect{20, 20, 40, 40}, 1.0)); // content survives...
    CHECK(r.w < 60.0);                                 // ...and the flat field is trimmed away
    CHECK(r.h < 60.0);
    CHECK(chooseCropWindow(m, 0.0) == r); // deterministic
}

TEST_CASE("crop search: the trim never eats into a protect rect") {
    ImportanceMap m = makeMap(200, 100);
    addMass(m, 20, 20, 60, 60, 1.0f);
    SmartCropOptions o;
    o.protectRects.push_back({150, 40, 30, 30}); // near-zero mass, but marked as must-keep
    const Rect r = chooseCropWindow(m, 0.0, o);
    CHECK(containsRect(r, Rect{150, 40, 30, 30}, 1.0));
    CHECK(containsRect(r, Rect{20, 20, 40, 40}, 1.0));
}

TEST_CASE("crop search: an excluded region stops attracting the window") {
    // Two equal blobs; excluding the right one masks its mass, so a square window must prefer
    // the left blob rather than compromising between the two.
    ImportanceMap m = makeMap(300, 100);
    addMass(m, 40, 30, 80, 70, 1.0f);
    addMass(m, 220, 30, 260, 70, 1.0f);
    SmartCropOptions o;
    o.excludeRects.push_back({220, 30, 40, 40});
    const Rect r = chooseCropWindow(m, 1.0, o);
    CHECK(containsRect(r, Rect{40, 30, 40, 40}, 2.0));
    CHECK(r.center().x < 150.0); // firmly on the left half, not straddling the middle
}

TEST_CASE("crop search: the result honors the target aspect and stays inside the source") {
    ImportanceMap m = makeMap(200, 100);
    addMass(m, 30, 20, 70, 60, 1.0f);
    for (const double aspect : {1.0, 16.0 / 9.0, 0.5}) {
        const Rect r = chooseCropWindow(m, aspect);
        CHECK(r.w / r.h == doctest::Approx(aspect));
        CHECK(r.x >= 0.0);
        CHECK(r.y >= 0.0);
        CHECK(r.right() <= 200.0 + 1e-9);
        CHECK(r.bottom() <= 100.0 + 1e-9);
    }
}

TEST_CASE("crop search: importance mass in one quadrant pulls the window onto it") {
    // Heavy mass wholly inside the top-left quadrant of a wide map; a square crop must keep it.
    ImportanceMap m = makeMap(240, 120);
    addMass(m, 20, 15, 70, 60, 1.0f);
    const Rect r = chooseCropWindow(m, 1.0);
    CHECK(containsRect(r, Rect{20, 15, 50, 45}, 2.0));
    // And the window leans left: its centre sits left of the map's centre.
    CHECK(r.center().x < 120.0);
}

TEST_CASE("crop search: pure-mass objective equals a brute-force sweep (integral-image check)") {
    // With every penalty/reward off, the winner must be the window with the maximal plain mass
    // sum — computed here by brute force. Distinct per-cell values avoid ties.
    ImportanceMap m = makeMap(48, 32, 0.0f);
    for (std::uint32_t y = 0; y < m.height; ++y)
        for (std::uint32_t x = 0; x < m.width; ++x) {
            const std::uint32_t i = y * m.width + x;
            m.w[i] = static_cast<float>((i * 2654435761u >> 8) & 0xFFFF) / 65535.0f;
        }
    SmartCropOptions o;
    o.lambdaCut = o.lambdaComp = o.lambdaBlob = 0.0;
    o.scaleSteps = 0; // max fit only: 32x32 windows sliding in x
    o.coarseSteps = 64; // >= the 16 free positions -> exhaustive
    const Rect r = chooseCropWindow(m, 1.0, o);
    double bestSum = -1.0;
    std::uint32_t bestX = 0;
    for (std::uint32_t x0 = 0; x0 + 32 <= 48; ++x0) {
        double s = 0.0;
        for (std::uint32_t y = 0; y < 32; ++y)
            for (std::uint32_t x = x0; x < x0 + 32; ++x)
                s += m.w[y * m.width + x];
        if (s > bestSum) {
            bestSum = s;
            bestX = x0;
        }
    }
    CHECK(r.x == doctest::Approx(static_cast<double>(bestX)));
    CHECK(r.w == doctest::Approx(32.0));
}

TEST_CASE("crop search: a protect rect at the edge is kept whole or excluded, never sliced") {
    // Main mass in the middle-left; a protect box hugging the right edge. Whatever the search
    // prefers, the protect box must not be cut through.
    ImportanceMap m = makeMap(300, 100);
    addMass(m, 40, 20, 140, 80, 0.8f);
    SmartCropOptions o;
    o.protectRects.push_back({280, 30, 18, 40});
    const Rect r = chooseCropWindow(m, 1.0, o);
    const Rect protect{280, 30, 18, 40};
    const Rect overlap = r.intersected(protect);
    const bool wholly = containsRect(r, protect, 2.0);
    const bool excluded = overlap.empty() || overlap.w * overlap.h < 1e-6;
    CHECK((wholly || excluded));
    // With the protect mass far from the main mass and a 100px-wide window, exclusion is the
    // sane outcome — the main blob must still be kept.
    CHECK(containsRect(r, Rect{40, 20, 100, 60}, 2.0));
}

TEST_CASE("crop search: a salient blob is not sliced when a clean placement exists") {
    // A compact heavy blob near x=200 and diffuse mass to its left; the 100-wide window can
    // cover the blob cleanly, and must not stop halfway across it.
    ImportanceMap m = makeMap(300, 100);
    addMass(m, 60, 30, 160, 70, 0.15f);  // diffuse
    addMass(m, 190, 35, 220, 65, 1.5f);  // the blob
    const Rect r = chooseCropWindow(m, 1.0);
    const Rect blob{190, 35, 30, 30};
    const Rect overlap = r.intersected(blob);
    const bool wholly = containsRect(r, blob, 2.0);
    const bool excluded = overlap.empty();
    CHECK((wholly || excluded));
    CHECK(wholly); // the blob dominates the mass: it should in fact be the thing we keep
}

TEST_CASE("crop search: a symmetric map yields a horizontally centred crop") {
    ImportanceMap m = makeMap(240, 120);
    addMass(m, 50, 40, 90, 80, 1.0f);   // left blob
    addMass(m, 150, 40, 190, 80, 1.0f); // mirrored right blob
    const Rect r = chooseCropWindow(m, 1.5);
    CHECK(r.center().x == doctest::Approx(120.0).epsilon(0.03));
}

TEST_CASE("crop search: deterministic — identical inputs give the identical rect") {
    ImportanceMap m = makeMap(200, 150);
    addMass(m, 30, 30, 90, 90, 0.7f);
    addMass(m, 120, 60, 170, 130, 0.4f);
    const Rect a = chooseCropWindow(m, 4.0 / 3.0);
    const Rect b = chooseCropWindow(m, 4.0 / 3.0);
    CHECK(a == b);
}

// ---- keep-region extraction (Smart Recompose plan §1-§2) ---------------------------------

TEST_CASE("keep regions: two separated blobs become two mass-ordered doc-space regions") {
    ImportanceMap m = makeMap(200, 100);
    addMass(m, 20, 20, 50, 50, 1.0f);   // A: 30x30 @ 1.0 -> mass 900
    addMass(m, 120, 30, 150, 80, 0.5f); // B: 30x50 @ 0.5 -> mass 750
    const std::vector<KeepRegion> r = extractKeepRegions(m);
    REQUIRE(r.size() == 2);
    CHECK(r[0].massFrac > r[1].massFrac); // heaviest first
    CHECK(r[0].source == KeepRegion::Source::Auto);
    // Snug covering boxes (1:1 map here, so within a cell).
    CHECK(containsRect(r[0].rect, Rect{20, 20, 30, 30}, 1.0));
    CHECK(containsRect(r[1].rect, Rect{120, 30, 30, 50}, 1.0));
    // Snug means snug: no box balloons to half the document.
    CHECK(r[0].rect.w < 40.0);
    CHECK(r[1].rect.w < 40.0);
}

TEST_CASE("keep regions: near-touching blobs merge into one chip") {
    ImportanceMap m = makeMap(200, 100);
    addMass(m, 20, 20, 40, 40, 1.0f); // [20,40) and [41,60): a 1-cell gap
    addMass(m, 41, 20, 60, 40, 1.0f);
    KeepRegionOptions o;
    o.mergeGapCells = 1;
    const std::vector<KeepRegion> merged = extractKeepRegions(m, o);
    REQUIRE(merged.size() == 1);
    CHECK(containsRect(merged[0].rect, Rect{20, 20, 40, 20}, 1.0));
    o.mergeGapCells = 0; // gap of 1 cell no longer bridges
    CHECK(extractKeepRegions(m, o).size() == 2);
}

TEST_CASE("keep regions: the legibility cap keeps the heaviest blobs") {
    ImportanceMap m = makeMap(300, 100);
    for (int i = 0; i < 6; ++i) // six blobs of descending mass, spread out
        addMass(m, static_cast<std::uint32_t>(10 + i * 48), 30,
                static_cast<std::uint32_t>(30 + i * 48), 50, 1.0f - 0.1f * static_cast<float>(i));
    KeepRegionOptions o;
    o.maxRegions = 3;
    o.blobMinMassFrac = 0.01;
    const std::vector<KeepRegion> r = extractKeepRegions(m, o);
    REQUIRE(r.size() == 3);
    // The heaviest (leftmost) blob is kept; the lightest (rightmost) is not.
    CHECK(r[0].rect.x < 15.0);
    for (const KeepRegion& k : r)
        CHECK(k.rect.x < 5 * 48.0);
}

TEST_CASE("keep regions: face rects append as their own clamped regions (F1 hook)") {
    ImportanceMap m = makeMap(200, 100);
    addMass(m, 20, 20, 50, 50, 1.0f);
    const std::vector<KeepRegion> r =
        extractKeepRegions(m, {}, {{-10.0, -10.0, 50.0, 50.0}, {500.0, 0.0, 50.0, 50.0}});
    REQUIRE(r.size() == 2); // one blob + one clamped face; the off-canvas face is dropped
    CHECK(r[1].source == KeepRegion::Source::Face);
    CHECK(r[1].rect == Rect{0.0, 0.0, 40.0, 40.0});
    CHECK(extractKeepRegions(ImportanceMap{}).empty());
}

TEST_CASE("keep regions: a faint attached extremity joins its chip (support expansion)") {
    // The "castle spire" ghost (user 2026-07-02): the object's body clears the detection
    // threshold, its faint top does not — the chip under-covered the object. The bounded
    // geodesic dilation must grow the chip over the connected faint part (within reach);
    // a detached faint blob elsewhere must NOT grow it (and is no chip of its own).
    ImportanceMap m = makeMap(200, 100);
    addMass(m, 40, 40, 60, 60, 1.0f);  // the body: clears blobThreshold (reach = 10 cells)
    addMass(m, 45, 32, 55, 40, 0.2f);  // its faint top: below 0.3, above the support level
    addMass(m, 150, 10, 160, 20, 0.2f); // a detached faint speck far away
    const std::vector<KeepRegion> r = extractKeepRegions(m);
    REQUIRE(r.size() == 1);
    CHECK(containsRect(r[0].rect, Rect{40, 32, 20, 28}, 1.0)); // body + top, one chip
    CHECK(r[0].rect.right() < 140.0); // the detached speck neither grew it nor chipped itself
    // With the support pass disabled the top is (wrongly) left out — proving it did the work.
    KeepRegionOptions off;
    off.supportThresholdFrac = 0.0;
    const std::vector<KeepRegion> bare = extractKeepRegions(m, off);
    REQUIRE(bare.size() == 1);
    CHECK(bare[0].rect.y >= 39.0);
}

TEST_CASE("keep regions: a flooded support level creeps at most the bounded reach") {
    ImportanceMap m = makeMap(200, 100);
    addMass(m, 0, 0, 200, 100, 0.2f); // a faint busy background everywhere (all support-level)
    addMass(m, 40, 40, 60, 60, 1.0f); // the one real subject (20x20 -> reach = 10 cells)
    const std::vector<KeepRegion> r = extractKeepRegions(m);
    REQUIRE(r.size() == 1);
    // The support level floods the whole picture; the walk admits at most `reach` cells of it
    // per side — never a chip across the frame.
    CHECK(r[0].rect.w <= 42.0);
    CHECK(r[0].rect.h <= 42.0);
    CHECK(r[0].rect.w >= 30.0); // and it DID grow (the bound is the limiter, not a refusal)
}

TEST_CASE("keep regions: deterministic — two runs are identical") {
    ImportanceMap m = makeMap(240, 120);
    addMass(m, 30, 30, 70, 70, 0.8f);
    addMass(m, 150, 40, 200, 90, 0.8f); // equal mass: stable order must hold
    const auto a = extractKeepRegions(m);
    const auto b = extractKeepRegions(m);
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].rect == b[i].rect);
        CHECK(a[i].massFrac == b[i].massFrac);
    }
}

TEST_CASE("crop search: image overload — the checkered quadrant survives the crop") {
    Image img = solid(400, 400, 128, 128, 128);
    for (std::uint32_t y = 20; y < 180; y += 8)
        for (std::uint32_t x = 20; x < 180; x += 8)
            paintBlock(img, x, y, x + 4, y + 4, 255);
    const Rect r = chooseCropWindow(img, 1.0);
    CHECK(containsRect(r, Rect{20, 20, 160, 160}, 8.0));
}

// ---- placement solver + recompose (Smart Recompose plan §2/§4) ---------------------------

namespace {

using mosaic::common::Color8;

// Paint an opaque colored block (the gray-only paintBlock's colored sibling).
void paintColor(Image& img, std::uint32_t x0, std::uint32_t y0, std::uint32_t x1,
                std::uint32_t y1, Color8 c) {
    for (std::uint32_t y = y0; y < y1; ++y)
        for (std::uint32_t x = x0; x < x1; ++x) {
            std::uint8_t* p = img.rgba.data() + (static_cast<std::size_t>(y) * img.width + x) * 4;
            p[0] = c.r;
            p[1] = c.g;
            p[2] = c.b;
            p[3] = c.a;
        }
}

// Count pixels of `img` exactly matching `c`.
std::size_t countColor(const Image& img, Color8 c) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < img.pixelCount(); ++i) {
        const std::uint8_t* p = img.rgba.data() + i * 4;
        if (p[0] == c.r && p[1] == c.g && p[2] == c.b && p[3] == c.a)
            ++n;
    }
    return n;
}

// A deterministic mock filler: paints every hole with the flat field color.
bool grayFill(Image& img, const std::vector<Rect>& holes) {
    for (const Rect& h : holes)
        paintColor(img, static_cast<std::uint32_t>(h.x), static_cast<std::uint32_t>(h.y),
                   static_cast<std::uint32_t>(h.right()), static_cast<std::uint32_t>(h.bottom()),
                   {120, 120, 120, 255});
    return true;
}

} // namespace

TEST_CASE("placement solver: unconstrained regions land at the proportional position") {
    const std::vector<Rect> placed =
        solvePlacements({{90, 90, 20, 20}}, 400, 200, 200, 200, 4.0);
    REQUIRE(placed.size() == 1);
    CHECK(placed[0].center().x == doctest::Approx(50.0)); // 100/400 * 200
    CHECK(placed[0].center().y == doctest::Approx(100.0));
    CHECK(placed[0].w == 20.0); // rigid: sizes never change
    CHECK(placed[0].h == 20.0);
}

TEST_CASE("placement solver: order and separation survive a hard squeeze") {
    // Two 80-wide regions hugging opposite edges of a 400-wide source, squeezed into 200.
    const std::vector<Rect> src = {{0, 50, 80, 60}, {320, 50, 80, 60}};
    const std::vector<Rect> placed = solvePlacements(src, 400, 200, 200, 200, 4.0);
    REQUIRE(placed.size() == 2);
    CHECK(placed[0].x >= 0.0);
    CHECK(placed[1].right() <= 200.0 + 1e-9);
    CHECK(placed[1].x >= placed[0].right() + 4.0 - 1e-9); // ordered, min gap kept
    CHECK(placed[0].w == 80.0);
    CHECK(placed[1].w == 80.0);
}

TEST_CASE("placement solver: infeasible rigid placements return empty") {
    // Two 120-wide y-overlapping regions cannot share a 200-wide frame.
    CHECK(solvePlacements({{0, 50, 120, 60}, {280, 50, 120, 60}}, 400, 200, 200, 200, 4.0)
              .empty());
    // A region larger than the frame can never fit.
    CHECK(solvePlacements({{0, 0, 300, 60}}, 400, 200, 200, 200, 4.0).empty());
}

TEST_CASE("placement solver: deterministic") {
    const std::vector<Rect> src = {{10, 20, 60, 60}, {200, 40, 80, 90}, {330, 100, 50, 50}};
    const auto a = solvePlacements(src, 400, 200, 220, 220, 4.0);
    const auto b = solvePlacements(src, 400, 200, 220, 220, 4.0);
    CHECK(a == b);
}

TEST_CASE("recompose: person/field/tower — both subjects survive, rigid and ordered") {
    // The founding scenario: red 'person' left, blue 'tower' right, flat gray field between,
    // 2:1 -> 1:1. Cropping must lose one of them; recompose keeps both.
    Image src = solid(400, 200, 120, 120, 120);
    const Color8 red{255, 0, 0, 255}, blue{0, 0, 255, 255};
    paintColor(src, 40, 60, 80, 140, red);    // person: 40x80
    paintColor(src, 340, 20, 390, 180, blue); // tower: 50x160
    const std::vector<KeepRegion> regions = {
        {{40, 60, 40, 80}, 0.5, KeepRegion::Source::Auto},
        {{340, 20, 50, 160}, 0.5, KeepRegion::Source::Auto}};
    const RecomposeResult r = recompose(src, 1.0, regions, grayFill);
    REQUIRE(r.ok);
    CHECK(r.image.width == 200);
    CHECK(r.image.height == 200);
    REQUIRE(r.placements.size() == 2);
    // Order preserved: person still left of tower.
    CHECK(r.placements[0].target.center().x < r.placements[1].target.center().x);
    // Rigidity: every subject pixel survives byte-identical (feather touches only the pad band
    // OUTSIDE the snug rects, and the field is flat so blends resolve to pure colors anyway).
    CHECK(countColor(r.image, red) == 40 * 80);
    CHECK(countColor(r.image, blue) == 50 * 160);
    // And the subjects sit where the placements say: probe each snug center.
    const auto probe = [&](const Rect& t) {
        const auto x = static_cast<std::uint32_t>(t.center().x);
        const auto y = static_cast<std::uint32_t>(t.center().y);
        const std::uint8_t* p =
            r.image.rgba.data() + (static_cast<std::size_t>(y) * r.image.width + x) * 4;
        return Color8{p[0], p[1], p[2], p[3]};
    };
    CHECK(probe(r.placements[0].target) == red);
    CHECK(probe(r.placements[1].target) == blue);
}

TEST_CASE("recompose: deterministic and honest about failure") {
    Image src = solid(300, 150, 100, 100, 100);
    paintColor(src, 20, 40, 60, 100, {200, 50, 50, 255});
    const std::vector<KeepRegion> regions = {{{20, 40, 40, 60}, 1.0, KeepRegion::Source::Auto}};
    const RecomposeResult a = recompose(src, 1.0, regions, grayFill);
    const RecomposeResult b = recompose(src, 1.0, regions, grayFill);
    REQUIRE(a.ok);
    CHECK(a.image == b.image);
    // Fail paths report, never crash: no regions / bad aspect / failing filler.
    CHECK_FALSE(recompose(src, 1.0, {}, grayFill).ok);
    CHECK_FALSE(recompose(src, 0.0, regions, grayFill).ok);
    const auto failFill = [](Image&, const std::vector<Rect>&) { return false; };
    const RecomposeResult f = recompose(src, 1.0, regions, failFill);
    CHECK_FALSE(f.ok);
    CHECK(f.detail == "hole fill failed");
    // Infeasible rigid placement is a graceful failure too.
    const std::vector<KeepRegion> tooBig = {
        {{0, 10, 140, 100}, 0.5, KeepRegion::Source::Auto},
        {{160, 10, 140, 100}, 0.5, KeepRegion::Source::Auto}};
    CHECK_FALSE(recompose(src, 1.0, tooBig, grayFill).ok);
}

TEST_CASE("recompose: prepare + assemble == recompose (the nudge split is a pure refactor)") {
    using mosaic::core::retarget::assembleRecompose;
    using mosaic::core::retarget::prepareRecompose;
    using mosaic::core::retarget::RecomposeStaged;
    Image src = solid(400, 200, 120, 120, 120);
    paintColor(src, 40, 60, 80, 140, {255, 0, 0, 255});
    paintColor(src, 340, 20, 390, 180, {0, 0, 255, 255});
    const std::vector<KeepRegion> regions = {
        {{40, 60, 40, 80}, 0.5, KeepRegion::Source::Auto},
        {{340, 20, 50, 160}, 0.5, KeepRegion::Source::Auto}};
    const RecomposeResult whole = recompose(src, 1.0, regions, grayFill);
    const RecomposeStaged staged = prepareRecompose(src, 1.0, regions, grayFill);
    REQUIRE(whole.ok);
    REQUIRE(staged.ok);
    CHECK(staged.targetW == 200);
    CHECK(staged.targetH == 200);
    REQUIRE(staged.placed.size() == 2);
    CHECK(staged.placed[0] == whole.placements[0].target);
    CHECK(assembleRecompose(staged) == whole.image);
    // Failure paths surface through the staged form too, with nothing to assemble.
    const RecomposeStaged bad = prepareRecompose(src, 0.0, regions, grayFill);
    CHECK_FALSE(bad.ok);
    CHECK(assembleRecompose(bad).empty());
}

TEST_CASE("recompose: a nudged placement re-assembles the subject at the new spot") {
    using mosaic::core::retarget::assembleRecompose;
    using mosaic::core::retarget::prepareRecompose;
    using mosaic::core::retarget::RecomposeStaged;
    Image src = solid(400, 200, 120, 120, 120);
    const Color8 red{255, 0, 0, 255};
    paintColor(src, 40, 60, 80, 140, red);
    const std::vector<KeepRegion> regions = {{{40, 60, 40, 80}, 1.0, KeepRegion::Source::Auto}};
    RecomposeStaged staged = prepareRecompose(src, 1.0, regions, grayFill);
    REQUIRE(staged.ok);
    const Image before = assembleRecompose(staged);
    // Nudge the subject 60 px right and re-assemble: same pieces, same background, new spot —
    // and the subject stays byte-identical (rigidity survives the nudge).
    staged.placed[0].x += 60.0;
    staged.placed[0].y += 10.0;
    const Image after = assembleRecompose(staged);
    CHECK(after != before);
    CHECK(countColor(after, red) == 40 * 80);
    const auto probe = [&](const Image& img, double cx, double cy) {
        const auto x = static_cast<std::uint32_t>(cx);
        const auto y = static_cast<std::uint32_t>(cy);
        const std::uint8_t* p =
            img.rgba.data() + (static_cast<std::size_t>(y) * img.width + x) * 4;
        return Color8{p[0], p[1], p[2], p[3]};
    };
    CHECK(probe(after, staged.placed[0].center().x, staged.placed[0].center().y) == red);
    // Re-assembly is repeatable: the staged state is not consumed by assembling.
    CHECK(assembleRecompose(staged) == after);
}

TEST_CASE("recompose: the pad band adapts to the destination — no pasted square") {
    // The user's "they genuinely look pasted in" (2026-07-02): on a background whose colour
    // varies, the feather-only band carried the SOURCE surroundings to the new spot — a visible
    // square. With the step-5 seam blend the band must move toward the DESTINATION background,
    // while the snug core stays byte-identical (rigidity is never traded for blending).
    using mosaic::core::retarget::assembleRecompose;
    using mosaic::core::retarget::prepareRecompose;
    using mosaic::core::retarget::RecomposeStaged;
    Image src = solid(400, 200, 0, 0, 0);
    for (std::uint32_t y = 0; y < 200; ++y) // horizontal luminance ramp: position is visible
        for (std::uint32_t x = 0; x < 400; ++x) {
            const auto v = static_cast<std::uint8_t>(40 + (x * 160) / 399);
            std::uint8_t* p = src.rgba.data() + (static_cast<std::size_t>(y) * 400 + x) * 4;
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    const Color8 red{255, 0, 0, 255};
    paintColor(src, 280, 40, 360, 140, red); // the subject: moves left under a 1:1 ask
    const std::vector<KeepRegion> regions = {{{280, 40, 80, 100}, 1.0, KeepRegion::Source::Auto}};
    RecomposeStaged staged = prepareRecompose(src, 1.0, regions, grayFill);
    REQUIRE(staged.ok);
    const Image feather = assembleRecompose(staged, /*blendSeams=*/false);
    const Image blended = assembleRecompose(staged, /*blendSeams=*/true);
    // Rigidity: the subject is byte-identical in both, blending never enters the core.
    CHECK(countColor(feather, red) == 80 * 100);
    CHECK(countColor(blended, red) == 80 * 100);
    // The band (piece rect minus core) must sit closer to the destination background after the
    // blend than the feathered source-surroundings did.
    const Rect snug = staged.placed[0];
    const auto& piece = staged.pieces[0];
    const long ox = static_cast<long>(std::lround(snug.x - piece.padL));
    const long oy = static_cast<long>(std::lround(snug.y - piece.padT));
    double featherDiff = 0.0, blendedDiff = 0.0;
    for (long y = std::max(0L, oy);
         y < std::min<long>(staged.targetH, oy + static_cast<long>(piece.image.height)); ++y)
        for (long x = std::max(0L, ox);
             x < std::min<long>(staged.targetW, ox + static_cast<long>(piece.image.width)); ++x) {
            if (x >= snug.x && x < snug.right() && y >= snug.y && y < snug.bottom())
                continue; // the core is the subject's, not the band's
            const std::size_t p =
                (static_cast<std::size_t>(y) * staged.targetW + static_cast<std::size_t>(x)) * 4;
            for (int ch = 0; ch < 3; ++ch) {
                const double db = static_cast<double>(blended.rgba[p + ch]) -
                                  staged.background.rgba[p + ch];
                const double df = static_cast<double>(feather.rgba[p + ch]) -
                                  staged.background.rgba[p + ch];
                blendedDiff += db * db;
                featherDiff += df * df;
            }
        }
    CHECK(blendedDiff < featherDiff);
    // And deterministic: the fixed-sweep solve reproduces itself.
    CHECK(assembleRecompose(staged, true) == blended);
}

TEST_CASE("inpaint fill adapter: heals holes with the real engine, deterministic, cancellable") {
    using mosaic::core::retarget::makeInpaintFill;
    // A flat gray field with a loud red hole: after the fill the red must be gone, replaced by
    // something drawn from the surroundings (which are all gray).
    Image imgA = solid(48, 32, 100, 100, 100);
    paintColor(imgA, 18, 12, 30, 22, {255, 0, 0, 255});
    Image imgB = imgA;
    const std::vector<Rect> holes = {{18, 12, 12, 10}};
    const mosaic::core::inpaint::InpaintEngine engine = mosaic::core::inpaint::makeDefaultEngine();
    const auto fill = makeInpaintFill(engine, {});
    REQUIRE(fill(imgA, holes));
    CHECK(countColor(imgA, {255, 0, 0, 255}) == 0);
    // Deterministic: a second run over the same input is byte-identical (project requirement —
    // the recompose pipeline inherits its determinism from the filler).
    REQUIRE(fill(imgB, holes));
    CHECK(imgA == imgB);
    // An empty hole list is a clean no-op; a pre-cancelled run reports failure.
    Image untouched = solid(16, 16, 10, 20, 30);
    const Image copy = untouched;
    CHECK(fill(untouched, {}));
    CHECK(untouched == copy);
    std::atomic<bool> cancel{true};
    const auto cancelled = makeInpaintFill(engine, {}, &cancel);
    Image victim = solid(48, 32, 100, 100, 100);
    CHECK_FALSE(cancelled(victim, holes));
}
