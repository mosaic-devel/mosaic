#include "render/tile_compositor.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "common/profiler.hpp"
#include "core/adjustments.hpp"
#include "core/blend_mode.hpp"
#include "core/document.hpp"
#include "render/gpu_budget.hpp"
#include "render/gpu_policy.hpp"
#include "render/gpu_timer.hpp"
#include "render/half_float.hpp"
#include "render/stylize.hpp"
#include "render/vulkan_context.hpp"

#include <shaders/adjust_tile.comp.spv.hpp>
#include <shaders/adjust_tile_indexed.comp.spv.hpp>
#include <shaders/composite_tile.comp.spv.hpp>
#include <shaders/composite_tile_indexed.comp.spv.hpp>
#include <shaders/tile_resolve.comp.spv.hpp>

// The host half of the fused tile kernel. tile_compositor.hpp carries the design; this file
// carries three things that are easy to get subtly wrong and so are spelled out where they
// happen:
//
//   1. LAYOUTS. Every long-lived image this class owns lives permanently in
//      VK_IMAGE_LAYOUT_GENERAL after one UNDEFINED -> GENERAL transition. That is deliberate:
//      GENERAL is legal for storage images, for sampled images, AND as the source/destination of
//      vkCmdCopy*Image, so the whole class needs no layout ping-pong at all -- only memory
//      barriers. Layer sources are the exception (SHADER_READ_ONLY_OPTIMAL) because they are
//      never written after upload.
//   2. BARRIERS. Layers composite bottom -> top onto the SAME accumulator texels, so there is a
//      read-after-write hazard between consecutive layers and none at all between macrotiles of
//      one layer (they write disjoint regions -- that is the whole point of tiling). So: one
//      global memory barrier BETWEEN layers, and none inside a layer.
//   3. THE CLIP BASE never leaves the device. compositor.cpp's walkStep snapshots the placed
//      alpha of each non-clipped layer for the clipped ones above it; the kernel publishes that
//      to `uClipOut` (composite_tile.comp binding 4) and reads it from `uClip`. Reading it back
//      to the host would reinstate exactly the per-layer round trip residency exists to remove.

namespace mosaic::render {
namespace {

// A source-cache accounting page. The residency ledger charges a layer's device bytes -- pixels
// AND mask sheet -- in whole pages of this size, so `TileResidency`'s LRU + pin policy applies at
// the granularity this cut actually manages (a whole layer image). ⚠ Within that ledger a
// TileKey's `tx` is a PAGE INDEX, not a tile coordinate: nothing dereferences it, it only has to
// be unique per layer. The per-TILE atlas the key vocabulary was designed for arrives with the
// descriptor-indexed variant (plan item 10), and this ledger is forward-compatible with it
// because the policy class is the same.
constexpr std::uint64_t kSourcePageBytes = 256ull * 1024ull;

// The most copy regions one incremental layer upload may issue before it gives up and re-sends
// the whole image. Every region costs a `VkBufferImageCopy` plus a row-by-row host memcpy, so a
// dirty set shredded into many small runs is genuinely slower than one big copy -- and a cap is
// also what stops a pathological set (a dither pattern, a "select all inverse" fill) from turning
// one upload into thousands of commands. 64 is generous: a 3840x2160 layer at the default 256 px
// macrotile is 15x9 tiles, so even a FULLY dirty layer coalesces into 9 row runs.
constexpr std::size_t kMaxUploadRegions = 64;

constexpr std::uint32_t kBindingAcc = 0;
constexpr std::uint32_t kBindingSrc = 1;
constexpr std::uint32_t kBindingMask = 2;
constexpr std::uint32_t kBindingClip = 3;
constexpr std::uint32_t kBindingClipOut = 4;
constexpr std::uint32_t kBindingTiles = 5;    // the dirty-macrotile list (item 10)
constexpr std::uint32_t kBindingSources = 6;  // the indexed variant's runtime array, LAST by rule
constexpr std::uint32_t kWorkgroup = 8;  // must match composite_tile.comp's local_size

// composite_tile.comp's `layout(constant_id = 0) const int kTileList`.
constexpr std::uint32_t kSpecTileList = 0;

// One dirty macrotile in the list buffer: two ivec4, in the order the per-tile loop would have
// pushed them. The shader's std430 block declares exactly this, so the two must move together.
struct TileRecord {
    std::int32_t tileOrigin[2];
    std::int32_t accOrigin[2];
    std::int32_t extent[2];
    std::int32_t pad[2];
};
static_assert(sizeof(TileRecord) == 32, "tile record must match composite_tile.comp's 2x ivec4");

// The most layers the descriptor-indexed variant's runtime array will ever be sized for -- two
// descriptors each (pixels, mask). A cap rather than "however many the device allows" because the
// array's size is baked into a descriptor set LAYOUT built once at create(), and a document with
// more top-level layers than this simply takes the TileList shape, which costs it descriptor
// writes and not one pixel. 64 top-level raster layers is already an unusual document.
constexpr std::uint32_t kIndexedMaxLayers = 64;

// MOSAIC_TILE_DISPATCH: force one dispatch shape, for MEASUREMENT. The plan's item 10 says the
// SSBO half is "worth measuring before reaching for the extension", and this is what makes that a
// command rather than an argument -- the same shape as MOSAIC_GPU_PROFILE, and read in the same
// one place. It is NOT a preference: unset (the app's case) means Auto, which already picks the
// best shape the device can run, and every shape draws the identical picture.
[[nodiscard]] TileDispatch dispatchFromEnv() noexcept {
    const char* p = std::getenv("MOSAIC_TILE_DISPATCH");
    if (p == nullptr) return TileDispatch::Auto;
    if (std::strcmp(p, "per-tile") == 0) return TileDispatch::PerTile;
    if (std::strcmp(p, "list") == 0) return TileDispatch::TileList;
    if (std::strcmp(p, "indexed") == 0) return TileDispatch::Indexed;
    return TileDispatch::Auto;
}

// The kernel's push block, mirrored byte for byte. The static_assert is the contract: the shader
// declares 124 bytes and Vulkan 1.0 guarantees 128, so this has 4 bytes of headroom and no more.
struct PushBlock {
    float inv0[4];              //   0  TARGET px -> source px, row 0 (w = srcOriginX)
    float inv1[4];              //  16  row 1                        (w = srcOriginY)
    float mask0[4];             //  32  mask map, row 0              (w = srcTrueWidth)
    float mask1[4];             //  48  row 1                        (w = srcTrueHeight)
    float scale[2];             //  64  source texels per output texel, >= 1
    std::int32_t tileOrigin[2]; //  72  this tile's origin in the TARGET buffer (document space)
    std::int32_t accOrigin[2];  //  80  the tile's origin inside the accumulator / clip atlas
    std::int32_t extent[2];     //  88  the tile's valid extent (edge macrotiles are partial)
    std::int32_t filter;        //  96  render::ResampleFilter, ALREADY resolved
    std::int32_t superN;        // 100  Supersample sub-sample count
    std::int32_t blend;         // 104  core::BlendMode
    float opacity;              // 108
    std::int32_t maskMode;      // 112  0 none / 1 proportional / 2 affine-in-source / 3 unlinked
    std::int32_t clipMode;      // 116  0 unclipped / 1 multiply alpha by the clip base
    std::int32_t clipWrite;     // 120  0 publish nothing / 1 publish this layer's placed alpha
};
static_assert(sizeof(PushBlock) == 124, "push block must match composite_tile.comp's 124 bytes");
static_assert(sizeof(PushBlock) <= vk10::kMaxPushConstantsSize,
              "push block must fit Vulkan 1.0's guaranteed 128 bytes");

// adjust_tile.comp's push block, mirrored byte for byte. Smaller than the composite kernel's and
// laid out independently of it: the two never serve the same step, and both are pushed into the
// SAME 124-byte range of the SAME pipeline layout, so all that is required of this one is that it
// fit. `p` mirrors the shader's `vec4 uP[3]`, whose std430 stride is 16 -- i.e. a plain float[12].
struct AdjustPush {
    float maskMap[4];           //   0  doc px -> mask texel: (sx, sy, originX, originY)
    float p[12];                //  16  the kind's scalars, in the kernel's lane order
    std::int32_t tileOrigin[2]; //  64  this tile's origin in the TARGET buffer (document space)
    std::int32_t accOrigin[2];  //  72  the tile's origin inside the accumulator / clip atlas
    std::int32_t extent[2];     //  80  the tile's valid extent (edge macrotiles are partial)
    std::int32_t kind;          //  88  adjust_tile.comp's OWN dense kind enum
    float opacity;              //  92
    std::int32_t maskMode;      //  96  0 no mask / 1 the clamped-domain stretch
    std::int32_t clipMode;      // 100  0 unclipped / 1 multiply the amount by the clip base
    std::int32_t flags;         // 104  curve actives, preserve-luminosity, a per-kind choice
};
static_assert(sizeof(AdjustPush) == 108, "push block must match adjust_tile.comp's 108 bytes");
static_assert(sizeof(AdjustPush) <= sizeof(PushBlock),
              "the adjustment block shares the composite kernel's push range and must fit it");

constexpr std::uint32_t kResolveBindingAcc = 0;
constexpr std::uint32_t kResolveBindingDst = 1;

// tile_resolve.comp's push block, mirrored byte for byte.
struct ResolvePush {
    std::int32_t accOrigin[2]; //  0  the macrotile's origin inside the accumulator atlas
    std::int32_t dstOrigin[2]; //  8  ... and in the destination (document space)
    std::int32_t extent[2];    // 16  its valid extent (edge macrotiles are partial)
};
static_assert(sizeof(ResolvePush) == 24, "push block must match tile_resolve.comp's 24 bytes");
static_assert(sizeof(ResolvePush) <= vk10::kMaxPushConstantsSize,
              "push block must fit Vulkan 1.0's guaranteed 128 bytes");

[[nodiscard]] std::uint32_t alignUp(std::uint32_t v, std::uint32_t a) noexcept {
    return a == 0 ? v : ((v + a - 1) / a) * a;
}
[[nodiscard]] std::uint32_t alignDown(std::uint32_t v, std::uint32_t a) noexcept {
    return a == 0 ? v : (v / a) * a;
}

// FNV-1a over raw bytes -- the plan-diff key. It only has to detect CHANGE, never to be
// cryptographic, and it must be cheap enough to run per layer per frame.
void hashBytes(std::uint64_t& h, const void* p, std::size_t n) noexcept {
    const auto* b = static_cast<const std::uint8_t*>(p);
    for (std::size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
}
template <typename T>
void hashValue(std::uint64_t& h, const T& v) noexcept {
    hashBytes(h, &v, sizeof(T));
}

// The doc-space AABB of a LAYER-LOCAL rect placed by `t`, grown by `pad` document pixels so a
// filter footprint at the edge is inside the dirty set. Clamped to the canvas by the caller.
//
// The pad is in TARGET pixels and is the caller's, because it depends on the placement's scale --
// see planDocument, where it is derived once per layer and then stored on the Step so the region
// path can reuse exactly the same number. A pad computed two different ways would under-dirty a
// thin band at one edge, which shows up as a stale seam at some zooms and nowhere else.
[[nodiscard]] common::Rect placedRectBounds(const common::Affine2D& t, const common::Rect& r,
                                            double pad) {
    const common::Vec2 c0 = t.apply({r.x, r.y});
    const common::Vec2 c1 = t.apply({r.right(), r.y});
    const common::Vec2 c2 = t.apply({r.x, r.bottom()});
    const common::Vec2 c3 = t.apply({r.right(), r.bottom()});
    const double x0 = std::min({c0.x, c1.x, c2.x, c3.x}) - pad;
    const double y0 = std::min({c0.y, c1.y, c2.y, c3.y}) - pad;
    const double x1 = std::max({c0.x, c1.x, c2.x, c3.x}) + pad;
    const double y1 = std::max({c0.y, c1.y, c2.y, c3.y}) + pad;
    return common::Rect{x0, y0, x1 - x0, y1 - y0};
}

// The whole layer's footprint: the rect above over [0,w] x [0,h].
[[nodiscard]] common::Rect placedBounds(const common::Affine2D& t, std::uint32_t w,
                                        std::uint32_t h, double pad) {
    return placedRectBounds(
        t, common::Rect{0.0, 0.0, static_cast<double>(w), static_cast<double>(h)}, pad);
}

// ---- What a LEAF composites FROM (S60-a) ------------------------------------------------------
//
// compositor.cpp's renderLayerRaw treats four of the seven kinds identically -- sample a
// FIXED-RESOLUTION source image through a placement, fold a linked mask at the source pixel, fold
// an unlinked one after placement -- and they differ only in WHICH image and in one extra factor
// in the placement:
//
//   raster    raster->image()                          place = pre * layer.transform()
//   magic     magic->source()                          place = pre * layer.transform()
//   text      tlayer->cachedImage()                    place = ... * cacheImageToLayer()
//   texture   xlayer->cachedImageF() else cachedImage()  place = ... * cacheImageToLayer()
//
// `pre` is the identity at the document root, which is the only place this lane plans. VECTOR is
// deliberately absent and stays a refusal: core::vec::rasterizeObjectF evaluates the object at
// TARGET resolution through the placement, so there is no source of fixed size to make resident,
// and a bitmap stand-in would be a different picture at every zoom rather than merely a slower one.
struct LeafSource {
    const common::Image* pixels8 = nullptr;   // exactly one of the two is set
    const common::ImageF* pixelsF = nullptr;  // the texture generator's float (sky) lane
    common::Affine2D toLayer = common::Affine2D::identity();  // source px -> layer-local
    std::uint32_t width = 0, height = 0;
    // The key a device copy of these pixels is stamped with; see LayerSource::sourceRevision.
    std::uint64_t revision = 0;
};

// The leaf's source, or the refusal that says why it has none.
//
// ⚠ An EMPTY source is a REFUSAL, never a transparent layer, and the reason is the same one the
// empty-raster arm has always made: the CPU walk still runs walkStep for it, so it still publishes
// an all-zero clip base and still hides everything clipped above it. Reproducing that costs more
// than declining, and it covers the unpopulated text/texture cache exactly as it covers a
// zero-sized raster.
[[nodiscard]] TileRefusal leafSourceFor(const core::Layer& layer, LeafSource& out) {
    out = {};
    if (const auto* raster = layer.as<core::RasterLayer>()) {
        out.pixels8 = &raster->image();
        out.revision = raster->contentRevision();
    } else if (const auto* magic = layer.as<core::MagicLayer>()) {
        out.pixels8 = &magic->source();
        out.revision = magic->contentRevision();
    } else if (const auto* tlayer = layer.as<core::TextLayer>()) {
        // ⚠ cacheGeneration(), NOT contentRevision(). The app re-renders this cache for reasons
        // that never touch the block -- the DRAFT half-res bake taken during a live block edit or
        // a font hover, the crisp pass that replaces it, the Area clip flip, a transform re-bake --
        // and core::text::refreshTextCache stamps m_cacheRevision back to the SAME
        // m_contentRevision each time. A device copy keyed on contentRevision would show the draft
        // bake for as long as the layer stayed resident. (core/layer.hpp spells out all four.)
        out.pixels8 = tlayer->cachedImage();
        out.toLayer = tlayer->cacheImageToLayer();
        out.revision = tlayer->cacheGeneration();
    } else if (const auto* xlayer = layer.as<core::TextureLayer>()) {
        // The FLOAT lane wins wherever it is populated, exactly as the CPU arm prefers it: the sky
        // generator renders unquantised so its gradients do not band, and reaching past it to the
        // (absent) 8-bit cache would composite nothing at all. Same generation argument as text --
        // a canvas resize re-renders with the params revision standing still.
        out.pixelsF = xlayer->cachedImageF();
        if (out.pixelsF == nullptr) out.pixels8 = xlayer->cachedImage();
        out.toLayer = xlayer->cacheImageToLayer();
        out.revision = xlayer->cacheGeneration();
    } else {
        return TileRefusal::UnsupportedKind;  // vector, and anything added after this was written
    }
    if (out.pixels8 != nullptr) {
        out.width = out.pixels8->width;
        out.height = out.pixels8->height;
    } else if (out.pixelsF != nullptr) {
        out.width = out.pixelsF->width;
        out.height = out.pixelsF->height;
    }
    if (out.width == 0 || out.height == 0) return TileRefusal::UnsupportedKind;
    return TileRefusal::None;
}

// The device format a leaf source uploads as. The float lane rides the SAME kernel unchanged:
// composite_tile.comp reads uSrc with texelFetch only ("any float or UNORM format works"), so the
// sampler never interprets the format and the shader sees straight-alpha floats either way. What
// it is NOT is a free choice -- see the caps gate in planDocument.
[[nodiscard]] VkFormat sourceFormat(const LeafSource& s) noexcept {
    return s.pixelsF != nullptr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
}
[[nodiscard]] std::uint32_t sourceTexelBytes(VkFormat f) noexcept {
    return f == VK_FORMAT_R16G16B16A16_SFLOAT ? 8u : 4u;
}

// ---- ADJUSTMENT LAYERS (S60-a) -----------------------------------------------------------------
//
// An adjustment is a FUNCTION OF THE BACKDROP, not a source, so the lane serves it with a second
// kernel over the same dirty macrotiles (shaders/adjust_tile.comp). Everything below is the host
// half of that kernel: which kinds are served, how each one's scalars are resolved, and what makes
// a served one's device state stale.
//
// ⚠ ADMISSION IS PER KIND, and it has to be, for the reason tile_compositor.hpp states: a lane
// that guessed at an unported kind would draw the WRONG PICTURE, where a refusal merely draws the
// right one slowly. So this table is deliberately exhaustive-by-refusal -- anything it does not
// recognise, including a kind appended to core::AdjustmentKind after it was written, refuses.

// The SHADER's kind enum, mirrored. Dense and its own, because core::AdjustmentKind's ORDER is
// explicitly free (kinds serialize by token string, so the enum may be reordered) and a kernel that
// indexed it directly would silently re-point every arm the day a member moved. This function is
// the only place the two vocabularies meet.
enum : std::int32_t {
    kAdjInvert = 0,
    kAdjBrightnessContrast = 1,
    kAdjLevels = 2,
    kAdjExposure = 3,
    kAdjHueSaturation = 4,
    kAdjColorBalance = 5,
    kAdjGrayscale = 6,
    kAdjCurves = 7,
    kAdjGradientMap = 8,
    kAdjVibrance = 9,
    kAdjPhotoFilter = 10,
    kAdjHazeRemoval = 11,
};

constexpr std::int32_t kAdjFlagCurveR = 1;       // bits 0..2: which Curves channels are ACTIVE
constexpr std::int32_t kAdjFlagCurveG = 2;
constexpr std::int32_t kAdjFlagCurveB = 4;
constexpr std::int32_t kAdjFlagPreserveLum = 8;  // ColorBalance / PhotoFilter
constexpr std::int32_t kAdjChoiceShift = 4;      // bits 4..7: Grayscale's projection method

// The transfer TABLE the two lookup kinds ride on: 256 entries, one per 8-bit lattice point.
constexpr std::uint32_t kAdjTableSize = 256;

// sRGB decode, ANALYTIC. compositor.cpp resolves Photo Filter's gel colour through its 2049-entry
// LUT; the two agree to ~3e-8 in this direction (the decode curve's curvature is bounded), which is
// six orders of magnitude below the 1/255 the parity test measures. Recreating the LUT here to save
// that would be a copy of a table for nothing.
[[nodiscard]] float adjustSrgbToLinear(float e) noexcept {
    if (e <= 0.04045f) return std::max(0.0f, e) / 12.92f;
    return std::pow((e + 0.055f) / 1.055f, 2.4f);
}

// A transform that maps every point to itself -- compositor.cpp's `isIdentity`, which
// adjustmentMaskDomain consults before it maps the sheet's corners.
[[nodiscard]] bool isIdentityAffine(const common::Affine2D& t) noexcept {
    return t.m00 == 1.0 && t.m01 == 0.0 && t.m02 == 0.0 && t.m10 == 0.0 && t.m11 == 1.0 &&
           t.m12 == 0.0;
}

// Everything the adjustment kernel needs about one layer, or the refusal that says why it cannot
// have it. Pure and re-derivable: `planDocument` and `ensureLayerResident` both call this rather
// than passing state between themselves, so the two cannot come to different conclusions about
// which kind is in hand or whether its table is stale.
struct AdjustPlan {
    std::int32_t kind = 0;
    std::int32_t flags = 0;
    float p[12] = {};
    // A bag whose values are the kind's own no-op: applyAdjustment RETURNS before touching a pixel,
    // so this lane must not dispatch either. It is not merely an optimisation -- a float round trip
    // through a nominally-identity transfer is not the identity (the HSL pair alone is not), and a
    // freshly inserted layer has to composite byte-identically to no layer at all.
    bool identity = false;
    bool table = false;             // needs the 256-entry transfer table on the device
    std::uint64_t fingerprint = 0;  // the params bag, hashed: the table's staleness key
    TileRefusal refusal = TileRefusal::None;
    const char* reason = "";
};

[[nodiscard]] AdjustPlan planAdjustment(const core::AdjustmentLayer& adj) {
    using enum core::AdjustmentKind;
    AdjustPlan out;
    const core::AdjustmentKind kind = adj.adjustmentKind();

    const auto decline = [&out](const char* why) {
        out.refusal = TileRefusal::Adjustment;
        out.reason = why;
        return out;
    };

    // ---- The two whole FAMILIES that refuse before any parameter is read ------------------------
    //
    // Both are structural, not numerical. A stylize kind owns its own opacity/mask/clip blend
    // (render/stylize.hpp), so it is not even the same shape of step; a spatial kind reads the
    // backdrop's NEIGHBOURHOOD, so a macrotile recomposite would need the dirty set grown by the
    // kernel's reach -- which the whole point of this lane is that it does not do.
    if (isStylizeKind(kind)) return decline("the stylize family owns its own mask/clip blend");
    // The per-INSTANCE overload, deliberately: Grayscale is per-pixel for its projections and
    // spatial for Dithered / Adaptive threshold, and only the layer knows which.
    if (core::adjustmentIsSpatial(adj)) return decline("spatial: it reads a neighbourhood");

    // Schema-clamped reads, exactly as compositor.cpp's scalarAdjustConsts takes them: an absent
    // key falls back to the declared default and a present one clamps to the declared range, so the
    // two lanes cannot disagree about what a parameter means.
    const auto schema = [&](const char* key) {
        const core::AdjustmentParamDesc* d = core::adjustmentParamDesc(kind, key);
        return d != nullptr ? core::adjustmentParamValue(adj, *d) : 0.0;
    };

    switch (kind) {
        case Invert:
            out.kind = kAdjInvert;
            break;
        case BrightnessContrast: {
            // RAW bag reads, not schema ones: this kind's behaviour predates the schema and
            // compositor.cpp keeps it byte-identical. There is deliberately no identity early-out
            // for it either -- scalarAdjustConsts leaves `identity` false on its default arm.
            const auto& bag = adj.params();
            const auto raw = [&bag](const char* key) {
                const auto it = bag.find(key);
                return it == bag.end() ? 0.0 : it->second;
            };
            out.kind = kAdjBrightnessContrast;
            out.p[0] = static_cast<float>(raw("brightness"));
            out.p[1] = static_cast<float>(raw("contrast"));
            break;
        }
        case Levels: {
            const double inB = schema("in_black"), inW = schema("in_white"), gamma = schema("gamma");
            const double outB = schema("out_black"), outW = schema("out_white");
            out.kind = kAdjLevels;
            out.p[0] = static_cast<float>(inB);
            out.p[1] = static_cast<float>(1.0 / std::max(inW - inB, 1e-4));
            out.p[2] = static_cast<float>(1.0 / gamma);
            out.p[3] = static_cast<float>(outB);
            out.p[4] = static_cast<float>(outW);
            out.identity = inB == 0.0 && inW == 1.0 && gamma == 1.0 && outB == 0.0 && outW == 1.0;
            break;
        }
        case Exposure: {
            const double ev = schema("exposure"), offset = schema("offset");
            const double gamma = schema("gamma");
            out.kind = kAdjExposure;
            out.p[0] = static_cast<float>(std::exp2(ev));
            out.p[1] = static_cast<float>(offset);
            out.p[2] = static_cast<float>(1.0 / gamma);
            out.identity = ev == 0.0 && offset == 0.0 && gamma == 1.0;
            break;
        }
        case HueSaturation: {
            const double hue = schema("hue"), sat = schema("saturation");
            const double light = schema("lightness");
            out.kind = kAdjHueSaturation;
            out.p[0] = static_cast<float>(hue / 360.0);
            out.p[1] = static_cast<float>(1.0 + sat / 100.0);
            out.p[2] = static_cast<float>(light / 100.0);
            out.identity = hue == 0.0 && sat == 0.0 && light == 0.0;
            break;
        }
        case ColorBalance: {
            // compositor.cpp's kStrength, and it must not drift: a full slider shifts its band's
            // channel by up to this many colour units.
            constexpr double kStrength = 0.4;
            static constexpr const char* kKeys[9] = {
                "shadows_cr",    "shadows_mg",    "shadows_yb",
                "midtones_cr",   "midtones_mg",   "midtones_yb",
                "highlights_cr", "highlights_mg", "highlights_yb",
            };
            bool allZero = true;
            for (int i = 0; i < 9; ++i) {
                const double s = schema(kKeys[i]);
                allZero = allZero && s == 0.0;
                // ONE PUSH VEC4 PER BAND: lane 4*band + channel, .w unused. The kernel reads
                // uP[0].xyz / uP[1].xyz / uP[2].xyz, so the packing is the mapping.
                out.p[4 * (i / 3) + (i % 3)] = static_cast<float>(s / 100.0 * kStrength);
            }
            out.kind = kAdjColorBalance;
            if (schema("preserve_luminosity") >= 0.5) out.flags |= kAdjFlagPreserveLum;
            out.identity = allZero;
            break;
        }
        case Grayscale: {
            // ⚠ THE QUANTISED VARIANT REFUSES, and it is the clearest case of the lattice rule.
            // "How do I show this with N greys?" rounds the projection onto N-1 steps, and the
            // accumulator this kernel reads is rgba16f where the reference's is fp32 -- so a pixel
            // within ~2.4e-4 of a step lands on the OTHER side of it and the two lanes differ by a
            // whole grey level, not by an LSB. A smooth gradient crosses every step, so this is a
            // guaranteed failure rather than a risk.
            if (std::round(schema("grays")) <= 255.0)
                return decline("quantised: a lattice step flips a level on an fp16 backdrop");
            // adjustmentIsSpatial already refused Dithered and Adaptive threshold above, so what is
            // left is a pure per-pixel projection whatever the index says.
            out.kind = kAdjGrayscale;
            out.p[0] = static_cast<float>(schema("strength") / 100.0);
            out.flags |= (static_cast<std::int32_t>(std::lround(schema("method"))) & 15)
                         << kAdjChoiceShift;
            out.identity = out.p[0] == 0.0f;  // zero strength changes nothing
            break;
        }
        case Curves: {
            out.kind = kAdjCurves;
            out.table = true;
            // WHICH channels are active, resolved exactly as curvesConsts resolves it: a channel
            // whose per-channel curve AND the composite curve are both the identity is passed
            // through VERBATIM rather than through a nominally-identity lookup.
            const core::brush::Curve composite =
                core::adjustmentCurve(adj, core::CurveChannel::Composite);
            const bool compositeId = composite.isIdentity();
            constexpr int kFirst = static_cast<int>(core::CurveChannel::Red);
            for (int k = 0; k < 3; ++k) {
                const core::brush::Curve per =
                    core::adjustmentCurve(adj, static_cast<core::CurveChannel>(kFirst + k));
                if (compositeId && per.isIdentity()) continue;
                out.flags |= 1 << k;
            }
            out.identity = (out.flags & (kAdjFlagCurveR | kAdjFlagCurveG | kAdjFlagCurveB)) == 0;
            break;
        }
        case GradientMap:
            // No identity early-out, and deliberately: a gradient map has no identity ramp
            // (docs/adjustment-layers.md §2.6), the Threshold/Posterize/Matte Removal class.
            out.kind = kAdjGradientMap;
            out.table = true;
            break;
        case Vibrance: {
            const double amount = schema("vibrance");
            out.kind = kAdjVibrance;
            out.p[0] = static_cast<float>(amount / 100.0);
            out.identity = amount == 0.0;
            break;
        }
        case PhotoFilter: {
            const double density = schema("density") / 100.0;
            const auto preset = static_cast<core::PhotoFilterPreset>(std::lround(schema("filter")));
            const common::Color8 col =
                preset == core::PhotoFilterPreset::Custom
                    ? common::Color8{static_cast<std::uint8_t>(std::lround(schema("color_r"))),
                                     static_cast<std::uint8_t>(std::lround(schema("color_g"))),
                                     static_cast<std::uint8_t>(std::lround(schema("color_b"))), 255}
                    : core::photoFilterPresetColor(preset);
            out.kind = kAdjPhotoFilter;
            out.p[0] = adjustSrgbToLinear(static_cast<float>(col.r) / 255.0f);
            out.p[1] = adjustSrgbToLinear(static_cast<float>(col.g) / 255.0f);
            out.p[2] = adjustSrgbToLinear(static_cast<float>(col.b) / 255.0f);
            out.p[3] = static_cast<float>(density);
            if (schema("preserve_luminosity") >= 0.5) out.flags |= kAdjFlagPreserveLum;
            out.identity = density == 0.0;  // the EFFECTIVE no-op, not default-equality
            break;
        }
        case HazeRemoval: {
            const double amount = schema("amount"), air = schema("airlight") / 100.0;
            const double tint = schema("tint") / 100.0, sat = schema("saturation") / 100.0;
            out.kind = kAdjHazeRemoval;
            out.p[0] = static_cast<float>(std::clamp(air * (1.0 + 0.10 * tint), 0.0, 1.0));
            out.p[1] = static_cast<float>(air);
            out.p[2] = static_cast<float>(std::clamp(air * (1.0 - 0.10 * tint), 0.0, 1.0));
            out.p[3] = static_cast<float>(1.0 / (1.0 - 0.9 * amount / 100.0));
            out.p[4] = static_cast<float>(sat);
            out.identity = amount == 0.0 && sat == 1.0;
            break;
        }

        // ---- The NAMED refusals, each with the pixel-level reason it is one ---------------------
        case Threshold:
        case Posterize:
            // A lattice quantiser: infinite slope at every step, so the fp16 accumulator flips a
            // whole level for any pixel that lands near one -- see the Grayscale note above.
            return decline("a lattice quantiser: an fp16 backdrop flips a whole level at a step");
        case MatteRemoval:
            // Porter-Duff unmatting divides by the pixel's own alpha, so its conditioning is 1/a.
            // Over a nearly transparent backdrop -- exactly where the kind does its work -- an
            // fp16 alpha's half-ulp becomes an arbitrarily large colour error, and the honest
            // answer to that is the fp32 walk rather than a wider tolerance.
            return decline("conditioned on 1/alpha: fp16 coverage cannot hold 1/255 through it");
        case PhotometricMatch:
            // Implemented, per-pixel, and still out of scope: thirteen scalars (against twelve push
            // lanes), a row-dependent gradient term, and a fused log-luminance transfer with a
            // highlight shoulder that is its own transcription. It is also the sky-estimate grade
            // rather than a stack adjustment, so it is the last one a real document needs here.
            return decline("13 scalars and a row-dependent transfer: its own pass");
        default:
            // The spatial and stylize kinds returned above; ANYTHING appended to
            // core::AdjustmentKind after this was written lands here, which is the only safe
            // answer -- a new kind is unported until someone ports it.
            return decline("not ported to the tile kernel");
    }

    // The staleness key for the transfer table, and the diff key for a parameter edit. The WHOLE
    // bag is hashed rather than the resolved constants above, and the asymmetry is deliberate:
    // over-hashing costs one full-canvas recomposite (the Curves editor's own `channel` row is in
    // the bag, and switching tabs would trigger it), while under-hashing shows the previous curve
    // for as long as the layer stays resident. Only one of those is a bug.
    std::uint64_t h = 1469598103934665603ull;
    const std::int32_t kindI = static_cast<std::int32_t>(kind);
    hashValue(h, kindI);
    for (const auto& [key, value] : adj.params()) {
        hashBytes(h, key.data(), key.size());
        hashValue(h, value);
    }
    out.fingerprint = h | 1ull;  // ZERO is reserved: markLayerDirty poisons a fingerprint with it
    return out;
}

// The 256-entry transfer table the two lookup kinds read, as a 256x1 RGBA sheet.
//
// A common::ImageF because the leaf residency path already converts one to R16G16B16A16_SFLOAT --
// half is ~11 bits of mantissa against the 8 the readback ends in, three bits finer than the
// quantisation this lane exists to avoid, and exactly the precision the accumulator itself carries
// -- so the table needs no upload code of its own. Built only when the fingerprint above says the
// resident copy is stale, never per frame.
void buildAdjustTable(const core::AdjustmentLayer& adj, common::ImageF& out) {
    out = common::ImageF(kAdjTableSize, 1);
    if (adj.adjustmentKind() == core::AdjustmentKind::GradientMap) {
        // gradientMapConsts: the stored ramp through core::vec::sampleAt -- the same evaluation the
        // vector rasteriser and the layer-effects overlays use, midpoints and all -- with `reverse`
        // baked into the table rather than flipped per pixel. The stop ALPHA rides along as the
        // per-tone strength, which is why all four channels are filled.
        const core::vec::Gradient ramp = core::adjustmentGradientMap(adj);
        const core::AdjustmentParamDesc* rev =
            core::adjustmentParamDesc(core::AdjustmentKind::GradientMap, "reverse");
        const bool reverse = rev != nullptr && core::adjustmentParamValue(adj, *rev) >= 0.5;
        for (std::uint32_t i = 0; i < kAdjTableSize; ++i) {
            const double t = static_cast<double>(i) / 255.0;
            const common::ColorF s = core::vec::sampleAt(ramp, {reverse ? 1.0 - t : t, 0.0});
            const std::size_t p = static_cast<std::size_t>(i) * 4;
            out.rgba[p] = s.r;
            out.rgba[p + 1] = s.g;
            out.rgba[p + 2] = s.b;
            out.rgba[p + 3] = s.a;
        }
        return;
    }
    // curvesConsts: per-channel curve FIRST, then the composite curve on its result -- the order
    // every editor with both has used since Photoshop 4. All three channels are built even when the
    // kernel will pass one through verbatim; the flags decide that, and an unbuilt lane would be a
    // second thing to keep in step for no saving worth measuring.
    const core::brush::Curve composite = core::adjustmentCurve(adj, core::CurveChannel::Composite);
    constexpr int kFirst = static_cast<int>(core::CurveChannel::Red);
    for (int k = 0; k < 3; ++k) {
        const core::brush::Curve per =
            core::adjustmentCurve(adj, static_cast<core::CurveChannel>(kFirst + k));
        for (std::uint32_t i = 0; i < kAdjTableSize; ++i)
            out.rgba[static_cast<std::size_t>(i) * 4 + static_cast<std::size_t>(k)] =
                static_cast<float>(composite.eval(per.eval(static_cast<double>(i) / 255.0)));
    }
    // Alpha is never read by the Curves arm; 1 rather than 0 so a stray read shows as a no-op
    // rather than as an invisible layer.
    for (std::uint32_t i = 0; i < kAdjTableSize; ++i)
        out.rgba[static_cast<std::size_t>(i) * 4 + 3] = 1.0f;
}

}  // namespace

// ---- Host-side resolution the kernel relies on ------------------------------------------------

bool isLosslessGridPlacement(const common::Affine2D& t) noexcept {
    const bool linearIdentity = t.m00 == 1.0 && t.m01 == 0.0 && t.m10 == 0.0 && t.m11 == 1.0;
    return linearIdentity && t.m02 == std::floor(t.m02) && t.m12 == std::floor(t.m12);
}

ResampleFilter resolveTileFilter(ResampleFilter user, const common::Affine2D& t,
                                 bool liveDrag) noexcept {
    const ResampleFilter f = user == ResampleFilter::Auto ? chooseAutoFilter(t, liveDrag) : user;
    // The CPU reference short-circuits a linear-identity placement with an integer translation
    // (or ANY translation under Nearest) to a whole-pixel COPY. That copy is exactly what Nearest
    // computes -- but NOT what convolving Mitchell or Gaussian computes, because those kernels
    // are approximating rather than interpolating and blur even at integer offsets. Feeding the
    // kernel the unresolved filter would silently break parity for exactly those two.
    const bool linearIdentity = t.m00 == 1.0 && t.m01 == 0.0 && t.m10 == 0.0 && t.m11 == 1.0;
    if (linearIdentity && (f == ResampleFilter::Nearest || isLosslessGridPlacement(t)))
        return ResampleFilter::Nearest;
    return f;
}

std::int32_t tileSupersampleN(const common::Affine2D& inverse) noexcept {
    const double foot = std::max({1.0, std::hypot(inverse.m00, inverse.m10),
                                  std::hypot(inverse.m01, inverse.m11)});
    return static_cast<std::int32_t>(
        std::clamp(static_cast<int>(std::ceil(foot)) + 1, 2, 8));
}

std::string_view tileRefusalName(TileRefusal r) noexcept {
    switch (r) {
        case TileRefusal::None: return "none";
        case TileRefusal::NoDevice: return "no Vulkan device";
        case TileRefusal::DeviceTooSmall: return "device below the kernel's floor";
        case TileRefusal::DocumentTooLarge: return "accumulator over the memory budget";
        case TileRefusal::OutOfBudget: return "source atlas over the memory budget";
        case TileRefusal::NestedGroup: return "group layer";
        case TileRefusal::Adjustment: return "unsupported adjustment kind";
        case TileRefusal::LayerEffects: return "layer effects";
        case TileRefusal::LivePartition: return "live coverage partition";
        case TileRefusal::UnsupportedKind: return "unsupported layer kind";
        case TileRefusal::LayerTooLarge: return "layer over maxImageDimension2D";
        case TileRefusal::SingularTransform: return "singular layer transform";
        case TileRefusal::DeviceError: return "device error";
    }
    return "unknown";
}

std::string_view tileDispatchName(TileDispatch d) noexcept {
    switch (d) {
        case TileDispatch::Auto: return "auto";
        case TileDispatch::PerTile: return "per-tile loop";
        case TileDispatch::TileList: return "SSBO tile list";
        case TileDispatch::Indexed: return "descriptor-indexed";
    }
    return "unknown";
}

std::string_view dispatchRefusalName(DispatchRefusal r) noexcept {
    switch (r) {
        case DispatchRefusal::None: return "none";
        case DispatchRefusal::NotRequested: return "not requested";
        case DispatchRefusal::NoDescriptorIndexing: return "no descriptor indexing";
        case DispatchRefusal::SpirvUnsupported: return "device will not load the variant SPIR-V";
        case DispatchRefusal::DescriptorBudget: return "sampled-image budget too small";
        case DispatchRefusal::PipelineFailed: return "indexed pipeline creation failed";
        case DispatchRefusal::TooManyLayers: return "more layers than the descriptor array holds";
    }
    return "unknown";
}

// ---- Construction ------------------------------------------------------------------------------

std::unique_ptr<TileCompositor> TileCompositor::create(std::string& error) {
    // CPU-only mode (render/gpu_policy.hpp, S60-b item 14). Asked HERE as well as in the overload
    // below, and not only there: this one would otherwise stand up `VulkanContext::shared()` --
    // a whole device -- purely to hand it to a function that is about to refuse it.
    if (!computeLaneAllowed("tile compositor", error)) return nullptr;
    return create(VulkanContext::shared(/*enableValidation=*/true, error), error);
}

std::unique_ptr<TileCompositor> TileCompositor::create(std::shared_ptr<VulkanContext> ctx,
                                                       std::string& error) {
    // The app's path in: it passes WindowRenderer::computeContext(), so the device already exists
    // and the refusal is about not BUILDING the lane on it. The CPU walk in compositor.cpp is the
    // fallback, and it is the parity reference this lane is tested against.
    if (!computeLaneAllowed("tile compositor", error)) return nullptr;
    if (!ctx) {
        if (error.empty()) error = "no Vulkan device";
        return nullptr;
    }
    auto self = std::unique_ptr<TileCompositor>(new TileCompositor());
    self->m_ctx = std::move(ctx);

    const GpuCaps& caps = self->m_ctx->caps();
    // Lane admission: ASK, never assume. 2 storage images + 3 sampled images + 1 storage buffer
    // (the dirty-macrotile list) + 124 B of push constants, against Vulkan 1.0's guaranteed
    // 4 / 16 / 4 / 128.
    //
    // The ADJUSTMENT kernel needs no question of its own: it declares 1 storage image, 3 sampled
    // images, the same storage buffer and 108 bytes of push constants -- a strict subset of the
    // above, on the same set layout -- so a device that fits the composite kernel fits both.
    if (!caps.fitsStorageImages(2) || !caps.fitsSampledImages(3) || !caps.fitsStorageBuffers(1) ||
        !caps.fitsPushConstants(sizeof(PushBlock))) {
        error = "device cannot host the tile kernel's descriptors";
        return nullptr;
    }
    // The kernel hard-codes `rgba16f` in its storage-image layout qualifier, which is sound
    // precisely because rgba16f is the one float format Vulkan 1.0 guarantees for both storage
    // and linear filtering. A device whose caps chose anything else is telling us the guarantee
    // did not hold, and the honest answer is the CPU lane, not a reinterpreted accumulator.
    if (caps.workingFormat != VK_FORMAT_R16G16B16A16_SFLOAT) {
        error = "working format is not rgba16f";
        return nullptr;
    }
    if (caps.maxImageDim < kDirtyTileSize) {
        error = "maxImageDimension2D below one tile";
        return nullptr;
    }

    if (!self->initPipeline(error)) return nullptr;
    if (!self->initResolvePipeline(error)) return nullptr;
    if (!self->initStaticImages(error)) return nullptr;
    // Item 10's tier half. NOT lane admission: a device without descriptor indexing composites
    // exactly the same picture through the tile-list shape, so this refuses BY NAME
    // (`indexedRefusal()`) and leaves `error` alone. `MOSAIC_GPU_PROFILE=floor` takes this branch
    // on any device, which is how the refusal path gets exercised on hardware that has the tier.
    (void)self->initIndexedPipeline();
    self->m_requested = dispatchFromEnv();

    // Device-time instrumentation (plan section 8.1). NOT a lane-admission test: a device with no
    // timestamp counter composites exactly as well, it just cannot say how long the device spent,
    // so a null timer is swallowed here and every use of m_timer below is null-guarded. `error` is
    // left untouched on this path -- create() succeeded.
    std::string timerError;
    self->m_timer = GpuTimer::create(*self->m_ctx, GpuTimer::kDefaultMaxScopes, timerError);
    return self;
}

TileCompositor::~TileCompositor() {
    if (!m_ctx) return;
    const VkDevice dev = m_ctx->device();
    m_ctx->waitIdle();
    m_timer.reset();  // owns a VkQueryPool on this device; destroy it while the device is alive
    destroyAtlas();
    destroySources();
    for (GpuBuffer& b : m_staging) destroyBuffer(b);
    m_staging.clear();
    destroyBuffer(m_zero);
    destroyBuffer(m_tileList);
    destroyImage(m_dummyMask);
    destroyImage(m_dummyClipRead);
    destroyImage(m_dummyClipWrite);
    if (m_fence != VK_NULL_HANDLE) vkDestroyFence(dev, m_fence, nullptr);
    if (m_cmd != VK_NULL_HANDLE) vkFreeCommandBuffers(dev, m_pool, 1, &m_cmd);
    if (m_sampler != VK_NULL_HANDLE) vkDestroySampler(dev, m_sampler, nullptr);
    if (m_resolvePool != VK_NULL_HANDLE) vkDestroyDescriptorPool(dev, m_resolvePool, nullptr);
    if (m_resolvePipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, m_resolvePipeline, nullptr);
    if (m_resolveShader != VK_NULL_HANDLE) vkDestroyShaderModule(dev, m_resolveShader, nullptr);
    if (m_resolvePipeLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(dev, m_resolvePipeLayout, nullptr);
    if (m_resolveSetLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(dev, m_resolveSetLayout, nullptr);
    if (m_indexedPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(dev, m_indexedPool, nullptr);
    if (m_indexedAdjustPipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(dev, m_indexedAdjustPipeline, nullptr);
    if (m_indexedAdjustShader != VK_NULL_HANDLE)
        vkDestroyShaderModule(dev, m_indexedAdjustShader, nullptr);
    if (m_indexedPipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, m_indexedPipeline, nullptr);
    if (m_indexedShader != VK_NULL_HANDLE) vkDestroyShaderModule(dev, m_indexedShader, nullptr);
    if (m_indexedPipeLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(dev, m_indexedPipeLayout, nullptr);
    if (m_indexedSetLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(dev, m_indexedSetLayout, nullptr);
    if (m_descPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(dev, m_descPool, nullptr);
    if (m_adjustListPipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(dev, m_adjustListPipeline, nullptr);
    if (m_adjustPipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, m_adjustPipeline, nullptr);
    if (m_adjustShader != VK_NULL_HANDLE) vkDestroyShaderModule(dev, m_adjustShader, nullptr);
    if (m_listPipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, m_listPipeline, nullptr);
    if (m_pipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, m_pipeline, nullptr);
    if (m_shader != VK_NULL_HANDLE) vkDestroyShaderModule(dev, m_shader, nullptr);
    if (m_pipeLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, m_pipeLayout, nullptr);
    if (m_setLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev, m_setLayout, nullptr);
    if (m_pool != VK_NULL_HANDLE) vkDestroyCommandPool(dev, m_pool, nullptr);
}

const GpuCaps& TileCompositor::caps() const noexcept { return m_ctx->caps(); }

std::uint32_t TileCompositor::validationErrors() const noexcept {
    return m_ctx ? m_ctx->validationErrors() : 0;
}

bool TileCompositor::initPipeline(std::string& error) {
    const VkDevice dev = m_ctx->device();
    m_pool = m_ctx->createCommandPool(error);
    if (m_pool == VK_NULL_HANDLE) return false;

    // Binding 5 (the dirty-macrotile list) is declared for BOTH specializations, not only the one
    // that reads it: the shader statically references the buffer in either case -- a
    // specialization constant is resolved at pipeline creation, not at glslc time -- so a set
    // layout without it would be a layout/shader mismatch on the per-tile pipeline too.
    const VkDescriptorSetLayoutBinding bindings[6] = {
        {kBindingAcc, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {kBindingSrc, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
         nullptr},
        {kBindingMask, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
         nullptr},
        {kBindingClip, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
         nullptr},
        {kBindingClipOut, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT,
         nullptr},
        {kBindingTiles, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
         nullptr},
    };
    const VkDescriptorSetLayoutCreateInfo slci{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 6,
        .pBindings = bindings,
    };
    if (vkCreateDescriptorSetLayout(dev, &slci, nullptr, &m_setLayout) != VK_SUCCESS) {
        error = "vkCreateDescriptorSetLayout failed";
        return false;
    }
    const VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushBlock)};
    const VkPipelineLayoutCreateInfo plci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &m_setLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr,
    };
    if (vkCreatePipelineLayout(dev, &plci, nullptr, &m_pipeLayout) != VK_SUCCESS) {
        error = "vkCreatePipelineLayout failed";
        return false;
    }
    const VkShaderModuleCreateInfo smci{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = mosaic::shaders::composite_tile_comp_size,
        .pCode = mosaic::shaders::composite_tile_comp,
    };
    if (vkCreateShaderModule(dev, &smci, nullptr, &m_shader) != VK_SUCCESS) {
        error = "vkCreateShaderModule failed";
        return false;
    }
    // TWO PIPELINES FROM ONE MODULE: the same SPIR-V specialized with kTileList = 0 (one dispatch
    // per macrotile, the reference) and = 1 (one dispatch per layer, the list reads the geometry).
    // Both values are stated rather than one being left to the shader's default, because "we chose
    // the per-tile shape" and "we forgot to specialize" should not look the same in the code.
    const VkSpecializationMapEntry specEntry{kSpecTileList, 0, sizeof(std::int32_t)};
    const std::int32_t specValues[2] = {0, 1};
    const VkSpecializationInfo specInfos[2] = {
        {1, &specEntry, sizeof(std::int32_t), &specValues[0]},
        {1, &specEntry, sizeof(std::int32_t), &specValues[1]},
    };
    VkComputePipelineCreateInfo cpci[2] = {};
    for (int i = 0; i < 2; ++i) {
        cpci[i] = VkComputePipelineCreateInfo{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                      .module = m_shader,
                      .pName = "main",
                      .pSpecializationInfo = &specInfos[i]},
            .layout = m_pipeLayout,
        };
    }
    VkPipeline pipes[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 2, cpci, nullptr, pipes) != VK_SUCCESS) {
        error = "vkCreateComputePipelines failed";
        return false;
    }
    m_pipeline = pipes[0];
    m_listPipeline = pipes[1];

    // ---- The ADJUSTMENT kernel, on the SAME layouts ---------------------------------------------
    //
    // A different shader, not a different lane: adjust_tile.comp declares a SUBSET of the bindings
    // above (it has no source sheet and publishes no clip base) and pushes a SMALLER block into the
    // same 124-byte range, so it reuses `m_setLayout` and `m_pipeLayout` verbatim. That is what
    // keeps the descriptor pool, the per-(layer, atlas image) set allocation and the dispatch
    // geometry identical for both kernels -- the only thing an adjustment step changes in the
    // command buffer is which pipeline is bound.
    //
    // A shader may declare fewer bindings than its set layout has; the host still writes every one,
    // because an unwritten descriptor is undefined behaviour even for a binding nothing reads.
    const VkShaderModuleCreateInfo asmci{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = mosaic::shaders::adjust_tile_comp_size,
        .pCode = mosaic::shaders::adjust_tile_comp,
    };
    if (vkCreateShaderModule(dev, &asmci, nullptr, &m_adjustShader) != VK_SUCCESS) {
        error = "vkCreateShaderModule failed for the adjustment kernel";
        return false;
    }
    VkComputePipelineCreateInfo acpci[2] = {};
    for (int i = 0; i < 2; ++i) {
        acpci[i] = VkComputePipelineCreateInfo{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                      .module = m_adjustShader,
                      .pName = "main",
                      .pSpecializationInfo = &specInfos[i]},
            .layout = m_pipeLayout,
        };
    }
    VkPipeline adjustPipes[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 2, acpci, nullptr, adjustPipes) !=
        VK_SUCCESS) {
        error = "vkCreateComputePipelines failed for the adjustment kernel";
        return false;
    }
    m_adjustPipeline = adjustPipes[0];
    m_adjustListPipeline = adjustPipes[1];

    // texelFetch only -- every reconstruction kernel is evaluated in the shader, never by the
    // sampler -- so this state is deliberately inert. It exists because the bindings are
    // COMBINED_IMAGE_SAMPLER, not because anything filters.
    const VkSamplerCreateInfo sci{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
    };
    if (vkCreateSampler(dev, &sci, nullptr, &m_sampler) != VK_SUCCESS) {
        error = "vkCreateSampler failed";
        return false;
    }
    const VkCommandBufferAllocateInfo cbai{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = m_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (vkAllocateCommandBuffers(dev, &cbai, &m_cmd) != VK_SUCCESS) {
        error = "vkAllocateCommandBuffers failed";
        return false;
    }
    const VkFenceCreateInfo fci{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(dev, &fci, nullptr, &m_fence) != VK_SUCCESS) {
        error = "vkCreateFence failed";
        return false;
    }
    // The dirty-macrotile list starts out small and non-null, and it stays non-null for the rest of
    // this object's life. That matters even for the per-tile shape: its pipeline never READS the
    // buffer (the specialization folds the branch away) but the shader MODULE still declares
    // binding 5, and a descriptor set with an unwritten binding is undefined behaviour whether or
    // not anything reads it. A buffer that always exists is one fewer state to be wrong about.
    if (!makeDeviceBuffer(sizeof(TileRecord) * 64,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          m_tileList, error))
        return false;
    return true;
}

bool TileCompositor::initIndexedPipeline() {
    // ---- The caps gate, in the order that makes the refusal readable ---------------------------
    //
    // ⚠ TWO questions, and asking only the first is the classic mistake. `descriptorIndexing` says
    // the DEVICE can index descriptors; `fitsSpirvVersion` says it can LOAD the blob we compiled to
    // use that. They come apart on a Vulkan 1.0/1.1 device carrying VK_EXT_descriptor_indexing --
    // real hardware, not a hypothetical -- where the feature is present and a newer-SPIR-V module
    // is rejected outright. Neither question is a version test at the call site.
    const GpuCaps& c = caps();
    if (!c.descriptorIndexing) {
        m_indexedRefusal = DispatchRefusal::NoDescriptorIndexing;
        return false;
    }
    if (!c.fitsSpirvVersion(spirv::kVersion1_3)) {
        m_indexedRefusal = DispatchRefusal::SpirvUnsupported;
        return false;
    }
    // The runtime array is sized from the device's own sampled-image budget, minus the one the
    // clip base takes, and capped at kIndexedMaxLayers. Rounded DOWN to an even number because a
    // layer occupies a (pixels, mask) pair and half a layer is not a thing.
    const std::uint32_t budget = std::min(c.limits.maxPerStageDescriptorSampledImages,
                                          c.limits.maxDescriptorSetSampledImages);
    const std::uint32_t room = budget > 1 ? (budget - 1) & ~1u : 0u;
    m_indexedMaxSources = std::min(2u * kIndexedMaxLayers, room);
    if (m_indexedMaxSources < 2) {
        m_indexedMaxSources = 0;
        m_indexedRefusal = DispatchRefusal::DescriptorBudget;
        return false;
    }

    const VkDevice dev = m_ctx->device();
    // ONE set for the whole composite's sources. The bindings the per-layer shape rewrites per
    // layer (1 and 2) are gone; what is left varies per ATLAS IMAGE, of which a document that fits
    // maxImageDimension2D has exactly one. The array is binding 6 because a variable descriptor
    // count is legal only on a set's LAST binding.
    const VkDescriptorSetLayoutBinding bindings[5] = {
        {kBindingAcc, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {kBindingClip, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
         nullptr},
        {kBindingClipOut, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT,
         nullptr},
        {kBindingTiles, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
         nullptr},
        {kBindingSources, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, m_indexedMaxSources,
         VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    // PARTIALLY_BOUND so a set allocated for N layers need not write descriptors nothing reads;
    // VARIABLE_DESCRIPTOR_COUNT so a two-layer document allocates two layers' worth rather than
    // the layout's maximum. Both are among the four sub-features `decide()` demands before it sets
    // `descriptorIndexing` at all, so asking for them here cannot surprise the device.
    const VkDescriptorBindingFlags bindingFlags[5] = {
        0, 0, 0, 0,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT,
    };
    const VkDescriptorSetLayoutBindingFlagsCreateInfo bfci{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount = 5,
        .pBindingFlags = bindingFlags,
    };
    const VkDescriptorSetLayoutCreateInfo slci{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = &bfci,
        .bindingCount = 5,
        .pBindings = bindings,
    };
    if (vkCreateDescriptorSetLayout(dev, &slci, nullptr, &m_indexedSetLayout) != VK_SUCCESS) {
        m_indexedRefusal = DispatchRefusal::PipelineFailed;
        return false;
    }
    // The SAME push block: the variant spends the per-tile lanes on a layer index rather than
    // growing the range, so both pipeline layouts declare 124 bytes and the host has one struct.
    const VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushBlock)};
    const VkPipelineLayoutCreateInfo plci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &m_indexedSetLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr,
    };
    if (vkCreatePipelineLayout(dev, &plci, nullptr, &m_indexedPipeLayout) != VK_SUCCESS) {
        m_indexedRefusal = DispatchRefusal::PipelineFailed;
        return false;
    }
    const VkShaderModuleCreateInfo smci{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = mosaic::shaders::composite_tile_indexed_comp_size,
        .pCode = mosaic::shaders::composite_tile_indexed_comp,
    };
    if (vkCreateShaderModule(dev, &smci, nullptr, &m_indexedShader) != VK_SUCCESS) {
        m_indexedRefusal = DispatchRefusal::PipelineFailed;
        return false;
    }
    const VkComputePipelineCreateInfo cpci{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = m_indexedShader,
                  .pName = "main"},
        .layout = m_indexedPipeLayout,
    };
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &m_indexedPipeline) !=
        VK_SUCCESS) {
        m_indexedRefusal = DispatchRefusal::PipelineFailed;
        return false;
    }
    // The ADJUSTMENT kernel's indexed twin, on this same set layout. It is part of the SAME gate on
    // purpose: a device that can run one of the two variants but not the other would make the
    // active dispatch shape depend on what a document happens to CONTAIN, and `activeDispatch()`
    // would stop being answerable without one. Failing here refuses the whole tier, which costs
    // descriptor writes and not one pixel.
    const VkShaderModuleCreateInfo asmci{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = mosaic::shaders::adjust_tile_indexed_comp_size,
        .pCode = mosaic::shaders::adjust_tile_indexed_comp,
    };
    if (vkCreateShaderModule(dev, &asmci, nullptr, &m_indexedAdjustShader) != VK_SUCCESS) {
        m_indexedRefusal = DispatchRefusal::PipelineFailed;
        return false;
    }
    const VkComputePipelineCreateInfo acpci{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = m_indexedAdjustShader,
                  .pName = "main"},
        .layout = m_indexedPipeLayout,
    };
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &acpci, nullptr,
                                 &m_indexedAdjustPipeline) != VK_SUCCESS) {
        m_indexedRefusal = DispatchRefusal::PipelineFailed;
        return false;
    }
    m_indexedRefusal = DispatchRefusal::None;
    return true;
}

bool TileCompositor::ensureIndexedPool(std::uint32_t sets, std::string& error) {
    if (m_indexedPool != VK_NULL_HANDLE && m_indexedPoolSets >= sets)
        return vkResetDescriptorPool(m_ctx->device(), m_indexedPool, 0) == VK_SUCCESS;
    if (m_indexedPool != VK_NULL_HANDLE) {
        m_ctx->waitIdle();
        vkDestroyDescriptorPool(m_ctx->device(), m_indexedPool, nullptr);
        m_indexedPool = VK_NULL_HANDLE;
    }
    // One set per accumulator atlas image, so this is tiny -- but each set may draw the array's
    // full width, so the sampled-image pool has to be sized for the layout's maximum rather than
    // for today's layer count.
    const std::uint32_t want = std::max<std::uint32_t>(sets * 2, 4);
    const VkDescriptorPoolSize sizes[3] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, want * 2},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, want * (m_indexedMaxSources + 1)},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, want},
    };
    const VkDescriptorPoolCreateInfo dpci{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = want,
        .poolSizeCount = 3,
        .pPoolSizes = sizes,
    };
    if (vkCreateDescriptorPool(m_ctx->device(), &dpci, nullptr, &m_indexedPool) != VK_SUCCESS) {
        error = "vkCreateDescriptorPool failed for the indexed dispatch";
        m_indexedPoolSets = 0;
        return false;
    }
    m_indexedPoolSets = want;
    return true;
}

TileDispatch TileCompositor::activeDispatch() const noexcept {
    // BOTH kernels, not only the composite one. `initIndexedPipeline` builds the composite variant
    // first, so a device that loads that blob and declines the adjustment one would leave this
    // handle set and the other null -- and the shape would then depend on what a document happens
    // to CONTAIN, which is exactly the question this function must be able to answer on its own.
    const bool haveIndexed =
        m_indexedPipeline != VK_NULL_HANDLE && m_indexedAdjustPipeline != VK_NULL_HANDLE;
    switch (m_requested) {
        case TileDispatch::PerTile: return TileDispatch::PerTile;
        case TileDispatch::TileList: return TileDispatch::TileList;
        case TileDispatch::Indexed:
        case TileDispatch::Auto: break;
    }
    // An explicit Indexed downgrades to the floor shape when the device cannot serve it -- never a
    // failure. Auto does NOT reach for the extension at all, and that is a measurement, not a
    // preference: on the RX 6600 XT (2026-07-29, quiet box, 20 iterations) the two are the same
    // lane to within run-to-run noise -- `gpu full` 5.683 vs 5.713 ms at 3840x2160 and 23.313 vs
    // 23.331 ms at 5000x8000 -- because the SSBO tile list already collapses the dispatch to one
    // per layer, and indexing the SOURCES only removes descriptor writes the driver was not
    // charging much for. The list shape needs nothing beyond the Vulkan 1.0 floor; Indexed needs a
    // second SPIR-V blob, a runtime-sized descriptor array and a device-dependent code path. Equal
    // speed at strictly greater risk is not a default. MOSAIC_TILE_DISPATCH=indexed keeps it
    // exercised, and this comment is the thing to re-read if a future device disagrees.
    if (m_requested == TileDispatch::Auto)
        return TileDispatch::TileList;
    return haveIndexed ? TileDispatch::Indexed : TileDispatch::TileList;
}

bool TileCompositor::initResolvePipeline(std::string& error) {
    const VkDevice dev = m_ctx->device();
    // Two storage images and nothing else: the resolve is a format conversion, not a composite.
    const VkDescriptorSetLayoutBinding bindings[2] = {
        {kResolveBindingAcc, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT,
         nullptr},
        {kResolveBindingDst, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT,
         nullptr},
    };
    const VkDescriptorSetLayoutCreateInfo slci{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = bindings,
    };
    if (vkCreateDescriptorSetLayout(dev, &slci, nullptr, &m_resolveSetLayout) != VK_SUCCESS) {
        error = "vkCreateDescriptorSetLayout failed for the resolve pass";
        return false;
    }
    const VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ResolvePush)};
    const VkPipelineLayoutCreateInfo plci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &m_resolveSetLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr,
    };
    if (vkCreatePipelineLayout(dev, &plci, nullptr, &m_resolvePipeLayout) != VK_SUCCESS) {
        error = "vkCreatePipelineLayout failed for the resolve pass";
        return false;
    }
    const VkShaderModuleCreateInfo smci{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = mosaic::shaders::tile_resolve_comp_size,
        .pCode = mosaic::shaders::tile_resolve_comp,
    };
    if (vkCreateShaderModule(dev, &smci, nullptr, &m_resolveShader) != VK_SUCCESS) {
        error = "vkCreateShaderModule failed for the resolve pass";
        return false;
    }
    const VkComputePipelineCreateInfo cpci{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = m_resolveShader,
                  .pName = "main"},
        .layout = m_resolvePipeLayout,
    };
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &m_resolvePipeline) !=
        VK_SUCCESS) {
        error = "vkCreateComputePipelines failed for the resolve pass";
        return false;
    }
    return true;
}

bool TileCompositor::ensureResolvePool(std::uint32_t sets, std::string& error) {
    if (m_resolvePool != VK_NULL_HANDLE && m_resolvePoolSets >= sets)
        return vkResetDescriptorPool(m_ctx->device(), m_resolvePool, 0) == VK_SUCCESS;
    if (m_resolvePool != VK_NULL_HANDLE) {
        m_ctx->waitIdle();
        vkDestroyDescriptorPool(m_ctx->device(), m_resolvePool, nullptr);
        m_resolvePool = VK_NULL_HANDLE;
    }
    // One set per atlas image, so this is tiny and grows only when the document does.
    const std::uint32_t want = std::max<std::uint32_t>(sets * 2, 4);
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, want * 2};
    const VkDescriptorPoolCreateInfo dpci{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = want,
        .poolSizeCount = 1,
        .pPoolSizes = &size,
    };
    if (vkCreateDescriptorPool(m_ctx->device(), &dpci, nullptr, &m_resolvePool) != VK_SUCCESS) {
        error = "vkCreateDescriptorPool failed for the resolve pass";
        m_resolvePoolSets = 0;
        return false;
    }
    m_resolvePoolSets = want;
    return true;
}

bool TileCompositor::initStaticImages(std::string& error) {
    // 1x1 stand-ins, so a descriptor set is never written with a hole. A binding the shader does
    // not read must still be a valid descriptor: the spec is explicit that an unwritten one is
    // undefined behaviour even for an unused binding, and validation layers say so loudly.
    if (!makeImage(1, 1, VK_FORMAT_R8_UNORM,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, m_dummyMask,
                   error))
        return false;
    if (!makeImage(1, 1, VK_FORMAT_R16_SFLOAT,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, m_dummyClipRead,
                   error))
        return false;
    if (!makeImage(1, 1, VK_FORMAT_R16_SFLOAT,
                   VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, m_dummyClipWrite,
                   error))
        return false;

    GpuBuffer stage;
    if (!makeHostBuffer(4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, stage, error)) return false;
    const std::uint8_t full = 255;  // an all-revealing mask, so a stray read cannot hide a bug
    std::memcpy(stage.mapped, &full, 1);

    vkResetCommandBuffer(m_cmd, 0);
    const VkCommandBufferBeginInfo cbi{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(m_cmd, &cbi);

    const auto transition = [&](const GpuImage& img, VkImageLayout newLayout, VkAccessFlags dst) {
        const VkImageMemoryBarrier b{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = dst,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = newLayout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = img.image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
    };
    transition(m_dummyMask, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT);
    const VkBufferImageCopy copy{
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {1, 1, 1},
    };
    vkCmdCopyBufferToImage(m_cmd, stage.buffer, m_dummyMask.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    const VkImageMemoryBarrier toRead{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = m_dummyMask.image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &toRead);
    // The two clip stand-ins are never read or written -- only bound -- so they need a legal
    // layout and nothing else.
    transition(m_dummyClipRead, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_READ_BIT);
    transition(m_dummyClipWrite, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT);
    vkEndCommandBuffer(m_cmd);

    vkResetFences(m_ctx->device(), 1, &m_fence);
    const VkSubmitInfo si{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &m_cmd,
    };
    if (m_ctx->submit(si, m_fence) != VK_SUCCESS) {
        error = "vkQueueSubmit failed while priming the stand-in images";
        destroyBuffer(stage);
        return false;
    }
    vkWaitForFences(m_ctx->device(), 1, &m_fence, VK_TRUE, UINT64_MAX);
    destroyBuffer(stage);
    return true;
}

// ---- Geometry ----------------------------------------------------------------------------------

bool TileCompositor::setDocumentSize(std::uint32_t w, std::uint32_t h, std::string& error) {
    if (w == m_docW && h == m_docH && !m_acc.empty()) return true;
    m_ctx->waitIdle();
    destroyAtlas();
    m_docW = w;
    m_docH = h;
    m_accValid = false;
    m_lastPlan.clear();
    m_dirty.reset(core::TileGrid(w, h, kDirtyTileSize));
    // Tile indices mean nothing across grids, so the unresolved set is re-targeted with the dirty
    // one; the resolve target is forgotten too, which is what makes the next resolve a full one.
    m_unresolved.reset(core::TileGrid(w, h, kDirtyTileSize));
    m_lastResolveImage = VK_NULL_HANDLE;
    m_lastResolveW = 0;
    m_lastResolveH = 0;
    if (w == 0 || h == 0) {
        m_macroGrid = core::TileGrid();
        return true;
    }
    if (!buildAtlas(error)) {
        destroyAtlas();
        return false;
    }
    markAllDirty();
    return true;
}

bool TileCompositor::buildAtlas(std::string& error) {
    const GpuCaps& c = caps();
    // The macrotile is `64 << k` with k from GpuCaps -- but never larger than an image the device
    // will actually create. Halving down to the dirty tile keeps k a free knob (§3.1) instead of
    // a device requirement.
    m_macrotile = std::max(c.macrotileSize, kDirtyTileSize);
    while (m_macrotile > c.maxImageDim && m_macrotile > kDirtyTileSize) m_macrotile >>= 1;
    m_macroGrid = core::TileGrid(m_docW, m_docH, m_macrotile);

    // The atlas image is a whole number of macrotile slots, so every slot copy is a full
    // `m_macrotile` square and no edge case has to be special-cased in the clear or the readback.
    // A document that fits under maxImageDimension2D therefore gets ONE atlas image whose slots
    // sit at their natural document positions -- the tiled path and the simple path are the same
    // code, which is the only way the tiled path stays tested.
    const std::uint32_t cap = alignDown(c.maxImageDim, m_macrotile);
    if (cap == 0) {
        error = "maxImageDimension2D below one macrotile";
        return false;
    }
    const std::uint32_t atlasW = std::min(cap, alignUp(m_docW, m_macrotile));
    const std::uint32_t atlasH = std::min(cap, alignUp(m_docH, m_macrotile));
    m_slotsX = atlasW / m_macrotile;
    m_slotsY = atlasH / m_macrotile;
    m_slotsPerImage = m_slotsX * m_slotsY;
    if (m_slotsPerImage == 0) {
        error = "atlas holds no macrotile slots";
        return false;
    }
    const std::uint64_t needed = m_macroGrid.tileCount();
    const std::uint64_t images = (needed + m_slotsPerImage - 1) / m_slotsPerImage;

    const std::uint64_t perImage =
        static_cast<std::uint64_t>(atlasW) * atlasH * c.workingFormatBytes();
    m_accBytes = images * perImage;

    // Budget BEFORE allocating: a resident accumulator that does not fit is a refusal, not an
    // out-of-memory crash. 5000x8000 at rgba16f is 320 MiB and that is the honest price of
    // residency -- the caller stays on the CPU lane when the device cannot pay it.
    const GpuMemoryBudget mem = queryGpuMemory(*m_ctx);
    const std::uint64_t budget = atlasBudgetBytes(mem, c);
    if (m_accBytes > budget) {
        error = "resident accumulator (" + std::to_string(m_accBytes / (1024 * 1024)) +
                " MiB) exceeds the atlas budget (" + std::to_string(budget / (1024 * 1024)) +
                " MiB)";
        return false;
    }

    m_acc.resize(static_cast<std::size_t>(images));
    for (GpuImage& img : m_acc) {
        // SAMPLED is not used by this kernel -- the accumulator is only ever a storage image here
        // -- but it is what plan item 11 needs when the present pass samples the accumulator
        // directly instead of a re-uploaded CPU copy. Asking for it now costs nothing (rgba16f is
        // guaranteed for both) and saves reallocating every atlas image later.
        if (!makeImage(atlasW, atlasH, c.workingFormat,
                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       img, error))
            return false;
    }
    // Everything the source cache may hold is what is left after the accumulator. A shrink
    // evicts pages, and a page is only a ledger entry -- the thing that actually goes is the
    // layer image behind it, so the eviction list has to be honoured, not discarded.
    releaseEvicted(
        m_residency.reconfigure(budget > m_accBytes ? budget - m_accBytes : 0, kSourcePageBytes));
    return true;
}

void TileCompositor::destroyAtlas() noexcept {
    for (GpuImage& img : m_acc) destroyImage(img);
    m_acc.clear();
    for (GpuImage& img : m_clip) destroyImage(img);
    m_clip.clear();
    // The zero buffer is one macrotile wide, and the macrotile can change with the document
    // (buildAtlas halves k until a slot fits maxImageDimension2D), so it belongs to the atlas.
    destroyBuffer(m_zero);
    m_accValid = false;
    m_accInitialised = false;
    m_clipInitialised = false;
}

void TileCompositor::destroySources() noexcept {
    for (auto& [id, src] : m_sources) {
        destroyImage(src.pixels);
        destroyImage(src.mask);
    }
    m_sources.clear();
    m_uploading.clear();
    m_residency.clear();
}

TileCompositor::Slot TileCompositor::slotFor(std::uint32_t mx, std::uint32_t my) const noexcept {
    Slot s;
    if (m_slotsPerImage == 0) return s;
    const std::uint64_t idx = static_cast<std::uint64_t>(my) * m_macroGrid.tilesX() + mx;
    s.image = static_cast<std::uint32_t>(idx / m_slotsPerImage);
    const std::uint32_t within = static_cast<std::uint32_t>(idx % m_slotsPerImage);
    s.x = static_cast<std::int32_t>((within % m_slotsX) * m_macrotile);
    s.y = static_cast<std::int32_t>((within / m_slotsX) * m_macrotile);
    return s;
}

// ---- The dirty set -----------------------------------------------------------------------------

void TileCompositor::reset() noexcept {
    if (m_ctx) m_ctx->waitIdle();
    destroySources();
    m_lastPlan.clear();
    m_lastDoc = nullptr;
    m_accValid = false;  // the layout stays valid; only the CONTENT is now meaningless
    markAllDirty();
    markResolveDirty();  // whatever the present texture holds is the OTHER document
}

void TileCompositor::markAllDirty() noexcept { m_dirty.addAll(); }

void TileCompositor::markResolveDirty() noexcept {
    m_unresolved.addAll();
    m_lastResolveImage = VK_NULL_HANDLE;
}

void TileCompositor::markDirty(const common::Rect& docRect) noexcept { m_dirty.add(docRect); }

void TileCompositor::markLayerDirty(core::LayerId id) noexcept {
    dropLayer(id);
    // Dirty where the layer WAS, and poison its fingerprint so the next plan diff also dirties
    // where it now IS. Clearing the whole plan instead would be correct but would dirty the
    // canvas for every brush dab, which is exactly the cost this class exists to avoid.
    bool found = false;
    for (Step& s : m_lastPlan) {
        if (s.layer == id) {
            m_dirty.add(s.bounds);
            s.fingerprint = 0;  // reserved: planDocument forces every real fingerprint odd
            found = true;
        }
    }
    // Never seen it: nothing bounds the change, so nothing may be assumed clean.
    if (!found) markAllDirty();
}

void TileCompositor::markLayerDirty(const core::RasterLayer& layer,
                                    const common::Rect& layerRect) noexcept {
    const core::LayerId id = layer.id();
    // An empty rect is not "nothing changed", it is a caller with no region to give. The safe
    // reading of "somewhere" is "everywhere", which is precisely the other overload.
    if (layerRect.empty()) {
        markLayerDirty(id);
        return;
    }

    const common::Image& img = layer.image();
    const auto it = m_sources.find(id);
    if (it != m_sources.end()) {
        LayerSource& src = it->second;
        // A raster layer's SOURCE revision IS its content revision (leafSourceFor), which is why
        // this overload can take one and the cache-backed kinds cannot be named this way at all:
        // their pixels are replaced wholesale by a re-render, never edited in a region.
        const std::uint64_t rev = layer.contentRevision();
        // Extend the staleness ledger by ONE revision step at a time. `rev == pendingRevision`
        // is a caller naming a second rect for the same edit (an old and a new position, say);
        // `rev == pendingRevision + 1` is the next edit. Anything else means a content change
        // went by unnamed, and the bytes it moved would be lost inside a neighbouring claim --
        // so the ledger admits it does not know, and the next composite re-sends the image.
        //
        // The grid check is the same admission in another key: a resized image invalidates every
        // tile index in `pending`, and reinterpreting them would upload the wrong pixels to the
        // wrong place. (Re-targeting the set here would allocate; this function must not throw,
        // and the full upload rebuilds it correctly anyway.)
        const bool contiguous = rev == src.pendingRevision || rev == src.pendingRevision + 1;
        const bool sameShape = src.pixels.width == img.width && src.pixels.height == img.height &&
                               src.pending.grid().width() == img.width &&
                               src.pending.grid().height() == img.height &&
                               src.pending.grid().tileSize() == kDirtyTileSize;
        if (!src.pendingKnown || !contiguous || !sameShape) {
            src.pendingKnown = false;
        } else {
            src.pending.add(layerRect);
            src.pendingRevision = rev;
        }
    }

    // ... and the accumulator side. The rect is dirtied HERE rather than left to the plan diff
    // because the two are not equivalent: an edit that changes pixels WITHOUT bumping
    // contentRevision is invisible to the diff (the fingerprints all match) but is exactly the
    // thing this call is reporting, and the upload above will send it. Dirtying at the call site
    // makes the recomposite unconditional on the revision bookkeeping.
    for (const Step& s : m_lastPlan) {
        if (s.layer != id) continue;
        m_dirty.add(placedRectBounds(s.place, layerRect, s.pad));
        return;
    }
    // The lane has never composited this layer, so nothing here maps layer pixels into document
    // space and nothing may be assumed clean.
    markAllDirty();
}

// ---- Planning ----------------------------------------------------------------------------------

TileRefusal TileCompositor::planDocument(const core::Document& doc, const CompositeOptions& opts,
                                         std::vector<Step>& out, std::string& why) const {
    out.clear();
    why.clear();
    const core::GroupLayer& root = doc.root();
    const GpuCaps& c = caps();
    // The parent-space rect a layer mask spans at the DOCUMENT ROOT, which is the only place this
    // lane plans: compositor.cpp threads exactly this in as `GroupWalk::maskDomain`, and the walk's
    // placement `pre` is the identity here.
    const common::Rect docRect{0.0, 0.0, static_cast<double>(doc.width()),
                               static_cast<double>(doc.height())};

    // `anyClips` decides whether a non-clipped layer has to publish a clip base at all -- exactly
    // compositor.cpp's groupHasClips, and skipping the publish when nothing clips is what keeps
    // the common document's dispatch free of the extra store.
    bool anyClips = false;
    for (std::size_t i = 0; i < root.childCount(); ++i)
        if (root.child(i).clipToBelow()) anyClips = true;

    bool haveClipBase = false;
    for (std::size_t i = 0; i < root.childCount(); ++i) {
        const core::Layer& layer = root.child(i);
        // The CPU walk skips these before doing anything else; matching the skip exactly is what
        // keeps the clip-base bookkeeping below in step with it.
        if (!layer.visible() || layer.opacity() <= 0.0f) continue;

        if (layer.as<core::GroupLayer>() != nullptr) return TileRefusal::NestedGroup;
        // ---- ADJUSTMENT LAYERS ---------------------------------------------------------------
        //
        // AHEAD of the effects and partition gates, and that ordering is a mirror rather than a
        // convenience: compositeChildren routes an adjustment straight to walkStep without ever
        // calling renderLayer, so neither layer effects nor a coverage partition can reach one on
        // the CPU lane either. Refusing on them here would decline a document the golden lane
        // composites without so much as looking at them.
        if (const auto* adjLayer = layer.as<core::AdjustmentLayer>()) {
            const AdjustPlan ap = planAdjustment(*adjLayer);
            if (ap.refusal != TileRefusal::None) {
                why = std::string(core::adjustmentKindName(adjLayer->adjustmentKind())) + ": " +
                      ap.reason;
                return ap.refusal;
            }
            // A defaults bag is a byte-level no-op -- applyAdjustment returns before touching a
            // pixel -- so the lane must not dispatch either, and it must not touch the clip-base
            // bookkeeping below: an adjustment never publishes a base, identity or not.
            if (ap.identity) continue;

            Step s;
            s.layer = layer.id();
            s.adjust = true;
            s.adjustKind = ap.kind;
            s.adjustFlags = ap.flags;
            for (int k = 0; k < 12; ++k) s.adjustParams[k] = ap.p[k];
            s.opacity = layer.opacity();
            // ⚠ NORMAL, whatever the layer says, and deliberately: walkStep takes the adjustment
            // branch before any blend() call, so the mode never reaches a pixel on the golden lane.
            // Hashing the layer's real mode into the fingerprint below would make changing it dirty
            // the whole canvas for a recomposite that produces identical bytes.
            s.blend = core::BlendMode::Normal;
            // The layer's own transform reaches the picture ONLY through where its mask sheet
            // lands (adjustmentMaskDomain), so `place` is carried for the fingerprint's sake and
            // `inverse` is never read. Keeping the transform in the hash is what makes a Move on a
            // masked adjustment a plan change.
            s.place = layer.transform();

            const core::RasterMask* mk = layer.mask();
            if (mk != nullptr && (!mk->enabled || mk->empty())) mk = nullptr;
            if (mk != nullptr) {
                if (!c.fitsImage(mk->width, mk->height)) return TileRefusal::LayerTooLarge;
                // adjustmentMaskDomain, verbatim: an UNLINKED sheet is already in parent space, so
                // only a linked one takes the layer's own transform.
                const common::Affine2D place = mk->linked
                                                   ? layer.transform() * core::maskPlacement(layer, *mk)
                                                   : core::maskPlacement(layer, *mk);
                const common::Rect dom =
                    isIdentityAffine(place)
                        ? docRect
                        : place.mapBounds(common::Rect{0.0, 0.0, static_cast<double>(mk->width),
                                                       static_cast<double>(mk->height)});
                // adjustmentMaskAt returns FULL coverage for a degenerate sheet or domain, which is
                // the same picture as no mask at all -- and one fewer sampled image to bind.
                if (dom.w > 0.0 && dom.h > 0.0) {
                    s.maskMode = 1;
                    s.maskMap[0] = static_cast<float>(static_cast<double>(mk->width) / dom.w);
                    s.maskMap[1] = static_cast<float>(static_cast<double>(mk->height) / dom.h);
                    s.maskMap[2] = static_cast<float>(dom.x);
                    s.maskMap[3] = static_cast<float>(dom.y);
                }
            }

            s.clip = layer.clipToBelow() && haveClipBase;
            s.clipWrite = false;  // walkStep returns before the capture: never a clip base

            // ⚠ THE WHOLE CANVAS, always, and it is not laziness. An unmasked adjustment plainly
            // reaches every pixel; a MASKED one does too, because adjustmentMaskAt CLAMPS its
            // sample into the sheet's domain rather than reading zero outside it, so the edge
            // texel's coverage carries on to the canvas border. There is no rectangle outside
            // which a masked adjustment is guaranteed to be a no-op, so there is none to name.
            s.bounds = docRect;
            s.pad = 0.0;

            std::uint64_t h = 1469598103934665603ull;
            hashValue(h, s.place.m00);
            hashValue(h, s.place.m01);
            hashValue(h, s.place.m02);
            hashValue(h, s.place.m10);
            hashValue(h, s.place.m11);
            hashValue(h, s.place.m12);
            hashValue(h, s.opacity);
            hashValue(h, s.maskMode);
            hashValue(h, s.adjustKind);
            hashValue(h, s.adjustFlags);
            for (int k = 0; k < 12; ++k) hashValue(h, s.adjustParams[k]);
            for (int k = 0; k < 4; ++k) hashValue(h, s.maskMap[k]);
            const std::uint8_t adjFlags = static_cast<std::uint8_t>(s.clip ? 1u : 0u);
            hashValue(h, adjFlags);
            s.fingerprint = h | 1ull;
            // The TABLE's staleness key, and nothing else: a kind with no table has nothing on the
            // device that a parameter edit can invalidate, so it keeps a standing 0 and the
            // fingerprint above (which hashes the resolved scalars) carries the change on its own.
            // Moving it for every edit instead would re-upload an unrelated MASK on every slider
            // tick, because the residency ledger keys both sheets off the one revision.
            s.sourceRevision = ap.table ? ap.fingerprint : 0;
            s.maskRevision = layer.maskRevision();
            out.push_back(s);
            continue;
        }
        if (layer.hasEffects() && !layer.effects().empty()) return TileRefusal::LayerEffects;
        if (core::livePartitionFor(layer)) return TileRefusal::LivePartition;
        // WHICH image this leaf composites from, and where that image sits in layer-local space.
        // A kind with no fixed-resolution source (vector), and an empty or unpopulated one, both
        // refuse here -- see leafSourceFor for why an empty source is not a transparent layer.
        LeafSource src;
        if (const TileRefusal why = leafSourceFor(layer, src); why != TileRefusal::None) return why;
        if (!c.fitsImage(src.width, src.height)) return TileRefusal::LayerTooLarge;
        // The float lane uploads R16G16B16A16_SFLOAT, so it needs that format to be usable here.
        // `create()` already refuses the WHOLE lane when `workingFormat` is not rgba16f (the
        // kernel's storage-image qualifier hard-codes it), so this is unreachable today -- it is
        // written anyway because it is the float arm's OWN precondition, and the day the
        // accumulator gains a second working format is the day this stops being redundant and
        // starts being the thing that keeps a sky cache off a device that cannot sample it.
        // GpuCaps asks no finer question about a format than which one it settled on.
        if (src.pixelsF != nullptr && c.workingFormat != VK_FORMAT_R16G16B16A16_SFLOAT)
            return TileRefusal::DeviceTooSmall;

        Step s;
        s.layer = layer.id();
        // SOURCE px -> document. `pre` is the identity at the document root, and `toLayer` is the
        // identity for every kind whose source IS its own storage -- so this is BIT-for-bit the
        // layer transform there, unchanged from before this arm existed (`T * I` reduces to T
        // exactly: every product is x*1 or x*0 and every sum adds a true zero). For text and
        // texture it folds the cache's own pixel -> layer map in FRONT of the transform, mirroring
        // compositor.cpp's `pre * layer.transform() * cacheImageToLayer()`.
        // Everything below derives from THIS -- the inverse, the resolved filter, the sub-sample
        // count, the source-texels-per-target-texel scales, the filter-footprint pad, the dirty
        // bounds and the placement fingerprint -- so a cache that re-rendered at a new origin is a
        // placement change and diffPlanIntoDirty unions the old and new footprints for it.
        s.place = layer.transform() * src.toLayer;
        const std::optional<common::Affine2D> inv = s.place.inverse();
        if (!inv) return TileRefusal::SingularTransform;
        s.inverse = *inv;
        s.filter = resolveTileFilter(opts.resampleFilter, s.place, opts.liveDrag);
        s.superN = tileSupersampleN(s.inverse);
        s.blend = layer.blendMode();
        s.opacity = layer.opacity();
        // Source texels per output texel, per source axis -- the inverse map's COLUMN lengths,
        // floored at 1 exactly as compositor.cpp's sclX/sclY are. The unclamped values are kept
        // for the dirty-bounds pad below, where the floor would give the wrong answer.
        const double rawSclX = std::hypot(s.inverse.m00, s.inverse.m10);
        const double rawSclY = std::hypot(s.inverse.m01, s.inverse.m11);
        s.scaleX = static_cast<float>(std::max(1.0, rawSclX));
        s.scaleY = static_cast<float>(std::max(1.0, rawSclY));

        const core::RasterMask* mk = layer.mask();
        if (mk != nullptr && (!mk->enabled || mk->empty())) mk = nullptr;
        if (mk != nullptr) {
            if (!c.fitsImage(mk->width, mk->height)) return TileRefusal::LayerTooLarge;
            // A LEAF's linked mask folds at the SOURCE pixel, proportionally when the resolutions
            // differ (mode 1). An UNLINKED one folds after placement, in the layer's PARENT space
            // -- which at the document root is the identity map, because foldUnlinkedMask samples
            // through pre^-1 and `pre` is the identity here (mode 3).
            s.maskMode = mk->linked ? 1 : 3;
            s.maskXform = common::Affine2D::identity();
        }

        s.clip = layer.clipToBelow() && haveClipBase;
        s.clipWrite = !layer.clipToBelow() && anyClips;
        // A clipped layer never becomes the clip base itself -- walkStep returns before the
        // capture -- so the two flags are mutually exclusive by construction.
        if (!layer.clipToBelow() && anyClips) haveClipBase = true;

        // The doc-space footprint the layer can touch, which is what the dirty set is told.
        //
        // The pad is NOT simply the kernel radius: the kernel's support is measured in SOURCE
        // texels (`min(kernelRadius * uScale, kMaxFootprintRadius)`), and a target pixel `d` px
        // outside the placed rect maps to a source point `d * sourcePerTarget` outside the source
        // rect -- so the reach in TARGET px is the source radius DIVIDED by that ratio. Under
        // minification the two nearly cancel (a couple of px); under strong magnification they do
        // not, because uScale is floored at 1 while the ratio keeps shrinking, and a 10x enlarged
        // layer really does reach ~30 target px past its own rect. Getting this wrong under-dirties
        // a thin band at the layer's edge, which shows up as a stale seam only at some zooms --
        // exactly the kind of defect a golden test never catches.
        const double perTarget = std::max(1e-6, std::min(rawSclX, rawSclY));
        const double srcRadius =
            std::min(3.0 * std::max(1.0, std::max(rawSclX, rawSclY)), 8.0);
        s.pad = 1.0 + srcRadius / perTarget;
        s.bounds = placedBounds(s.place, src.width, src.height, s.pad);

        std::uint64_t h = 1469598103934665603ull;
        hashValue(h, s.place.m00);
        hashValue(h, s.place.m01);
        hashValue(h, s.place.m02);
        hashValue(h, s.place.m10);
        hashValue(h, s.place.m11);
        hashValue(h, s.place.m12);
        hashValue(h, s.opacity);
        const std::int32_t blendI = static_cast<std::int32_t>(s.blend);
        hashValue(h, blendI);
        const std::int32_t filterI = static_cast<std::int32_t>(s.filter);
        hashValue(h, filterI);
        hashValue(h, s.superN);
        hashValue(h, s.maskMode);
        const std::uint8_t flags =
            static_cast<std::uint8_t>((s.clip ? 1u : 0u) | (s.clipWrite ? 2u : 0u));
        hashValue(h, flags);
        // The two revisions are deliberately NOT hashed in -- they travel beside the fingerprint
        // as plain numbers, because the diff has to tell "this layer moved" (whole footprint) from
        // "this layer was repainted somewhere the caller named" (that region only). See Step.
        // `src.revision` rather than `layer.contentRevision()`: for the cache-backed kinds those
        // two are different numbers, and only the first one moves when the cache is re-rendered.
        s.sourceRevision = src.revision;
        s.maskRevision = layer.maskRevision();
        // Forced odd, so ZERO is reserved: markLayerDirty() poisons a layer's fingerprint with 0
        // to force the next plan diff to treat it as changed, and a real hash colliding with the
        // sentinel would silently skip that layer's recomposite.
        s.fingerprint = h | 1ull;

        out.push_back(s);
    }
    return TileRefusal::None;
}

void TileCompositor::diffPlanIntoDirty(const std::vector<Step>& next) noexcept {
    if (!m_accValid || m_lastPlan.size() != next.size()) {
        markAllDirty();
        return;
    }
    for (std::size_t i = 0; i < next.size(); ++i) {
        // Identity AND order both matter: a reorder changes what composites over what, even when
        // every layer's own state is untouched.
        if (m_lastPlan[i].layer != next[i].layer) {
            markAllDirty();
            return;
        }
    }
    for (std::size_t i = 0; i < next.size(); ++i) {
        const Step& was = m_lastPlan[i];
        const Step& now = next[i];
        const bool placementMoved = was.fingerprint != now.fingerprint;
        const bool maskMoved = was.maskRevision != now.maskRevision;
        const bool contentMoved = was.sourceRevision != now.sourceRevision;
        if (!placementMoved && !maskMoved && !contentMoved) continue;

        // A CONTENT-ONLY change whose region the caller named is the whole point of the
        // incremental path: markLayerDirty(layer, rect) has already dirtied exactly the document
        // macrotiles those pixels project onto, so falling through to the whole-footprint union
        // below would throw that away and put a full-canvas layer's every macrotile back in the
        // dispatch list -- which is what made `gpu edit 256` cost 18.8 ms at 3840x2160.
        //
        // The claim is trusted only when every link in it holds: the ledger knows where the
        // change is, it describes exactly the revision this plan sees, and the device image is
        // still at the revision the accumulator was built from. Any doubt takes the union.
        if (!placementMoved && !maskMoved) {
            const auto it = m_sources.find(now.layer);
            if (it != m_sources.end() && it->second.pendingKnown && !it->second.pending.empty() &&
                it->second.pendingRevision == now.sourceRevision &&
                it->second.sourceRevision == was.sourceRevision)
                continue;
        }

        // A layer that moved dirties the union of where it WAS and where it IS. Anything above it
        // in the stack recomposites over the same macrotiles for free, because the walk replays
        // every layer for a dirty tile.
        m_dirty.add(was.bounds);
        m_dirty.add(now.bounds);
    }
}

// ---- Residency ---------------------------------------------------------------------------------

std::uint64_t TileCompositor::residentSourceBytes() const noexcept {
    std::uint64_t total = 0;
    for (const auto& [id, src] : m_sources) total += src.bytes;
    return total;
}

void TileCompositor::dropLayer(core::LayerId id) noexcept {
    const auto it = m_sources.find(id);
    if (it == m_sources.end()) return;
    for (std::uint32_t p = 0; p < it->second.pages; ++p) m_residency.evict({id, p, 0});
    destroyImage(it->second.pixels);
    destroyImage(it->second.mask);
    m_sources.erase(it);
}

void TileCompositor::discardRecordedUploads() noexcept {
    for (const core::LayerId id : m_uploading) dropLayer(id);
    m_uploading.clear();
}

void TileCompositor::releaseEvicted(const std::vector<core::TileKey>& evicted) noexcept {
    // A page is only a byte ledger; the thing that actually goes is the whole layer image. So an
    // eviction of ANY page releases every other page the same layer still holds -- otherwise the
    // ledger would keep charging for bytes that no longer exist.
    for (const core::TileKey& k : evicted) dropLayer(k.layer);
}

bool TileCompositor::planUploadRegions(const core::TileSet& pending, std::uint32_t imgW,
                                       std::uint32_t imgH,
                                       std::vector<UploadRegion>& out) const {
    out.clear();
    if (pending.empty() || imgW == 0 || imgH == 0 || m_macrotile == 0) return false;

    // The SAME projection the dispatch uses (§3.1's `macrotiles(k)`), applied to the layer's own
    // grid instead of the canvas's. One vocabulary on both sides is what makes it impossible for
    // an upload to refresh less than a recomposite is about to read.
    const std::uint32_t shift =
        m_macrotile <= kDirtyTileSize
            ? 0u
            : static_cast<std::uint32_t>(std::countr_zero(m_macrotile / kDirtyTileSize));
    const core::TileSet macro = pending.macrotiles(shift);
    const core::TileGrid& g = macro.grid();
    if (g.tilesX() == 0 || g.tilesY() == 0) return false;

    std::uint64_t bytes = 0;
    for (std::uint32_t ty = 0; ty < g.tilesY(); ++ty) {
        std::uint32_t runStart = 0;
        bool inRun = false;
        // One past the last column, so a run that reaches the right edge is flushed by the same
        // branch as one that ends in the middle -- no duplicated emit after the loop.
        for (std::uint32_t tx = 0; tx <= g.tilesX(); ++tx) {
            const bool on = tx < g.tilesX() && macro.test(core::TileCoord{tx, ty});
            if (on && !inRun) {
                runStart = tx;
                inRun = true;
                continue;
            }
            if (on || !inRun) continue;
            inRun = false;
            // tileBounds() is clipped to its grid, and the grid IS the image, so the right- and
            // bottom-edge runs are partial exactly where the image is.
            const common::Rect a = g.tileBounds(core::TileCoord{runStart, ty});
            const common::Rect b = g.tileBounds(core::TileCoord{tx - 1, ty});
            UploadRegion r;
            r.x = static_cast<std::uint32_t>(a.x);
            r.y = static_cast<std::uint32_t>(a.y);
            r.w = static_cast<std::uint32_t>(b.right() - a.x);
            r.h = static_cast<std::uint32_t>(a.h);
            if (r.w == 0 || r.h == 0) continue;
            if (out.size() >= kMaxUploadRegions) {
                out.clear();
                return false;
            }
            bytes += static_cast<std::uint64_t>(r.w) * r.h * 4ull;
            out.push_back(r);
        }
    }
    if (out.empty()) return false;
    // Once the runs add up to the whole image there is nothing left to save, and one copy beats
    // N of them. This is the "everything is dirty" case -- a filter, a paste, an undo -- and it
    // must not pay a tiling tax for the privilege.
    if (bytes >= static_cast<std::uint64_t>(imgW) * imgH * 4ull) {
        out.clear();
        return false;
    }
    return true;
}

bool TileCompositor::uploadLayerRegions(const common::Image& img, LayerSource& src,
                                        const std::vector<UploadRegion>& regions,
                                        VkCommandBuffer cmd, UploadTally& tally,
                                        std::string& error) {
    std::uint64_t bytes = 0;
    for (const UploadRegion& r : regions) bytes += static_cast<std::uint64_t>(r.w) * r.h * 4ull;
    if (bytes == 0) return false;

    GpuBuffer stage;
    if (!makeHostBuffer(static_cast<VkDeviceSize>(bytes), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, stage,
                        error))
        return false;

    // Pack the regions back to back and TIGHTLY. Copying whole image rows instead (bufferOffset
    // into a staging copy of the layer's own storage, bufferRowLength = img.width) would need no
    // per-row memcpy at all -- but it also moves the untouched horizontal span, so a 256 px dab
    // in a 3840 px layer would send 15x the bytes, which is most of the saving this exists for.
    auto* dst = static_cast<std::uint8_t*>(stage.mapped);
    std::vector<VkBufferImageCopy> copies;
    copies.reserve(regions.size());
    VkDeviceSize offset = 0;
    for (const UploadRegion& r : regions) {
        const std::size_t rowBytes = static_cast<std::size_t>(r.w) * 4u;
        for (std::uint32_t row = 0; row < r.h; ++row) {
            const std::size_t srcY = static_cast<std::size_t>(r.y) + row;
            const std::uint8_t* srcRow =
                img.rgba.data() + (srcY * img.width + r.x) * 4u;
            std::memcpy(dst + static_cast<std::size_t>(offset) + row * rowBytes, srcRow, rowBytes);
        }
        // `bufferOffset` must be a multiple of the texel block size. Every region is a whole
        // number of 4-byte texels, so every offset is 4-aligned by construction.
        copies.push_back(VkBufferImageCopy{
            .bufferOffset = offset,
            .bufferRowLength = r.w,
            .bufferImageHeight = r.h,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageOffset = {static_cast<std::int32_t>(r.x), static_cast<std::int32_t>(r.y), 0},
            .imageExtent = {r.w, r.h, 1},
        });
        offset += static_cast<VkDeviceSize>(rowBytes) * r.h;
    }

    // ⚠ `oldLayout` is SHADER_READ_ONLY_OPTIMAL and must NEVER be UNDEFINED here. A transition
    // out of UNDEFINED is permitted to DISCARD the image's contents, which on a partial upload
    // throws away every pixel the copies do not rewrite -- and it would look perfect on the frame
    // right after a full upload and wrong on the next one, which is the worst failure shape there
    // is. The full-upload path below may use UNDEFINED precisely because it rewrites everything.
    const VkImageMemoryBarrier toDst{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = src.pixels.image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toDst);
    vkCmdCopyBufferToImage(cmd, stage.buffer, src.pixels.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<std::uint32_t>(copies.size()), copies.data());
    const VkImageMemoryBarrier toRead{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = src.pixels.image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &toRead);

    m_staging.push_back(stage);  // freed once this composite's fence signals
    tally.bytes += bytes;
    tally.regions += copies.size();
    ++tally.partial;
    return true;
}

bool TileCompositor::ensureLayerResident(const core::Layer& layer, VkCommandBuffer cmd,
                                         UploadTally& tally, TileRefusal& refusal,
                                         std::string& error) {
    const core::LayerId id = layer.id();
    // Re-derived rather than carried on the Step, so this function and planDocument cannot come to
    // disagree about which image a leaf reads; nothing mutates the document between them.
    LeafSource leaf;
    // The ADJUSTMENT arm's synthesised sheet. An adjustment has no pixels of its own, so for the
    // ten kinds that need nothing on the device this function does no work at all -- which is what
    // makes `uploadBytes` stay put for them. The two LOOKUP kinds (Curves, Gradient Map) do need a
    // 256-entry transfer table, and it is built here, into a buffer this call owns, so that the
    // residency path below stays ONE code path: the table is a source sheet in every respect that
    // matters -- uploaded when it changes, charged to the residency ledger, evicted with the layer.
    common::ImageF table;
    if (const auto* adj = layer.as<core::AdjustmentLayer>()) {
        const AdjustPlan ap = planAdjustment(*adj);
        if (ap.refusal != TileRefusal::None) {
            // planDocument already rejected this document, so reaching here means the tree changed
            // under the plan -- the same reasoning the leaf arm's refusal carries.
            refusal = ap.refusal;
            return false;
        }
        // The SAME key planDocument put on the Step, derived the same way: a table's staleness is
        // the params bag, and a kind with no table has nothing to make stale.
        leaf.revision = ap.table ? ap.fingerprint : 0;
        if (ap.table) {
            buildAdjustTable(*adj, table);
            leaf.pixelsF = &table;
            leaf.width = table.width;
            leaf.height = table.height;
        }
    } else if (const TileRefusal why = leafSourceFor(layer, leaf); why != TileRefusal::None) {
        refusal = why;
        return false;
    }
    const VkFormat format = sourceFormat(leaf);
    const std::uint32_t texelBytes = sourceTexelBytes(format);
    const core::RasterMask* mk = layer.mask();
    if (mk != nullptr && (!mk->enabled || mk->empty())) mk = nullptr;

    // NOTHING TO MAKE RESIDENT. Only an adjustment can reach this -- leafSourceFor refuses an empty
    // leaf outright -- and it is the common case for one: no transfer table and no mask. Drop
    // whatever a previous revision of this layer left behind (a table it no longer needs, a mask
    // that was removed) and upload not one byte. The dispatch loop binds the 1x1 stand-ins for it.
    if (leaf.width == 0 && mk == nullptr) {
        dropLayer(id);
        return true;
    }

    const auto it = m_sources.find(id);
    if (it != m_sources.end()) {
        LayerSource& src = it->second;
        // A source revision that moved with nothing said about WHERE is exactly the case the
        // ledger exists to catch. The bytes are somewhere; the only safe somewhere is everywhere.
        // For a text or texture layer this is ALWAYS the branch a re-rendered cache takes: the app
        // names no region inside a bake it just replaced wholesale, so a generation this ledger
        // has not seen means the full upload path, every time.
        if (src.pendingRevision != leaf.revision) src.pendingKnown = false;

        // These make the device image structurally the same object as the CPU one. If any is
        // false the image itself has to be rebuilt, and there is nothing to patch. The FORMAT
        // belongs here beside the dimensions: a texture layer that switched between its 8-bit and
        // its float lane can keep both, and an R8G8B8A8 image cannot hold halves.
        const bool sameShape = src.pixels.width == leaf.width &&
                               src.pixels.height == leaf.height && src.format == format;
        const bool sameMask = src.maskRevision == layer.maskRevision() &&
                              src.mask.valid() == (mk != nullptr);

        if (sameShape && sameMask && src.pendingKnown) {
            const auto keepResident = [&] {
                // Nothing about the residency ledger moves on either branch below: same image,
                // same byte count, same pages. Touch so the LRU sees the use and pin for the
                // duration of this composite.
                for (std::uint32_t p = 0; p < src.pages; ++p) {
                    const core::TileKey key{id, p, 0};
                    m_residency.touch(key);
                    m_residency.pin(key);
                }
            };
            if (src.pending.empty()) {
                // Resident and current: upload NOTHING. This branch is the whole point of the
                // class, and `Tile upload` on Lane::GpuDevice staying at ~0 is its assertion.
                src.sourceRevision = leaf.revision;
                keepResident();
                return true;
            }
            // The INCREMENTAL path: only the macrotiles the caller named cross the bus. 8-bit
            // only -- planUploadRegions and uploadLayerRegions both count in 4-byte texels, and
            // the one entry point that fills `pending` takes a core::RasterLayer, so a float
            // source cannot reach here. Guarded anyway: a wrong stride is silent corruption.
            std::vector<UploadRegion> regions;
            if (leaf.pixels8 != nullptr &&
                planUploadRegions(src.pending, leaf.width, leaf.height, regions)) {
                if (!uploadLayerRegions(*leaf.pixels8, src, regions, cmd, tally, error)) {
                    // Half a partial upload is not a state worth reasoning about: forget where
                    // the change was, so the next attempt re-sends the image whole.
                    src.pendingKnown = false;
                    refusal = TileRefusal::DeviceError;
                    return false;
                }
                src.sourceRevision = leaf.revision;
                src.pendingRevision = src.sourceRevision;
                src.pending.clear();
                m_uploading.push_back(id);  // these copies are recorded, not yet executed
                keepResident();
                return true;
            }
            // planUploadRegions declined -- too many runs, or the runs already cover the image.
            // Falling through to the full upload is not a failure, it is the cheaper answer.
        }
    }

    dropLayer(id);

    const std::uint64_t pixelBytes =
        static_cast<std::uint64_t>(leaf.width) * leaf.height * texelBytes;
    const std::uint64_t maskBytes =
        mk == nullptr ? 0ull : static_cast<std::uint64_t>(mk->width) * mk->height;
    const std::uint64_t bytes = pixelBytes + maskBytes;
    const std::uint32_t pages =
        static_cast<std::uint32_t>((bytes + kSourcePageBytes - 1) / kSourcePageBytes);

    for (std::uint32_t p = 0; p < pages; ++p) {
        const TileAdmission adm = m_residency.admit({id, p, 0});
        if (!adm.ok) {
            // This composite needs more source bytes at once than the atlas can hold. Roll back
            // cleanly and refuse -- the caller's answer is the CPU path, which is slow and right.
            // Thrashing would be slow AND wrong.
            for (std::uint32_t q = 0; q < p; ++q) m_residency.evict({id, q, 0});
            refusal = TileRefusal::OutOfBudget;
            return false;
        }
        releaseEvicted(adm.evicted);
    }

    LayerSource src;
    src.sourceRevision = leaf.revision;
    src.maskRevision = layer.maskRevision();
    src.format = format;
    src.pages = pages;
    src.bytes = bytes;
    // A leaf ALWAYS has pixels; an adjustment reaching here has a mask, a transfer table, or both.
    // A zero-extent image is not a legal VkImage, so the sheet is only created when there is one.
    if (leaf.width > 0 &&
        !makeImage(leaf.width, leaf.height, format,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, src.pixels,
                   error)) {
        for (std::uint32_t q = 0; q < pages; ++q) m_residency.evict({id, q, 0});
        refusal = TileRefusal::DeviceError;
        return false;
    }
    if (mk != nullptr) {
        src.maskW = mk->width;
        src.maskH = mk->height;
        if (!makeImage(mk->width, mk->height, VK_FORMAT_R8_UNORM,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, src.mask,
                       error)) {
            destroyImage(src.pixels);
            for (std::uint32_t q = 0; q < pages; ++q) m_residency.evict({id, q, 0});
            refusal = TileRefusal::DeviceError;
            return false;
        }
    }

    // Staging on every device. `VK_EXT_host_image_copy` (GpuCaps::hostImageCopy) would remove the
    // staging buffer entirely on a 1.4 driver and is the single biggest upload win available --
    // it is deliberately NOT taken here, because it is a tier optimisation on a path that is
    // already off the per-frame hot loop (a layer uploads when it CHANGES), and the floor path
    // has to exist and be exercised either way.
    //
    // `fill` writes the texels straight into the mapped staging buffer: a memcpy for every source
    // that is already in its device encoding, and the float -> half conversion for the one that is
    // not. Converting IN the staging buffer rather than into a temporary is what keeps a
    // canvas-sized float cache from costing a second full copy of itself on every re-render.
    const auto uploadWith = [&](const GpuImage& dst, VkDeviceSize size, auto&& fill) -> bool {
        GpuBuffer stage;
        if (!makeHostBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, stage, error)) return false;
        fill(static_cast<std::uint8_t*>(stage.mapped));
        const VkImageMemoryBarrier toDst{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = dst.image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);
        const VkBufferImageCopy copy{
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {dst.width, dst.height, 1},
        };
        vkCmdCopyBufferToImage(cmd, stage.buffer, dst.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        const VkImageMemoryBarrier toRead{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = dst.image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &toRead);
        m_staging.push_back(stage);  // freed once this composite's fence signals
        tally.bytes += size;
        ++tally.regions;
        return true;
    };
    const auto upload = [&](const GpuImage& dst, const void* data, VkDeviceSize size) -> bool {
        return uploadWith(dst, size, [&](std::uint8_t* p) {
            std::memcpy(p, data, static_cast<std::size_t>(size));
        });
    };

    const auto sendPixels = [&]() -> bool {
        if (leaf.pixels8 != nullptr)
            return upload(src.pixels, leaf.pixels8->rgba.data(),
                          static_cast<VkDeviceSize>(pixelBytes));
        // The FLOAT lane (the texture generator's sky cache). R16G16B16A16_SFLOAT is the only
        // encoding Vulkan 1.0 guarantees that can hold this cache at all, and half_float.hpp
        // rounds to nearest EVEN -- so every texel becomes its nearest representable neighbour at
        // ~11 bits of mantissa, against the 8 the readback ends in. That is three bits FINER than
        // the quantisation this lane exists to avoid (an 8-bit round trip, which bands a sky
        // gradient) and it is the same precision the accumulator itself already carries, which is
        // what the 1/255 parity bound was set against. Refusing instead would leave every sky
        // document on the CPU walk to dodge a rounding the accumulator applies one line later.
        return uploadWith(src.pixels, static_cast<VkDeviceSize>(pixelBytes),
                          [&](std::uint8_t* p) {
                              const std::vector<float>& f = leaf.pixelsF->rgba;
                              auto* h = reinterpret_cast<std::uint16_t*>(p);
                              for (std::size_t i = 0; i < f.size(); ++i) h[i] = floatToHalf(f[i]);
                          });
    };
    if (leaf.width > 0 && !sendPixels()) {
        destroyImage(src.pixels);
        destroyImage(src.mask);
        for (std::uint32_t q = 0; q < pages; ++q) m_residency.evict({id, q, 0});
        refusal = TileRefusal::DeviceError;
        return false;
    }
    if (mk != nullptr &&
        !upload(src.mask, mk->coverage.data(), static_cast<VkDeviceSize>(maskBytes))) {
        destroyImage(src.pixels);
        destroyImage(src.mask);
        for (std::uint32_t q = 0; q < pages; ++q) m_residency.evict({id, q, 0});
        refusal = TileRefusal::DeviceError;
        return false;
    }

    // The image now matches the CPU pixels exactly, so the staleness ledger restarts empty and
    // KNOWN -- which is what makes the very next region claim eligible for an incremental upload.
    // The set is sized to THIS image; a later resize invalidates every index in it, which is why
    // markLayerDirty(layer, rect) checks the grid before touching it.
    // A sheetless entry (a mask-only adjustment) keeps an empty grid: there is no image for a
    // region claim to name, and only markLayerDirty(const RasterLayer&, rect) ever fills this set.
    if (leaf.width > 0) src.pending.reset(core::TileGrid(leaf.width, leaf.height, kDirtyTileSize));
    src.pendingKnown = true;
    src.pendingRevision = src.sourceRevision;
    ++tally.full;
    m_uploading.push_back(id);  // these copies are recorded, not yet executed

    m_sources.emplace(id, std::move(src));
    for (std::uint32_t p = 0; p < pages; ++p) m_residency.pin({id, p, 0});
    return true;
}

// ---- The dirty-macrotile list (item 10) ----------------------------------------------------------

bool TileCompositor::buildTileList(const std::vector<std::pair<core::TileCoord, Slot>>& tiles,
                                   std::vector<AtlasRun>& runs, VkCommandBuffer cmd,
                                   std::string& error) {
    if (runs.empty()) return false;

    // Pad each run to the device's storage-buffer offset alignment, so ONE buffer serves every
    // atlas image through a descriptor RANGE. That is what keeps a base index out of the push
    // block -- index 0 of the bound range is always this dispatch's first macrotile -- and Vulkan
    // caps `minStorageBufferOffsetAlignment` at 256 bytes, so the padding is bounded whatever the
    // device reports. (floorLimits() reports exactly that 256, for exactly this reason.)
    const VkDeviceSize align =
        std::max<VkDeviceSize>(caps().limits.minStorageBufferOffsetAlignment, 4);
    VkDeviceSize total = 0;
    for (AtlasRun& r : runs) {
        r.offset = total;
        r.bytes = static_cast<VkDeviceSize>(r.count) * sizeof(TileRecord);
        total += ((r.bytes + align - 1) / align) * align;
    }
    if (total == 0 || !caps().fitsStorageBufferRange(total)) return false;

    if (m_tileList.size < total) {
        // Grow with slack so a canvas that gains a macrotile does not reallocate every frame, and
        // grow into a NEW buffer before releasing the old one: on an allocation failure the old
        // one is still bindable, which is what keeps binding 5 valid on the per-tile fallback.
        GpuBuffer grown;
        const VkDeviceSize want = total + total / 2 + sizeof(TileRecord);
        if (!makeDeviceBuffer(want,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              grown, error))
            return false;
        m_ctx->waitIdle();  // the previous composite's fence has signalled; belt and braces
        destroyBuffer(m_tileList);
        m_tileList = grown;
    }

    GpuBuffer stage;
    if (!makeHostBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, stage, error)) return false;
    auto* dst = static_cast<std::uint8_t*>(stage.mapped);
    std::memset(dst, 0, static_cast<std::size_t>(total));
    for (const AtlasRun& r : runs) {
        for (std::uint32_t i = 0; i < r.count; ++i) {
            const auto& [coord, slot] = tiles[r.first + i];
            const common::Rect bounds = m_macroGrid.tileBounds(coord);
            // ⚠ THE PARITY ARGUMENT LIVES HERE: these are the same six integers, computed the same
            // way, that the per-tile loop below writes into its push block. Nothing is recomputed
            // in a different space, rounded differently, or pre-composed -- so the two shapes
            // cannot draw different pixels, and the tests assert byte-identity rather than a
            // tolerance.
            const TileRecord rec{
                {static_cast<std::int32_t>(bounds.x), static_cast<std::int32_t>(bounds.y)},
                {slot.x, slot.y},
                {static_cast<std::int32_t>(bounds.w), static_cast<std::int32_t>(bounds.h)},
                {0, 0},
            };
            std::memcpy(dst + r.offset + static_cast<VkDeviceSize>(i) * sizeof(TileRecord), &rec,
                        sizeof(rec));
        }
    }
    const VkBufferCopy copy{.srcOffset = 0, .dstOffset = 0, .size = total};
    vkCmdCopyBuffer(cmd, stage.buffer, m_tileList.buffer, 1, &copy);
    m_staging.push_back(stage);  // freed once this composite's fence signals
    return true;
}

// ---- The composite ------------------------------------------------------------------------------

TileCompositeStatus TileCompositor::composite(const core::Document& doc,
                                              const CompositeOptions& opts) {
    // Three rows, and the relationship between them is the diagnostic:
    //   "Tile composite"  Lane::Gpu       -- what the CALLER pays: plan + upload + submit + fence.
    //   "Tile composite"  Lane::GpuDevice -- what the DEVICE spent (the timestamp pair below).
    //   "Tile composite (plan/diff/fence wait)" -- where the host half of the gap went.
    // A resident compositor that is fence-bound and one that is shader-bound look identical from
    // the call site, and only the second is worth writing a shader for.
    MOSAIC_PERF_SCOPE("Tile composite", common::Lane::Gpu);
    TileCompositeStatus st;
    // Layer ids are unique only within a document, so a cache carried across a switch would serve
    // the wrong pixels. This catches a live tab switch; reset() is what covers the rest.
    if (m_lastDoc != nullptr && m_lastDoc != &doc) reset();
    m_lastDoc = &doc;
    if (!setDocumentSize(doc.width(), doc.height(), st.error)) {
        st.refusal = TileRefusal::DocumentTooLarge;
        ++m_stats.refusals;
        return st;
    }
    if (m_acc.empty()) {
        st.ok = true;  // a zero-sized document composites to nothing, successfully
        return st;
    }
    // The checkerboard is a PRESENTATION step, not a composite step; folding it into the resident
    // accumulator would bake opaque grey into the pixels every readback consumer reads. The
    // present pass owns it (canvas_present.comp already draws it), so a caller that asks for it
    // here is asking for something this lane deliberately does not do.
    if (opts.checkerboard) {
        st.refusal = TileRefusal::UnsupportedKind;
        st.error = "checkerboard flattening belongs to the present pass";
        ++m_stats.refusals;
        return st;
    }

    std::vector<Step> plan;
    std::string planWhy;
    const TileRefusal refusal = [&] {
        MOSAIC_PERF_SCOPE("Tile composite (plan)", common::Lane::Cpu);
        return planDocument(doc, opts, plan, planWhy);
    }();
    if (refusal != TileRefusal::None) {
        st.refusal = refusal;
        // The specific reason when the plan had one (which kind, and the pixel-level why), the
        // enum's own name otherwise. A caller logging this gets a one-line answer to "why is this
        // document on the CPU path", which is what the Settings capability readout wants.
        st.error = planWhy.empty() ? std::string(tileRefusalName(refusal)) : planWhy;
        ++m_stats.refusals;
        return st;
    }

    {
        // Item 7's plan diff -- per-layer fingerprints and the union of where each changed layer
        // WAS and IS. It is the thing that turns a nudge into a handful of macrotiles, so if it
        // ever costs more than the dispatches it saves, this row is where that shows up.
        MOSAIC_PERF_SCOPE("Tile composite (diff)", common::Lane::Cpu);
        diffPlanIntoDirty(plan);
    }
    const core::TileSet macroDirty = m_dirty.macrotiles(
        m_macrotile == kDirtyTileSize
            ? 0u
            : static_cast<std::uint32_t>(std::countr_zero(m_macrotile / kDirtyTileSize)));

    if (macroDirty.empty()) {
        // Nothing changed. This is the branch that makes the compositor "resident" rather than
        // "on the GPU": no dispatch, no upload, no readback, no submit at all.
        m_lastPlan = plan;
        st.ok = true;
        st.layers = plan.size();
        for (const Step& s : plan)
            if (s.adjust) ++st.adjustments;
        st.dispatch = activeDispatch();  // the shape it WOULD have taken; nothing was dispatched
        ++m_stats.composites;
        return st;
    }

    std::vector<std::pair<core::TileCoord, Slot>> tiles;
    tiles.reserve(static_cast<std::size_t>(macroDirty.count()));
    macroDirty.forEach([&](core::TileCoord c) { tiles.emplace_back(c, slotFor(c.tx, c.ty)); });

    // ---- Item 10: group the dirty macrotiles by accumulator atlas image ------------------------
    //
    // `forEach` walks row-major and `slotFor`'s image index is monotone in that order, so an
    // image's tiles are already adjacent: one pass finds every run. A document that fits
    // maxImageDimension2D has exactly ONE run, which is the case the list shape is built for.
    std::vector<AtlasRun> runs;
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        if (runs.empty() || runs.back().image != tiles[i].second.image) {
            AtlasRun r;
            r.image = tiles[i].second.image;
            r.first = static_cast<std::uint32_t>(i);
            runs.push_back(r);
        }
        ++runs.back().count;
    }

    // The shape this composite will take, resolved in one place and reported on the status.
    // Downgrades from here are ALWAYS to something that draws the same picture.
    TileDispatch mode = activeDispatch();
    if (mode == TileDispatch::Indexed &&
        (plan.size() * 2 > m_indexedMaxSources || plan.empty()))
        mode = TileDispatch::TileList;  // more layers than the array holds -- see kIndexedMaxLayers
    if (mode != TileDispatch::PerTile) {
        // A list dispatch spends the z axis on the macrotile index. Vulkan 1.0 guarantees 65535
        // there -- roughly 4 gigapixels of 256 px macrotiles -- so this is a guard rather than a
        // real limit, and the answer to hitting it is the loop that never had the constraint.
        const std::uint32_t zLimit = caps().limits.maxComputeWorkGroupCount[2];
        for (const AtlasRun& r : runs)
            if (r.count > zLimit) mode = TileDispatch::PerTile;
    }

    const bool needClip =
        std::any_of(plan.begin(), plan.end(), [](const Step& s) { return s.clip || s.clipWrite; });
    if (needClip && m_clip.empty() && !m_acc.empty()) {
        std::string err;
        m_clip.resize(m_acc.size());
        for (GpuImage& img : m_clip) {
            if (!makeImage(m_acc[0].width, m_acc[0].height, VK_FORMAT_R16_SFLOAT,
                           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, img, err)) {
                for (GpuImage& g : m_clip) destroyImage(g);
                m_clip.clear();
                st.refusal = TileRefusal::DeviceError;
                st.error = err;
                ++m_stats.refusals;
                return st;
            }
        }
    }

    vkResetCommandBuffer(m_cmd, 0);
    const VkCommandBufferBeginInfo cbi{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(m_cmd, &cbi);

    // Device timing. The pool reset must be the first thing on the command buffer, and every scope
    // below is null-tolerant: on a device without a timestamp counter each one is a no-op and an
    // int that stays -1. An early return between a beginScope and its endScope simply abandons
    // the scope -- the command buffer is never submitted on those paths, so there is nothing to
    // report and the next beginSubmission() clears the bookkeeping.
    if (m_timer) m_timer->beginSubmission(m_cmd);
    const auto gpuBegin = [this](std::string_view what) -> std::int32_t {
        return m_timer ? m_timer->beginScope(m_cmd, what) : -1;
    };
    const auto gpuEnd = [this](std::int32_t scope) {
        if (m_timer) m_timer->endScope(m_cmd, scope);
    };
    const std::int32_t tAll = gpuBegin("Tile composite");

    // Every long-lived image settles in GENERAL and stays there: legal for storage, for sampling,
    // and as a copy source/destination, so nothing below needs a layout transition again.
    if (!m_accInitialised || (!m_clip.empty() && !m_clipInitialised)) {
        std::vector<VkImageMemoryBarrier> init;
        if (!m_accInitialised)
            for (const GpuImage& img : m_acc)
                init.push_back(VkImageMemoryBarrier{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .srcAccessMask = 0,
                    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                                     VK_ACCESS_TRANSFER_WRITE_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = img.image,
                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                });
        if (!m_clipInitialised)
            for (const GpuImage& img : m_clip)
                init.push_back(VkImageMemoryBarrier{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .srcAccessMask = 0,
                    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = img.image,
                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                });
        if (!init.empty())
            vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT |
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr,
                                 static_cast<std::uint32_t>(init.size()), init.data());
        m_accInitialised = true;
        if (!m_clip.empty()) m_clipInitialised = true;
    }

    // Uploads first, so a layer's pixels are in place before any dispatch reads them and the
    // whole composite is one submit.
    //
    // This is the row invariant 1 is about ("a layer's pixels are uploaded when they CHANGE").
    // On a steady frame it must be ~0 ms with nothing transferred; a device-time upload row that
    // is consistently non-zero means residency is not working, which no wall clock can tell you.
    const std::int32_t tUpload = gpuBegin("Tile upload");
    UploadTally tally;
    // Every abandon path from here to the submit must run this, and for a reason that only became
    // load-bearing with the incremental path: a source's cache entry claims BOTH its contents and
    // its layout, and a command buffer that is never submitted delivers neither. Leaving the entry
    // behind would let the next composite patch a macrotile into an image that is still in
    // UNDEFINED -- correct-looking once, wrong afterwards.
    m_uploading.clear();
    for (const Step& s : plan) {
        const core::Layer* layer = doc.find(s.layer);
        if (layer == nullptr) {
            vkEndCommandBuffer(m_cmd);
            discardRecordedUploads();
            m_residency.unpinAll();
            for (GpuBuffer& b : m_staging) destroyBuffer(b);
            m_staging.clear();
            st.refusal = TileRefusal::UnsupportedKind;
            st.error = "planned layer vanished between plan and upload";
            ++m_stats.refusals;
            return st;
        }
        TileRefusal why = TileRefusal::None;
        if (!ensureLayerResident(*layer, m_cmd, tally, why, st.error)) {
            vkEndCommandBuffer(m_cmd);
            discardRecordedUploads();
            m_residency.unpinAll();
            for (GpuBuffer& b : m_staging) destroyBuffer(b);
            m_staging.clear();
            st.refusal = why;
            ++m_stats.refusals;
            return st;
        }
    }

    gpuEnd(tUpload);

    // Clear the dirty macrotiles. GENERAL is a legal copy destination, so this needs no layout
    // change -- only a zero buffer one macrotile wide, reused for every slot.
    const std::int32_t tClear = gpuBegin("Tile clear");
    if (m_zero.buffer == VK_NULL_HANDLE) {
        const VkDeviceSize zeroSize = static_cast<VkDeviceSize>(m_macrotile) * m_macrotile *
                                      caps().workingFormatBytes();
        if (!makeHostBuffer(zeroSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, m_zero, st.error)) {
            vkEndCommandBuffer(m_cmd);
            discardRecordedUploads();
            m_residency.unpinAll();
            for (GpuBuffer& b : m_staging) destroyBuffer(b);
            m_staging.clear();
            st.refusal = TileRefusal::DeviceError;
            ++m_stats.refusals;
            return st;
        }
        std::memset(m_zero.mapped, 0, static_cast<std::size_t>(zeroSize));
    }
    for (const auto& [coord, slot] : tiles) {
        const VkBufferImageCopy copy{
            .bufferOffset = 0,
            .bufferRowLength = m_macrotile,
            .bufferImageHeight = m_macrotile,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageOffset = {slot.x, slot.y, 0},
            .imageExtent = {m_macrotile, m_macrotile, 1},
        };
        vkCmdCopyBufferToImage(m_cmd, m_zero.buffer, m_acc[slot.image].image,
                               VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    }
    // The dirty list rides with the clears deliberately: it is a transfer, and the barrier below
    // is already a GLOBAL transfer-write -> shader-read one, so it needs no barrier of its own. A
    // decline here (an allocation, a run past the storage-buffer range) costs the shape, not the
    // composite -- nothing has been recorded for it, so the per-tile loop simply takes over.
    if (mode != TileDispatch::PerTile && !buildTileList(tiles, runs, m_cmd, st.error)) {
        mode = TileDispatch::PerTile;
        st.error.clear();
    }
    const VkMemoryBarrier clearDone{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
    };
    vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &clearDone, 0, nullptr, 0,
                         nullptr);
    gpuEnd(tClear);

    // ---- Descriptors ---------------------------------------------------------------------------
    //
    // WHICH SETS EXIST is the one host-side difference between the three shapes:
    //   PerTile / TileList   one set per (layer, atlas image): the source and mask vary per layer,
    //                        the accumulator and clip images vary per atlas image.
    //   Indexed              one set per ATLAS IMAGE for the WHOLE composite -- every layer's two
    //                        sheets live in the runtime array, so the per-layer descriptor writes
    //                        disappear. That collapse is what descriptor indexing buys; the
    //                        DISPATCH count was already collapsed by the list, at the 1.0 floor.
    const bool indexed = mode == TileDispatch::Indexed;
    const std::uint32_t setCount = indexed
                                       ? static_cast<std::uint32_t>(runs.size())
                                       : static_cast<std::uint32_t>(plan.size() * m_acc.size());
    const bool poolOk = setCount == 0 || (indexed ? ensureIndexedPool(setCount, st.error)
                                                  : ensureDescriptorPool(setCount, st.error));
    if (!poolOk) {
        vkEndCommandBuffer(m_cmd);
        discardRecordedUploads();
        m_residency.unpinAll();
        for (GpuBuffer& b : m_staging) destroyBuffer(b);
        m_staging.clear();
        st.refusal = TileRefusal::DeviceError;
        ++m_stats.refusals;
        return st;
    }

    // Which run serves each atlas image, so a set can name its own slice of the list buffer. An
    // image with nothing dirty keeps UINT32_MAX and binds the whole buffer -- a descriptor that is
    // valid and never read, which is what the spec asks for and what a hole is not.
    std::vector<std::uint32_t> runOfImage(m_acc.size(), UINT32_MAX);
    for (std::size_t ri = 0; ri < runs.size(); ++ri)
        runOfImage[runs[ri].image] = static_cast<std::uint32_t>(ri);

    // The two sheets a step binds. A LEAF always has resident pixels -- ensureLayerResident refuses
    // the whole composite otherwise, so the lookup below cannot miss for one -- but an ADJUSTMENT
    // may legitimately have NEITHER: most kinds need no transfer table, and most layers carry no
    // mask. Both roles then take the 1x1 all-revealing stand-in, because a descriptor set with a
    // hole is undefined behaviour even for a binding the shader never reads.
    struct Sheets {
        VkImageView pixels;
        VkImageView mask;
    };
    const auto sheetsFor = [this](core::LayerId layerId) {
        Sheets v{m_dummyMask.view, m_dummyMask.view};
        const auto sit = m_sources.find(layerId);
        if (sit == m_sources.end()) return v;
        if (sit->second.pixels.valid()) v.pixels = sit->second.pixels.view;
        if (sit->second.mask.valid()) v.mask = sit->second.mask.view;
        return v;
    };

    std::vector<VkDescriptorSetLayout> layouts(setCount,
                                               indexed ? m_indexedSetLayout : m_setLayout);
    std::vector<VkDescriptorSet> sets(setCount, VK_NULL_HANDLE);
    const std::uint32_t sourceCount = static_cast<std::uint32_t>(plan.size()) * 2;
    const std::vector<std::uint32_t> variableCounts(setCount, sourceCount);
    // Exactly two descriptors per planned layer, so a two-layer document never draws the layout's
    // maximum out of the pool. This is the "runtime-sized" half of the runtime-sized array.
    const VkDescriptorSetVariableDescriptorCountAllocateInfo vdcai{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO,
        .descriptorSetCount = setCount,
        .pDescriptorCounts = variableCounts.data(),
    };
    if (setCount > 0) {
        const VkDescriptorSetAllocateInfo dsai{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .pNext = indexed ? &vdcai : nullptr,
            .descriptorPool = indexed ? m_indexedPool : m_descPool,
            .descriptorSetCount = setCount,
            .pSetLayouts = layouts.data(),
        };
        if (vkAllocateDescriptorSets(m_ctx->device(), &dsai, sets.data()) != VK_SUCCESS) {
            vkEndCommandBuffer(m_cmd);
            discardRecordedUploads();
            m_residency.unpinAll();
            for (GpuBuffer& b : m_staging) destroyBuffer(b);
            m_staging.clear();
            st.refusal = TileRefusal::DeviceError;
            st.error = "vkAllocateDescriptorSets failed";
            ++m_stats.refusals;
            return st;
        }
    }

    if (indexed) {
        // Every layer's two sheets, interleaved (pixels, mask) exactly as the shader indexes them.
        std::vector<VkDescriptorImageInfo> sourceInfos(sourceCount);
        for (std::size_t li = 0; li < plan.size(); ++li) {
            const Sheets sh = sheetsFor(plan[li].layer);
            sourceInfos[li * 2] =
                VkDescriptorImageInfo{m_sampler, sh.pixels,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            sourceInfos[li * 2 + 1] =
                VkDescriptorImageInfo{m_sampler, sh.mask,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        }
        const bool haveClip = !m_clip.empty();
        for (std::size_t ri = 0; ri < runs.size(); ++ri) {
            const AtlasRun& r = runs[ri];
            const VkDescriptorImageInfo accInfo{VK_NULL_HANDLE, m_acc[r.image].view,
                                                VK_IMAGE_LAYOUT_GENERAL};
            // ⚠ The clip base is bound to BOTH its bindings here, where the per-layer shapes bind
            // a stand-in to whichever role a layer is not playing. With one set serving every
            // layer there is no per-layer moment at which to swap them -- and the aliasing is
            // inert, because a layer never both reads and publishes a clip base (walkStep returns
            // before the capture) and the barrier between layers orders the writer against the
            // reader. GENERAL is legal for a sampled image and for a storage image alike, which is
            // why the atlas has lived there since item 8.
            const VkDescriptorImageInfo clipInfo{
                m_sampler, haveClip ? m_clip[r.image].view : m_dummyClipRead.view,
                haveClip ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            const VkDescriptorImageInfo clipOutInfo{
                VK_NULL_HANDLE, haveClip ? m_clip[r.image].view : m_dummyClipWrite.view,
                VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorBufferInfo tileInfo{m_tileList.buffer, r.offset, r.bytes};
            const VkWriteDescriptorSet writes[5] = {
                {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 .dstSet = sets[ri],
                 .dstBinding = kBindingAcc,
                 .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                 .pImageInfo = &accInfo},
                {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 .dstSet = sets[ri],
                 .dstBinding = kBindingClip,
                 .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                 .pImageInfo = &clipInfo},
                {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 .dstSet = sets[ri],
                 .dstBinding = kBindingClipOut,
                 .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                 .pImageInfo = &clipOutInfo},
                {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 .dstSet = sets[ri],
                 .dstBinding = kBindingTiles,
                 .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                 .pBufferInfo = &tileInfo},
                {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 .dstSet = sets[ri],
                 .dstBinding = kBindingSources,
                 .descriptorCount = sourceCount,
                 .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                 .pImageInfo = sourceInfos.data()},
            };
            vkUpdateDescriptorSets(m_ctx->device(), 5, writes, 0, nullptr);
        }
    } else {
        for (std::size_t li = 0; li < plan.size(); ++li) {
            const Sheets sh = sheetsFor(plan[li].layer);
            for (std::size_t ai = 0; ai < m_acc.size(); ++ai) {
                const VkDescriptorSet set = sets[li * m_acc.size() + ai];
                const VkDescriptorImageInfo accInfo{VK_NULL_HANDLE, m_acc[ai].view,
                                                    VK_IMAGE_LAYOUT_GENERAL};
                const VkDescriptorImageInfo srcInfo{m_sampler, sh.pixels,
                                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                const VkDescriptorImageInfo maskInfo{m_sampler, sh.mask,
                                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                // A layer either READS a clip base or PUBLISHES one, never both, so whichever role
                // it is not playing gets the 1x1 stand-in. That keeps the real clip image from
                // being bound as a storage image and a sampled image in the same dispatch, which
                // would be legal but is not worth having to reason about.
                const VkDescriptorImageInfo clipInfo{
                    m_sampler, plan[li].clip ? m_clip[ai].view : m_dummyClipRead.view,
                    plan[li].clip ? VK_IMAGE_LAYOUT_GENERAL
                                  : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                const VkDescriptorImageInfo clipOutInfo{
                    VK_NULL_HANDLE,
                    plan[li].clipWrite ? m_clip[ai].view : m_dummyClipWrite.view,
                    VK_IMAGE_LAYOUT_GENERAL};
                // Bound whichever shape is running: the per-tile pipeline never reads it (its
                // specialization folds the branch away), but the MODULE still declares it, so a
                // set without it would be a layout/shader mismatch rather than a saved write.
                // ⚠ PerTile takes the whole buffer from zero rather than this image's run: on the
                // downgrade path the runs carry offsets that `buildTileList` assigned and then
                // failed to allocate for, and an offset past the end of the buffer is an invalid
                // descriptor even for a binding nothing reads.
                const std::uint32_t ri =
                    mode == TileDispatch::PerTile ? UINT32_MAX : runOfImage[ai];
                const VkDescriptorBufferInfo tileInfo{
                    m_tileList.buffer, ri == UINT32_MAX ? 0 : runs[ri].offset,
                    ri == UINT32_MAX ? VK_WHOLE_SIZE : runs[ri].bytes};
                const VkWriteDescriptorSet writes[6] = {
                    {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                     .dstSet = set,
                     .dstBinding = kBindingAcc,
                     .descriptorCount = 1,
                     .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                     .pImageInfo = &accInfo},
                    {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                     .dstSet = set,
                     .dstBinding = kBindingSrc,
                     .descriptorCount = 1,
                     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                     .pImageInfo = &srcInfo},
                    {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                     .dstSet = set,
                     .dstBinding = kBindingMask,
                     .descriptorCount = 1,
                     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                     .pImageInfo = &maskInfo},
                    {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                     .dstSet = set,
                     .dstBinding = kBindingClip,
                     .descriptorCount = 1,
                     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                     .pImageInfo = &clipInfo},
                    {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                     .dstSet = set,
                     .dstBinding = kBindingClipOut,
                     .descriptorCount = 1,
                     .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                     .pImageInfo = &clipOutInfo},
                    {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                     .dstSet = set,
                     .dstBinding = kBindingTiles,
                     .descriptorCount = 1,
                     .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                     .pBufferInfo = &tileInfo},
                };
                vkUpdateDescriptorSets(m_ctx->device(), 6, writes, 0, nullptr);
            }
        }
    }

    // The fused kernel itself -- transform + resample + mask fold + clip + blend. THIS is the row
    // a shader change is allowed to move; the rest of this function is host cost wearing a GPU
    // name. The three shapes below differ ONLY in how a dispatch learns which macrotile it is on,
    // so they write the same bytes; the per-tile loop stays the reference the other two are held
    // to (tests/test_tile_compositor.cpp asserts byte-identity, not a tolerance).
    const std::int32_t tBlend = gpuBegin("Tile blend");
    const VkPipelineLayout pipeLayout = indexed ? m_indexedPipeLayout : m_pipeLayout;
    // TWO KERNELS, ONE WALK. A leaf step runs composite_tile.comp over its resident source sheet;
    // an ADJUSTMENT step runs adjust_tile.comp over the accumulator the steps below it built. They
    // share the descriptor set layout, the pipeline layout, the tile geometry and the per-layer
    // barrier -- the pipeline bind IS the whole switch. Skipping a redundant bind keeps a document
    // with no adjustments at exactly the command stream it had before this arm existed.
    const auto pipelineFor = [&](const Step& s) {
        if (indexed) return s.adjust ? m_indexedAdjustPipeline : m_indexedPipeline;
        if (mode == TileDispatch::TileList) return s.adjust ? m_adjustListPipeline : m_listPipeline;
        return s.adjust ? m_adjustPipeline : m_pipeline;
    };
    VkPipeline boundPipeline = VK_NULL_HANDLE;
    const auto bindPipeline = [&](const Step& s) {
        const VkPipeline p = pipelineFor(s);
        if (p == boundPipeline) return;
        boundPipeline = p;
        vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    };
    const VkMemoryBarrier layerDone{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
    };
    // A list dispatch is sized to the WIDEST macrotile in the set and selects the macrotile in z;
    // a narrower edge tile is handled by the kernel's own extent guard, exactly as it is per-tile.
    // Sizing to the widest rather than to `m_macrotile` matters on a canvas smaller than one
    // macrotile, where the difference is the whole dispatch.
    std::uint32_t widest = 0, tallest = 0;
    for (const auto& tile : tiles) {
        const common::Rect b = m_macroGrid.tileBounds(tile.first);
        widest = std::max(widest, static_cast<std::uint32_t>(b.w));
        tallest = std::max(tallest, static_cast<std::uint32_t>(b.h));
    }
    const std::uint32_t listGroupsX = (widest + kWorkgroup - 1) / kWorkgroup;
    const std::uint32_t listGroupsY = (tallest + kWorkgroup - 1) / kWorkgroup;

    // Everything in the push block that does not depend on the tile. The per-tile shape fills in
    // the three tile lanes afterwards; the two list shapes leave them alone (the list supplies
    // them) except for the indexed one's layer slot, which rides in uTileOrigin.x.
    const auto layerPush = [](const Step& s) {
        PushBlock pc{};
        pc.inv0[0] = static_cast<float>(s.inverse.m00);
        pc.inv0[1] = static_cast<float>(s.inverse.m01);
        pc.inv0[2] = static_cast<float>(s.inverse.m02);
        pc.inv1[0] = static_cast<float>(s.inverse.m10);
        pc.inv1[1] = static_cast<float>(s.inverse.m11);
        pc.inv1[2] = static_cast<float>(s.inverse.m12);
        pc.mask0[0] = static_cast<float>(s.maskXform.m00);
        pc.mask0[1] = static_cast<float>(s.maskXform.m01);
        pc.mask0[2] = static_cast<float>(s.maskXform.m02);
        pc.mask1[0] = static_cast<float>(s.maskXform.m10);
        pc.mask1[1] = static_cast<float>(s.maskXform.m11);
        pc.mask1[2] = static_cast<float>(s.maskXform.m12);
        // The whole layer is bound, so the source window is the layer: origin (0,0), and a true
        // size of 0 tells the kernel to take textureSize(uSrc).
        pc.scale[0] = s.scaleX;
        pc.scale[1] = s.scaleY;
        pc.filter = static_cast<std::int32_t>(s.filter);
        pc.superN = s.superN;
        pc.blend = static_cast<std::int32_t>(s.blend);
        pc.opacity = s.opacity;
        pc.maskMode = s.maskMode;
        pc.clipMode = s.clip ? 1 : 0;
        pc.clipWrite = s.clipWrite ? 1 : 0;
        return pc;
    };

    // The adjustment kernel's twin of layerPush. Every value is already resolved on the Step (see
    // planAdjustment), so this is a transcription and nothing more.
    const auto adjustPush = [](const Step& s) {
        AdjustPush pc{};
        for (int k = 0; k < 4; ++k) pc.maskMap[k] = s.maskMap[k];
        for (int k = 0; k < 12; ++k) pc.p[k] = s.adjustParams[k];
        pc.kind = s.adjustKind;
        pc.opacity = s.opacity;
        pc.maskMode = s.maskMode;
        pc.clipMode = s.clip ? 1 : 0;
        pc.flags = s.adjustFlags;
        return pc;
    };

    // One layer's push, for the two shapes whose tile geometry travels in the LIST rather than in
    // the block. `layerSlot` is the descriptor-indexed variant's array index, which both kernels
    // carry in uTileOrigin.x for the same reason: list-driven means the per-tile lanes are free.
    const auto pushWholeLayer = [&](const Step& s, std::int32_t layerSlot) {
        if (s.adjust) {
            AdjustPush pc = adjustPush(s);
            if (layerSlot >= 0) pc.tileOrigin[0] = layerSlot;
            vkCmdPushConstants(m_cmd, pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            return;
        }
        PushBlock pc = layerPush(s);
        if (layerSlot >= 0) pc.tileOrigin[0] = layerSlot;
        vkCmdPushConstants(m_cmd, pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    };

    for (const Step& s : plan)
        if (s.adjust) ++st.adjustments;

    if (indexed) {
        // ATLAS IMAGE outer, layer inner: ONE descriptor bind for the whole composite on a
        // single-atlas document, against one per layer for the other two shapes. Reordering is
        // sound because atlas images hold DISJOINT macrotiles -- finishing every layer of image A
        // before starting image B changes no ordering that matters, and within an image the
        // bottom-to-top layer sequence and its barriers are untouched.
        for (std::size_t ri = 0; ri < runs.size(); ++ri) {
            vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1,
                                    &sets[ri], 0, nullptr);
            for (std::size_t li = 0; li < plan.size(); ++li) {
                bindPipeline(plan[li]);
                pushWholeLayer(plan[li], static_cast<std::int32_t>(li));  // uLayer
                vkCmdDispatch(m_cmd, listGroupsX, listGroupsY, runs[ri].count);
                ++st.dispatches;
                if (li + 1 < plan.size())
                    vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &layerDone, 0,
                                         nullptr, 0, nullptr);
            }
        }
    } else if (mode == TileDispatch::TileList) {
        for (std::size_t li = 0; li < plan.size(); ++li) {
            bindPipeline(plan[li]);
            for (const AtlasRun& r : runs) {
                const VkDescriptorSet set = sets[li * m_acc.size() + r.image];
                vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1,
                                        &set, 0, nullptr);
                pushWholeLayer(plan[li], -1);
                vkCmdDispatch(m_cmd, listGroupsX, listGroupsY, r.count);
                ++st.dispatches;
            }
            // Layers composite onto the SAME texels bottom -> top, so the next layer reads what
            // this one wrote. Macrotiles within a layer write disjoint regions and need no barrier.
            if (li + 1 < plan.size())
                vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &layerDone, 0,
                                     nullptr, 0, nullptr);
        }
    } else {
        for (std::size_t li = 0; li < plan.size(); ++li) {
            const Step& step = plan[li];
            bindPipeline(step);
            // Only one of the two is filled; the step's kind decides which, once per layer rather
            // than once per macrotile.
            const PushBlock base = step.adjust ? PushBlock{} : layerPush(step);
            const AdjustPush adjBase = step.adjust ? adjustPush(step) : AdjustPush{};
            std::uint32_t boundImage = UINT32_MAX;
            for (const auto& [coord, slot] : tiles) {
                if (slot.image != boundImage) {
                    boundImage = slot.image;
                    const VkDescriptorSet set = sets[li * m_acc.size() + boundImage];
                    vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1,
                                            &set, 0, nullptr);
                }
                const common::Rect bounds = m_macroGrid.tileBounds(coord);
                const std::uint32_t ew = static_cast<std::uint32_t>(bounds.w);
                const std::uint32_t eh = static_cast<std::uint32_t>(bounds.h);
                if (ew == 0 || eh == 0) continue;

                // uTileOrigin travels as an INTEGER and is never baked into the inverse: every
                // sample point is then evaluated at the same target pixel whatever macrotile it
                // lands in, so one recomposited macrotile is bit-identical to the same pixels
                // inside a whole-canvas dispatch. That is what makes k a free knob, what makes a
                // dirty-tile recomposite seamless -- and what lets the list shape above carry the
                // very same integers in a buffer without moving a pixel.
                if (step.adjust) {
                    AdjustPush pc = adjBase;
                    pc.tileOrigin[0] = static_cast<std::int32_t>(bounds.x);
                    pc.tileOrigin[1] = static_cast<std::int32_t>(bounds.y);
                    pc.accOrigin[0] = slot.x;
                    pc.accOrigin[1] = slot.y;
                    pc.extent[0] = static_cast<std::int32_t>(ew);
                    pc.extent[1] = static_cast<std::int32_t>(eh);
                    vkCmdPushConstants(m_cmd, pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                       sizeof(pc), &pc);
                } else {
                    PushBlock pc = base;
                    pc.tileOrigin[0] = static_cast<std::int32_t>(bounds.x);
                    pc.tileOrigin[1] = static_cast<std::int32_t>(bounds.y);
                    pc.accOrigin[0] = slot.x;
                    pc.accOrigin[1] = slot.y;
                    pc.extent[0] = static_cast<std::int32_t>(ew);
                    pc.extent[1] = static_cast<std::int32_t>(eh);
                    vkCmdPushConstants(m_cmd, pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                       sizeof(pc), &pc);
                }
                vkCmdDispatch(m_cmd, (ew + kWorkgroup - 1) / kWorkgroup,
                              (eh + kWorkgroup - 1) / kWorkgroup, 1);
                ++st.dispatches;
            }
            if (li + 1 < plan.size())
                vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &layerDone, 0,
                                     nullptr, 0, nullptr);
        }
    }
    gpuEnd(tBlend);
    gpuEnd(tAll);
    vkEndCommandBuffer(m_cmd);

    vkResetFences(m_ctx->device(), 1, &m_fence);
    const VkSubmitInfo si{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &m_cmd,
    };
    if (m_ctx->submit(si, m_fence) != VK_SUCCESS) {
        discardRecordedUploads();
        m_residency.unpinAll();
        for (GpuBuffer& b : m_staging) destroyBuffer(b);
        m_staging.clear();
        st.refusal = TileRefusal::DeviceError;
        st.error = "vkQueueSubmit failed";
        ++m_stats.refusals;
        return st;
    }
    {
        // Split out because its fix differs from everything else here: a big fence-wait beside a
        // small device row is an ASYNC-SUBMIT problem (S60-c), not a shader problem.
        MOSAIC_PERF_SCOPE("Tile composite (fence wait)", common::Lane::Gpu);
        vkWaitForFences(m_ctx->device(), 1, &m_fence, VK_TRUE, UINT64_MAX);
    }
    // Only now are the timestamps readable: resolveAndRecord() never blocks, so calling it before
    // the fence would silently drop the samples rather than stall.
    if (m_timer) (void)m_timer->resolveAndRecord();

    for (GpuBuffer& b : m_staging) destroyBuffer(b);
    m_staging.clear();
    m_uploading.clear();  // executed, so the cache's claim about these layers is now true
    m_residency.unpinAll();
    // Hand the tiles over to the resolve set BEFORE clearing: a caller may composite twice between
    // presents (an edit plus an undo in one turn), and the second composite must not lose the
    // first one's tiles -- the accumulator would then be ahead of the present texture in exactly
    // the macrotiles nobody looks at again until they change once more.
    m_unresolved.unite(m_dirty);
    m_dirty.clear();
    m_lastPlan = std::move(plan);
    m_accValid = true;
    ++m_revision;

    st.ok = true;
    st.layers = m_lastPlan.size();
    st.macrotiles = tiles.size();
    st.dispatch = mode;
    st.uploadBytes = tally.bytes;
    st.uploadRegions = tally.regions;
    st.partialUploads = tally.partial;
    st.fullUploads = tally.full;
    ++m_stats.composites;
    m_stats.dispatches += st.dispatches;
    m_stats.macrotiles += st.macrotiles;
    m_stats.uploadBytes += tally.bytes;
    m_stats.uploadRegions += tally.regions;
    m_stats.partialUploads += tally.partial;
    m_stats.fullUploads += tally.full;
    return st;
}

bool TileCompositor::ensureDescriptorPool(std::uint32_t sets, std::string& error) {
    if (m_descPool != VK_NULL_HANDLE && m_descPoolSets >= sets) {
        return vkResetDescriptorPool(m_ctx->device(), m_descPool, 0) == VK_SUCCESS;
    }
    if (m_descPool != VK_NULL_HANDLE) {
        m_ctx->waitIdle();
        vkDestroyDescriptorPool(m_ctx->device(), m_descPool, nullptr);
        m_descPool = VK_NULL_HANDLE;
    }
    // Grow with slack so a document that gains a layer does not rebuild the pool every frame.
    const std::uint32_t want = std::max<std::uint32_t>(sets * 2, 16);
    const VkDescriptorPoolSize sizes[3] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, want * 2},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, want * 3},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, want},
    };
    const VkDescriptorPoolCreateInfo dpci{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = want,
        .poolSizeCount = 3,
        .pPoolSizes = sizes,
    };
    if (vkCreateDescriptorPool(m_ctx->device(), &dpci, nullptr, &m_descPool) != VK_SUCCESS) {
        error = "vkCreateDescriptorPool failed";
        m_descPoolSets = 0;
        return false;
    }
    m_descPoolSets = want;
    return true;
}

// ---- The present path: resolve ON THE DEVICE ------------------------------------------------------

bool TileCompositor::createResolveTarget(std::uint32_t w, std::uint32_t h, ResolveTarget& out,
                                         std::string& error) {
    out = ResolveTarget{};
    if (w == 0 || h == 0) {
        error = "resolve target has no extent";
        return false;
    }
    if (!caps().fitsImage(w, h)) {
        error = "resolve target exceeds maxImageDimension2D";
        return false;
    }
    GpuImage img;
    if (!makeImage(w, h, VK_FORMAT_R8G8B8A8_UNORM,
                   VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                   img, error))
        return false;
    out.image = img.image;
    out.view = img.view;
    out.memory = img.memory;
    out.width = w;
    out.height = h;
    out.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

void TileCompositor::destroyResolveTarget(ResolveTarget& t) noexcept {
    if (t.memory == VK_NULL_HANDLE) {  // borrowed: not ours to free
        t = ResolveTarget{};
        return;
    }
    GpuImage img;
    img.image = t.image;
    img.view = t.view;
    img.memory = t.memory;
    img.width = t.width;
    img.height = t.height;
    destroyImage(img);
    t = ResolveTarget{};
}

bool TileCompositor::resolve(const ResolveTarget& dst, std::string& error, bool* wrote) {
    if (wrote != nullptr) *wrote = false;
    // Item 11's per-frame cost, and the row that replaces "Canvas upload (full)" / "(region)" once
    // item 13 flips: those two measured three CPU copies plus a staging write, this measures one
    // dispatch per dirty macrotile and moves ZERO host bytes.
    MOSAIC_PERF_SCOPE("Tile resolve", common::Lane::Gpu);
    if (!dst.valid() || dst.view == VK_NULL_HANDLE) {
        error = "resolve target is not an image";
        return false;
    }
    if (m_acc.empty() || !m_accValid) {
        error = "no resident accumulator";
        return false;
    }
    if (dst.width < m_docW || dst.height < m_docH) {
        error = "resolve target is smaller than the document";
        return false;
    }

    // A full resolve whenever the destination is not provably the one we last wrote. UNDEFINED is
    // included because that transition DISCARDS the image's contents -- a partial resolve out of
    // UNDEFINED would leave every macrotile it did not touch as garbage, and it would look right
    // on the first frame (everything is dirty) and wrong on the second.
    const bool full = dst.image != m_lastResolveImage || dst.width != m_lastResolveW ||
                      dst.height != m_lastResolveH ||
                      dst.layout == VK_IMAGE_LAYOUT_UNDEFINED;
    if (full) m_unresolved.addAll();
    if (m_unresolved.empty()) {
        // The composite changed nothing, so the present texture is already correct. No submit, no
        // dispatch, no bytes, AND NO LAYOUT CHANGE -- `wrote` stays false so the caller does not
        // tell the renderer otherwise. This branch and composite()'s twin are what "resident"
        // means.
        return true;
    }
    if (wrote != nullptr) *wrote = true;

    const core::TileSet macro = m_unresolved.macrotiles(
        m_macrotile == kDirtyTileSize
            ? 0u
            : static_cast<std::uint32_t>(std::countr_zero(m_macrotile / kDirtyTileSize)));
    std::vector<std::pair<core::TileCoord, Slot>> tiles;
    tiles.reserve(static_cast<std::size_t>(macro.count()));
    macro.forEach([&](core::TileCoord c) { tiles.emplace_back(c, slotFor(c.tx, c.ty)); });
    if (tiles.empty()) {
        m_unresolved.clear();
        return true;
    }

    const auto setCount = static_cast<std::uint32_t>(m_acc.size());
    if (!ensureResolvePool(setCount, error)) return false;
    std::vector<VkDescriptorSetLayout> layouts(setCount, m_resolveSetLayout);
    std::vector<VkDescriptorSet> sets(setCount, VK_NULL_HANDLE);
    const VkDescriptorSetAllocateInfo dsai{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = m_resolvePool,
        .descriptorSetCount = setCount,
        .pSetLayouts = layouts.data(),
    };
    if (vkAllocateDescriptorSets(m_ctx->device(), &dsai, sets.data()) != VK_SUCCESS) {
        error = "vkAllocateDescriptorSets failed for the resolve pass";
        return false;
    }
    for (std::size_t i = 0; i < m_acc.size(); ++i) {
        const VkDescriptorImageInfo accInfo{VK_NULL_HANDLE, m_acc[i].view, VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo dstInfo{VK_NULL_HANDLE, dst.view, VK_IMAGE_LAYOUT_GENERAL};
        const VkWriteDescriptorSet writes[2] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = sets[i],
             .dstBinding = kResolveBindingAcc,
             .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
             .pImageInfo = &accInfo},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = sets[i],
             .dstBinding = kResolveBindingDst,
             .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
             .pImageInfo = &dstInfo},
        };
        vkUpdateDescriptorSets(m_ctx->device(), 2, writes, 0, nullptr);
    }

    vkResetCommandBuffer(m_cmd, 0);
    const VkCommandBufferBeginInfo cbi{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(m_cmd, &cbi);
    if (m_timer) m_timer->beginSubmission(m_cmd);
    const std::int32_t tResolve = m_timer ? m_timer->beginScope(m_cmd, "Tile resolve") : -1;

    // The destination joins the accumulator in GENERAL and stays there. `dst.layout` is the
    // caller's fact, not a guess (see ResolveTarget::layout).
    const bool wasRead = dst.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
                         dst.layout == VK_IMAGE_LAYOUT_GENERAL;
    const VkImageMemoryBarrier toWrite{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        // Read-after-write against the present pass, and write-after-write against a previous
        // resolve into the same image; the source mask covers both because GENERAL can mean either.
        .srcAccessMask = static_cast<VkAccessFlags>(
            wasRead ? (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT) : 0),
        .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .oldLayout = dst.layout,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = dst.image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(m_cmd,
                         wasRead ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                 : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &toWrite);
    // ... and make the composite's writes to the accumulator visible to this read. The composite
    // was a separate, already-fenced submit, but the dependency has to be expressed for the read
    // to be defined -- a fence orders execution against the HOST, not against a later submit.
    const VkMemoryBarrier accReady{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
    };
    vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &accReady, 0, nullptr, 0,
                         nullptr);

    vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_resolvePipeline);
    std::uint32_t boundImage = UINT32_MAX;
    std::uint64_t dispatched = 0;
    for (const auto& [coord, slot] : tiles) {
        if (slot.image != boundImage) {
            boundImage = slot.image;
            vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_resolvePipeLayout, 0,
                                    1, &sets[boundImage], 0, nullptr);
        }
        const common::Rect bounds = m_macroGrid.tileBounds(coord);
        const auto ew = static_cast<std::uint32_t>(bounds.w);
        const auto eh = static_cast<std::uint32_t>(bounds.h);
        if (ew == 0 || eh == 0) continue;
        const ResolvePush pc{
            .accOrigin = {slot.x, slot.y},
            .dstOrigin = {static_cast<std::int32_t>(bounds.x), static_cast<std::int32_t>(bounds.y)},
            .extent = {static_cast<std::int32_t>(ew), static_cast<std::int32_t>(eh)},
        };
        vkCmdPushConstants(m_cmd, m_resolvePipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc),
                           &pc);
        vkCmdDispatch(m_cmd, (ew + kWorkgroup - 1) / kWorkgroup, (eh + kWorkgroup - 1) / kWorkgroup,
                      1);
        ++dispatched;
    }
    if (m_timer) m_timer->endScope(m_cmd, tResolve);
    vkEndCommandBuffer(m_cmd);

    vkResetFences(m_ctx->device(), 1, &m_fence);
    const VkSubmitInfo si{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &m_cmd,
    };
    if (m_ctx->submit(si, m_fence) != VK_SUCCESS) {
        error = "vkQueueSubmit failed during resolve";
        return false;
    }
    vkWaitForFences(m_ctx->device(), 1, &m_fence, VK_TRUE, UINT64_MAX);
    if (m_timer) (void)m_timer->resolveAndRecord();

    m_unresolved.clear();
    m_lastResolveImage = dst.image;
    m_lastResolveW = dst.width;
    m_lastResolveH = dst.height;
    ++m_stats.resolves;
    m_stats.resolveTiles += dispatched;
    return true;
}

// ---- Readback -----------------------------------------------------------------------------------

bool TileCompositor::readTarget(const ResolveTarget& src, const common::Rect& roi,
                                common::Image& out, std::string& error) {
    // Readback gets a profiler row of its own precisely because it is the thing that must NOT
    // happen per frame (docs/s60-readback-consumers.md section 7a: "a new consumer then shows up
    // as a number that moved"). A count that tracks the frame count is the regression.
    MOSAIC_PERF_SCOPE("Tile readback (8-bit target)", common::Lane::Gpu);
    if (!src.valid()) {
        error = "no resolve target";
        return false;
    }
    common::Rect r = roi;
    if (r.w <= 0.0 || r.h <= 0.0)
        r = common::Rect{0.0, 0.0, static_cast<double>(src.width), static_cast<double>(src.height)};
    const std::int64_t x0 = std::max<std::int64_t>(0, static_cast<std::int64_t>(std::floor(r.x)));
    const std::int64_t y0 = std::max<std::int64_t>(0, static_cast<std::int64_t>(std::floor(r.y)));
    const std::int64_t x1 =
        std::min<std::int64_t>(src.width, static_cast<std::int64_t>(std::ceil(r.right())));
    const std::int64_t y1 =
        std::min<std::int64_t>(src.height, static_cast<std::int64_t>(std::ceil(r.bottom())));
    if (x1 <= x0 || y1 <= y0) {
        error = "empty readback region";
        return false;
    }
    const auto outW = static_cast<std::uint32_t>(x1 - x0);
    const auto outH = static_cast<std::uint32_t>(y1 - y0);
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(outW) * outH * 4;

    GpuBuffer dst;
    if (!makeHostBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, dst, error)) return false;

    vkResetCommandBuffer(m_cmd, 0);
    const VkCommandBufferBeginInfo cbi{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(m_cmd, &cbi);
    // resolve() leaves the target in GENERAL, which is a legal copy source, so this is a memory
    // barrier rather than a transition.
    const VkImageMemoryBarrier toSrc{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = src.image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);
    const VkBufferImageCopy copy{
        .bufferOffset = 0,
        .bufferRowLength = outW,
        .bufferImageHeight = outH,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageOffset = {static_cast<std::int32_t>(x0), static_cast<std::int32_t>(y0), 0},
        .imageExtent = {outW, outH, 1},
    };
    vkCmdCopyImageToBuffer(m_cmd, src.image, VK_IMAGE_LAYOUT_GENERAL, dst.buffer, 1, &copy);
    const VkBufferMemoryBarrier toHost{
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = dst.buffer,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                         nullptr, 1, &toHost, 0, nullptr);
    vkEndCommandBuffer(m_cmd);

    vkResetFences(m_ctx->device(), 1, &m_fence);
    const VkSubmitInfo si{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &m_cmd,
    };
    if (m_ctx->submit(si, m_fence) != VK_SUCCESS) {
        destroyBuffer(dst);
        error = "vkQueueSubmit failed during a target readback";
        return false;
    }
    vkWaitForFences(m_ctx->device(), 1, &m_fence, VK_TRUE, UINT64_MAX);

    out = common::Image(outW, outH);
    std::memcpy(out.rgba.data(), dst.mapped, static_cast<std::size_t>(bytes));
    destroyBuffer(dst);
    ++m_stats.readbacks;
    m_stats.readbackBytes += static_cast<std::uint64_t>(bytes);
    return true;
}

bool TileCompositor::readback(const common::Rect& roi, common::Image& out, std::string& error) {
    // The expensive twin of readTarget: fp16 accumulator -> 8-bit, twice the bytes off the device.
    // Same reason for the row -- see readTarget.
    MOSAIC_PERF_SCOPE("Tile readback (accumulator)", common::Lane::Gpu);
    if (m_acc.empty() || !m_accValid) {
        error = "no resident accumulator";
        return false;
    }
    common::Rect r = roi;
    if (r.w <= 0.0 || r.h <= 0.0)
        r = common::Rect{0.0, 0.0, static_cast<double>(m_docW), static_cast<double>(m_docH)};
    const std::int64_t x0 = std::max<std::int64_t>(0, static_cast<std::int64_t>(std::floor(r.x)));
    const std::int64_t y0 = std::max<std::int64_t>(0, static_cast<std::int64_t>(std::floor(r.y)));
    const std::int64_t x1 =
        std::min<std::int64_t>(m_docW, static_cast<std::int64_t>(std::ceil(r.right())));
    const std::int64_t y1 =
        std::min<std::int64_t>(m_docH, static_cast<std::int64_t>(std::ceil(r.bottom())));
    if (x1 <= x0 || y1 <= y0) {
        error = "empty readback region";
        return false;
    }
    const std::uint32_t outW = static_cast<std::uint32_t>(x1 - x0);
    const std::uint32_t outH = static_cast<std::uint32_t>(y1 - y0);

    const core::TileRange range = m_macroGrid.tilesCovering(
        common::Rect{static_cast<double>(x0), static_cast<double>(y0), static_cast<double>(outW),
                     static_cast<double>(outH)});
    if (range.empty()) {
        error = "readback region covers no macrotile";
        return false;
    }
    const std::uint64_t tileCount = range.count();
    const VkDeviceSize tileBytes =
        static_cast<VkDeviceSize>(m_macrotile) * m_macrotile * caps().workingFormatBytes();

    GpuBuffer dst;
    if (!makeHostBuffer(tileBytes * tileCount, VK_BUFFER_USAGE_TRANSFER_DST_BIT, dst, error))
        return false;

    vkResetCommandBuffer(m_cmd, 0);
    const VkCommandBufferBeginInfo cbi{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(m_cmd, &cbi);
    // The accumulator lives in GENERAL, which is a legal copy source, so this is a memory barrier
    // rather than a transition: make the last composite's shader writes visible to the transfer.
    const VkMemoryBarrier toTransfer{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
    };
    vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &toTransfer, 0, nullptr, 0,
                         nullptr);
    std::uint64_t slotIndex = 0;
    for (std::uint32_t ty = range.y0; ty < range.y1; ++ty)
        for (std::uint32_t tx = range.x0; tx < range.x1; ++tx, ++slotIndex) {
            const Slot slot = slotFor(tx, ty);
            const VkBufferImageCopy copy{
                .bufferOffset = tileBytes * slotIndex,
                .bufferRowLength = m_macrotile,
                .bufferImageHeight = m_macrotile,
                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                .imageOffset = {slot.x, slot.y, 0},
                .imageExtent = {m_macrotile, m_macrotile, 1},
            };
            vkCmdCopyImageToBuffer(m_cmd, m_acc[slot.image].image, VK_IMAGE_LAYOUT_GENERAL,
                                   dst.buffer, 1, &copy);
        }
    const VkBufferMemoryBarrier toHost{
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = dst.buffer,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                         nullptr, 1, &toHost, 0, nullptr);
    vkEndCommandBuffer(m_cmd);

    vkResetFences(m_ctx->device(), 1, &m_fence);
    const VkSubmitInfo si{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &m_cmd,
    };
    if (m_ctx->submit(si, m_fence) != VK_SUCCESS) {
        destroyBuffer(dst);
        error = "vkQueueSubmit failed during readback";
        return false;
    }
    vkWaitForFences(m_ctx->device(), 1, &m_fence, VK_TRUE, UINT64_MAX);

    // De-tile: the buffer holds whole macrotiles in range order; the output is a plain raster.
    common::ImageF acc(outW, outH);
    const auto* raw = static_cast<const std::uint16_t*>(dst.mapped);
    const std::size_t tileTexels = static_cast<std::size_t>(m_macrotile) * m_macrotile;
    slotIndex = 0;
    for (std::uint32_t ty = range.y0; ty < range.y1; ++ty)
        for (std::uint32_t tx = range.x0; tx < range.x1; ++tx, ++slotIndex) {
            const std::uint16_t* tile = raw + slotIndex * tileTexels * 4;
            const std::int64_t tox = static_cast<std::int64_t>(tx) * m_macrotile;
            const std::int64_t toy = static_cast<std::int64_t>(ty) * m_macrotile;
            for (std::uint32_t j = 0; j < m_macrotile; ++j) {
                const std::int64_t dy = toy + j - y0;
                if (dy < 0 || dy >= static_cast<std::int64_t>(outH)) continue;
                for (std::uint32_t i = 0; i < m_macrotile; ++i) {
                    const std::int64_t dx = tox + i - x0;
                    if (dx < 0 || dx >= static_cast<std::int64_t>(outW)) continue;
                    const std::size_t sp = (static_cast<std::size_t>(j) * m_macrotile + i) * 4;
                    const std::size_t dp =
                        (static_cast<std::size_t>(dy) * outW + static_cast<std::size_t>(dx)) * 4;
                    for (int ch = 0; ch < 4; ++ch)
                        acc.rgba[dp + ch] = halfToFloat(tile[sp + ch]);
                }
            }
        }
    destroyBuffer(dst);

    out = common::toImage8(acc);
    ++m_stats.readbacks;
    m_stats.readbackBytes += static_cast<std::uint64_t>(tileBytes) * tileCount;
    return true;
}

// ---- Vulkan helpers ------------------------------------------------------------------------------

bool TileCompositor::makeImage(std::uint32_t w, std::uint32_t h, VkFormat fmt,
                               VkImageUsageFlags usage, GpuImage& out, std::string& error) const {
    out = {};
    out.width = w;
    out.height = h;
    const VkDevice dev = m_ctx->device();
    const VkImageCreateInfo ici{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt,
        .extent = {w, h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(dev, &ici, nullptr, &out.image) != VK_SUCCESS) {
        error = "vkCreateImage failed";
        out = {};
        return false;
    }
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(dev, out.image, &req);
    const std::uint32_t type =
        m_ctx->findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) {
        error = "no device-local memory type for the image";
        destroyImage(out);
        return false;
    }
    const VkMemoryAllocateInfo mai{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = type,
    };
    if (vkAllocateMemory(dev, &mai, nullptr, &out.memory) != VK_SUCCESS) {
        error = "vkAllocateMemory failed for an image";
        destroyImage(out);
        return false;
    }
    if (vkBindImageMemory(dev, out.image, out.memory, 0) != VK_SUCCESS) {
        error = "vkBindImageMemory failed";
        destroyImage(out);
        return false;
    }
    const VkImageViewCreateInfo vci{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = out.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = fmt,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    if (vkCreateImageView(dev, &vci, nullptr, &out.view) != VK_SUCCESS) {
        error = "vkCreateImageView failed";
        destroyImage(out);
        return false;
    }
    return true;
}

bool TileCompositor::makeHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage, GpuBuffer& out,
                                    std::string& error) const {
    out = {};
    const VkDevice dev = m_ctx->device();
    const VkBufferCreateInfo bci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = std::max<VkDeviceSize>(size, 4),
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(dev, &bci, nullptr, &out.buffer) != VK_SUCCESS) {
        error = "vkCreateBuffer failed";
        out = {};
        return false;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(dev, out.buffer, &req);
    const std::uint32_t type = m_ctx->findMemoryType(
        req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) {
        error = "no host-visible memory type";
        destroyBuffer(out);
        return false;
    }
    const VkMemoryAllocateInfo mai{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = type,
    };
    if (vkAllocateMemory(dev, &mai, nullptr, &out.memory) != VK_SUCCESS) {
        error = "vkAllocateMemory failed for a staging buffer";
        destroyBuffer(out);
        return false;
    }
    if (vkBindBufferMemory(dev, out.buffer, out.memory, 0) != VK_SUCCESS) {
        error = "vkBindBufferMemory failed";
        destroyBuffer(out);
        return false;
    }
    if (vkMapMemory(dev, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped) != VK_SUCCESS) {
        error = "vkMapMemory failed";
        destroyBuffer(out);
        return false;
    }
    out.size = size;
    return true;
}

bool TileCompositor::makeDeviceBuffer(VkDeviceSize size, VkBufferUsageFlags usage, GpuBuffer& out,
                                      std::string& error) const {
    out = {};
    const VkDevice dev = m_ctx->device();
    const VkBufferCreateInfo bci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = std::max<VkDeviceSize>(size, 4),
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(dev, &bci, nullptr, &out.buffer) != VK_SUCCESS) {
        error = "vkCreateBuffer failed";
        out = {};
        return false;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(dev, out.buffer, &req);
    const std::uint32_t type =
        m_ctx->findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) {
        error = "no device-local memory type for a buffer";
        destroyBuffer(out);
        return false;
    }
    const VkMemoryAllocateInfo mai{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = type,
    };
    if (vkAllocateMemory(dev, &mai, nullptr, &out.memory) != VK_SUCCESS) {
        error = "vkAllocateMemory failed for a device buffer";
        destroyBuffer(out);
        return false;
    }
    if (vkBindBufferMemory(dev, out.buffer, out.memory, 0) != VK_SUCCESS) {
        error = "vkBindBufferMemory failed";
        destroyBuffer(out);
        return false;
    }
    out.size = size;  // `mapped` stays null: nothing on the host reads this back
    return true;
}

void TileCompositor::destroyImage(GpuImage& img) const noexcept {
    if (!m_ctx) return;
    const VkDevice dev = m_ctx->device();
    if (img.view != VK_NULL_HANDLE) vkDestroyImageView(dev, img.view, nullptr);
    if (img.image != VK_NULL_HANDLE) vkDestroyImage(dev, img.image, nullptr);
    if (img.memory != VK_NULL_HANDLE) vkFreeMemory(dev, img.memory, nullptr);
    img = {};
}

void TileCompositor::destroyBuffer(GpuBuffer& buf) const noexcept {
    if (!m_ctx) return;
    const VkDevice dev = m_ctx->device();
    if (buf.memory != VK_NULL_HANDLE && buf.mapped != nullptr) vkUnmapMemory(dev, buf.memory);
    if (buf.buffer != VK_NULL_HANDLE) vkDestroyBuffer(dev, buf.buffer, nullptr);
    if (buf.memory != VK_NULL_HANDLE) vkFreeMemory(dev, buf.memory, nullptr);
    buf = {};
}

}  // namespace mosaic::render
