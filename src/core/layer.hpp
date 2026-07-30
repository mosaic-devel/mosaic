#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/exif.hpp"
#include "common/geometry.hpp"
#include "common/image.hpp"
#include "core/blend_mode.hpp"
#include "core/layer_effects.hpp"
#include "core/text/text_model.hpp"
#include "core/texture/texture_params.hpp"
#include "core/vector/object.hpp"

// The layer tree -- the heart of Mosaic's non-destructive document model (PLAN §3.7). A
// document is a tree of layers; the composite is derived from it by the compositor (S7), so
// the tree is the single source of truth and every edit is a re-playable command (S6-b).
//
// Layer kinds: Group, Raster, Vector, Text, Adjustment, Magic, Texture. Every layer carries the
// shared chrome (name, visibility, opacity, blend mode, transform, clip-to-below, an optional
// raster mask); derived kinds add their own payload. Several payloads are intentionally thin
// here and grow in their own sessions (Vector S25, Text S29, Adjustment params S32, Magic
// resampling S50, Texture generators S55) -- S6 establishes the tree and the shared model.
namespace mosaic::core {

class GroupLayer;  // a layer's parent is always a group

// Stable per-document identifier used by commands and serialization to refer to a layer
// without a pointer. 0 is never a valid id.
using LayerId = std::uint64_t;
inline constexpr LayerId kInvalidLayerId = 0;

enum class LayerKind { Group, Raster, Vector, Text, Adjustment, Magic, Texture };
[[nodiscard]] std::string_view layerKindName(LayerKind kind);

// A grayscale coverage mask attached to a layer (8-bit; 255 = fully visible). Vector-GEOMETRY
// masks are a separate, unbuilt feature (S25 stack).
//
// ---- THE GRID CONTRACT (S31; docs/document-model.md) -----------------------------------------
// A mask is a finite SHEET of coverage cells and everything OFF the sheet reads zero, so where the
// sheet lands is not cosmetic -- it decides what the layer is allowed to show at all. `toLocal`
// says where it lands: the affine from MASK PIXEL space into the space the mask lives in, which is
// the layer's LOCAL space when `linked` and its PARENT's space when not.
//
//   * raster/magic/text/texture -- the sheet IS the layer's own sampling grid: its source image,
//     or the renderer's pixel cache (1 mask px per source px). The compositor folds it AT THE
//     SOURCE PIXEL, before placement, so `toLocal` stays the identity for these kinds.
//   * group/vector/adjustment -- these have no pixel grid of their own (they are rasterised or
//     resampled at the target's resolution), so the sheet is the DOCUMENT WINDOW as it stood when
//     the mask was built: `toLocal` is the inverse of the layer's world (linked) or parent
//     (unlinked) transform captured at that moment, so cell (x,y) IS document pixel (x,y) then,
//     and the sheet afterwards rides the layer exactly like the layer's own content does.
//
// The captured placement is what makes a SHAPE layer maskable at all. Shape authoring keeps the
// geometry centred on the local origin and puts the position in the layer transform
// (ui/shape_gesture.hpp), so the old rule -- a document-window sheet pinned to layer-local (0,0),
// with no `toLocal` -- put three quadrants of every shape off its own mask sheet and erased them
// the instant a mask was added. Documents written before `toLocal` existed load with the identity
// and so read back exactly as they were saved.
//
// Build masks through core::revealAllMask / core::maskFromSelection and read the placement back
// through core::maskPlacement / core::maskToDocument: no consumer should compose these transforms
// by hand, because a consumer that disagrees with the compositor about the grid IS this bug.
struct RasterMask {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> coverage;  // size == width*height
    bool enabled = true;                 // a disabled mask is ignored by the compositor
    // Linked (the default): the mask rides the layer's transform -- move the layer and the mask
    // moves with it. Unlinked: the mask stays put in the layer's PARENT space (document space for
    // a top-level layer) while the pixels move under it (S31; the compositor folds it after the
    // layer transform instead of before). The flag also picks which space `toLocal` maps INTO.
    bool linked = true;
    // Mask px -> layer-local (linked) / parent (unlinked) -- the sheet's placement, captured when
    // the mask was built (see the grid contract above). Identity for a source-grid sheet and for
    // anything built at the document origin, which is every mask that predates the field.
    common::Affine2D toLocal;

    RasterMask() = default;
    RasterMask(std::uint32_t w, std::uint32_t h, std::uint8_t fill = 255)
        : width(w), height(h), coverage(static_cast<std::size_t>(w) * h, fill) {}

    [[nodiscard]] bool empty() const noexcept { return width == 0 || height == 0; }
    bool operator==(const RasterMask&) const = default;
};

// ---- Warp (S35-b; docs/warp-tools.md) ---------------------------------------------------------
// The hand-authored deformation a layer carries, sibling to the RasterMask slot above -- and it
// answers the same question the grid contract does: WHAT SPACE DOES THIS LIVE IN. The answer is the
// layer's OWN pixel space, which is the only space where a lattice over the pixels means anything:
// `source` frames the pixels the lattice was laid over and `points` say where each node went, both
// in layer-local px. No `toLocal` is needed and none exists, because a warp is never anything but
// its own layer's sampling grid (the kinds that have no pixel grid of their own cannot be warped at
// all -- the tool refuses them by name).
//
// A warp is stored on the layer even though the pixels have already been baked through it. That is
// deliberate and it is the whole of the tool's re-editability: re-entering Mesh Warp restores the
// handles exactly where they were left, and render::warpImage's two-grid form then applies only the
// DIFFERENCE between the stored grid and the edited one -- so a re-edit deforms the already-warped
// pixels by exactly the change, never by the whole displacement a second time.
enum class WarpKind : std::uint8_t {
    Mesh,        // a Catmull-Rom lattice: cols x rows control points, C1 across patch boundaries
    Perspective, // one 3x3 homography from the 4 source corners to the 4 destination corners
};
[[nodiscard]] std::string_view warpKindName(WarpKind kind);

struct WarpGrid {
    WarpKind kind = WarpKind::Mesh;
    int cols = 4;  // control points per axis, >= 2; Perspective is ALWAYS 2x2
    int rows = 4;
    common::Rect source;  // the source rect in LAYER-LOCAL px that the lattice frames
    // rows*cols destination positions, ROW-MAJOR (index = row*cols + col), layer-local px. An
    // undeformed grid holds exactly the lattice positions of `source` -- see identity().
    std::vector<common::Vec2> points;

    // A grid the kernel and the overlay can both work with: at least 2x2, a matching point count, a
    // non-degenerate source rect, and exactly 2x2 for Perspective.
    [[nodiscard]] bool valid() const;
    // Where node (col, row) sits when nothing has been dragged: the uniform lattice over `source`.
    [[nodiscard]] common::Vec2 latticePoint(int col, int row) const;
    [[nodiscard]] common::Vec2 point(int col, int row) const;
    // Every point still on its lattice position, i.e. the layer is warped by nothing at all.
    // Compared EXACTLY: latticePoint recomputes the same expression it was seeded from, so an
    // untouched grid is bit-identical and a dragged one is not.
    [[nodiscard]] bool identity() const;

    bool operator==(const WarpGrid&) const = default;
};

// The undeformed grid for a source rect: `cols` x `rows` nodes sitting on their lattice positions
// (Perspective forces 2x2 whatever is asked for). This is what the tool stages when a layer has no
// warp yet, and what warpImage's single-grid form uses as its source lattice.
[[nodiscard]] WarpGrid identityWarpGrid(const common::Rect& source, int cols, int rows,
                                        WarpKind kind = WarpKind::Mesh);

// `g`'s own lattice, undeformed: same kind, size and source rect, every point back on its node.
[[nodiscard]] WarpGrid identityLike(const WarpGrid& g);

// `g` slid by `delta` -- both the source rect and every point. What re-homes a stored grid after a
// bake shifted the layer's pixel origin: warpImage reports the new image's (0,0) at (offX, offY) of
// the old space, so the grid that is persisted must be the live one translated by (-offX, -offY) or
// it would describe handles in a coordinate system the layer no longer has.
[[nodiscard]] WarpGrid translatedWarpGrid(const WarpGrid& g, common::Vec2 delta);

// ---- Coverage partitions (docs/document-model.md "Coverage partitions") -----------------------
// Cut splits ONE surface into two layers along a selection's coverage m: the lifted fragment keeps
// alpha A*m, the residual keeps A*(1-m). Their sub-pixel coverages are DISJOINT -- they tile each
// pixel exactly -- but Porter-Duff `over` assumes coverage is INDEPENDENT and so charges m*(1-m)
// to the overlap. Recombining the halves therefore yields 1 - m + m^2 instead of 1, losing up to
// 25% alpha at m = 0.5: the translucent seam a feathered (or merely anti-aliased) cut shows when
// it is pasted back in place.
//
// There is no `over`-based repair. Solving a + b(1-a) = A for either half forces (1-a)(1-b) = 0,
// i.e. one of the two edges must be hard -- either an aliased fragment or a hole that leaves a
// full-opacity fringe of the cut content behind. So instead the halves RECORD that they are a
// partition, and the compositor recombines them with the disjoint operator
// (ao = min(1, aa+ab), Co = (aa*Ca + ab*Cb)/ao), which reconstructs the surface exactly while
// leaving both edges soft.
//
// The link only means anything while the halves still tile: every field below is a snapshot to
// check against, and `partitionPairLive()` is the single predicate that decides. Move, repaint,
// mask, restyle or reorder either half and the pair falls back to plain `over` -- which is the
// honest answer, because the coverages genuinely stopped being complementary.
struct CoveragePartition {
    LayerId partner = kInvalidLayerId;  // the other half
    std::uint64_t token = 0;            // shared id of the split (both halves carry the same one)
    std::uint64_t selfAlphaHash = 0;    // this half's alphaFingerprint() at split time
    // partner-local -> self-local at split time (worldTransform(self)^-1 * worldTransform(partner)).
    // RELATIVE on purpose: a crop or canvas resize rebases both halves together and the partition
    // survives it, while moving one half alone breaks it.
    common::Affine2D relToPartner = common::Affine2D::identity();
};

// ---------------------------------------------------------------------------------------------
// Layer base
// ---------------------------------------------------------------------------------------------
class Layer {
public:
    Layer(LayerId id, LayerKind kind, std::string name)
        : m_id(id), m_kind(kind), m_name(std::move(name)) {}
    virtual ~Layer() = default;

    // Layers are owned through unique_ptr and referenced by id; copying would slice and would
    // duplicate ids, so it is disabled. (A proper deep "duplicate" with fresh ids is S10.)
    Layer(const Layer&) = delete;
    Layer& operator=(const Layer&) = delete;

    [[nodiscard]] LayerId id() const noexcept { return m_id; }
    [[nodiscard]] LayerKind kind() const noexcept { return m_kind; }

    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    void setName(std::string name) { m_name = std::move(name); }

    [[nodiscard]] bool visible() const noexcept { return m_visible; }
    void setVisible(bool v) noexcept { m_visible = v; }

    [[nodiscard]] float opacity() const noexcept { return m_opacity; }
    void setOpacity(float o) noexcept { m_opacity = o < 0.0f ? 0.0f : (o > 1.0f ? 1.0f : o); }

    [[nodiscard]] BlendMode blendMode() const noexcept { return m_blend; }
    void setBlendMode(BlendMode m) noexcept { m_blend = m; }

    [[nodiscard]] bool clipToBelow() const noexcept { return m_clip; }
    void setClipToBelow(bool c) noexcept { m_clip = c; }

    [[nodiscard]] bool locked() const noexcept { return m_locked; }
    void setLocked(bool l) noexcept { m_locked = l; }

    // Marks a layer created by pasting raw pixel data (NOT a whole-layer copy, which pastes
    // like Layer→Duplicate). The Layers panel badges marked rows as unorganized clipboard
    // output; renaming the layer adopts it and clears the mark (SetNameCommand owns that
    // rule, so undo restores it).
    [[nodiscard]] bool pastedMarker() const noexcept { return m_pastedMarker; }
    void setPastedMarker(bool m) noexcept { m_pastedMarker = m; }

    [[nodiscard]] const common::Affine2D& transform() const noexcept { return m_transform; }
    void setTransform(const common::Affine2D& t) noexcept { m_transform = t; }

    // The layer-local bbox of the layer's actual content — the tight alpha>0 pixel box for
    // raster/magic kinds — or nullopt when there is none (fully transparent, or a kind with no
    // pixel extent yet; vector/text bounds arrive with their own sessions). The Move tool's
    // handles, hit-tests, and thumbnails frame THIS, not the (usually document-sized) image.
    [[nodiscard]] virtual std::optional<common::Rect> contentBounds() const {
        return std::nullopt;
    }

    // contentBounds() dilated by how far the layer's effects paint outside the content edge
    // (outside strokes, drop shadows, outer glow -- effectsOutwardReach), or contentBounds()
    // itself with no effects. The compositor sizes a group's isolated buffer and (S60) the
    // dirty-region recomposite to THIS, so an effect drawn beyond the content box is not clipped
    // (docs/layer-effects.md §4). nullopt when there is no content to attach an effect to.
    [[nodiscard]] std::optional<common::Rect> effectsBounds() const {
        const std::optional<common::Rect> cb = contentBounds();
        if (!cb || !m_effects || m_effects->empty()) return cb;
        const double m = effectsOutwardReach(*m_effects);
        if (m <= 0.0) return cb;
        return common::Rect{cb->x - m, cb->y - m, cb->w + 2.0 * m, cb->h + 2.0 * m};
    }

    // Advances whenever the layer's pixel content changes (raster/magic kinds bump it in
    // invalidateContentBounds): a cheap cache key for derived pixels like panel thumbnails.
    [[nodiscard]] virtual std::uint64_t contentRevision() const noexcept { return 0; }

    [[nodiscard]] bool hasMask() const noexcept { return m_mask.has_value(); }
    [[nodiscard]] RasterMask* mask() noexcept { return m_mask ? &*m_mask : nullptr; }
    [[nodiscard]] const RasterMask* mask() const noexcept { return m_mask ? &*m_mask : nullptr; }
    void setMask(RasterMask mask) {
        m_mask = std::move(mask);
        ++m_maskRevision;
    }
    void clearMask() noexcept {
        m_mask.reset();
        ++m_maskRevision;
    }
    // Detach and return the mask (for undo); precondition: hasMask().
    [[nodiscard]] RasterMask takeMask() {
        RasterMask m = std::move(*m_mask);
        m_mask.reset();
        ++m_maskRevision;
        return m;
    }
    // Advances whenever the mask changes -- set/clear above, and bumped explicitly by anything
    // that mutates the mask IN PLACE through mask() (the S31 mask-paint stroke, the flag
    // commands). A cheap cache key for the layer panel's mask thumbnail, mirroring
    // contentRevision() for pixels; masks stay plain value structs (no counter inside RasterMask,
    // which is compared and copied wholesale by commands).
    [[nodiscard]] std::uint64_t maskRevision() const noexcept { return m_maskRevision; }
    void bumpMaskRevision() noexcept { ++m_maskRevision; }

    // Non-destructive per-layer effects (LE-a; docs/layer-effects.md), beside the RasterMask on
    // the base so every kind inherits them. std::nullopt (the default) is "no effects" -- the
    // compositor then takes today's exact renderLayer->walkStep path, byte-identical. The
    // compositor applies these between rendering the layer's isolated RGBA and blending it.
    [[nodiscard]] bool hasEffects() const noexcept { return m_effects.has_value(); }
    [[nodiscard]] const LayerEffects& effects() const noexcept { return *m_effects; }
    [[nodiscard]] LayerEffects& mutableEffects() noexcept { return *m_effects; }
    void setEffects(LayerEffects fx) {
        m_effects = std::move(fx);
        ++m_effectsRevision;
    }
    void clearEffects() noexcept {
        m_effects.reset();
        ++m_effectsRevision;
    }
    // Advances whenever the effect stack changes -- set/clear above, and bumped explicitly by
    // anything that mutates it IN PLACE through mutableEffects(). Exactly the maskRevision() story
    // one field down: effects are not pixel CONTENT, so no contentRevision() moves for them, yet a
    // group's composite renders its children THROUGH them (render::applyEffects) -- so a child's
    // shadow, glow or overlay changes every derived image above it (the layer panel's group and
    // adjustment thumbnails) with nothing else to notice by.
    [[nodiscard]] std::uint64_t effectsRevision() const noexcept { return m_effectsRevision; }
    void bumpEffectsRevision() noexcept { ++m_effectsRevision; }

    // The hand-authored warp this layer carries (S35-b; see WarpGrid above), or nullopt -- which is
    // every layer that has never been warped, and the only state the kinds with no pixel grid can
    // be in. The PIXELS have already been baked through it; what the grid buys is re-editability,
    // so re-entering the warp tool finds the handles where the user left them and edits the
    // difference (render::warpImage's from/to form).
    [[nodiscard]] bool hasWarp() const noexcept { return m_warp.has_value(); }
    [[nodiscard]] const WarpGrid* warp() const noexcept { return m_warp ? &*m_warp : nullptr; }
    void setWarp(WarpGrid grid) {
        m_warp = std::move(grid);
        ++m_warpRevision;
    }
    void clearWarp() noexcept {
        m_warp.reset();
        ++m_warpRevision;
    }
    // Advances on every warp change, exactly as maskRevision() and effectsRevision() do -- and for
    // the same reason those exist. A warp is not pixel CONTENT (the bake bumps contentRevision on
    // its own), yet the layer dock's badge, the tool's own re-entry and anything else keyed on what
    // the layer IS have nothing else to notice a grid edit by. A slot that moves no revision is a
    // known bug class in this tree; this one moves.
    [[nodiscard]] std::uint64_t warpRevision() const noexcept { return m_warpRevision; }
    void bumpWarpRevision() noexcept { ++m_warpRevision; }

    // This layer's half of a coverage partition, if it is one (see CoveragePartition above);
    // nullopt for every ordinary layer. Purely a compositing hint: a stale or dangling link is
    // inert, because partitionPairLive() re-checks every condition against the live tree.
    [[nodiscard]] const std::optional<CoveragePartition>& partition() const noexcept {
        return m_partition;
    }
    void setPartition(std::optional<CoveragePartition> p) noexcept { m_partition = p; }

    // EXIF camera metadata carried over from the photo this layer's pixels were loaded from
    // (common/exif.hpp; io reads it at load time, orientation already baked into the pixels).
    // std::nullopt -- the default, and the only state for layers that never came from a photo.
    // The sky generator's "Estimate from layer" reads focalLength35mm / dateTimeOriginal /
    // gpsLatitude+gpsLongitude from here; the .mosaic manifest persists it ("exif").
    [[nodiscard]] const std::optional<common::ExifData>& exif() const noexcept { return m_exif; }
    void setExif(std::optional<common::ExifData> e) { m_exif = std::move(e); }

    // The owning group, or nullptr for the document root / a detached layer.
    [[nodiscard]] GroupLayer* parent() const noexcept { return m_parent; }

    // Safe down-cast helpers (RTTI is enabled, PLAN §3.1). e.g. layer.as<RasterLayer>().
    template <class T>
    [[nodiscard]] T* as() noexcept {
        return dynamic_cast<T*>(this);
    }
    template <class T>
    [[nodiscard]] const T* as() const noexcept {
        return dynamic_cast<const T*>(this);
    }

private:
    friend class GroupLayer;  // maintains m_parent on insert/remove

    LayerId m_id;
    LayerKind m_kind;
    std::string m_name;
    bool m_visible = true;
    bool m_locked = false;
    bool m_clip = false;
    bool m_pastedMarker = false;
    float m_opacity = 1.0f;
    BlendMode m_blend = BlendMode::Normal;
    common::Affine2D m_transform = common::Affine2D::identity();
    std::optional<RasterMask> m_mask;
    std::uint64_t m_maskRevision = 0;  // bumped on every mask change (see maskRevision())
    std::optional<LayerEffects> m_effects;  // LE-a: non-destructive per-layer styles (nullopt = none)
    std::uint64_t m_effectsRevision = 0;   // bumped on every effects change (effectsRevision())
    std::optional<WarpGrid> m_warp;        // S35-b: the hand-authored deformation (nullopt = none)
    std::uint64_t m_warpRevision = 0;      // bumped on every warp change (warpRevision())
    std::optional<common::ExifData> m_exif;  // camera metadata from the source photo (nullopt = none)
    std::optional<CoveragePartition> m_partition;  // this half of a split surface (nullopt = none)
    GroupLayer* m_parent = nullptr;
};

// ---------------------------------------------------------------------------------------------
// Group
// ---------------------------------------------------------------------------------------------
class GroupLayer : public Layer {
public:
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    GroupLayer(LayerId id, std::string name) : Layer(id, LayerKind::Group, std::move(name)) {}

    // Children are ordered BOTTOM (index 0) -> TOP (last): the compositor draws front-to-back,
    // and "an adjustment affects the layers below it" means the lower indices. The layer panel
    // (S10) displays this reversed (top of the stack at the top of the list).
    [[nodiscard]] std::size_t childCount() const noexcept { return m_children.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_children.empty(); }
    [[nodiscard]] const std::vector<std::unique_ptr<Layer>>& children() const noexcept {
        return m_children;
    }
    [[nodiscard]] Layer& child(std::size_t i) { return *m_children[i]; }
    [[nodiscard]] const Layer& child(std::size_t i) const { return *m_children[i]; }

    // Insert `layer` at `index` (clamped to [0, childCount]); returns the inserted layer.
    Layer& insert(std::size_t index, std::unique_ptr<Layer> layer);
    // Convenience: insert on top of the stack (== insert(childCount, ...)).
    Layer& addOnTop(std::unique_ptr<Layer> layer) { return insert(childCount(), std::move(layer)); }

    // Remove and return ownership of the child at `index` (its parent pointer is cleared).
    [[nodiscard]] std::unique_ptr<Layer> removeAt(std::size_t index);

    // Index of a *direct* child by id, or npos.
    [[nodiscard]] std::size_t indexOf(LayerId id) const noexcept;

    [[nodiscard]] bool expanded() const noexcept { return m_expanded; }
    void setExpanded(bool e) noexcept { m_expanded = e; }

    // A group's content is its visible children's content, mapped through THEIR transforms
    // into the group's local space (children compose in group space; the group's own transform
    // is applied by whoever places the group). Recomputed per call — pure rect math over the
    // children's (cached) bounds. Lets the Move tool frame and transform whole groups.
    [[nodiscard]] std::optional<common::Rect> contentBounds() const override;

private:
    std::vector<std::unique_ptr<Layer>> m_children;  // [0]=bottom .. [n-1]=top
    bool m_expanded = true;
};

// ---------------------------------------------------------------------------------------------
// Raster
// ---------------------------------------------------------------------------------------------
class RasterLayer : public Layer {
public:
    RasterLayer(LayerId id, std::string name, std::uint32_t w, std::uint32_t h)
        : Layer(id, LayerKind::Raster, std::move(name)), m_image(w, h) {}

    // Pixel storage is 8-bit RGBA for now; it migrates to the float/tiled representation with
    // the compositor and the .mosaic tile store (S7/S48). Mutating through this reference
    // STALES contentBounds(): any edit after the layer entered the document must be followed
    // by invalidateContentBounds() (SetLayerPixelsCommand does this; creation-time fills
    // happen before the first query, so they are safe by construction).
    [[nodiscard]] common::Image& image() noexcept { return m_image; }
    [[nodiscard]] const common::Image& image() const noexcept { return m_image; }

    // The cached tight alpha bbox of image() (lazy; recomputed after invalidation).
    [[nodiscard]] std::optional<common::Rect> contentBounds() const override;
    void invalidateContentBounds() noexcept {
        m_boundsValid = false;
        ++m_contentRevision;
    }
    [[nodiscard]] std::uint64_t contentRevision() const noexcept override {
        return m_contentRevision;
    }

    // FNV-1a over the ALPHA channel, memoised against contentRevision(). This identifies the
    // layer's coverage rather than the counter that tracks edits to it -- which matters for
    // coverage partitions, because undo/redo lands on byte-identical pixels through a FRESH
    // command, so the revision has moved on even though nothing about the coverage has. Colour is
    // deliberately not hashed: the reconstruction only ever rewrites alpha, so repainting inside a
    // hole without changing its shape leaves the partition perfectly valid.
    [[nodiscard]] std::uint64_t alphaFingerprint() const;

private:
    common::Image m_image;
    mutable std::optional<common::Rect> m_contentBounds;
    mutable bool m_boundsValid = false;
    std::uint64_t m_contentRevision = 0;
    mutable std::uint64_t m_alphaHash = 0;
    mutable std::uint64_t m_alphaHashRevision = static_cast<std::uint64_t>(-1);
};

// ---------------------------------------------------------------------------------------------
// Vector (S25) -- holds exactly one vec::Object (geometry + fill + stroke), in layer-local
// space; the layer transform places it. "Shape/path/gradient layer" are all this one kind,
// distinguished only by the object's contents (docs/vector-model.md §1). Mutating the object
// through object() STALES contentBounds(): follow edits with invalidateContentBounds(), as the
// raster kinds do.
// ---------------------------------------------------------------------------------------------
class VectorLayer : public Layer {
public:
    VectorLayer(LayerId id, std::string name) : Layer(id, LayerKind::Vector, std::move(name)) {}

    [[nodiscard]] bool hasObject() const noexcept { return m_object.has_value(); }
    [[nodiscard]] vec::Object* object() noexcept { return m_object ? &*m_object : nullptr; }
    [[nodiscard]] const vec::Object* object() const noexcept { return m_object ? &*m_object : nullptr; }
    void setObject(vec::Object o) {
        m_object = std::move(o);
        invalidateContentBounds();
    }
    void clearObject() noexcept {
        m_object.reset();
        invalidateContentBounds();
    }

    // The tight layer-local bbox of the flattened geometry (lazy; recomputed after invalidation).
    [[nodiscard]] std::optional<common::Rect> contentBounds() const override;
    void invalidateContentBounds() noexcept {
        m_boundsValid = false;
        ++m_contentRevision;
    }
    [[nodiscard]] std::uint64_t contentRevision() const noexcept override { return m_contentRevision; }

private:
    std::optional<vec::Object> m_object;
    mutable std::optional<common::Rect> m_contentBounds;
    mutable bool m_boundsValid = false;
    std::uint64_t m_contentRevision = 0;
};

// ---------------------------------------------------------------------------------------------
// Text (S29) -- holds exactly one text::TextBlock (rich runs + paragraphs + frame/AA), in
// layer-local em space; the layer transform places it (mirroring "one object per vector layer",
// docs/type-tool.md §3.2). Mutating the block through mutableBlock() STALES contentBounds():
// follow such edits with invalidateContentBounds(), as the raster/vector kinds do.
//
// contentBounds() is a CACHE the renderer populates: computing it needs shaping/layout (a font
// stack + a FontProvider) which the base layer model deliberately does not depend on, so the
// box is nullopt until the text render path measures the block and calls setCachedContentBounds().
// ---------------------------------------------------------------------------------------------
class TextLayer : public Layer {
public:
    TextLayer(LayerId id, std::string name, std::string text = {})
        : Layer(id, LayerKind::Text, std::move(name)),
          m_block(text::makeBlock(std::move(text))) {}

    [[nodiscard]] const text::TextBlock& block() const noexcept { return m_block; }
    // Mutable access for in-place edits (run/paragraph helpers); follow with invalidateContentBounds().
    [[nodiscard]] text::TextBlock& mutableBlock() noexcept { return m_block; }
    void setBlock(text::TextBlock b) {
        m_block = std::move(b);
        invalidateContentBounds();
    }

    // Back-compat plain-text convenience: the block's whole text as one default-styled run.
    [[nodiscard]] const std::string& text() const noexcept { return m_block.utf8; }
    void setText(std::string text) {
        m_block = text::makeBlock(std::move(text));
        invalidateContentBounds();
    }

    // The cached tight layer-local bbox of the laid-out text, or nullopt until the renderer has
    // measured it (see the class note). The Move gizmo / thumbnails frame THIS (S29-b wires it).
    [[nodiscard]] std::optional<common::Rect> contentBounds() const override { return m_cachedBounds; }
    void setCachedContentBounds(std::optional<common::Rect> b) const noexcept { m_cachedBounds = b; }
    void invalidateContentBounds() noexcept {
        m_cachedBounds.reset();
        ++m_contentRevision;
    }
    [[nodiscard]] std::uint64_t contentRevision() const noexcept override { return m_contentRevision; }

    // The rendered-pixel cache (S29-b; docs/type-tool.md §5.4): the block rasterized at BASE
    // resolution (1 px per layer unit), plus the image-pixel -> layer-local map the compositor places
    // it by. The app's text render pass (text::refreshTextCache) populates it whenever the block
    // changes, so the compositor stays font-free and just composites it like a raster source. Pixels
    // are a CACHE: zoom magnifies them in a raster document (correct there); the GPU-resident Slug
    // path re-evaluates coverage at device resolution later. nullptr cache => the layer is empty.
    [[nodiscard]] const common::Image* cachedImage() const noexcept {
        return m_cacheImage ? &*m_cacheImage : nullptr;
    }
    [[nodiscard]] const common::Affine2D& cacheImageToLayer() const noexcept {
        return m_cacheImageToLayer;
    }
    // The 2x2 linear part of the layer transform that was BAKED into the cached pixels (item 8): the
    // renderer rasterizes the glyph contours through scale/rotation/shear so a stretched text layer
    // stays crisp instead of upsampling a 1px/unit bitmap. It is part of the cache-validity key -- a
    // change to the layer transform's linear part re-renders (the residual translation composites as
    // before). Identity for an untransformed layer => the base-res cache, unchanged from S29-b.
    [[nodiscard]] const common::Affine2D& cacheLinear() const noexcept { return m_cacheLinear; }
    void setCachedImage(std::optional<common::Image> img, common::Affine2D imageToLayer,
                        bool clipped = false,
                        common::Affine2D bakedLinear = common::Affine2D::identity()) const {
        m_cacheImage = std::move(img);
        m_cacheImageToLayer = imageToLayer;
        m_cacheClipped = clipped;
        m_cacheLinear = bakedLinear;
        m_cacheRevision = m_contentRevision;
        ++m_cacheGeneration;
    }
    // True when the pixel/bounds caches already reflect the current block (no re-render needed).
    [[nodiscard]] bool cacheCurrent() const noexcept { return m_cacheRevision == m_contentRevision; }
    // How many times the pixel cache has been REPLACED. Monotonic, and it moves on every
    // setCachedImage -- including the ones that put different pixels behind an UNCHANGED
    // contentRevision, which is the whole reason it exists (S60-a):
    //
    //   * the DRAFT (half-res) bake taken while a block-edit gesture or a font hover is live, and
    //     the crisp re-render that lands when the gesture settles. refreshTextCache re-renders on
    //     `linearClose(cacheLinear, requested)`, and setCachedImage then stamps m_cacheRevision
    //     back to the SAME m_contentRevision -- so cacheCurrent() flips false and true again with
    //     the counter never moving.
    //   * the clip flip (an Area block renders unclipped while it is the edit target, clipped
    //     once it is not) and the 3D overlay-key re-bake, on the same argument.
    //   * a rotate/scale that re-bakes the linear part into the glyph raster: contentRevision is
    //     untouched, the pixels AND cacheImageToLayer both change.
    //
    // So anything holding a derived copy of these pixels -- render::TileCompositor's device-
    // resident source, above all -- must key on THIS and not on contentRevision(), or it serves
    // the draft bake for as long as the layer stays resident. contentRevision() remains the key
    // for "the BLOCK changed"; this one is "the PIXELS changed".
    [[nodiscard]] std::uint64_t cacheGeneration() const noexcept { return m_cacheGeneration; }
    // The LAYER-LOCAL bounds of the cached pixels' actual INK (the alpha > 0 bbox, measured by the
    // renderer as it caches -- nullopt when empty/unmeasured). The tightest honest answer to
    // "where is this text ON SCREEN": a 3D solid's projected extent is a conservative estimate and
    // a bent block's flat box is a fiction, but the rendered alpha is neither -- the rotate
    // affordance anchors to this ("the rotate handles still conform poorly", user 2026-07-14).
    [[nodiscard]] const std::optional<common::Rect>& cachedInkBounds() const noexcept {
        return m_cacheInkBounds;
    }
    void setCachedInkBounds(std::optional<common::Rect> r) const noexcept {
        m_cacheInkBounds = std::move(r);
    }
    // Whether the cached pixels were clipped to the Area box (overset text cut). The renderer leaves
    // the block being EDITED unclipped (so you can see/fix overflow) and clips it back when deselected,
    // so this is part of the cache's validity key (round-4 #3): re-render when it must flip.
    [[nodiscard]] bool cacheClipped() const noexcept { return m_cacheClipped; }

    // The Layer-Effects overlays (colour/gradient/pattern) BAKED into the cached pixels (S30-e,
    // docs/type-tool.md §12): an EXTRUDED block's 3D render consumes them per face, so they are
    // part of the cache-validity key -- an effects edit must re-render the solid even though the
    // block itself is untouched. Flat blocks always store the defaults (their overlays are the
    // 2D effect pass's business and never touch this cache).
    [[nodiscard]] const std::array<OverlayEffect, 3>& cachedOverlays() const noexcept {
        return m_cacheOverlays;
    }
    void setCachedOverlays(std::array<OverlayEffect, 3> ov) const noexcept {
        m_cacheOverlays = std::move(ov);
    }

    // Display-name auto-tracking (round-4 #5): while true, the layer's shown name follows the text
    // content (Affinity-style). A future manual-rename clears it; for now it stays true.
    [[nodiscard]] bool autoNamed() const noexcept { return m_autoName; }
    void setAutoNamed(bool on) noexcept { m_autoName = on; }

    // The 3D reflection environment (S30-d follow-up): a downsampled snapshot of the document
    // composited BELOW this layer, sampled by the extrude render lanes when the block's
    // Extrude::reflectCanvas is set. Render support like the pixel cache (not document content;
    // no undo). The app builds/refreshes it (core cannot composite documents); setting it bumps
    // the content revision so the pixel cache re-renders with the fresh mirror.
    [[nodiscard]] const common::ImageF* reflectionEnv() const noexcept {
        return m_reflectionEnv ? &*m_reflectionEnv : nullptr;
    }
    [[nodiscard]] const common::Affine2D& reflectionEnvTransform() const noexcept {
        return m_reflectionEnvToPx;  // layer-local design point -> env image pixel
    }
    void setReflectionEnv(std::optional<common::ImageF> img, common::Affine2D layerToEnv) {
        m_reflectionEnv = std::move(img);
        m_reflectionEnvToPx = layerToEnv;
        invalidateContentBounds();
    }
    // The backdrop fingerprint the stored snapshot mirrors (the app's staleness key) -- per layer,
    // so mirrors refresh with or without an edit session on this block (round 3).
    [[nodiscard]] std::uint64_t reflectionEnvFingerprint() const noexcept {
        return m_reflectionEnvFp;
    }
    void setReflectionEnvFingerprint(std::uint64_t fp) noexcept { m_reflectionEnvFp = fp; }

private:
    text::TextBlock m_block;
    mutable std::optional<common::Rect> m_cachedBounds;
    mutable std::optional<common::Image> m_cacheImage;
    mutable common::Affine2D m_cacheImageToLayer = common::Affine2D::identity();
    mutable common::Affine2D m_cacheLinear = common::Affine2D::identity();  // baked linear (cache key)
    mutable std::uint64_t m_cacheRevision = static_cast<std::uint64_t>(-1);  // != revision => stale
    mutable std::uint64_t m_cacheGeneration = 0;  // ++ per setCachedImage (see cacheGeneration())
    mutable bool m_cacheClipped = false;  // was the cache clipped to the Area box? (cache-validity key)
    mutable std::array<OverlayEffect, 3> m_cacheOverlays{};  // baked 3D overlays (cache-validity key)
    mutable std::optional<common::Rect> m_cacheInkBounds;  // alpha>0 bbox, layer-local (see accessor)
    std::optional<common::ImageF> m_reflectionEnv;  // 3D canvas-reflection snapshot (app-built)
    common::Affine2D m_reflectionEnvToPx = common::Affine2D::identity();
    std::uint64_t m_reflectionEnvFp = 0;            // the backdrop state the snapshot mirrors
    bool m_autoName = true;               // name tracks content until a manual rename (round-4 #5)
    std::uint64_t m_contentRevision = 0;
};

// ---------------------------------------------------------------------------------------------
// Adjustment / Filter (non-destructive; typed parameters added in S32)
// ---------------------------------------------------------------------------------------------
enum class AdjustmentKind {
    BrightnessContrast,
    Levels,
    Curves,
    Exposure,
    HueSaturation,
    ColorBalance,
    Grayscale,
    Invert,
    Threshold,
    Posterize,
    // The S55 estimate-from-layer harmonization grade (docs/research-sky-estimate-from-layer.md
    // §6): a fused von-Kries white balance + log-luminance exposure/contrast transfer + vertical
    // gradient + scotopic night mix, every knob a scalar in the params bag (the keys are listed
    // at texture/sky_estimate.hpp::photometricMatchParams). Unlike the S32-S35 kinds it ships
    // with its math already wired in the compositor.
    PhotometricMatch,
    // The S33 blur family (docs/blur-filters.md). Unlike the color kinds these are SPATIAL --
    // they read the backdrop's neighborhood and diffuse its alpha -- so the compositor routes
    // them through its blur branch (adjustmentIsSpatial) and the region/group-extent machinery
    // accounts for their reach (§5 of the doc).
    GaussianBlur,
    BoxBlur,
    MotionBlur,
    RadialBlur,   // Spin / Zoom by the "mode" param
    SurfaceBlur,  // edge-preserving bilateral -- deliberately NOT a guided filter
    LensBlur,     // polygonal-aperture bokeh
    DofBlur,      // Depth of Field: ONE focus band, rendered by level interpolation
    // The S34 photographic/compositing repairs (docs/adjustment-layers.md §2.2-§2.5). APPEND
    // ONLY -- kinds serialize by TOKEN STRING (io/mosaic/docjson.cpp kAdjustmentTokens), so the
    // enum ORDER is free, but appending keeps every existing switch's diff to added rows.
    ShadowsHighlights,  // SPATIAL: local tone recovery under a blurred-luminance mask
    Defringe,           // SPATIAL when its lateral-CA rescale is on; hue-band chroma suppression
    MatteRemoval,       // per-pixel compositing algebra against a known matte / alpha
    HazeRemoval,        // per-pixel airlight unmixing at a CONSTANT transmission (never estimated)
    // S35 artistic / stylize family (docs/filters-stylize.md). APPEND ONLY, same reasoning as
    // above. Seven of the nine are SPATIAL (they read a neighbourhood or resample); AddNoise and
    // Vignette are per-pixel but position-dependent, so they route through the stylize branch
    // (render::isStylizeKind) rather than the compositor's scalar colour loop.
    Sharpen,      // the 3x3 Laplacian sharpen (amount only)
    UnsharpMask,  // radius / amount / threshold, the darkroom technique
    AddNoise,     // gaussian or uniform, hash-seeded on the PARENT pixel (deterministic)
    Denoise,      // Lee 1980 local-statistics MMSE -- this form only, never another denoiser
    Pixelate,     // mosaic cells, lattice anchored in parent space
    Emboss,       // directional relief on premultiplied luma
    OilPaint,     // Kuwahara 1976, the original four-quadrant form -- never a later variant
    Wave,         // sinusoidal displacement, Wave / Ripple by the "mode" param
    Vignette,     // radial exposure falloff in linear light -- a PLAIN exposure scale, and only
                  // that: never a highlight-priority / contrast-protecting variant, never a
                  // tone-dependent falloff, never a lens-profile auto-correction mode
    // S34-a, the cheap high-value remainder of the galleries (docs/adjustment-layers.md
    // §2.6-§2.9). APPEND ONLY, same reasoning as above.
    GradientMap,  // per-pixel: the backdrop's luma through a USER-authored ramp (never derived)
    Vibrance,     // per-pixel: saturation weighted by the pixel's OWN saturation
    PhotoFilter,  // per-pixel: a coloured filter x density, in LINEAR light
    HighPass,     // SPATIAL: p - G(p) + 1/2 -- the unsharp difference drawn on its own
};
[[nodiscard]] std::string_view adjustmentKindName(AdjustmentKind kind);

class AdjustmentLayer : public Layer {
public:
    AdjustmentLayer(LayerId id, std::string name, AdjustmentKind kind)
        : Layer(id, LayerKind::Adjustment, std::move(name)), m_kind(kind) {}

    [[nodiscard]] AdjustmentKind adjustmentKind() const noexcept { return m_kind; }
    void setAdjustmentKind(AdjustmentKind kind) noexcept { m_kind = kind; }

    // A simple name->value bag until the typed parameter system (S32) replaces it; keeps
    // adjustment layers serializable and round-trippable in the meantime.
    [[nodiscard]] std::map<std::string, double>& params() noexcept { return m_params; }
    [[nodiscard]] const std::map<std::string, double>& params() const noexcept { return m_params; }

private:
    AdjustmentKind m_kind;
    std::map<std::string, double> m_params;
};

// The composition of every ancestor group's transform above `layer` (the document root
// included, though it stays identity in practice): the space `layer.transform()` maps INTO.
// A layer's own transform is parent-relative — children compose inside their group's space,
// and the compositor nests its sampling accordingly — so anything reasoning in DOCUMENT space
// (hit tests, the Move tool, thumbnails, pixel selection, the clipboard) must compose the
// chain or it goes stale the moment a group is transformed.
[[nodiscard]] common::Affine2D parentWorldTransform(const Layer& layer);

// parentWorldTransform(layer) ∘ layer.transform(): layer-local -> document space.
[[nodiscard]] common::Affine2D worldTransform(const Layer& layer);

// ---- Mask placement (the RasterMask grid contract, above) ------------------------------------
// Where a layer's mask sheet sits: MASK PX -> the space the mask lives in (the layer's LOCAL space
// when linked, its PARENT's when not). That is RasterMask::toLocal plus the proportional
// source-image scale a raster/magic sheet takes when its resolution differs from the image's --
// everything between the mask's cells and the layer, and nothing above it. The compositor folds
// through this composed with its own buffer placement; everyone else wants maskToDocument.
[[nodiscard]] common::Affine2D maskPlacement(const Layer& layer, const RasterMask& mask);

// MASK PX -> DOCUMENT space: maskPlacement composed with the layer's world transform (linked) or
// its parent's alone (unlinked) -- exactly where the compositor folds the mask. The single map
// every consumer outside the compositor's buffer space must use (mask painting,
// selectionFromLayerMask, SetMaskPixelsCommand::dirtyRegion, mask thumbnails).
[[nodiscard]] common::Affine2D maskToDocument(const Layer& layer, const RasterMask& mask);

// The RasterMask::toLocal a mask built NOW for `layer` has to carry: the identity for the kinds
// whose sheet is their own sampling grid (raster/magic/text/texture) and otherwise the inverse of
// the world (linked) / parent (unlinked) transform, which is what pins a document-window sheet onto
// the document pixels it was built from. A singular transform degrades to the identity (nothing
// shows through it anyway).
[[nodiscard]] common::Affine2D newMaskToLocal(const Layer& layer, bool linked);

// Do `lower` and `upper` still hold the two complementary halves of one surface (see
// CoveragePartition)? Both must be visible, un-effected, unclipped rasters with cross-referencing
// tokens, on the document's integer grid, still carrying the pixels they were split with
// (contentRevision), and still at the same RELATIVE world transform as at the split. `lower`
// additionally has to deliver its alpha untouched (full opacity, no mask), because it is the half
// whose alpha the reconstruction rewrites.
//
// Deliberately NOT conditions on `upper`: blend mode (alpha compositing is blend-independent --
// ao = as + ab(1-as) -- so a Multiply cutout reconstructs just as exactly, and gets to blend
// against a WHOLE surface instead of a holey one), opacity, and a linked mask. Both of the latter
// simply attenuate the fragment, and an attenuated fragment has a well-defined target too:
// r + k*f, still reachable by the same rewrite once k is folded in.
//
// Every condition is re-derived from the live tree, so the link needs no invalidation hooks:
// anything that makes the coverages stop tiling simply stops the compositor compensating, and the
// document falls back to what `over` honestly says about two independent layers.
[[nodiscard]] bool partitionPairLive(const Layer& lower, const Layer& upper);

// Is `layer` placed on the document's own integer pixel grid -- identity or a whole-pixel
// translation, no rotation, scale or shear? The compositor then resamples it with the lossless
// Nearest kernel, so a partition's two halves land on the same pixels they were split across and
// the reconstruction can read one half's alpha straight off the other. Under any other placement
// the halves are filtered onto grids that disagree by up to half a pixel.
[[nodiscard]] bool integerGridPlacement(const Layer& layer);

// The upper half of a live partition, and how much the stack between it and the common ancestor
// attenuates it. `scale` folds the fragment's own opacity together with every enclosing
// pass-through group's, so the compositor can compute the fragment's EFFECTIVE alpha -- the
// quantity the rewrite actually needs.
struct LivePartition {
    const Layer* upper = nullptr;
    float scale = 1.0f;
    [[nodiscard]] explicit operator bool() const noexcept { return upper != nullptr; }
};

// Does `upper` composite ABOVE `lower`, with the two still relatable? Every group between each half
// and their common ancestor must be pass-through -- visible, Normal, unmasked, un-effected,
// unclipped -- because such a group composites its buffer with plain `over` and `over` COMPOSES,
// which is what lets the fragment be dragged into a group, or several layers up the stack, and
// still reconstruct. A pass-through group's opacity is allowed and accumulates into `scale` (for
// `over` with Normal, a group's opacity is exactly a scaling of the alpha it contains); on
// `lower`'s side it must be 1, since that is the half being rewritten.
//
// Layers BETWEEN the two halves are deliberately not restricted: they occlude the reconstructed
// surface exactly as they would have occluded the uncut original, which is the whole point.
[[nodiscard]] LivePartition partitionReachable(const Layer& lower, const Layer& upper);

// The live upper half of `layer`'s coverage partition, or an empty LivePartition when `layer` is
// not the lower half of one (including when it is the UPPER half -- the reconstruction is carried
// entirely by the lower one). Resolves the partner id against the tree `layer` sits in, then
// applies partitionPairLive + partitionReachable. The single question the compositor asks.
[[nodiscard]] LivePartition livePartitionFor(const Layer& layer);

// Stamp a fresh partition across two halves of a just-split surface, cross-referencing each other
// under a new token and snapshotting their revisions + relative placement. Both layers must
// already be in the tree (world transforms and ids must be final). A singular placement is
// silently declined -- it composites to nothing, so it is never a partition.
void linkCoveragePartition(Layer& a, Layer& b);

// ---------------------------------------------------------------------------------------------
// Magic (linked/smart source -- keeps the original full-res pixels; resampling in S50)
// ---------------------------------------------------------------------------------------------
class MagicLayer : public Layer {
public:
    MagicLayer(LayerId id, std::string name, common::Image source)
        : Layer(id, LayerKind::Magic, std::move(name)), m_source(std::move(source)) {}

    // The original source pixels are preserved (never edited destructively); the layer's
    // transform maps source -> document and the resampler reads from here (S50). As with
    // RasterLayer::image(), mutating after the first query requires invalidateContentBounds().
    [[nodiscard]] const common::Image& source() const noexcept { return m_source; }
    [[nodiscard]] common::Image& source() noexcept { return m_source; }

    // The cached tight alpha bbox of source() (lazy; recomputed after invalidation).
    [[nodiscard]] std::optional<common::Rect> contentBounds() const override;
    void invalidateContentBounds() noexcept {
        m_boundsValid = false;
        ++m_contentRevision;
    }
    [[nodiscard]] std::uint64_t contentRevision() const noexcept override {
        return m_contentRevision;
    }

private:
    common::Image m_source;
    mutable std::optional<common::Rect> m_contentBounds;
    mutable bool m_boundsValid = false;
    std::uint64_t m_contentRevision = 0;
};

// ---------------------------------------------------------------------------------------------
// Texture (S55 Texture Generator; docs/texture-generator.md §3) -- a live-regenerating
// procedural layer. The MODEL is the texture::TextureParams value (generator + seed + scale +
// the per-generator spec); pixels are a CACHE the app's texture render pass
// (texture::refreshTextureCaches) repopulates whenever the params change, exactly the TextLayer
// pattern: the compositor treats the cache as a raster source and never sees a generator.
// The cache is typed per generator (§4.4): Sky renders into a FLOAT ImageF (banding-free
// gradients, HDR export later -- the reflectionEnv precedent), Paper/Grass into 8-bit; exactly
// one of the two is populated. Params edits go through SetTextureCommand (coalescing undo);
// Rasterize bakes the cache into a RasterLayer and drops the params.
// ---------------------------------------------------------------------------------------------
class TextureLayer : public Layer {
public:
    TextureLayer(LayerId id, std::string name, texture::TextureParams params)
        : Layer(id, LayerKind::Texture, std::move(name)), m_params(std::move(params)) {}

    [[nodiscard]] const texture::TextureParams& params() const noexcept { return m_params; }
    void setParams(texture::TextureParams p) {
        m_params = std::move(p);
        invalidateContentBounds();
    }

    // The regenerated pixel cache (one arm per generator lane; the other stays nullptr). The
    // image-pixel -> layer-local map places it, like TextLayer::cacheImageToLayer (identity for
    // the whole-canvas render the S55-a refresh pass produces).
    [[nodiscard]] const common::Image* cachedImage() const noexcept {
        return m_cacheImage ? &*m_cacheImage : nullptr;
    }
    [[nodiscard]] const common::ImageF* cachedImageF() const noexcept {
        return m_cacheImageF ? &*m_cacheImageF : nullptr;
    }
    [[nodiscard]] const common::Affine2D& cacheImageToLayer() const noexcept {
        return m_cacheImageToLayer;
    }
    void setCachedImage(std::optional<common::Image> img, std::optional<common::ImageF> imgF,
                        common::Affine2D imageToLayer) const {
        m_cacheImage = std::move(img);
        m_cacheImageF = std::move(imgF);
        m_cacheImageToLayer = imageToLayer;
        m_cacheRevision = m_contentRevision;
        ++m_cacheGeneration;
    }
    // True when the pixel cache already reflects the current params (no re-render needed).
    [[nodiscard]] bool cacheCurrent() const noexcept { return m_cacheRevision == m_contentRevision; }
    // TextLayer::cacheGeneration's twin, and it exists for the same reason (S60-a): the pixels are
    // replaced without contentRevision moving whenever refreshTextureCache re-renders for a reason
    // that is not a params edit -- a CANVAS RESIZE, which the pass detects by comparing the cache's
    // extent with the document's (cacheMatchesSize) and which leaves the params revision exactly
    // where it was. A device copy keyed on contentRevision() would keep the pre-resize texture.
    [[nodiscard]] std::uint64_t cacheGeneration() const noexcept { return m_cacheGeneration; }

    // The layer-local extent of the cached pixels (the render pass sets it as it caches; nullopt
    // until first rendered, like TextLayer's measured bounds).
    [[nodiscard]] std::optional<common::Rect> contentBounds() const override {
        return m_cachedBounds;
    }
    void setCachedContentBounds(std::optional<common::Rect> b) const noexcept {
        m_cachedBounds = b;
    }
    void invalidateContentBounds() noexcept {
        m_cachedBounds.reset();
        ++m_contentRevision;
    }
    [[nodiscard]] std::uint64_t contentRevision() const noexcept override {
        return m_contentRevision;
    }

private:
    texture::TextureParams m_params;
    mutable std::optional<common::Rect> m_cachedBounds;
    mutable std::optional<common::Image> m_cacheImage;    // paper/grass (8-bit lane)
    mutable std::optional<common::ImageF> m_cacheImageF;  // sky (float lane)
    mutable common::Affine2D m_cacheImageToLayer = common::Affine2D::identity();
    mutable std::uint64_t m_cacheRevision = static_cast<std::uint64_t>(-1);  // != revision => stale
    mutable std::uint64_t m_cacheGeneration = 0;  // ++ per setCachedImage (see cacheGeneration())
    std::uint64_t m_contentRevision = 0;
};

}  // namespace mosaic::core
