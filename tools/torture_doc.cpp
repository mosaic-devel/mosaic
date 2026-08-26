// torture_doc -- emit ONE deliberately punishing .mosaic file, for measuring where Mosaic's time
// goes on a document nobody would call unreasonable.
//
// The S60 plan (docs/s60-performance-plan.md) benchmarks synthetic canvases: `--bench` composites
// N flat layers at a size. That measures the compositor and nothing else. The costs the README's
// "performance degrades on large documents" actually names are the ones a synthetic canvas has no
// way to reach -- text shaping and extrusion at real headline sizes, the layer-effects pass over a
// large alpha, a spatial adjustment reading a 40 MP backdrop, vector rasterisation of live
// booleans, and the tree walk over a stack deep enough that per-layer overhead stops being noise.
//
// So this builds the document instead. Every element is one the app really creates:
//
//   * a 5152x7728 (39.8 MP) photograph as the base raster -- the canvas size is the PHOTO's, which
//     is how a real document gets big: nobody types 5152 into File->New
//   * ~70 layers over 6 nested groups, with masks, clipping, blend modes and per-layer transforms
//   * 3D text at headline size (the extrude mesher's cost is superlinear in glyph count x depth
//     x bevel segments) with canvas reflection ON -- the ExtrudeEnv snapshot path
//   * a full layer-effects stack on that text: stacked drop shadows, glows, overlays, satin,
//     bevel and concentric strokes, several with a reach large enough to grow effectsBounds well
//     past the content box
//   * spatial adjustment layers (GaussianBlur, ShadowsHighlights, HighPass) -- the compositor's
//     blur branch, over the full canvas
//   * live boolean vector compounds, a many-node hand-built Path, dithered multi-stop gradients
//   * ~10 appended saves of genuine history, each a real edit
//
// It is a MEASUREMENT FIXTURE, not a test: nothing here asserts a time. Open it under
// `--profile` (or MOSAIC_PROFILE=1) and read the table; run it again under `--cpu` to separate
// the compute lanes from what only presentation can explain.
//
// Usage: torture_doc <output.mosaic> [--photo <path.jpg>] [--saves N]
//
// With no --photo the base raster is a synthesised gradient of the same size, so the tool still
// produces a valid fixture on a machine that does not have the photo.

#include "common/exif.hpp"
#include "common/geometry.hpp"
#include "common/image.hpp"
#include "core/blend_mode.hpp"
#include "core/document.hpp"
#include "core/layer.hpp"
#include "core/layer_effects.hpp"
#include "core/text/extrude.hpp"
#include "core/text/text_model.hpp"
#include "core/vector/geometry.hpp"
#include "core/vector/object.hpp"
#include "core/vector/paint.hpp"
#include "io/io.hpp"
#include "io/mosaic/docio.hpp"
#include "io/mosaic/file.hpp"
#include "io/mosaic/journal_session.hpp"
#include "io/mosaic/save.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace io = mosaic::io::native;
namespace core = mosaic::core;
namespace vec = mosaic::core::vec;
namespace text = mosaic::core::text;
namespace common = mosaic::common;
namespace fs = std::filesystem;

namespace {

using common::Affine2D;
using common::ColorF;
using common::Vec2;

int g_failures = 0;

// ---- progress -------------------------------------------------------------------------------
// The tool serialises a 40 MP document a couple of dozen times; silence for minutes reads as a
// hang. Every stage announces itself and reports its own wall clock.
class Stage {
public:
    explicit Stage(std::string what) : m_what(std::move(what)) {
        std::printf("  %-46s", m_what.c_str());
        std::fflush(stdout);
    }
    ~Stage() {
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_start)
                .count();
        std::printf("%8.0f ms\n", ms);
        std::fflush(stdout);
    }

private:
    std::string m_what;
    std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
};

void fail(const std::string& what) {
    std::printf("  FAIL  %s\n", what.c_str());
    ++g_failures;
}

// ---- pixel helpers --------------------------------------------------------------------------

std::uint8_t* pixel(common::Image& img, int x, int y) {
    return img.rgba.data() + (static_cast<std::size_t>(y) * img.width + x) * 4;
}

// A cheap deterministic hash -- the noise/stipple layers want texture that survives compression
// rather than a flat fill the tile store would collapse to nothing.
std::uint32_t hash2(int x, int y) {
    std::uint32_t h =
        static_cast<std::uint32_t>(x) * 0x9E3779B1u ^ static_cast<std::uint32_t>(y) * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0xC2B2AE3Du;
    h ^= h >> 13;
    return h;
}

void paintDisc(common::Image& img, int cx, int cy, int radius, ColorF c, float feather = 0.0f) {
    const int w = static_cast<int>(img.width);
    const int h = static_cast<int>(img.height);
    const double r = radius;
    for (int y = std::max(0, cy - radius); y < std::min(h, cy + radius + 1); ++y)
        for (int x = std::max(0, cx - radius); x < std::min(w, cx + radius + 1); ++x) {
            const double dx = x - cx, dy = y - cy;
            const double d = std::sqrt(dx * dx + dy * dy);
            if (d > r)
                continue;
            const double t = feather > 0.0f ? std::clamp((r - d) / (r * feather), 0.0, 1.0) : 1.0;
            std::uint8_t* px = pixel(img, x, y);
            const double a = c.a * t;
            px[0] = static_cast<std::uint8_t>(
                std::clamp(px[0] * (1 - a) + c.r * 255.0 * a, 0.0, 255.0));
            px[1] = static_cast<std::uint8_t>(
                std::clamp(px[1] * (1 - a) + c.g * 255.0 * a, 0.0, 255.0));
            px[2] = static_cast<std::uint8_t>(
                std::clamp(px[2] * (1 - a) + c.b * 255.0 * a, 0.0, 255.0));
            px[3] = static_cast<std::uint8_t>(std::clamp(px[3] + 255.0 * a, 0.0, 255.0));
        }
}

// ---- canvas ----------------------------------------------------------------------------------
// Set from the photo when one is supplied; the fallback keeps the same shape so the fixture is
// the same fixture either way.
int g_canvasW = 5152;
int g_canvasH = 7728;

// ---- paints ------------------------------------------------------------------------------------

vec::Gradient ramp(vec::GradientType type, std::initializer_list<ColorF> colors,
                   vec::DitherKind dither = vec::DitherKind::BlueNoise) {
    vec::Gradient g;
    g.type = type;
    g.dither = dither; // a ramp this wide bands hard at 8 bit; dithering is the real-world setting
    const std::size_t n = colors.size();
    std::size_t i = 0;
    for (const ColorF& c : colors) {
        vec::GradientStop s;
        s.offset = n > 1 ? static_cast<double>(i) / (n - 1) : 0.0;
        s.color = c;
        s.midpoint = (i % 2 == 0) ? 0.42 : 0.61; // off-centre: the non-linear per-segment path
        g.stops.push_back(s);
        ++i;
    }
    return g;
}

// ---- text --------------------------------------------------------------------------------------

// The headline solid. Deliberately expensive on every axis the mesher charges for: glyph count,
// depth, a curved bevel with a high ring count on BOTH caps, three lights, per-run materials, and
// the canvas mirror -- which is the path that needs an ExtrudeEnv snapshot of the composite below.
text::TextBlock headlineBlock(const std::string& words, float sizePx, float depth) {
    text::CharStyle base;
    base.font.family = "Inter";
    base.font.weight = 900.0f;
    base.sizePx = sizePx;
    base.tracking = -18.0f; // tight display tracking: real kerning work, not a monospace grid
    base.features = {"liga", "kern", "ss01"};
    base.kerning = text::Kerning::Optical; // the shape-derived path, not the font's own pairs
    base.setSolidFill(ColorF{0.96f, 0.93f, 0.86f, 1.0f});

    text::TextBlock b = text::makeBlock(words, base);

    text::Extrude ex;
    ex.depth = depth;
    ex.bevelFront.profile = text::Bevel::Profile::Round;
    ex.bevelFront.size = depth * 0.28f;
    ex.bevelFront.segments = 12; // 12 rings per cap, both caps: the mesher's inner loop
    ex.bevelBack.profile = text::Bevel::Profile::Concave;
    ex.bevelBack.size = depth * 0.18f;
    ex.bevelBack.segments = 10;
    ex.material.albedo = {0.86f, 0.71f, 0.24f, 1.0f};
    ex.material.metalness = 0.85f;
    ex.material.roughness = 0.22f;
    ex.perspective = 26.0f;
    ex.lightingEnabled = true;
    ex.lights = {text::kDefaultKeyLight,
                 text::Light{{-0.6, 0.2, -0.75}, {0.55f, 0.72f, 1.0f, 1.0f}, 0.7f},
                 text::Light{{0.1, -0.9, -0.4}, {1.0f, 0.82f, 0.55f, 1.0f}, 0.45f}};
    ex.ambient = {0.18f, 0.19f, 0.24f, 1.0f};
    ex.reflectCanvas = true;     // needs the composite-below snapshot every time it re-renders
    ex.reflectSidesOnly = false; // ...on the caps too, which is the expensive half
    ex.overlayWrapSides = true;  // the S30-e overlay map wraps onto every wall and bevel texel
    // Per-run materials partition the mesh into index ranges -- one draw per material.
    ex.runMaterials[0] = text::Material{{0.92f, 0.35f, 0.28f, 1.0f}, 0.4f, 0.35f};
    b.extrude = ex;
    return b;
}

// The full effect stack, sized so several effects reach well outside the alpha edge (which is what
// grows effectsBounds and therefore the isolated buffer the compositor has to allocate).
core::LayerEffects headlineEffects() {
    core::LayerEffects fx;
    fx.fillOpacity = 0.94f;

    for (int i = 0; i < 3; ++i) {
        core::ShadowEffect sh;
        sh.enabled = true;
        sh.color = ColorF{0.02f, 0.01f, 0.05f, 1.0f};
        sh.opacity = 0.55f - 0.12f * i;
        sh.blend = core::BlendMode::Multiply;
        sh.angleDeg = 118.0f + 24.0f * i;
        sh.distance = 18.0f + 34.0f * i;
        sh.size = 26.0f + 46.0f * i; // the outermost reaches ~250 px past the glyphs
        sh.spread = 4.0f * i;
        fx.dropShadows.push_back(sh);
    }

    fx.outerGlow.enabled = true;
    fx.outerGlow.paint = ramp(vec::GradientType::Radial,
                              {ColorF{1.0f, 0.78f, 0.35f, 1.0f}, ColorF{0.9f, 0.2f, 0.4f, 0.0f}});
    fx.outerGlow.size = 120.0f;
    fx.outerGlow.choke = 12.0f;
    fx.outerGlow.opacity = 0.8f;

    fx.gradientOverlay.enabled = true;
    fx.gradientOverlay.paint =
        ramp(vec::GradientType::Conic,
             {ColorF{1.0f, 0.95f, 0.7f, 1.0f}, ColorF{0.55f, 0.3f, 0.9f, 1.0f},
              ColorF{0.2f, 0.8f, 0.85f, 1.0f}, ColorF{1.0f, 0.95f, 0.7f, 1.0f}});
    fx.gradientOverlay.blend = core::BlendMode::Overlay;
    fx.gradientOverlay.opacity = 0.65f;

    fx.colorOverlay.enabled = true;
    fx.colorOverlay.paint = vec::SolidPaint{ColorF{0.1f, 0.05f, 0.2f, 1.0f}};
    fx.colorOverlay.blend = core::BlendMode::SoftLight;
    fx.colorOverlay.opacity = 0.4f;

    fx.satin.enabled = true;
    fx.satin.size = 42.0f;
    fx.satin.distance = 26.0f;
    fx.satin.angleDeg = 31.0f;

    for (int i = 0; i < 2; ++i) {
        core::ShadowEffect in;
        in.enabled = true;
        in.color = ColorF{0.0f, 0.0f, 0.0f, 1.0f};
        in.opacity = 0.5f;
        in.blend = core::BlendMode::Multiply;
        in.angleDeg = 300.0f - 40.0f * i;
        in.distance = 8.0f + 10.0f * i;
        in.size = 18.0f + 22.0f * i;
        fx.innerShadows.push_back(in);
    }

    fx.innerGlow.enabled = true;
    fx.innerGlow.paint = vec::SolidPaint{ColorF{1.0f, 0.9f, 0.6f, 1.0f}};
    fx.innerGlow.size = 34.0f;
    fx.innerGlow.source = core::GlowEffect::Source::Edge;

    fx.bevel.enabled = true;
    fx.bevel.style = core::BevelEffect::Style::PillowEmboss;
    fx.bevel.size = 28.0f;
    fx.bevel.depth = 1.8f;
    fx.bevel.soften = 6.0f;

    // Three concentric strokes -- reach is the SUM of the outward widths (effectsOutwardReach).
    const ColorF ring[3] = {
        {0.05f, 0.04f, 0.08f, 1.0f}, {0.95f, 0.9f, 0.8f, 1.0f}, {0.85f, 0.25f, 0.35f, 1.0f}};
    for (int i = 0; i < 3; ++i) {
        core::StrokeEffect s;
        s.enabled = true;
        s.width = 10.0f + 6.0f * i;
        s.align = core::StrokeEffect::Align::Outside;
        s.paint = vec::SolidPaint{ring[i]};
        fx.strokes.push_back(s);
    }
    return fx;
}

// The body copy for the Area block. Long enough that wrapping, hyphenation and justification are
// real work rather than one line's worth.
std::string bodyCopy() {
    static const char* kPara =
        "Every mainstream editor format stores your work with no error correction and no history. "
        "A corrupted byte in the wrong place costs you the file, and your undo stack dies with the "
        "process. This document exists to be slow: it is the shape of a real job -- a photograph "
        "far larger than the screen, type set at a size that means something in print, effects "
        "that reach past the glyphs they belong to, and a stack deep enough that the walk itself "
        "costs. If this opens, edits and saves without a stall, nothing smaller will.\n";
    std::string s;
    for (int i = 0; i < 9; ++i)
        s += kPara;
    return s;
}

// ---- vector ------------------------------------------------------------------------------------

// A hand-built path of `lobes` rounded lobes -- a many-node contour, which is what makes the
// flattener and the scanline rasteriser work rather than the shape cache.
vec::Path rosette(int lobes, double outer, double inner) {
    vec::SubPath sp;
    sp.closed = true;
    const int n = lobes * 2;
    for (int i = 0; i < n; ++i) {
        const double a = 2.0 * M_PI * i / n;
        const double r = (i % 2 == 0) ? outer : inner;
        const Vec2 p{r * std::cos(a), r * std::sin(a)};
        // Handles tangent to the circle: smooth lobes, no straight segments to shortcut.
        const double t = r * 0.55 * (2.0 * M_PI / n);
        const Vec2 tangent{-std::sin(a) * t, std::cos(a) * t};
        vec::Node nd;
        nd.anchor = p;
        nd.inHandle = {p.x - tangent.x, p.y - tangent.y};
        nd.outHandle = {p.x + tangent.x, p.y + tangent.y};
        nd.type = vec::Node::Type::Smooth;
        sp.nodes.push_back(nd);
    }
    vec::Path path;
    path.subpaths.push_back(std::move(sp));
    // A counter-rotating inner contour: EvenOdd then has two contours to resolve per scanline.
    vec::SubPath hole;
    hole.closed = true;
    for (int i = 0; i < 48; ++i) {
        const double a = -2.0 * M_PI * i / 48;
        const double r = inner * 0.55;
        vec::Node nd;
        nd.anchor = {r * std::cos(a), r * std::sin(a)};
        nd.inHandle = nd.anchor;
        nd.outHandle = nd.anchor;
        hole.nodes.push_back(nd);
    }
    path.subpaths.push_back(std::move(hole));
    path.fillRule = vec::FillRule::EvenOdd;
    return path;
}

// A live boolean compound: the operands stay editable, so flatten() resolves the op on every
// rasterisation rather than baking once.
vec::Object booleanBadge() {
    vec::Object host;
    vec::BooleanCompound comp;
    comp.op = vec::BoolOp::Exclude;

    vec::Object a;
    vec::StarShape star;
    star.points = 24;
    star.outerRadius = 620;
    star.innerRadius = 300;
    star.pointRadius = 26;
    star.valleyRadius = 18;
    a.geometry = vec::ParametricShape{star};
    comp.children.push_back(std::move(a));

    vec::Object b;
    vec::RingShape ring;
    b.geometry = vec::ParametricShape{ring};
    comp.children.push_back(std::move(b));

    vec::Object c;
    c.geometry = rosette(11, 480.0, 250.0);
    comp.children.push_back(std::move(c));

    host.geometry = std::move(comp);
    host.fill = ramp(vec::GradientType::Radial,
                     {ColorF{0.98f, 0.86f, 0.45f, 0.92f}, ColorF{0.85f, 0.28f, 0.42f, 0.85f},
                      ColorF{0.24f, 0.16f, 0.42f, 0.75f}});
    host.stroke.enabled = true;
    host.stroke.width = 14.0;
    host.stroke.align = vec::StrokeAlign::Outside;
    host.stroke.join = vec::LineJoin::Round;
    host.stroke.cap = vec::LineCap::Round;
    host.stroke.dashArray = {48.0, 22.0, 8.0, 22.0}; // a dashed OUTSIDE stroke: the slow one
    host.stroke.paint = vec::SolidPaint{ColorF{0.06f, 0.05f, 0.1f, 1.0f}};
    return host;
}

// ---- masks ---------------------------------------------------------------------------------

// A soft radial mask sheet at `div`-reduced resolution -- masks ride their own grid, and a sheet
// whose resolution differs from the layer's is the case that exercises the placement transform.
core::RasterMask radialMask(std::uint32_t w, std::uint32_t h, double cxN, double cyN, double rN) {
    core::RasterMask m(w, h, 0);
    const double cx = cxN * w, cy = cyN * h, r = rN * std::max(w, h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const double dx = x - cx, dy = y - cy;
            const double d = std::sqrt(dx * dx + dy * dy) / r;
            const double v = std::clamp(1.0 - d * d, 0.0, 1.0);
            m.coverage[static_cast<std::size_t>(y) * w + x] = static_cast<std::uint8_t>(v * 255.0);
        }
    return m;
}

// ---- the document ------------------------------------------------------------------------------

template <class L> L* findLayer(core::Document& doc, const std::string& name) {
    // Depth-first: the tree is nested, and the history edits address layers inside groups.
    struct Walk {
        static core::Layer* go(core::GroupLayer& g, const std::string& want) {
            for (const auto& c : g.children()) {
                if (c->name() == want)
                    return c.get();
                if (auto* sub = c->as<core::GroupLayer>())
                    if (core::Layer* hit = go(*sub, want))
                        return hit;
            }
            return nullptr;
        }
    };
    return dynamic_cast<L*>(Walk::go(doc.root(), name));
}

std::unique_ptr<core::Document> buildDocument(const std::string& photoPath) {
    // The photo first: the canvas takes ITS size, which is how a document this big really happens.
    std::optional<mosaic::io::LoadedImage> photo;
    if (!photoPath.empty()) {
        Stage s("loading the photograph");
        std::string err;
        photo = mosaic::io::loadImageWithMetadata(photoPath, &err);
        if (!photo.has_value())
            std::printf("\n  (no photo: %s -- synthesising the base raster)\n", err.c_str());
    }
    if (photo.has_value() && !photo->image.empty()) {
        g_canvasW = static_cast<int>(photo->image.width);
        g_canvasH = static_cast<int>(photo->image.height);
    }
    const int W = g_canvasW, H = g_canvasH;
    std::printf("  canvas %dx%d (%.1f MP)\n", W, H, W * double(H) / 1e6);

    auto doc = std::make_unique<core::Document>(static_cast<std::uint32_t>(W),
                                                static_cast<std::uint32_t>(H),
                                                core::ColorSpace::DisplayP3, core::Precision::F32);
    doc->setTitle("Harbinger");
    doc->setUuid(io::mintDocumentUuid());
    doc->setDpi(300.0);

    // ---- 1. the base photograph ----------------------------------------------------------------
    {
        Stage s("base photograph layer");
        auto base = doc->makeRaster("Photograph", static_cast<std::uint32_t>(W),
                                    static_cast<std::uint32_t>(H));
        if (photo.has_value() && !photo->image.empty()) {
            base->image() = std::move(photo->image);
            base->setExif(photo->exif);
        } else {
            auto& img = base->image();
            for (int y = 0; y < H; ++y) {
                const double t = static_cast<double>(y) / (H - 1);
                for (int x = 0; x < W; ++x) {
                    const double u = static_cast<double>(x) / (W - 1);
                    std::uint8_t* px = pixel(img, x, y);
                    const std::uint32_t n = hash2(x, y) & 0x0F;
                    px[0] = static_cast<std::uint8_t>(20 + 200 * t * (0.4 + 0.6 * u) + n);
                    px[1] = static_cast<std::uint8_t>(30 + 150 * (1 - t) + n);
                    px[2] = static_cast<std::uint8_t>(90 + 120 * u + n);
                    px[3] = 255;
                }
            }
        }
        base->invalidateContentBounds();
        base->setLocked(true); // a locked backplate, as a real job would have it
        doc->root().addOnTop(std::move(base));
    }

    // ---- 2. the grade: adjustment layers, two of them SPATIAL -----------------------------------
    //
    // ⚠ AT THE ROOT, NOT IN A GROUP, and that is load-bearing rather than tidy. Mosaic's groups are
    // ISOLATED: renderLayerRaw composites a group's children into a fresh transparent buffer, and
    // an adjustment grades the accumulator it sits in. So an adjustment inside a group grades that
    // group's content and nothing below the group -- and a group holding ONLY adjustments has an
    // empty accumulator (GroupLayer::contentBounds unions its children's, and AdjustmentLayer has
    // none), so it grades nothing at all and renders transparent.
    //
    // That is correct in this model, but it made the first version of this fixture quietly INERT:
    // six adjustment layers in a "Grade" folder that never touched a pixel, while the profiler's
    // adjustment rows were really measuring the layer panel's thumbnail previews. A performance
    // fixture whose most expensive layers do nothing teaches the wrong lesson.
    {
        Stage s("grade (adjustments over the photograph)");
        core::GroupLayer& grade = doc->root();

        auto lift = doc->makeAdjustment("Levels", core::AdjustmentKind::Levels);
        lift->params() = {{"inBlack", 6.0},
                          {"inWhite", 246.0},
                          {"gamma", 1.12},
                          {"outBlack", 4.0},
                          {"outWhite", 252.0}};
        grade.addOnTop(std::move(lift));

        auto curves = doc->makeAdjustment("Curves", core::AdjustmentKind::Curves);
        curves->params() = {{"shadows", -0.06}, {"midtones", 0.08}, {"highlights", 0.04}};
        grade.addOnTop(std::move(curves));

        auto hsl = doc->makeAdjustment("Hue/Saturation", core::AdjustmentKind::HueSaturation);
        hsl->params() = {{"hue", -6.0}, {"saturation", 14.0}, {"lightness", -3.0}};
        grade.addOnTop(std::move(hsl));

        // SPATIAL: reads the backdrop's neighbourhood over the whole canvas.
        auto sh =
            doc->makeAdjustment("Shadows/Highlights", core::AdjustmentKind::ShadowsHighlights);
        sh->params() = {{"shadowAmount", 38.0},
                        {"shadowRadius", 180.0},
                        {"highlightAmount", 24.0},
                        {"highlightRadius", 140.0}};
        grade.addOnTop(std::move(sh));

        // SPATIAL, masked: a large-radius blur confined to a soft ellipse -- the region machinery
        // has to grow the read extent by the blur's reach and then fold the mask.
        auto blur = doc->makeAdjustment("Depth blur", core::AdjustmentKind::GaussianBlur);
        blur->params() = {{"radius", 96.0}};
        blur->setMask(radialMask(static_cast<std::uint32_t>(W / 8),
                                 static_cast<std::uint32_t>(H / 8), 0.5, 0.34, 0.62));
        blur->setOpacity(0.85f);
        grade.addOnTop(std::move(blur));

        auto vib = doc->makeAdjustment("Vibrance", core::AdjustmentKind::Vibrance);
        vib->params() = {{"vibrance", 26.0}, {"saturation", -4.0}};
        grade.addOnTop(std::move(vib));
    }

    // ---- 3. texture: full-canvas raster passes with blend modes and masks ----------------------
    {
        Stage s("texture group (raster passes)");
        auto tex = doc->makeGroup("Texture");

        auto grain = doc->makeRaster("Film grain", static_cast<std::uint32_t>(W),
                                     static_cast<std::uint32_t>(H));
        {
            auto& img = grain->image();
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    const std::uint32_t n = hash2(x, y);
                    std::uint8_t* px = pixel(img, x, y);
                    const std::uint8_t v = static_cast<std::uint8_t>(n & 0xFF);
                    px[0] = px[1] = px[2] = v;
                    px[3] = static_cast<std::uint8_t>(28 + ((n >> 8) & 0x1F));
                }
        }
        grain->setBlendMode(core::BlendMode::Overlay);
        grain->setOpacity(0.42f);
        grain->invalidateContentBounds();
        tex->addOnTop(std::move(grain));

        // Light leaks: several mid-size layers, each a feathered disc, each on its own blend mode.
        const core::BlendMode leakBlends[] = {
            core::BlendMode::Screen,    core::BlendMode::ColorDodge, core::BlendMode::Lighten,
            core::BlendMode::SoftLight, core::BlendMode::HardLight,  core::BlendMode::Overlay};
        for (int i = 0; i < 6; ++i) {
            const std::uint32_t lw = static_cast<std::uint32_t>(W / 2);
            const std::uint32_t lh = static_cast<std::uint32_t>(H / 4);
            auto leak = doc->makeRaster("Light leak " + std::to_string(i + 1), lw, lh);
            auto& img = leak->image();
            const ColorF tint{0.5f + 0.08f * i, 0.35f + 0.1f * ((i + 2) % 5), 0.9f - 0.09f * i,
                              0.7f};
            paintDisc(img, static_cast<int>(lw) / 2, static_cast<int>(lh) / 2,
                      static_cast<int>(lh) / 2 - 4, tint, 0.9f);
            leak->invalidateContentBounds();
            leak->setBlendMode(leakBlends[i]);
            leak->setOpacity(0.35f + 0.06f * i);
            leak->setTransform(Affine2D::trs({W * (0.08 + 0.14 * i), H * (0.05 + 0.13 * i)},
                                             0.35 * i, {1.1, 0.85}));
            tex->addOnTop(std::move(leak));
        }

        // A dust/scratch pass with its own soft mask, clipped to the leak below it.
        auto dust = doc->makeRaster("Dust", static_cast<std::uint32_t>(W / 2),
                                    static_cast<std::uint32_t>(H / 2));
        {
            auto& img = dust->image();
            for (int y = 0; y < static_cast<int>(img.height); ++y)
                for (int x = 0; x < static_cast<int>(img.width); ++x) {
                    const std::uint32_t n = hash2(x * 3, y * 7);
                    if ((n & 0x3FF) > 6)
                        continue;
                    std::uint8_t* px = pixel(img, x, y);
                    px[0] = px[1] = px[2] = 240;
                    px[3] = 180;
                }
        }
        dust->invalidateContentBounds();
        dust->setClipToBelow(true);
        dust->setMask(radialMask(static_cast<std::uint32_t>(W / 16),
                                 static_cast<std::uint32_t>(H / 16), 0.4, 0.6, 0.8));
        tex->addOnTop(std::move(dust));

        // The group itself carries effects: the compositor must render its children into an
        // isolated buffer sized to effectsBounds before it can apply them.
        core::LayerEffects gfx;
        gfx.outerGlow.enabled = true;
        gfx.outerGlow.paint = vec::SolidPaint{ColorF{0.9f, 0.7f, 0.35f, 1.0f}};
        gfx.outerGlow.size = 64.0f;
        gfx.colorOverlay.enabled = true;
        gfx.colorOverlay.paint = vec::SolidPaint{ColorF{0.25f, 0.18f, 0.4f, 1.0f}};
        gfx.colorOverlay.blend = core::BlendMode::Color;
        gfx.colorOverlay.opacity = 0.22f;
        tex->setEffects(std::move(gfx));
        tex->setOpacity(0.9f);
        doc->root().addOnTop(std::move(tex));
    }

    // ---- 4. vector ------------------------------------------------------------------------------
    core::LayerId pathLayerId = 0;
    {
        Stage s("vector group");
        auto vg = doc->makeGroup("Vector");

        // The text-on-a-path host, kept first so its id is stable for the PathFit reference.
        auto rail = doc->makeVector("Type rail");
        {
            vec::Object o;
            vec::EllipseShape e;
            e.radii = {W * 0.36, H * 0.16};
            o.geometry = vec::ParametricShape{e};
            o.fill = vec::NoPaint{};
            o.stroke.enabled = true;
            o.stroke.width = 3.0;
            o.stroke.paint = vec::SolidPaint{ColorF{1, 1, 1, 0.18f}};
            rail->setObject(std::move(o));
        }
        rail->setTransform(Affine2D::trs({W * 0.5, H * 0.30}, 0.0, {1, 1}));
        pathLayerId = rail->id();
        vg->addOnTop(std::move(rail));

        auto badge = doc->makeVector("Boolean badge");
        badge->setObject(booleanBadge());
        badge->setTransform(Affine2D::trs({W * 0.74, H * 0.18}, 0.4, {1.4, 1.4}));
        badge->setBlendMode(core::BlendMode::Screen);
        vg->addOnTop(std::move(badge));

        auto rose = doc->makeVector("Rosette");
        {
            vec::Object o;
            o.geometry = rosette(29, 900.0, 520.0);
            o.fill = ramp(vec::GradientType::Conic,
                          {ColorF{0.98f, 0.42f, 0.3f, 0.55f}, ColorF{0.3f, 0.75f, 0.95f, 0.55f},
                           ColorF{0.95f, 0.85f, 0.4f, 0.55f}, ColorF{0.98f, 0.42f, 0.3f, 0.55f}});
            o.stroke.enabled = true;
            o.stroke.width = 9.0;
            o.stroke.align = vec::StrokeAlign::Inside;
            o.stroke.paint =
                ramp(vec::GradientType::Linear, {ColorF{1, 1, 1, 0.9f}, ColorF{0, 0, 0, 0.5f}});
            o.paintOrder = vec::Object::PaintOrder::StrokeThenFill;
            rose->setObject(std::move(o));
        }
        rose->setTransform(Affine2D::trs({W * 0.28, H * 0.72}, 0.2, {1.6, 1.6}));
        rose->setOpacity(0.7f);
        vg->addOnTop(std::move(rose));

        // A gallery of parametric shapes -- cheap individually, but they make the tree walk real.
        for (int i = 0; i < 14; ++i) {
            auto sh = doc->makeVector("Shape " + std::to_string(i + 1));
            vec::Object o;
            switch (i % 7) {
            case 0: {
                vec::StarShape st;
                st.points = 5 + i;
                st.outerRadius = 180 + 12 * i;
                st.innerRadius = 80 + 6 * i;
                st.pointRadius = 8;
                o.geometry = vec::ParametricShape{st};
                break;
            }
            case 1: {
                vec::PolygonShape p;
                p.sides = 3 + (i % 9);
                p.radius = 200 + 10 * i;
                p.cornerRadius = 24;
                o.geometry = vec::ParametricShape{p};
                break;
            }
            case 2: {
                vec::HeartShape h;
                o.geometry = vec::ParametricShape{h};
                break;
            }
            case 3: {
                vec::ArrowShape a;
                o.geometry = vec::ParametricShape{a};
                break;
            }
            case 4: {
                vec::BannerShape b;
                o.geometry = vec::ParametricShape{b};
                break;
            }
            case 5: {
                vec::CalloutShape c;
                o.geometry = vec::ParametricShape{c};
                break;
            }
            default: {
                vec::RectShape r =
                    vec::RectShape::uniform({320, 220}, 40, vec::CornerStyle::Inverse);
                o.geometry = vec::ParametricShape{r};
                break;
            }
            }
            o.fill = ramp(vec::GradientType::Linear,
                          {ColorF{0.2f + 0.05f * (i % 6), 0.5f, 0.9f - 0.04f * i, 0.8f},
                           ColorF{0.95f, 0.4f + 0.03f * i, 0.25f, 0.8f}});
            o.stroke.enabled = (i % 3) != 0;
            o.stroke.width = 4.0 + i % 5;
            o.stroke.paint = vec::SolidPaint{ColorF{0.05f, 0.05f, 0.08f, 0.9f}};
            o.stroke.dashArray = (i % 4 == 0) ? std::vector<double>{18, 10} : std::vector<double>{};
            sh->setObject(std::move(o));
            sh->setTransform(
                Affine2D::trs({W * (0.10 + 0.06 * (i % 13)), H * (0.40 + 0.035 * (i % 15))},
                              0.15 * i, {1.0 + 0.05 * i, 1.0 + 0.05 * i}));
            sh->setOpacity(0.55f + 0.03f * (i % 8));
            sh->setBlendMode(i % 5 == 0 ? core::BlendMode::Multiply : core::BlendMode::Normal);
            if (i % 4 == 1) {
                core::LayerEffects sfx;
                core::ShadowEffect sd;
                sd.enabled = true;
                sd.size = 30.0f;
                sd.distance = 16.0f;
                sfx.dropShadows.push_back(sd);
                sh->setEffects(std::move(sfx));
            }
            vg->addOnTop(std::move(sh));
        }
        doc->root().addOnTop(std::move(vg));
    }

    // ---- 5. typography
    // ---------------------------------------------------------------------------
    {
        Stage s("typography group");
        auto tg = doc->makeGroup("Typography");

        // The headline: 3D, huge, fully effected.
        auto title = doc->makeText("Headline");
        title->setAutoNamed(false);
        title->setBlock(headlineBlock("HARBINGER", static_cast<float>(H) * 0.085f,
                                      static_cast<float>(H) * 0.014f));
        title->setEffects(headlineEffects());
        title->setTransform(Affine2D::trs({W * 0.06, H * 0.50}, -0.04, {1.0, 1.0}));
        tg->addOnTop(std::move(title));

        // A second solid, bent along an arc -- bend composes with 3D, so the warped outlines feed
        // the mesher.
        auto sub = doc->makeText("Subhead");
        sub->setAutoNamed(false);
        {
            text::TextBlock b = headlineBlock("OF A SINGLE FILE", static_cast<float>(H) * 0.032f,
                                              static_cast<float>(H) * 0.005f);
            b.bend = -0.55f;
            b.extrude->reflectSidesOnly = true;
            b.extrude->runMaterials.clear();
            sub->setBlock(std::move(b));
        }
        {
            core::LayerEffects fx;
            core::ShadowEffect sd;
            sd.enabled = true;
            sd.size = 40.0f;
            sd.distance = 22.0f;
            sd.opacity = 0.6f;
            fx.dropShadows.push_back(sd);
            fx.outerGlow.enabled = true;
            fx.outerGlow.size = 70.0f;
            fx.outerGlow.paint = vec::SolidPaint{ColorF{0.4f, 0.7f, 1.0f, 1.0f}};
            sub->setEffects(std::move(fx));
        }
        sub->setTransform(Affine2D::trs({W * 0.09, H * 0.58}, 0.0, {1, 1}));
        tg->addOnTop(std::move(sub));

        // Area text: wrapping + justification + hyphenation over a lot of copy.
        auto body = doc->makeText("Body copy");
        body->setAutoNamed(false);
        {
            text::CharStyle cs;
            cs.font.family = "Noto Serif";
            cs.sizePx = static_cast<float>(H) * 0.0085f;
            cs.setSolidFill(ColorF{0.93f, 0.93f, 0.95f, 1.0f});
            text::TextBlock b = text::makeBlock(bodyCopy(), cs, text::TextFrame::Area);
            b.areaSize = {W * 0.44, H * 0.20};
            b.aa = text::AntiAlias::Subpixel;
            for (auto& p : b.paragraphs) {
                p.align = text::Paragraph::Align::Justify;
                p.hyphenate = true;
                p.language = "en-GB";
                p.leading = 1.42f;
                p.spaceAfter = cs.sizePx * 0.5f;
                p.indentFirst = cs.sizePx * 1.5f;
            }
            // Per-run styling: an italic emphasis and a coloured lead-in in every paragraph, so the
            // run list is long and shaping cannot batch the block as one run.
            for (std::size_t at = 0; at + 90 < b.utf8.size(); at += 240)
                text::mutateStyleRange(b, at, at + 60, [](text::CharStyle& c) {
                    c.font.italic = true;
                    c.tracking = 12.0f;
                    c.setSolidFill(ColorF{0.98f, 0.75f, 0.4f, 1.0f});
                });
            body->setBlock(std::move(b));
        }
        body->setTransform(Affine2D::trs({W * 0.09, H * 0.63}, 0.0, {1, 1}));
        tg->addOnTop(std::move(body));

        // Vertical writing mode, upright orientation -- the vertical layout path.
        auto vert = doc->makeText("Vertical");
        vert->setAutoNamed(false);
        {
            text::CharStyle cs;
            cs.font.family = "Noto Sans CJK JP";
            cs.sizePx = static_cast<float>(H) * 0.016f;
            cs.setSolidFill(ColorF{1.0f, 0.85f, 0.5f, 1.0f});
            text::TextBlock b = text::makeBlock("縦書きのテキスト\n重い文書の試験", cs);
            b.writingMode = text::WritingMode::VerticalRL;
            b.orientation = text::TextOrientation::Upright;
            vert->setBlock(std::move(b));
        }
        vert->setTransform(Affine2D::trs({W * 0.90, H * 0.42}, 0.0, {1, 1}));
        tg->addOnTop(std::move(vert));

        // RTL.
        auto rtl = doc->makeText("Arabic");
        rtl->setAutoNamed(false);
        {
            text::CharStyle cs;
            cs.font.family = "Noto Naskh Arabic";
            cs.sizePx = static_cast<float>(H) * 0.018f;
            cs.setSolidFill(ColorF{0.85f, 0.95f, 1.0f, 1.0f});
            text::TextBlock b = text::makeBlock("نذير ملف واحد", cs);
            b.paragraphs[0].direction = text::Paragraph::Direction::RTL;
            b.paragraphs[0].align = text::Paragraph::Align::Right;
            rtl->setBlock(std::move(b));
        }
        rtl->setTransform(Affine2D::trs({W * 0.60, H * 0.88}, 0.0, {1, 1}));
        tg->addOnTop(std::move(rtl));

        // Text on a path, referencing the vector rail by id (the app re-bakes `baked` on load).
        auto onPath = doc->makeText("On a path");
        onPath->setAutoNamed(false);
        {
            text::CharStyle cs;
            cs.font.family = "Inter";
            cs.font.weight = 600.0f;
            cs.sizePx = static_cast<float>(H) * 0.014f;
            cs.tracking = 80.0f;
            cs.setSolidFill(ColorF{1.0f, 1.0f, 1.0f, 0.9f});
            text::TextBlock b = text::makeBlock(
                "A HARBINGER OF DEATH IN A SINGLE FILE · MEASURED, NOT GUESSED · ", cs);
            text::PathFit fit;
            fit.layer = pathLayerId;
            fit.s0 = 0.0;
            fit.s1 = 12000.0;
            b.pathFit = fit;
            onPath->setBlock(std::move(b));
        }
        onPath->setTransform(Affine2D::trs({W * 0.5, H * 0.30}, 0.0, {1, 1}));
        tg->addOnTop(std::move(onPath));

        // A run of ordinary captions: individually trivial, collectively a deep stack of text
        // layers each of which the walk must visit and each of which owns a render cache.
        for (int i = 0; i < 12; ++i) {
            auto cap = doc->makeText("Caption " + std::to_string(i + 1));
            cap->setAutoNamed(false);
            text::CharStyle cs;
            cs.font.family = (i % 2) ? "Inter" : "Noto Serif";
            cs.sizePx = static_cast<float>(H) * (0.006f + 0.0006f * (i % 5));
            cs.tracking = 20.0f + 8.0f * (i % 4);
            cs.setSolidFill(ColorF{0.9f, 0.9f, 0.92f, 0.85f});
            cap->setBlock(text::makeBlock("plate " + std::to_string(i + 1) +
                                              " — 39.8 megapixels, and every one of them",
                                          cs));
            cap->setTransform(Affine2D::trs({W * 0.10, H * (0.72 + 0.018 * i)}, 0.0, {1, 1}));
            cap->setOpacity(0.8f);
            tg->addOnTop(std::move(cap));
        }

        doc->root().addOnTop(std::move(tg));
    }

    // ---- 6. the top of the stack: document-wide adjustments ------------------------------------
    {
        Stage s("finishing adjustments");
        auto hp = doc->makeAdjustment("High pass", core::AdjustmentKind::HighPass); // SPATIAL
        hp->params() = {{"radius", 14.0}};
        hp->setBlendMode(core::BlendMode::Overlay);
        hp->setOpacity(0.45f);
        doc->root().addOnTop(std::move(hp));

        auto grainAdj = doc->makeAdjustment("Add noise", core::AdjustmentKind::AddNoise);
        grainAdj->params() = {{"amount", 3.5}, {"mode", 0.0}, {"monochrome", 1.0}};
        doc->root().addOnTop(std::move(grainAdj));

        auto vig = doc->makeAdjustment("Vignette", core::AdjustmentKind::Vignette);
        vig->params() = {{"amount", -0.55}, {"midpoint", 0.42}, {"roundness", 0.2}};
        doc->root().addOnTop(std::move(vig));
    }

    std::printf("  %zu layers\n", doc->layerCount());
    return doc;
}

// ---- history -----------------------------------------------------------------------------------

// One real, visible edit per save, each into a DIFFERENT layer, so a walk back through the History
// panel shows the document actually coming apart rather than a counter going down.
void applyEdit(core::Document& doc, int index) {
    const int W = g_canvasW, H = g_canvasH;
    switch (index % 10) {
    case 0:
        if (auto* l = findLayer<core::RasterLayer>(doc, "Film grain")) {
            paintDisc(l->image(), W / 3, H / 4, H / 12, ColorF{1.0f, 0.9f, 0.7f, 0.5f}, 0.8f);
            l->invalidateContentBounds();
        }
        break;
    case 1:
        if (auto* t = findLayer<core::TextLayer>(doc, "Headline")) {
            text::TextBlock b = t->block();
            b.extrude->depth *= 1.35f;
            b.extrude->bevelFront.segments = 16;
            t->setBlock(std::move(b));
        }
        break;
    case 2:
        if (auto* v = findLayer<core::VectorLayer>(doc, "Rosette")) {
            vec::Object o = *v->object();
            o.geometry = rosette(41, 1050.0, 540.0);
            v->setObject(std::move(o));
        }
        break;
    case 3:
        if (auto* a = findLayer<core::AdjustmentLayer>(doc, "Depth blur"))
            a->params()["radius"] = 148.0;
        break;
    case 4:
        if (auto* t = findLayer<core::TextLayer>(doc, "Body copy")) {
            text::TextBlock b = t->block();
            b.areaSize = {W * 0.52, H * 0.24};
            t->setBlock(std::move(b));
        }
        break;
    case 5:
        if (auto* l = findLayer<core::RasterLayer>(doc, "Light leak 3")) {
            paintDisc(l->image(), static_cast<int>(l->image().width) / 3,
                      static_cast<int>(l->image().height) / 2,
                      static_cast<int>(l->image().height) / 3, ColorF{0.4f, 0.85f, 1.0f, 0.6f},
                      0.7f);
            l->invalidateContentBounds();
        }
        break;
    case 6:
        if (auto* t = findLayer<core::TextLayer>(doc, "Subhead")) {
            core::LayerEffects fx = t->effects();
            core::StrokeEffect st;
            st.enabled = true;
            st.width = 22.0f;
            st.align = core::StrokeEffect::Align::Outside;
            st.paint = vec::SolidPaint{ColorF{1.0f, 0.4f, 0.2f, 1.0f}};
            fx.strokes.push_back(st);
            t->setEffects(std::move(fx));
        }
        break;
    case 7:
        if (auto* v = findLayer<core::VectorLayer>(doc, "Boolean badge")) {
            vec::Object o = *v->object();
            if (auto* comp = std::get_if<vec::BooleanCompound>(&o.geometry))
                comp->op = vec::BoolOp::Subtract;
            v->setObject(std::move(o));
        }
        break;
    case 8:
        if (auto* a = findLayer<core::AdjustmentLayer>(doc, "Shadows/Highlights"))
            a->params()["shadowRadius"] = 320.0;
        break;
    default:
        if (auto* g = findLayer<core::GroupLayer>(doc, "Texture"))
            g->setOpacity(g->opacity() > 0.5f ? 0.35f : 0.9f);
        break;
    }
}

std::vector<std::uint8_t> readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

bool tipFor(const fs::path& p, io::CommitTip& tip) {
    const io::OpenReport rep = io::openDocument(readFile(p));
    if (!rep.tipValid)
        return false;
    tip = rep.tip;
    std::string err;
    return io::stampTipIdentity(p.string(), tip, &err);
}

} // namespace

int main(int argc, char** argv) {
    std::string outPath;
    std::string photoPath;
    int saves = 10;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--photo" && i + 1 < argc)
            photoPath = argv[++i];
        else if (a == "--saves" && i + 1 < argc)
            saves = std::atoi(argv[++i]);
        else if (a == "-h" || a == "--help") {
            std::printf("usage: torture_doc <out.mosaic> [--photo <image>] [--saves N]\n");
            return 0;
        } else if (outPath.empty())
            outPath = a;
    }
    if (outPath.empty()) {
        std::fprintf(stderr, "usage: torture_doc <out.mosaic> [--photo <image>] [--saves N]\n");
        return 2;
    }

    std::printf("torture_doc -> %s\n", outPath.c_str());

    auto doc = buildDocument(photoPath);

    // The first write is the full checkpoint (Save As / first save): parity-striped content plus
    // the manifest, atomically renamed into place.
    {
        Stage s("serialising the checkpoint");
        std::string err;
        const auto input = io::buildDocumentCheckpoint(*doc, &err);
        if (!input.has_value()) {
            fail("buildDocumentCheckpoint: " + err);
            return 1;
        }
        const std::vector<std::uint8_t> bytes = io::buildCheckpoint(*input);
        if (!io::writeFileAtomic(outPath, bytes, &err)) {
            fail("writeFileAtomic: " + err);
            return 1;
        }
    }
    std::printf("  checkpoint written: %.1f MB\n", fs::file_size(outPath) / 1048576.0);

    // Then real appended saves, through the real commit-append API and the real differ -- so the
    // history in the file is the history the app would have written.
    io::CommitTip tip;
    if (!tipFor(outPath, tip)) {
        fail("the freshly written file has no commit tip to append onto");
        return 1;
    }
    std::string err;
    auto baseline = io::buildDocumentCheckpoint(*doc, &err);
    if (!baseline.has_value()) {
        fail("baselining for commit-append: " + err);
        return 1;
    }
    for (int s = 1; s <= saves; ++s) {
        Stage stage("appended save " + std::to_string(s) + "/" + std::to_string(saves));
        applyEdit(*doc, s - 1);
        auto after = io::buildDocumentCheckpoint(*doc, &err);
        if (!after.has_value()) {
            fail("serialising state " + std::to_string(s) + ": " + err);
            break;
        }
        io::SaveState st;
        st.stateId = tip.commitId + 1;
        st.chunks = io::diffDocumentStates(*baseline, *after);
        if (st.chunks.empty()) {
            baseline = std::move(after);
            continue; // the edit serialised identically: nothing to commit
        }
        if (io::appendSaveToFile(outPath, tip, {&st, 1}, &err) != io::SaveStatus::Ok) {
            fail("appendSaveToFile state " + std::to_string(s) + ": " + err);
            break;
        }
        baseline = std::move(after);
    }

    // Self-verify: re-read what was written through the real reader, exactly as the app opens it.
    {
        Stage s("verifying the file through the reader");
        const io::OpenReport rep = io::openDocument(readFile(outPath));
        std::string rerr;
        auto back = io::documentFromReport(rep, &rerr);
        if (!back.has_value() || back->document == nullptr)
            fail("re-opening the written file: " + rerr);
        else {
            std::printf("\n  reopened: %zu layers, %zu committed states, %zu rejected chunk(s)\n",
                        back->document->layerCount(), rep.commits.size(), back->rejectedChunks);
            if (back->document->layerCount() != doc->layerCount())
                fail("layer count changed across the round trip");
            if (back->rejectedChunks != 0)
                fail("the reader rejected content it should have accepted");
        }
    }

    const std::uintmax_t size = fs::file_size(outPath);
    std::printf("\n%s: %.1f MB (%llu bytes), %d save(s) of history\n", outPath.c_str(),
                size / 1048576.0, static_cast<unsigned long long>(size), saves);
    if (g_failures != 0)
        std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
