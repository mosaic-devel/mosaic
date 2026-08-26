#include "ui/layer_panel.hpp"

#include "common/i18n.hpp"
#include "common/log.hpp"
#include "core/command.hpp"
#include "core/commands.hpp"
#include "core/document.hpp"
#include "common/profiler.hpp"
#include "core/vector/raster.hpp"  // rasterizeObjectF: vector-layer thumbnails (S26)
#include "render/compositor.hpp" // compositeGroup: group thumbnails + group pixel selection
#include "ui/channels_panel.hpp" // the dock's Channels tab (per-channel histogram)
#include "ui/history_panel.hpp"  // the dock's History tab (S16-b)
#include "ui/icons.hpp"          // drawIcon / IconButton (the panel-chrome icon set, S16-g)
#include "ui/theme.hpp"

#include <FL/Enumerations.H>
#include <FL/Fl.H>
#include <FL/Fl_Image_Surface.H> // the drag ghost's offscreen RGBA composite (S16-g)
#include <FL/Fl_RGB_Image.H>
#include <FL/Fl_Scroll.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant> // holds_alternative: which vec::Geometry a vector layer carries (the path badge)

namespace mosaic::ui {
namespace {

constexpr int kHeaderH = 30;  // the Layers | History tab header strip (S16-b)
constexpr int kTabGap = 18;   // horizontal gap between header tab labels
constexpr int kPropsH = 64;   // the per-layer blend + opacity strip, below the header
constexpr int kToolbarH = 40; // the add/group/delete button strip at the bottom
constexpr int kRowH = 44;
constexpr int kThumb = 34;        // thumbnail square, in px
constexpr int kMaskGap = 20;      // layer-thumb -> mask-thumb gap; the link chain lives here (S31)
constexpr int kEyeW = 26;         // width of the visibility-toggle cell at the row's left
constexpr int kLockW = 22;        // width of the lock cell, just right of the eye
constexpr int kIndent = 14;       // horizontal indent per tree-depth level
constexpr int kTriW = 14;         // the group disclosure-triangle cell
constexpr int kNameBadgeGap = 8;  // name -> first badge
constexpr int kBadgeGap = 6;      // badge -> badge
constexpr int kTexBadgeW = 20;    // the texture badge box (the fx badge's clickable-chip sibling)
constexpr int kTexBadgeH = 18;    // matches the fx badge's box height
constexpr int kCaptionW = 54;     // left caption column in the properties strip
constexpr int kReadoutW = 42;     // the opacity "NN%" readout on the strip's right
constexpr int kDragThreshold = 5; // px of movement before a press becomes a reorder drag
// The chrome icons are a fixed 16px, blitted 1:1 (ui::kIconPx) -- do not scale them into a cell.
constexpr int kBtnW = 32;         // bottom-strip button cell (square-ish, icon-only)
constexpr int kBtnH = 26;

// The floating drag chip: its top-left rides a hair down-right of the cursor, so the arrow's own
// hotspot stays outside the card (the arrow body hangs down-right and used to bury the corner).
// Replaces the press-time grab-point anchoring, which made the chip's position depend on exactly
// where in the row you happened to grab -- it felt unpredictable (S16-g).
constexpr int kGhostCursorDX = 10;
constexpr int kGhostCursorDY = 6;
constexpr int kGhostAlpha = 212; // ~83%: the row beneath reads through, the chip still reads solid

spdlog::logger& uiLog() {
    static const auto logger = common::log::category("ui");
    return *logger;
}

Fl_Color toFl(common::Color8 c) {
    return fl_rgb_color(c.r, c.g, c.b);
}

// A small filled disclosure triangle: pointing down when expanded, right when collapsed.
void drawTriangle(int cx, int cy, bool expanded, const Palette& pal) {
    fl_color(toFl(pal.textMuted));
    fl_begin_polygon();
    if (expanded) {
        fl_vertex(cx - 4, cy - 3);
        fl_vertex(cx + 4, cy - 3);
        fl_vertex(cx, cy + 4);
    } else {
        fl_vertex(cx - 3, cy - 4);
        fl_vertex(cx + 4, cy);
        fl_vertex(cx - 3, cy + 4);
    }
    fl_end_polygon();
}

// Linear blend of two 8-bit channels (a*(1-t) + b*t, rounded), and of two colors.
std::uint8_t mix8(std::uint8_t a, std::uint8_t b, double t) {
    return static_cast<std::uint8_t>(std::lround(a * (1.0 - t) + b * t));
}
common::Color8 blend8(common::Color8 a, common::Color8 b, double t) {
    return {mix8(a.r, b.r, t), mix8(a.g, b.g, t), mix8(a.b, b.b, t), 255};
}
// Ground color of the floating drag-ghost chip: a control-toned card with a hint of accent so it
// reads as "lifted/selected".
common::Color8 ghostCardColor(const Palette& pal) { return blend8(pal.controlBg, pal.accent, 0.20); }

// The chip is blitted at kGhostAlpha over whatever sits beneath it, so a pixel of the card ends up
// this colour. The drop-line knob's anti-aliasing composites against it where the two overlap.
common::Color8 ghostOverColor(common::Color8 card, common::Color8 beneath) {
    return blend8(beneath, card, kGhostAlpha / 255.0);
}

void cbAdd(Fl_Widget*, void* p) { static_cast<LayerPanel*>(p)->addRasterLayer(); }
void cbGroup(Fl_Widget*, void* p) { static_cast<LayerPanel*>(p)->groupActive(); }
void cbDelete(Fl_Widget*, void* p) { static_cast<LayerPanel*>(p)->deleteActive(); }

// The inline rename field. Enter commits, Escape reverts, and losing focus commits (clicking away
// keeps what you typed -- the file-manager convention; discarding it would be a silent data loss).
// Hiding a focused widget makes FLTK fire FL_UNFOCUS, so commitRename() -> hide() re-enters here;
// LayerPanel::m_renameCommitting is the latch that makes the second pass a no-op.
class RenameInput : public TextInput {
public:
    explicit RenameInput(LayerPanel* panel) : TextInput(0, 0, 10, 10), m_panel(panel) {
        box(MOSAIC_INPUT_BOX);
        textsize(13);
        // NOTE: do NOT clear_visible_focus() here. Despite the name, that flag does not merely
        // suppress the dotted focus rectangle -- Fl_Widget::take_focus() bails out with 0 when it
        // is clear, so the field never receives the keyboard and you cannot type in it (verified
        // against FLTK 1.4). The app already calls Fl::visible_focus(0) globally, which is the
        // knob that actually suppresses focus rectangles.
    }

protected:
    int handle(int event) override {
        if (event == FL_KEYBOARD) {
            const int key = Fl::event_key();
            if (key == FL_Escape) {
                m_panel->cancelRename();
                return 1;
            }
            if (key == FL_Enter || key == FL_KP_Enter) {
                m_panel->commitRename();
                return 1;
            }
        }
        if (event == FL_UNFOCUS) {
            const int handled = TextInput::handle(event); // let Fl_Input drop its selection first
            m_panel->commitRename();
            return handled;
        }
        return TextInput::handle(event);
    }

private:
    LayerPanel* m_panel;
};

// The first line of `utf8`, trimmed and truncated -- so a Text layer's name follows its content
// (Affinity-style) without a whole paragraph becoming the name (round-4 #5). Empty/blank -> "".
std::string firstLineName(const std::string& utf8) {
    const std::size_t nl = utf8.find('\n');
    std::string line = utf8.substr(0, nl == std::string::npos ? utf8.size() : nl);
    const auto isWs = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    std::size_t b = 0, e = line.size();
    while (b < e && isWs(line[b])) ++b;
    while (e > b && isWs(line[e - 1])) --e;
    line = line.substr(b, e - b);
    constexpr std::size_t kMax = 24;  // keep the name short; long lines get an ellipsis
    if (line.size() > kMax) {
        std::size_t cut = kMax;
        while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80)
            --cut;  // don't split a UTF-8 codepoint
        line = line.substr(0, cut) + "…";
    }
    return line;
}

// The name shown for a layer in the panel: a Text layer that has not been manually renamed follows
// its content; everything else uses the stored name.
std::string displayName(const core::Layer& layer) {
    if (const auto* tl = layer.as<core::TextLayer>(); tl != nullptr && tl->autoNamed()) {
        std::string n = firstLineName(tl->block().utf8);
        if (!n.empty())
            return n;
    }
    return layer.name();
}

// The doc-space axis-aligned box of a rect carried through `t` (its four corners' extent).
common::Rect mapRectAabb(const common::Affine2D& t, const common::Rect& r) {
    const common::Vec2 c[4] = {t.apply({r.x, r.y}), t.apply({r.x + r.w, r.y}),
                               t.apply({r.x, r.y + r.h}), t.apply({r.x + r.w, r.y + r.h})};
    double x0 = c[0].x, x1 = c[0].x, y0 = c[0].y, y1 = c[0].y;
    for (const common::Vec2& p : c) {
        x0 = std::min(x0, p.x);
        x1 = std::max(x1, p.x);
        y0 = std::min(y0, p.y);
        y1 = std::max(y1, p.y);
    }
    return {x0, y0, x1 - x0, y1 - y0};
}

// The doc-space square a thumbnail frames, from the content box it is a portrait of (nullopt =>
// the document rect). Extracted so the arms that RENDER their source can size that render to the
// view instead of to the canvas -- they need the view before they draw, and the sampling loop
// re-derives the identical rect afterwards.
[[nodiscard]] common::Rect thumbView(const std::optional<common::Rect>& frame, int box,
                                     std::uint32_t docW, std::uint32_t docH) {
    common::Rect view = frame.value_or(
        common::Rect{0.0, 0.0, static_cast<double>(docW), static_cast<double>(docH)});
    // Never magnify past 4x: a one-pixel layer should read as a dot, not as a full-bleed slab.
    const double minExtent = box / 4.0;
    if (view.w < minExtent) {
        view.x -= (minExtent - view.w) * 0.5;
        view.w = minExtent;
    }
    if (view.h < minExtent) {
        view.y -= (minExtent - view.h) * 0.5;
        view.h = minExtent;
    }
    // Square it up (the box is square) and add a hair of padding so the content never bleeds off.
    const double side = std::max(view.w, view.h) * (1.0 + 2.0 / box);
    view.x -= (side - view.w) * 0.5;
    view.y -= (side - view.h) * 0.5;
    view.w = side;
    view.h = side;
    return view;
}

} // namespace

// ---- thumbnail (pure; unit-tested) ---------------------------------------------------------

common::Image layerThumbnail(const core::Layer& layer, int box, std::uint32_t docW,
                             std::uint32_t docH) {
    if (box < 1)
        box = 1;
    common::Image thumb(static_cast<std::uint32_t>(box), static_cast<std::uint32_t>(box));
    thumb.fill(common::Color8{60, 64, 82, 255}); // neutral ground for a layer with no content

    const common::Image* src = nullptr;
    common::Image groupFlat;          // a group's composited subtree (doc-space, transform applied)
    common::Image vectorFlat;         // a vector layer rasterized into doc space (S26)
    common::Image textureFlat;        // a texture layer's float sky cache, 8-bit for display (S55)
    common::Image adjustFlat;         // an adjustment's affected-scope preview (S32)
    const common::Affine2D world = core::worldTransform(layer); // ancestors compose (nested groups)
    common::Affine2D t = world; // src-pixel -> doc
    // The doc-space box the thumbnail will FRAME. Empty/absent => the layer has nothing to show.
    std::optional<common::Rect> frame;

    if (const auto* raster = layer.as<core::RasterLayer>()) {
        src = &raster->image();
        if (const auto cb = raster->contentBounds(); cb && !cb->empty())
            frame = mapRectAabb(world, *cb); // the tight alpha bbox, not the (canvas-sized) image
    } else if (const auto* magic = layer.as<core::MagicLayer>()) {
        src = &magic->source();
        if (const auto cb = magic->contentBounds(); cb && !cb->empty())
            frame = mapRectAabb(world, *cb);
    } else if (const auto* group = layer.as<core::GroupLayer>();
               group != nullptr && docW > 0 && docH > 0) {
        // Frame FIRST, then render the framed square at thumbnail resolution. This used to
        // composite the whole subtree at docW x docH -- 39.8 MP to draw 34x34 on a 5k canvas --
        // and then POINT-sample it, so the old path was both ~1000x the work and worse filtered
        // (see compositeGroupInto). contentBounds is cached geometry, so framing costs nothing.
        if (const auto cb = group->contentBounds(); cb && !cb->empty())
            frame = mapRectAabb(world, *cb); // group-local children bbox -> doc
        const common::Rect v = thumbView(frame, box, docW, docH);
        const auto side = static_cast<std::uint32_t>(box);
        // doc -> the box x box buffer. One buffer texel per thumbnail texel, so the sampling loop
        // below reads it 1:1 and the anti-aliasing is the compositor's own reduction filter.
        const common::Affine2D docToBuf =
            common::Affine2D::scaling(side / v.w, side / v.h) *
            common::Affine2D::translation(-v.x, -v.y);
        groupFlat = render::compositeGroupInto(*group, docToBuf, side, side);
        src = &groupFlat;
        if (const auto invBuf = docToBuf.inverse())
            t = *invBuf; // buffer px -> doc
        else
            src = nullptr;
    } else if (const auto* vlayer = layer.as<core::VectorLayer>();
               vlayer != nullptr && vlayer->hasObject() && docW > 0 && docH > 0) {
        // Rasterize fill+stroke into doc space through the layer's world transform (bbox-bounded,
        // so it only touches the shape's pixels), then sample it like the group flat.
        // Same story as the group arm: rasterise the FRAMED square at thumbnail resolution
        // rather than allocating a docW x docH float buffer (637 MB on this canvas) to fill a
        // few hundred pixels of it. The rasteriser's own analytic AA does the filtering.
        if (const auto cb = vlayer->contentBounds(); cb && !cb->empty())
            frame = mapRectAabb(world, *cb);
        const common::Rect v = thumbView(frame, box, docW, docH);
        const auto side = static_cast<std::uint32_t>(box);
        const common::Affine2D docToBuf =
            common::Affine2D::scaling(side / v.w, side / v.h) *
            common::Affine2D::translation(-v.x, -v.y);
        vectorFlat = common::toImage8(
            core::vec::rasterizeObjectF(*vlayer->object(), side, side, docToBuf * t));
        src = &vectorFlat;
        if (const auto invBuf = docToBuf.inverse())
            t = *invBuf;
        else
            src = nullptr;
    } else if (const auto* tlayer = layer.as<core::TextLayer>();
               tlayer != nullptr && tlayer->cachedImage() != nullptr) {
        // Text: sample the renderer-populated base-res pixel cache (the compositor stays font-free).
        // cacheImageToLayer maps image-px -> layer-local, so world*cacheImageToLayer maps image -> doc
        // and is the transform the loop below inverts to sample (S29-b; fixlist #10).
        src = tlayer->cachedImage();
        t = world * tlayer->cacheImageToLayer();
        if (!src->empty()) {
            // The CACHE's extent, not contentBounds(): a 3D-extruded block deliberately keeps its
            // contentBounds at the flat text frame, so framing on that would slice the solid's
            // depth off the thumbnail. The cache is grown to the projected extrude bounds.
            frame = mapRectAabb(t, {0.0, 0.0, static_cast<double>(src->width),
                                    static_cast<double>(src->height)});
        }
    } else if (const auto* adj = layer.as<core::AdjustmentLayer>();
               adj != nullptr && docW > 0 && docH > 0) {
        // Adjustment (S32): preview the layers it AFFECTS with the effect applied — the same
        // truncated compositor walk a group thumbnail already runs over its children, so this
        // row agrees pixel-for-pixel with a group thumb containing the adjustment. Rendered at
        // a small doc-proportional resolution (the walk rasters every sibling at preview size,
        // so cost is bounded by the preview, not the canvas).
        const double scale = 96.0 / std::max(docW, docH);
        const auto pw = static_cast<std::uint32_t>(std::max(1.0, std::round(docW * scale)));
        const auto ph = static_cast<std::uint32_t>(std::max(1.0, std::round(docH * scale)));
        adjustFlat = render::adjustmentPreview(*adj, docW, docH, pw, ph);
        if (!adjustFlat.empty()) {
            src = &adjustFlat;
            t = common::Affine2D::scaling(static_cast<double>(docW) / pw,
                                          static_cast<double>(docH) / ph); // preview px -> doc
            // No `frame`: an adjustment grades the whole canvas window, so the thumbnail frames
            // the document rect (the fallback below) rather than hunting a content bbox.
        }
    } else if (const auto* xlayer = layer.as<core::TextureLayer>();
               xlayer != nullptr &&
               (xlayer->cachedImage() != nullptr || xlayer->cachedImageF() != nullptr)) {
        // Texture (S55): sample the generator cache like text. The sky lane's float cache is
        // converted to 8-bit here for DISPLAY only -- the compositor keeps the float pixels.
        if (xlayer->cachedImage() != nullptr) {
            src = xlayer->cachedImage();
        } else {
            textureFlat = common::toImage8(*xlayer->cachedImageF());
            src = &textureFlat;
        }
        t = world * xlayer->cacheImageToLayer();
        if (!src->empty())
            frame = mapRectAabb(t, {0.0, 0.0, static_cast<double>(src->width),
                                    static_cast<double>(src->height)});
    }
    if (src == nullptr || src->empty() || docW == 0 || docH == 0)
        return thumb; // empty vector / text / texture keep the flat placeholder

    // FRAME THE CONTENT, not the canvas (user, 2026-07-09). A layer's thumbnail is a portrait of
    // the object it holds: a small shape in the corner of a 5k canvas used to be an invisible
    // speck, because the box framed the whole document. Falling back to the document rect keeps a
    // contentless-but-pixel-bearing layer (an all-transparent raster) showing its checkerboard.
    const common::Rect view = thumbView(frame, box, docW, docH);

    // Sampling only inside `view` IS the culling: the loop never touches a source pixel outside the
    // framed content. (compositeGroup above still flattens the whole document -- an extent-aware
    // group buffer is S60-a's job, not this function's.)
    const std::optional<common::Affine2D> inv = t.inverse();
    for (int ty = 0; ty < box; ++ty) {
        for (int tx = 0; tx < box; ++tx) {
            double a = 0.0;
            std::size_t sp = 0;
            if (inv) { // a singular transform shows nothing (compositor semantics)
                const common::Vec2 doc{view.x + (tx + 0.5) * view.w / box,
                                       view.y + (ty + 0.5) * view.h / box};
                const common::Vec2 p = inv->apply(doc); // thumb -> doc -> source pixel
                const long sx = static_cast<long>(std::floor(p.x));
                const long sy = static_cast<long>(std::floor(p.y));
                if (sx >= 0 && sy >= 0 && sx < static_cast<long>(src->width) &&
                    sy < static_cast<long>(src->height)) {
                    sp = (static_cast<std::size_t>(sy) * src->width + sx) * 4;
                    a = src->rgba[sp + 3] / 255.0;
                }
            }
            // 6px checkerboard so transparency reads in the thumbnail.
            const bool darkSquare = (((tx / 6) + (ty / 6)) & 1) != 0;
            const std::uint8_t checker = darkSquare ? 150 : 205;
            const auto over = [&](int ch) {
                return static_cast<std::uint8_t>(
                    std::lround(src->rgba[sp + ch] * a + checker * (1.0 - a)));
            };
            const std::size_t dp = (static_cast<std::size_t>(ty) * box + tx) * 4;
            thumb.rgba[dp + 0] = over(0);
            thumb.rgba[dp + 1] = over(1);
            thumb.rgba[dp + 2] = over(2);
            thumb.rgba[dp + 3] = 255;
        }
    }
    return thumb;
}

common::Image maskThumbnail(const core::RasterMask& mask, int box) {
    if (box < 1)
        box = 1;
    common::Image thumb(static_cast<std::uint32_t>(box), static_cast<std::uint32_t>(box));
    thumb.fill(common::Color8{34, 36, 46, 255}); // letterbox ground, darker than any coverage grey
    if (mask.empty())
        return thumb;
    // Aspect-fit the mask grid into the box, nearest-sampled (the grid is usually canvas-sized,
    // so this is a plain reduction; no transform -- the thumb shows the SHEET, like Photoshop's).
    const double s = std::min(box / static_cast<double>(mask.width),
                              box / static_cast<double>(mask.height));
    const int tw = std::max(1, static_cast<int>(std::floor(mask.width * s)));
    const int th = std::max(1, static_cast<int>(std::floor(mask.height * s)));
    const int ox = (box - tw) / 2;
    const int oy = (box - th) / 2;
    for (int ty = 0; ty < th; ++ty) {
        const std::uint32_t my = static_cast<std::uint32_t>(
            std::min<long>(static_cast<long>(mask.height) - 1,
                           static_cast<long>(ty) * mask.height / th));
        for (int tx = 0; tx < tw; ++tx) {
            const std::uint32_t mx = static_cast<std::uint32_t>(
                std::min<long>(static_cast<long>(mask.width) - 1,
                               static_cast<long>(tx) * mask.width / tw));
            const std::uint8_t v = mask.coverage[static_cast<std::size_t>(my) * mask.width + mx];
            const std::size_t dp =
                (static_cast<std::size_t>(oy + ty) * box + (ox + tx)) * 4;
            thumb.rgba[dp + 0] = v;
            thumb.rgba[dp + 1] = v;
            thumb.rgba[dp + 2] = v;
            thumb.rgba[dp + 3] = 255;
        }
    }
    return thumb;
}

LayerPanel::RowClick rowClickFor(bool shift, bool command) {
    if (shift)
        return LayerPanel::RowClick::Extend; // Shift wins over Ctrl (see the header)
    if (command)
        return LayerPanel::RowClick::Toggle;
    return LayerPanel::RowClick::Replace;
}

core::SelectOp thumbnailSelectOp(bool ctrl, bool alt) {
    if (ctrl && alt)
        return core::SelectOp::Intersect;
    if (ctrl)
        return core::SelectOp::Add;
    if (alt)
        return core::SelectOp::Subtract;
    return core::SelectOp::Replace;
}

std::size_t moveIndexFor(std::size_t endIndex, bool sameParent, std::size_t oldIndex) {
    // Removing the dragged layer from its old slot shifts everything above it (higher index) down by
    // one -- but only if the destination is the same parent and the target sits past the old slot.
    if (sameParent && endIndex > oldIndex)
        return endIndex - 1;
    return endIndex;
}

common::Color8 layerRowBackground(const Palette& pal, bool enabled, bool active, bool selected,
                                  bool hover) {
    if (!enabled)
        return pal.panelBg; // greyed chrome: rest state only (no active/hover/selection fill)
    if (active)
        return pal.controlActive;
    if (hover)
        return pal.controlHover;
    if (selected)
        return pal.controlSelected;
    return pal.panelBg;
}

// ---- LayerRow ------------------------------------------------------------------------------

LayerRow::LayerRow(int X, int Y, int W, int H, LayerPanel* panel, core::LayerId id)
    : Fl_Widget(X, Y, W, H), m_panel(panel), m_id(id) {}

bool LayerRow::layerVisible() const {
    if (core::Document* doc = m_panel->document())
        if (const core::Layer* l = doc->find(m_id))
            return l->visible();
    return true;
}

bool LayerRow::layerLocked() const {
    if (core::Document* doc = m_panel->document())
        if (const core::Layer* l = doc->find(m_id))
            return l->locked();
    return false;
}

bool LayerRow::layerIsGroup() const {
    if (core::Document* doc = m_panel->document())
        if (const core::Layer* l = doc->find(m_id))
            return l->kind() == core::LayerKind::Group;
    return false;
}

bool LayerRow::layerExpanded() const {
    if (core::Document* doc = m_panel->document())
        if (const core::Layer* l = doc->find(m_id))
            if (const auto* g = l->as<core::GroupLayer>())
                return g->expanded();
    return false;
}

// The Shift-click gesture answers for EVERY layer kind now, not just Raster/Magic/Group, so the
// row's affordance asks the one oracle the panel owns rather than keeping a second (and, as the
// user found, permanently out-of-date) copy of the kind list here.
bool LayerRow::layerHasPixels() const { return m_panel->layerHasSelectablePixels(m_id); }

bool LayerRow::layerMaskEnabled() const {
    if (core::Document* doc = m_panel->document())
        if (const core::Layer* l = doc->find(m_id))
            if (const core::RasterMask* m = l->mask())
                return m->enabled;
    return true;
}

bool LayerRow::layerMaskLinked() const {
    if (core::Document* doc = m_panel->document())
        if (const core::Layer* l = doc->find(m_id))
            if (const core::RasterMask* m = l->mask())
                return m->linked;
    return true;
}

// Shift over the thumbnail of a pixel-bearing layer shows the link-style hand: the visual
// affordance for the Shift-click "select the layer's pixels" gesture (S13). Called from this
// row's enter/move events AND from modifiersChanged() below -- modifier keys go to the focus
// widget, never the hovered row, and a still pointer (trackball, pen lifted) may never
// produce the move event this used to wait for (user report, 2026-06-12). Ctrl/Alt on top of
// Shift choose the boolean op (S14-b) -- tracked here so the frame's +/-/x glyph stays live.
void LayerRow::updateCursor() {
    const int ex = Fl::event_x();
    const int cx = contentX();
    const auto state = Fl::event_state();
    // Over the fx / texture badge (both buttons): hand cursor + a brighter outline (drawn in draw()).
    const bool overFx = m_hasEffects && m_fxW > 0 && ex >= m_fxX && ex < m_fxX + m_fxW;
    const bool overTx = m_txW > 0 && ex >= m_txX && ex < m_txX + m_txW;
    const bool overEye = ex >= eyeCellX() && ex < eyeCellX() + kEyeW;
    const bool overLock = ex >= lockCellX() && ex < lockCellX() + kLockW;
    const bool handThumb = (state & FL_SHIFT) != 0 && ex >= cx + kTriW && ex < cx + kTriW + kThumb &&
                           layerHasPixels();
    // The mask thumbnail is a button whatever the modifiers (plain click targets it, Shift-click
    // selects its coverage); the chain gap between the thumbnails toggles linkage (S31).
    const bool overMask = m_hasMask && ex >= maskThumbX() && ex < maskThumbX() + kThumb;
    const bool overLink = m_hasMask && ex >= cx + kTriW + kThumb && ex < maskThumbX();
    const core::SelectOp op = thumbnailSelectOp((state & FL_CTRL) != 0, (state & FL_ALT) != 0);
    const bool cursorWasHand = m_handCursor || m_fxHover || m_txHover || m_eyeHover ||
                               m_lockHover || m_maskHover || m_linkHover;
    bool dirty = false;
    if (handThumb != m_handCursor) {
        m_handCursor = handThumb; // the thumbnail affordance (ants + op chip) rides ONLY on this
        dirty = true;
    }
    if (overFx != m_fxHover) {
        m_fxHover = overFx;
        dirty = true;
    }
    if (overTx != m_txHover) {
        m_txHover = overTx;
        dirty = true;
    }
    if (overEye != m_eyeHover) {
        m_eyeHover = overEye;
        dirty = true;
    }
    if (overLock != m_lockHover) {
        m_lockHover = overLock; // the lock cell is invisible at rest; hovering reveals + inks it
        dirty = true;
    }
    if (overMask != m_maskHover) {
        m_maskHover = overMask;
        dirty = true;
    }
    if (overLink != m_linkHover) {
        m_linkHover = overLink; // inks the chain accent (and reveals the broken chain when unlinked)
        dirty = true;
    }
    if (handThumb && op != m_hoverOp) {
        m_hoverOp = op;
        dirty = true;
    }
    const bool cursorIsHand =
        handThumb || overFx || overTx || overEye || overLock || overMask || overLink;
    if (cursorIsHand != cursorWasHand)
        fl_cursor(cursorIsHand ? FL_CURSOR_HAND : FL_CURSOR_DEFAULT);
    if (dirty)
        requestRedraw();
}

void LayerRow::requestRedraw() {
    if (m_panel->renamingRow() == m_id)
        m_panel->redraw(); // the editor floats over us; only a full panel pass repaints it on top
    else
        redraw();
}

// A modifier keydown/keyup landed somewhere (forwarded by the main window): re-evaluate the
// hover affordance if the pointer currently sits on this row. Fl::event_x/state are already
// updated for the key event, so updateCursor reads the live modifier set.
void LayerRow::modifiersChanged() {
    if (Fl::belowmouse() == this)
        updateCursor();
}

int LayerRow::eyeCellX() const { return x(); }
int LayerRow::lockCellX() const { return x() + kEyeW; }
int LayerRow::contentX() const { return x() + kEyeW + kLockW + m_depth * kIndent; }
int LayerRow::maskThumbX() const { return contentX() + kTriW + kThumb + kMaskGap; }
int LayerRow::nameX() const {
    // A masked row's name budget starts after the SECOND thumbnail (S31).
    return contentX() + kTriW + kThumb + (m_hasMask ? kMaskGap + kThumb : 0) + 10;
}

LayerRow::TypeBadge typeBadgeFor(const core::Layer& layer) {
    if (const auto* vl = layer.as<core::VectorLayer>(); vl != nullptr && vl->hasObject()) {
        // Inside the one vector kind, precedence is GEOMETRY first and PAINT second.
        //
        // A pen PATH keeps the path mark whatever it is filled with. The ramp chip exists because
        // "gradient layer" is a specific IDIOM -- a full-bleed parametric rect whose fill is a
        // vec::Gradient (docs/vector-model.md §1) -- not because a gradient fill is a layer type;
        // a hand-drawn path that happens to carry one is still a path. The fill is also the half
        // of the answer the row already shows: it is right there in the thumbnail two cells to the
        // left, where the geometry is not.
        //
        // Anything that is neither a Path nor a gradient-filled shape falls through to the generic
        // shapes mark -- including any Geometry alternative added after this was written (live
        // booleans; geometry.hpp), which read far better as "a shape" than as "a path". The test
        // is a holds_alternative<Path>, never a count of the variant's arms, exactly so.
        if (std::holds_alternative<core::vec::Path>(vl->object()->geometry))
            return LayerRow::TypeBadge::VectorPath;
        return core::vec::isGradient(vl->object()->fill) ? LayerRow::TypeBadge::Gradient
                                                         : LayerRow::TypeBadge::VectorShape;
    }
    if (const auto* tl = layer.as<core::TextLayer>(); tl != nullptr)
        return tl->block().frame == core::text::TextFrame::Area
                   ? LayerRow::TypeBadge::TextArea     // marquee mark
                   : LayerRow::TypeBadge::TextPoint;   // dot mark (S29-b)
    if (layer.as<core::MagicLayer>() != nullptr)
        return LayerRow::TypeBadge::Magic;             // folded page (S50)
    if (layer.as<core::TextureLayer>() != nullptr)
        return LayerRow::TypeBadge::Texture;           // material chip (S55-f)
    if (layer.as<core::AdjustmentLayer>() != nullptr)
        return LayerRow::TypeBadge::Adjustment;        // half circle (S32)
    return LayerRow::TypeBadge::None;
}

int typeBadgeWidth(LayerRow::TypeBadge badge, bool pastedMarker) {
    using TypeBadge = LayerRow::TypeBadge;
    if (pastedMarker)
        return 9; // two overlapping squares
    switch (badge) {
    case TypeBadge::VectorShape:
        return 11; // square + overlapping circle
    case TypeBadge::VectorPath:
        return 11; // a bezier segment between two 3px anchor squares (S28)
    case TypeBadge::Gradient:
        return 11; // the framed ramp chip (S22)
    case TypeBadge::TextPoint:
        return 8; // the bare serif T
    case TypeBadge::TextArea:
        return 14; // the T plus its wrapped-paragraph lines
    case TypeBadge::Magic:
        return 9; // the folded-corner page
    case TypeBadge::Texture:
        return kTexBadgeW; // the framed, clickable texture chip (the fx badge's sibling)
    case TypeBadge::Adjustment:
        return 10; // the half-filled circle (passive type mark, like Magic's folded page)
    case TypeBadge::None:
        break;
    }
    return 0;
}

int LayerRow::typeBadgeWidth() const {
    return ui::typeBadgeWidth(m_typeBadge, m_pastedMarker); // the pure oracle above
}

void LayerRow::draw() {
    const Palette& pal = activePalette();

    if (m_panel->isRowLifted(m_id)) {
        // This row's layer is being dragged: leave a muted, dashed "slot" where it sits while the
        // floating ghost (drawn by LayerPanel, on top) carries its thumbnail + name under the cursor.
        fl_color(toFl(pal.panelBg));
        fl_rectf(x(), y(), w(), h());
        fl_color(toFl(pal.border));
        fl_line(x(), y() + h() - 1, x() + w(), y() + h() - 1); // keep the row separator
        fl_line_style(FL_DASH, 1);
        fl_color(toFl(pal.textMuted));
        fl_rect(x() + 4, y() + 3, w() - 8, h() - 6);
        fl_line_style(0); // reset the (global) FLTK line style
        fl_color(toFl(pal.textMuted));
        fl_font(FL_HELVETICA_ITALIC, 13);
        const int nx = nameX();
        const int avail = x() + w() - 8 - m_panel->scrollGutter() - nx; // no badges/dot in this state
        fl_draw(ellipsizeToWidth(m_name, avail).c_str(), nx, y(), avail, h(),
                FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        return;
    }

    const bool active = m_panel->activeLayer() == m_id;
    const bool multi = m_panel->multiSelectActive();
    const bool inSel = m_panel->isInMoveSelection(m_id);
    const bool visible = layerVisible();
    const bool locked = layerLocked();
    const bool group = layerIsGroup();
    // active_r(): the panel is greyed while an inpaint run locks the chrome. A muted palette dims the
    // glyphs + text toward the ground; the active/hover fill is dropped (rest-state only -- the hover
    // thumbnail ants can't appear since no events reach a disabled row).
    const bool enabled = active_r();
    Palette mp = pal;
    if (!enabled) {
        mp.text = blend8(pal.text, pal.panelBg, 0.5);
        mp.textMuted = blend8(pal.textMuted, pal.panelBg, 0.4);
    }

    // One selection vocabulary (shared with the S15-e Settings cards): the active layer reads via its
    // fill; every layer in a multi-selection gets a dot on the right -- accent for the active (edited)
    // one, muted grey for the others. No accent left line / grey left bar any more (the dot is the
    // single selection indicator; the fill already marks the active row).
    //
    // The rest of the selection used to be pure panelBg, i.e. indistinguishable from an UNSELECTED
    // row unless you counted dots. It now carries pal.controlSelected -- a slight tint, half a
    // hover -- so the set reads as a group at a glance. NOT controlActive: a full slab under every
    // selected row is exactly the treatment the preset grid was stripped of for being busy. See
    // layerRowBackground() for the active > hover > selected precedence.
    const common::Color8 bgC = layerRowBackground(pal, enabled, active, multi && inSel, m_hover);
    fl_color(toFl(bgC));
    fl_rectf(x(), y(), w(), h());
    fl_color(toFl(pal.border));
    fl_line(x(), y() + h() - 1, x() + w(), y() + h() - 1); // row separator
    // Selection dot on the right: the active layer ALWAYS gets one (it is the active-layer
    // indicator, larger + accent, replacing the old left line); the other layers in a multi-selection
    // get a smaller muted dot. Kept clear of the scrollbar when it shows.
    if (active || (multi && inSel)) {
        // The gutter comes from the panel, not from scrollbar.visible(): that flag is only correct
        // after Fl_Scroll::draw() has recalculated it (see HistoryRow::setScrollGutter).
        const int rightInset = 14 + m_panel->scrollGutter();
        const int dcx = x() + w() - rightInset;
        const int dcy = y() + h() / 2;
        const double r = active ? 4.5 : 3.5;
        // Colour: the active row is always accent. In "All selected layers" mode EVERY selected row
        // is accent too (the edit lands on all); otherwise the other selected rows read muted grey.
        const common::Color8 dotCol =
            !enabled ? mp.textMuted
                     : ((active || m_panel->editsAllSelected()) ? pal.accent : pal.textMuted);
        drawAAPrims(dcx - 6, dcy - 6, 13, 13, [&](int, int) { return bgC; },
                    {{dcx + 0.0, dcy + 0.0, r, 0.0, dotCol}});
    }

    // Eye and lock both always draw (user, 2026-07-09: a cell that is empty until you hover it just
    // looks like a hole). An unlocked padlock rests muted, a locked one reads at full strength, and
    // hovering either cell inks it accent -- the affordance that says "a button, not a status light".
    const int cellCy = y() + h() / 2;
    const common::Color8 eyeInk =
        (enabled && m_eyeHover) ? pal.accent : (visible ? mp.text : mp.textMuted);
    drawIcon(visible ? Icon::EyeOpen : Icon::EyeClosed, eyeCellX() + kEyeW / 2, cellCy, eyeInk);
    const common::Color8 lockInk =
        (enabled && m_lockHover) ? pal.accent : (locked ? mp.text : mp.textMuted);
    drawIcon(locked ? Icon::LockClosed : Icon::LockOpen, lockCellX() + kLockW / 2, cellCy, lockInk);

    const int cx = contentX();
    if (group)
        drawTriangle(cx + kTriW / 2, y() + h() / 2, layerExpanded(), mp);

    const int tx = cx + kTriW;
    const int ty = y() + (h() - kThumb) / 2;
    // The image goes down first so the frame (and the op chip, which rides over the thumb's
    // corner) is never overdrawn by it.
    if (!m_thumb.empty()) {
        fl_draw_image(m_thumb.rgba.data(), tx, ty, static_cast<int>(m_thumb.width),
                      static_cast<int>(m_thumb.height), 4, 0);
    }
    // Thumbnail frame: a mild outline at rest (pal.border alone is invisible against the active
    // row, which is brighter than it). While Shift is over a pixel-bearing thumbnail it becomes
    // black/white "marching ants" -- the canvas ants' own two-tone -- a still preview of the
    // selection the click would make, pairing with the hand cursor. The dashed rect alone is
    // 1 px and kept sinking into busy panels (white dashes vanished on the light theme's white
    // ground): a DARK hairline just outside separates it -- on BOTH themes, per the user
    // (fifth pass; pal.text flipped white on dark and was rejected, as were accent rings).
    if (m_handCursor) {
        fl_color(FL_BLACK);
        fl_rect(tx - 2, ty - 2, kThumb + 4, kThumb + 4); // outer contrast hairline
        fl_rect(tx - 1, ty - 1, kThumb + 2, kThumb + 2);
        fl_line_style(FL_DASH, 1);
        fl_color(FL_WHITE);
        fl_rect(tx - 1, ty - 1, kThumb + 2, kThumb + 2);
        fl_line_style(0); // reset the (global) FLTK line style
        if (m_hoverOp != core::SelectOp::Replace) {
            // The boolean-op glyph (S14-b): what a click would do (+ add / - subtract /
            // x intersect), styled exactly like the selection cursor's badge -- a white core
            // over a 1-px black halo, NO background box (a filled chip read as a foreign
            // black square on both themes, sixth-pass user report; a box outline would just
            // add a second rectangle to a corner the dashed ants frame already owns). The
            // halo carries it on light thumbnails, the core on dark ones. Inset clear of the
            // dashed frame so glyph and ants never collide. TOP-LEFT corner: the pointer's
            // hotspot is its top-left, so the arrow body hangs down-right and permanently
            // covered a bottom-right glyph (user report, 2026-06-12).
            const int gx = tx + 3;
            const int gy = ty + 3;
            const int mx = gx + 3;
            const int my = gy + 3;
            const auto strokes = [&](int dx, int dy) {
                if (m_hoverOp == core::SelectOp::Add) {
                    fl_line(gx + dx, my + dy, gx + 6 + dx, my + dy);
                    fl_line(mx + dx, gy + dy, mx + dx, gy + 6 + dy);
                } else if (m_hoverOp == core::SelectOp::Subtract) {
                    fl_line(gx + dx, my + dy, gx + 6 + dx, my + dy);
                } else { // Intersect
                    fl_line(gx + dx, gy + dy, gx + 6 + dx, gy + 6 + dy);
                    fl_line(gx + 6 + dx, gy + dy, gx + dx, gy + 6 + dy);
                }
            };
            fl_color(FL_BLACK); // the halo: the glyph at every 8-neighbourhood offset
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (dx != 0 || dy != 0)
                        strokes(dx, dy);
            fl_color(FL_WHITE); // the core
            strokes(0, 0);
        }
    } else {
        fl_color(toFl(blend8(pal.border, pal.textMuted, 0.45)));
        fl_rect(tx - 1, ty - 1, kThumb + 2, kThumb + 2);
    }

    // Mask thumbnail + chain (S31): a second thumb right of the pixel one. The chain in the gap
    // reads LINKED at rest (muted, accent on hover); unlinked draws nothing at rest -- hovering
    // the gap reveals the rings pulled apart ("click to re-link"), keeping resting rows quiet. A
    // disabled mask wears a corner-to-corner X. All code-drawn one-ink marks, like the type
    // badges (docs/icons-needed.md owns the real icon set).
    if (m_hasMask) {
        const int mx = maskThumbX();
        if (!m_maskThumb.empty()) {
            fl_draw_image(m_maskThumb.rgba.data(), mx, ty, static_cast<int>(m_maskThumb.width),
                          static_cast<int>(m_maskThumb.height), 4, 0);
        }
        fl_color(toFl(blend8(pal.border, pal.textMuted, 0.45)));
        fl_rect(mx - 1, ty - 1, kThumb + 2, kThumb + 2);
        if (!layerMaskEnabled()) {
            fl_color(toFl(mp.text));
            fl_line(mx, ty, mx + kThumb - 1, ty + kThumb - 1);
            fl_line(mx + kThumb - 1, ty, mx, ty + kThumb - 1);
        }
        const bool linked = layerMaskLinked();
        if (linked || m_linkHover) {
            const int lcx = tx + kThumb + kMaskGap / 2;
            const int lcy = y() + h() / 2;
            const common::Color8 chainInk = (enabled && m_linkHover) ? pal.accent : mp.textMuted;
            // Sized to breathe inside the gap: the glyph stays a couple of px clear of both
            // thumbnail borders (it used to span the whole 14px gap and read squashed).
            // Both rings go into ONE coverage patch: they INTERLOCK, and a second opaque patch
            // would guillotine the first where they cross (see drawAAArcs in widgets.hpp). The
            // 20 px gap leaves the patch 2 px clear of either thumbnail frame, so the row fill is
            // the whole ground it needs.
            if (linked) { // two interlocked rings
                drawAAArcs(bgC, {aaArcFromBox(lcx - 7, lcy - 4, 8, 8, 0.0, 360.0, 1.0, chainInk),
                                 aaArcFromBox(lcx - 1, lcy - 4, 8, 8, 0.0, 360.0, 1.0, chainInk)});
            } else { // the same rings pulled apart
                drawAAArcs(bgC, {aaArcFromBox(lcx - 8, lcy - 4, 7, 8, 0.0, 360.0, 1.0, chainInk),
                                 aaArcFromBox(lcx + 1, lcy - 4, 7, 8, 0.0, 360.0, 1.0, chainInk)});
            }
        }
        // The edit-target ring: on the active row, which thumbnail edits land on. Drawn only on
        // masked rows (elsewhere there is nothing to disambiguate -- the row fill already marks
        // active), and it yields the pixel thumb to the Shift ants preview.
        if (active && enabled) {
            const bool maskAimed = m_panel->maskEditTarget();
            if (maskAimed || !m_handCursor) {
                fl_color(toFl(pal.accent));
                const int ax = maskAimed ? mx : tx;
                fl_rect(ax - 2, ty - 2, kThumb + 4, kThumb + 4);
            }
        }
    }

    // Name, badges and the active-layer dot share one right-hand budget. fl_draw() does NOT clip to
    // the box it is handed, so the name must be ellipsized to the room actually left for it -- and
    // the dock is width-resizable, so that room is recomputed every draw, never cached. Order:
    // reserve the dot, reserve the badges this row will show, and give the rest to the name.
    const bool showDot = active || (multi && inSel);
    // The gutter comes from the panel (see the dot above): Fl_Scroll only knows inside its own draw().
    const int rightEdge = x() + w() - 8 - m_panel->scrollGutter();
    const int badgeRight = rightEdge - (showDot ? 16 : 0);

    // fx badge (LE-b): the layer carries non-destructive effects -> a small "fx" chip. Clicking it
    // re-opens the Layer Effects modal for this layer (LayerRow::handle -> openEffectsFor). Drawn
    // FIRST so it hugs the name; a type badge follows it.
    fl_font(FL_HELVETICA_BOLD_ITALIC, 11);
    const int fxW = m_hasEffects ? static_cast<int>(fl_width("fx")) + 8 : 0;
    const int typeW = typeBadgeWidth();
    // Each badge that shows costs its width plus the gap that precedes it (wider after the name).
    int reserved = 0;
    if (fxW > 0)
        reserved += kNameBadgeGap + fxW;
    if (typeW > 0)
        reserved += (fxW > 0 ? kBadgeGap : kNameBadgeGap) + typeW;

    fl_color(toFl(visible ? mp.text : mp.textMuted));
    fl_font(group ? FL_HELVETICA_BOLD : FL_HELVETICA, 13); // groups read as bold
    const int nx = nameX();
    const std::string shown = ellipsizeToWidth(m_name, badgeRight - reserved - nx);
    fl_draw(shown.c_str(), nx, y(), badgeRight - reserved - nx, h(), FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    const int shownW = static_cast<int>(fl_width(shown.c_str()));
    // A few px of slop past the glyphs keeps a click at the very end of a short name from missing;
    // an empty name still leaves a target to double-click. Never past the first badge.
    m_nameRight = std::min(nx + std::max(24, shownW + 4), badgeRight - reserved);

    // The badges ride after the (possibly truncated) name, left of the dot.
    int badgeX = nx + shownW + kNameBadgeGap;

    m_fxX = 0;
    m_fxW = 0;
    m_txX = 0; // cleared every draw; the Texture badge branch below re-sets it when it shows
    m_txW = 0;
    if (m_hasEffects) {
        fl_font(FL_HELVETICA_BOLD_ITALIC, 11);
        const int fw = fxW;
        const int boxH = 18;
        const int gx = std::min(badgeX, badgeRight - fw);
        const int gy = y() + (h() - boxH) / 2;
        // The layer-preview hairline colour at rest; accent on hover, so it reads as a button.
        fl_color(toFl(m_fxHover ? pal.accent : blend8(pal.border, pal.textMuted, 0.45)));
        fl_rect(gx, gy, fw, boxH);
        // Plain white (dark theme) / black (light theme) so the "fx" reads at any accent.
        fl_color(enabled ? (pal.dark ? FL_WHITE : FL_BLACK) : toFl(pal.textMuted));
        // Draw in the top boxH-2 so the italic f's descender clears the box bottom (a little padding).
        fl_draw("fx", gx, gy, fw, boxH - 2, FL_ALIGN_CENTER);
        m_fxX = gx;
        m_fxW = fw;
        badgeX = gx + fw + kBadgeGap;
    }

    if (m_pastedMarker) {
        // Unorganized-paste badge: two overlapping squares (a clipboard-ish ⧉), code-drawn
        // placeholder until the real icon set (docs/icons-needed.md, S52). Rides just after
        // the name, clamped to the row; renaming the layer clears it (SetNameCommand).
        int gx = std::min(badgeX, badgeRight - 9);
        const int gy = y() + (h() - 9) / 2;
        fl_color(toFl(mp.textMuted));
        fl_rect(gx + 3, gy, 6, 6);     // back square
        fl_rectf(gx, gy + 3, 6, 6);    // front square, filled to read at 9 px
    } else if (m_typeBadge == TypeBadge::Magic) {
        // Magic-layer badge (S50): a page with a folded corner -- the universal "the original
        // document is still in here" mark, and distinct at 10 px from the paste marker's two
        // squares, the vector square+circle and the text T. A magic layer keeps its full-resolution
        // source and resamples from it, so the badge says "there is more here than the pixels you
        // see". Code-drawn like its siblings until the real icon set (docs/icons-needed.md, S52).
        const int bw = 9;
        int gx = std::min(badgeX, badgeRight - bw);
        const int gy = y() + (h() - 11) / 2; // an 11 px-tall page
        fl_color(toFl(mp.textMuted));
        fl_line(gx, gy, gx + 5, gy);              // top edge, stopping where the corner folds
        fl_line(gx, gy, gx, gy + 10);             // left edge
        fl_line(gx, gy + 10, gx + 8, gy + 10);    // bottom edge
        fl_line(gx + 8, gy + 3, gx + 8, gy + 10); // right edge, below the fold
        fl_polygon(gx + 5, gy, gx + 8, gy + 3, gx + 5, gy + 3); // the folded flap, filled
    } else if (m_typeBadge == TypeBadge::Texture) {
        // Texture-generator badge (S55): the fx badge's sibling -- a framed, clickable CHIP (same box
        // treatment as "fx") carrying a mini CHECKERBOARD -- the universal "material swatch" mark.
        // Clicking it (re)opens the Texture Generator modal for this layer (LayerRow::handle ->
        // openTextureFor), exactly as the fx badge opens Layer Effects. The frame inks accent on
        // hover so it reads as a button.
        const int bw = kTexBadgeW;
        const int boxH = kTexBadgeH;
        const int gx = std::min(badgeX, badgeRight - bw);
        const int gy = y() + (h() - boxH) / 2;
        // Layer-preview hairline at rest; accent on hover -- the same frame rule the fx badge follows.
        fl_color(toFl(m_txHover ? pal.accent : blend8(pal.border, pal.textMuted, 0.45)));
        fl_rect(gx, gy, bw, boxH);
        // The one-ink checkerboard: white (dark theme) / black (light), muted while the panel is
        // disabled -- the same ink rule the "fx" text obeys. A 4x3 grid of 4px cells filled on the
        // alternate squares, centred with >= 1px clear of the frame; whole-cell fl_rectf at integer
        // coords, so the pattern stays crisp (cells rescale together under any FLTK screen scale --
        // shared edges, no half-pixel seams).
        fl_color(enabled ? (pal.dark ? FL_WHITE : FL_BLACK) : toFl(pal.textMuted));
        const int cell = 4;
        const int ox = gx + (bw - 4 * cell) / 2;   // = gx + 2: the frame px + a 1px inset
        const int oy = gy + (boxH - 3 * cell) / 2; // = gy + 3: the frame px + a 2px inset
        for (int row = 0; row < 3; ++row)
            for (int col = row & 1; col < 4; col += 2) // fill where row+col is even
                fl_rectf(ox + col * cell, oy + row * cell, cell, cell);
        m_txX = gx;
        m_txW = bw;
        badgeX = gx + bw + kBadgeGap;
    } else if (m_typeBadge == TypeBadge::Adjustment) {
        // Adjustment-layer badge (S32): a passive TYPE mark (NOT clickable -- the chip language
        // means "opens an editor" and the S32 panel opens by the layer simply being active), but
        // in the texture chip's FULL-CONTRAST one-ink (white on dark / black on light) rather
        // than the muted type-mark grey: an adjustment is a bigger deal than a shape or text
        // mark (user 2026-07-17). A half-filled circle: one half graded, one half not.
        const int d = 10;
        int gx = std::min(badgeX, badgeRight - d);
        const int gy = y() + (h() - d) / 2;
        const common::Color8 ink =
            enabled ? (pal.dark ? common::Color8{255, 255, 255, 255} : common::Color8{0, 0, 0, 255})
                    : mp.textMuted;
        // Wedge and outline share one patch, in this order, so the outline still lands ON the fill.
        drawAAArcs(bgC, {aaPieFromBox(gx, gy, d, d, 90.0, 270.0, ink),  // left half filled
                         aaArcFromBox(gx, gy, d, d, 0.0, 360.0, 1.0, ink)}); // outline over it
    } else if (m_typeBadge == TypeBadge::VectorShape) {
        // Vector-shape badge: an overlapping square + circle (the generic "shapes" glyph) -- reads
        // for any parametric shape (rect/ellipse/polygon/star), not a single primitive. Rides after
        // the name like the paste marker, but persists (it's a type indicator, not a rename marker).
        int gx = std::min(badgeX, badgeRight - 11);
        const int gy = y() + (h() - 10) / 2;
        fl_color(toFl(mp.textMuted));
        fl_rect(gx, gy, 8, 8); // square (upper-left), outline
        // The circle lands ON the square, and an opaque coverage patch erases anything `under` does
        // not restate -- so the sampler puts the square's hairline back where the two cross.
        const auto squareUnder = [&](int ux, int uy) {
            const bool inSquare = ux >= gx && ux < gx + 8 && uy >= gy && uy < gy + 8;
            const bool onEdge = ux == gx || ux == gx + 7 || uy == gy || uy == gy + 7;
            return (inSquare && onEdge) ? mp.textMuted : bgC;
        };
        // circle (lower-right), overlapping outline
        drawAAArcs(squareUnder, {aaArcFromBox(gx + 3, gy + 2, 8, 8, 0.0, 360.0, 1.0, mp.textMuted)});
    } else if (m_typeBadge == TypeBadge::VectorPath) {
        // Vector-PATH badge (S28): one bezier segment threaded through two anchor squares -- the
        // pen tool's own on-canvas node chrome shrunk to badge size (a hollow anchor and a filled,
        // "selected" one). A pen path is a VectorLayer exactly as a rectangle or a star is, so
        // without its own mark it wore the generic square+circle and was indistinguishable from
        // them in the dock -- the same argument that won the gradient layer its ramp chip.
        //
        // It is the one motif that reads "editable path" at this size, and it cannot be confused
        // with anything else in the cascade: every other mark is a solid silhouette, and this one
        // is mostly empty space with two 3px dots and a hairline in it. Code-drawn like its
        // siblings until the real icon set (docs/icons-needed.md, S52).
        const int bw = 11, bh = 11;
        const int gx = std::min(badgeX, badgeRight - bw);
        const int gy = y() + (h() - bh) / 2;
        fl_color(toFl(mp.textMuted));
        // The curve goes down first, so the anchors sit ON it -- an anchor is a point OF the path,
        // not a decoration beside it. Its four control points are the two anchor centres and one
        // pull each, which is what bows it into the S that says "curve" rather than "line".
        fl_begin_line();
        fl_curve(gx + 1.0, gy + 8.0, gx + 2.0, gy + 2.0, gx + 8.0, gy + 8.0, gx + 9.0, gy + 2.0);
        fl_end_line();
        fl_rect(gx, gy + 7, 3, 3);      // start anchor (hollow), centred on the curve's first point
        fl_rectf(gx + 8, gy + 1, 3, 3); // end anchor (filled), on its last -- reaches gx+10
    } else if (m_typeBadge == TypeBadge::Gradient) {
        // Gradient-layer badge (S22): a framed RAMP CHIP -- a swatch whose ink fades to the row
        // ground left to right. A gradient layer IS a vector layer (docs/vector-model.md §1: a
        // full-bleed rect whose fill is a vec::Gradient), so without its own mark it wore the
        // generic square+circle "shapes" glyph and read as an ordinary shape. A ramp is the one
        // thing that says "colour ramp" at 11 px. It stays one-ink like every other type mark --
        // it just uses that ink at eight strengths instead of one -- and is code-drawn like its
        // siblings until the real icon set lands (docs/icons-needed.md, S52).
        const int bw = 11, bh = 10;
        const int gx = std::min(badgeX, badgeRight - bw);
        const int gy = y() + (h() - bh) / 2;
        for (int i = 0; i < bw - 2; ++i) { // interior columns: full ink left -> the row ground right
            const double t = static_cast<double>(i) / static_cast<double>(bw - 3);
            fl_color(toFl(blend8(mp.textMuted, bgC, t)));
            fl_line(gx + 1 + i, gy + 1, gx + 1 + i, gy + bh - 2);
        }
        fl_color(toFl(mp.textMuted)); // the frame, so the faded end still has an edge to read on
        fl_rect(gx, gy, bw, bh);
    } else if (m_typeBadge == TypeBadge::TextPoint || m_typeBadge == TypeBadge::TextArea) {
        // Text-layer badge: a serif "T" drawn from the real Noto Serif cap-T's proportions -- a wide
        // crossbar with downward serif DROPS at both ends, a THIN stem, and a bracketed FOOT (the
        // earlier slab T missed all three). The frame mode is encoded minimally (user 2026-06-29):
        // Point text = the bare T; Area text = the T + stacked paragraph LINES ("wrapped lines").
        const bool area = m_typeBadge == TypeBadge::TextArea;
        const int bw = area ? 14 : 8;                // total badge width (T, plus Area's lines)
        int gx = std::min(badgeX, badgeRight - bw);
        const int gy = y() + (h() - 10) / 2;         // a 10 px-tall cell
        fl_color(toFl(mp.textMuted));
        fl_rectf(gx, gy, 8, 2);                       // crossbar (full width)
        fl_rectf(gx, gy + 2, 1, 2);                   // left end serif drop (the crossbar end hangs down)
        fl_rectf(gx + 7, gy + 2, 1, 2);               // right end serif drop
        fl_rectf(gx + 3, gy, 2, 10);                  // thin stem, centred under the crossbar
        fl_rectf(gx + 2, gy + 8, 4, 2);               // bracketed foot (wider than the stem)
        if (area) {                                   // wrapped paragraph lines past the T (a block = Area)
            fl_rectf(gx + 10, gy + 2, 4, 1);
            fl_rectf(gx + 10, gy + 5, 4, 1);
            fl_rectf(gx + 10, gy + 8, 3, 1);          // last line short (a ragged paragraph end)
        }
    }
}

int LayerRow::handle(int event) {
    switch (event) {
    case FL_ENTER:
        m_hover = true;
        updateCursor();
        requestRedraw();
        return 1;
    case FL_MOVE:
        updateCursor();
        return 1;
    case FL_LEAVE:
        m_hover = false;
        if (m_handCursor || m_fxHover || m_txHover || m_eyeHover || m_lockHover || m_maskHover ||
            m_linkHover) {
            m_handCursor = false;
            m_fxHover = false;
            m_txHover = false;
            m_eyeHover = false;
            m_lockHover = false;
            m_maskHover = false;
            m_linkHover = false;
            fl_cursor(FL_CURSOR_DEFAULT);
        }
        requestRedraw();
        return 1;
    case FL_PUSH: {
        const int ex = Fl::event_x();
        const int cx = contentX();
        if (Fl::event_button() == FL_RIGHT_MOUSE) {
            m_panel->showRowMenu(m_id); // selects the row, then opens the themed menu at the cursor
            return 1;
        }
        if (Fl::event_button() != FL_LEFT_MOUSE)
            return 1; // swallow the middle button rather than let it start a drag
        if (m_hasEffects && m_fxW > 0 && ex >= m_fxX && ex < m_fxX + m_fxW) {
            // The click opens the modal, so no motion event follows to clear the hand cursor -- reset
            // it now (otherwise it stays stuck until the pointer moves over another widget).
            m_fxHover = false;
            m_handCursor = false;
            fl_cursor(FL_CURSOR_DEFAULT);
            m_panel->openEffectsFor(m_id); // clicked the fx badge -> reopen the effects modal
        }
        else if (m_txW > 0 && ex >= m_txX && ex < m_txX + m_txW) {
            // Same as the fx badge: the click opens the modal, so reset the hand cursor now.
            m_txHover = false;
            m_handCursor = false;
            fl_cursor(FL_CURSOR_DEFAULT);
            m_panel->openTextureFor(m_id); // clicked the texture badge -> reopen the generator modal
        }
        else if (ex >= eyeCellX() && ex < eyeCellX() + kEyeW)
            m_panel->toggleVisible(m_id); // clicked the eye cell
        else if (ex >= lockCellX() && ex < lockCellX() + kLockW)
            m_panel->toggleLocked(m_id); // clicked the lock cell (hidden at rest; hover reveals it)
        else if (layerIsGroup() && ex >= cx && ex < cx + kTriW)
            m_panel->toggleExpanded(m_id); // clicked the disclosure triangle
        else if ((Fl::event_state() & FL_SHIFT) != 0 && ex >= cx + kTriW && ex < cx + kTriW + kThumb)
            m_panel->shiftClickThumbnail(m_id); // shift-click the thumbnail -> select pixels (S13)
        else if (m_hasMask && ex >= cx + kTriW + kThumb && ex < maskThumbX())
            m_panel->toggleMaskLinked(m_id); // clicked the chain gap -> link/unlink (S31)
        else if (m_hasMask && (Fl::event_state() & FL_SHIFT) != 0 && ex >= maskThumbX() &&
                 ex < maskThumbX() + kThumb)
            m_panel->shiftClickMaskThumbnail(m_id); // -> select the mask's coverage (S31)
        else if (m_hasMask && ex >= maskThumbX() && ex < maskThumbX() + kThumb) {
            m_panel->targetMask(m_id); // aim edits at the mask...
            m_panel->rowPressed(m_id); // ...and still select + arm a possible drag
        }
        else if (m_hasMask && ex >= cx + kTriW && ex < cx + kTriW + kThumb) {
            m_panel->targetPixels(m_id); // plain click the pixel thumb re-aims edits at pixels
            m_panel->rowPressed(m_id);
        }
        // Double-click the NAME (not the badges) -> inline rename. A MODIFIER held over the name is
        // the multi-selection grammar instead (Ctrl toggles, Shift extends): renaming under a held
        // modifier would open an editor with the name selected, so the next keystroke replaces it --
        // a silent, unasked-for edit in the middle of a selection sweep.
        else if (Fl::event_clicks() > 0 && (Fl::event_state() & (FL_SHIFT | FL_COMMAND)) == 0 &&
                 ex >= nameX() && ex < m_nameRight)
            m_panel->beginRename(m_id);
        else
            m_panel->rowPressed(m_id); // select + arm a possible drag
        return 1;
    }
    case FL_DRAG:
        if (Fl::event_button1() != 0)
            m_panel->rowDragged();
        return 1;
    case FL_RELEASE:
        if (Fl::event_button() == FL_LEFT_MOUSE)
            m_panel->rowReleased();
        return 1;
    default:
        return Fl_Widget::handle(event);
    }
}

// ---- LayerPanel ----------------------------------------------------------------------------

namespace {
void cbBlend(Fl_Widget*, void* p);
void cbOpacity(Fl_Widget*, void* p);
} // namespace

LayerPanel::LayerPanel(int X, int Y, int W, int H) : Panel(X, Y, W, H) {
    borderEdges(EdgeLeft); // the dock owns only the canvas|dock junction (see Panel)
    const Palette& pal = activePalette();
    begin();

    // Per-layer properties strip (blend mode + opacity) for the active layer, below the header.
    m_blendChoice = new Dropdown(0, 0, 10, 22);
    addBlendModeItems(*m_blendChoice); // grouped by family with dividers (shared with the Fill dialog)
    m_blendChoice->callback(cbBlend, this);
    m_blendChoice->tooltip(_("Blend mode of the active layer"));

    m_opacitySlider = new Slider(0, 0, 10, 20);
    m_opacitySlider->range(0.0, 1.0);
    m_opacitySlider->step(0.01);
    m_opacitySlider->value(1.0);
    m_opacitySlider->when(FL_WHEN_CHANGED | FL_WHEN_RELEASE_ALWAYS);
    m_opacitySlider->callback(cbOpacity, this);
    m_opacitySlider->tooltip(_("Opacity of the active layer"));

    m_scroll = new ScrollView(0, 0, 10, 10);
    m_scroll->type(Fl_Scroll::VERTICAL);
    m_scroll->box(FL_NO_BOX);
    m_scroll->color(toFl(pal.panelBg));
    m_scroll->end();

    // Bottom strip: the two constructive actions together on the left, the destructive one alone at
    // the right edge. Icon-only (ui::IconButton) -- three word-labelled buttons would not fit a
    // narrow dock, and the icons are the same vocabulary every editor in this class uses.
    m_addButton = new IconButton(0, 0, kBtnW, kBtnH, Icon::Plus);
    m_addButton->callback(cbAdd, this);
    m_addButton->tooltip(_("Add a new raster layer"));
    m_groupButton = new IconButton(0, 0, kBtnW, kBtnH, Icon::GroupLayers);
    m_groupButton->callback(cbGroup, this);
    m_groupButton->tooltip(_("Group the active layer"));
    m_deleteButton = new IconButton(0, 0, kBtnW, kBtnH, Icon::Trash);
    m_deleteButton->callback(cbDelete, this);
    m_deleteButton->tooltip(_("Delete the active layer"));

    // The History tab's body (S16-b): the full dock area below the shared header, hidden
    // until its tab is picked.
    m_history = new HistoryPanel(0, 0, 10, 10);
    m_history->hide();

    // The Channels tab's body: the per-channel histogram, hidden until its tab is picked.
    m_channels = new ChannelsPanel(0, 0, 10, 10);
    m_channels->hide();

    // LAST child on purpose: Fl_Group draws children in order (so the editor paints over the rows)
    // and dispatches events in reverse (so it gets first refusal on a click).
    m_renameEditor = new RenameInput(this);
    m_renameEditor->hide();

    end();
    resizable(nullptr); // layoutChildren() places every child; see resize()
    layoutChildren();
    syncProperties();
}

void LayerPanel::resize(int X, int Y, int W, int H) {
    cancelRename(); // the editor is positioned in absolute coords over a row; both are about to move
    Fl_Widget::resize(X, Y, W, H); // NOT Fl_Group::resize -- we place the children ourselves
    layoutChildren();
}

void LayerPanel::layoutChildren() {
    const int X = x();
    const int Y = y();
    const int W = w();
    const int H = h();

    const int ctlX = X + 10 + kCaptionW;
    m_blendChoice->resize(ctlX, Y + kHeaderH + 8, std::max(20, W - 10 - kCaptionW - 10), 22);
    m_opacitySlider->resize(ctlX, Y + kHeaderH + 8 + 28,
                            std::max(20, W - 10 - kCaptionW - kReadoutW - 10), 20);

    const int listTop = Y + kHeaderH + kPropsH;
    const int toolbarY = Y + H - kToolbarH;
    // The list starts past the splitter's grab strip: a row's eye cell must never share pixels with
    // the resize band. (Everything else on the panel already clears it -- the strip is 5px, the
    // captions and buttons inset by 10.)
    const int listX = X + splitterWidth();
    m_scroll->resize(listX, listTop, std::max(2, X + W - 1 - listX),
                     std::max(1, toolbarY - listTop));
    // Rows span the scroll's width; their y positions are the scroll's business, not ours.
    for (LayerRow* row : m_rows)
        row->resize(m_scroll->x(), row->y(), m_scroll->w(), kRowH);

    const int btnY = toolbarY + (kToolbarH - kBtnH) / 2;
    m_addButton->resize(X + 10, btnY, kBtnW, kBtnH);
    m_groupButton->resize(X + 10 + kBtnW + 6, btnY, kBtnW, kBtnH);
    m_deleteButton->resize(X + W - 10 - kBtnW, btnY, kBtnW, kBtnH);

    m_history->resize(X + 1, Y + kHeaderH, std::max(2, W - 2), std::max(1, H - kHeaderH));
    m_channels->resize(X + 1, Y + kHeaderH, std::max(2, W - 2), std::max(1, H - kHeaderH));
    updateScrollGutter(); // the viewport just changed height: so may the scrollbar's presence
}

void LayerPanel::updateScrollGutter() {
    if (m_scroll == nullptr)
        return;
    const int gutter = m_scroll->scrollbarGutter(static_cast<int>(m_rows.size()) * kRowH);
    if (gutter == m_scrollGutter)
        return;
    m_scrollGutter = gutter;
    redrawList();
}

// See the header: a lone m_scroll->redraw() paints the rows straight over the floating rename
// editor, which FLTK never damages because nothing told it to.
void LayerPanel::redrawList() {
    if (renaming())
        redraw(); // full panel: children draw in order, the editor is the last of them
    else if (m_scroll != nullptr)
        m_scroll->redraw();
}

void LayerPanel::setTab(DockTab tab) {
    if (tab == m_tab)
        return;
    m_tab = tab;
    applyTabVisibility();
    redraw();
}

void LayerPanel::applyTabVisibility() {
    const bool layers = m_tab == DockTab::Layers;
    cancelRename(); // an editor floating over a hidden row would strand itself on another tab
    for (Fl_Widget* widget : {static_cast<Fl_Widget*>(m_blendChoice),
                              static_cast<Fl_Widget*>(m_opacitySlider),
                              static_cast<Fl_Widget*>(m_scroll),
                              static_cast<Fl_Widget*>(m_addButton),
                              static_cast<Fl_Widget*>(m_groupButton),
                              static_cast<Fl_Widget*>(m_deleteButton)}) {
        if (widget == nullptr)
            continue;
        if (layers)
            widget->show();
        else
            widget->hide();
    }
    if (m_history != nullptr) {
        if (m_tab == DockTab::History) {
            m_history->show();
            m_history->onTabShown(); // settle the scroll layout + re-tick the ages before first paint
            m_history->take_focus(); // S16-g: arrows step history at once, no click-to-focus first
        } else {
            m_history->hide();
        }
    }
    if (m_channels != nullptr) {
        if (m_tab == DockTab::Channels) {
            m_channels->show();
            m_channels->onTabShown(); // pull the current composite + re-bin before the first paint
        } else {
            m_channels->hide();
        }
    }
}

std::pair<int, int> LayerPanel::tabSpan(int index) const {
    fl_font(FL_HELVETICA_BOLD, 13);
    int tx = x() + 12;
    const char* labels[3] = {_("Layers"), _("History"), _("Channels")};
    for (int i = 0; i < 3; ++i) {
        const int tw = static_cast<int>(std::ceil(fl_width(labels[i])));
        if (i == index)
            return {tx, tx + tw};
        tx += tw + kTabGap;
    }
    return {0, -1};
}

int LayerPanel::handle(int event) {
    // (The dock's WIDTH splitter is ui::RightDock's: the grab band runs down this panel's left edge,
    // but the dock is the container that owns it and claims the event before we ever see it. All the
    // panel still owes it is the list inset -- see layoutChildren.)

    // ---- landing a live rename ----------------------------------------------------------------
    // ONE rule, applied here (before the children see the event) so every target behaves the same:
    // a press anywhere in the dock that is not inside the editor lands the edit. Relying on the
    // editor's FL_UNFOCUS is not enough -- rows, the tab strip and the splitter are not focusable,
    // so clicking them never unfocuses it.
    if (event == FL_PUSH && renaming() && m_renameEditor != nullptr &&
        !Fl::event_inside(m_renameEditor))
        commitRename();
    // The editor is placed in absolute coords over one row; scrolling slides the rows out from
    // under it. Land the edit rather than let the field float over a different layer's row and lie
    // about what it names.
    if (event == FL_MOUSEWHEEL && renaming() && Fl::event_inside(m_scroll))
        commitRename();
    // Tab-strip clicks: the header strip has no child widgets, so unconsumed presses land
    // here. A small slop margin keeps the targets comfortable.
    if (event == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE &&
        Fl::event_y() < y() + kHeaderH) {
        const DockTab tabs[3] = {DockTab::Layers, DockTab::History, DockTab::Channels};
        for (int i = 0; i < 3; ++i) {
            const auto [x0, x1] = tabSpan(i);
            if (Fl::event_x() >= x0 - 4 && Fl::event_x() < x1 + 4) {
                setTab(tabs[i]);
                return 1;
            }
        }
    }
    return Panel::handle(event);
}

void LayerPanel::selectTopLayer() {
    m_active = core::kInvalidLayerId;
    if (m_doc != nullptr && !m_doc->root().empty())
        m_active = m_doc->root().child(m_doc->root().childCount() - 1).id();
}

void LayerPanel::setDocument(core::Document* doc) {
    m_doc = doc;
    m_thumbCache.clear(); // layer ids restart per document
    m_moveSelection.clear();
    m_selectAnchor = core::kInvalidLayerId; // ... and so does the Shift-extend anchor
    selectTopLayer();
    rebuildRows();
    if (m_history != nullptr)
        m_history->setDocument(doc); // the History tab follows the same document (S16-b)
    if (m_channels != nullptr)
        m_channels->setDocument(doc); // the Channels tab follows it too (histogram source)
}

namespace {
// The renderer-filled pixel cache a TEXT / TEXTURE layer draws from: whether there is one, and how
// big it is. Filling such a cache bumps no content revision (the renderer owns it, not the
// document), so this is the only signal a thumbnail key has that a text layer stopped being a blank
// placeholder -- or that a re-render resized it.
//
// The POINTER is very nearly inert and is deliberately NOT the interesting part: `&*m_cacheImage`
// is a stable address INSIDE the layer object, so across re-renders it only ever flips
// null <-> non-null, which `present` already says. It is still carried in the leaf key below
// because comparing it costs nothing and the null flip is the load-bearing case; nobody should read
// it as "the pixels changed".
struct PixelCacheKey {
    const void* ptr = nullptr;
    bool present = false;
    std::uint32_t w = 0;
    std::uint32_t h = 0;
};

PixelCacheKey pixelCacheKey(const core::Layer& layer) {
    if (const auto* tl = layer.as<core::TextLayer>()) {
        if (const common::Image* c = tl->cachedImage())
            return {c, true, c->width, c->height};
    } else if (const auto* xl = layer.as<core::TextureLayer>()) {
        // Texture caches are renderer-filled the same way (either lane) -- same key treatment.
        if (const common::Image* c = xl->cachedImage())
            return {c, true, c->width, c->height};
        if (const common::ImageF* cf = xl->cachedImageF())
            return {cf, true, cf->width, cf->height};
    }
    return {};
}

// An ADJUSTMENT's own knobs -- its kind and its params bag -- folded into the caller's FNV-1a
// accumulator. Shared by adjustmentScopeRevision (the adjustment row's own preview) and by
// subtreeRevision (a GROUP containing one composites it, so dragging a nested Levels has to
// refresh the group's row too). Extracted rather than handed to AdjustmentLayer as a
// contentRevision() override on purpose: params() gives out a MUTABLE reference, so there is no
// moment at which an override could reliably count -- a caller that edits the bag and forgets to
// announce it would silently freeze every thumbnail above it. Re-deriving the fold from the live
// bag cannot be forgotten.
void foldAdjustmentParams(std::uint64_t& h, const core::AdjustmentLayer& adj) {
    const auto fold = [&h](std::uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    fold(static_cast<std::uint64_t>(adj.adjustmentKind()));
    for (const auto& [key, value] : adj.params()) {
        for (const char c : key)
            fold(static_cast<std::uint64_t>(static_cast<unsigned char>(c)));
        fold(std::bit_cast<std::uint64_t>(value));
    }
}

// Everything a GROUP's thumbnail depends on, folded into one cache key: each subtree node's
// id, content revision, mask revision, effects revision, clip flag, transform, visibility, opacity
// and blend mode, plus an adjustment node's knobs and a text/texture node's pixel cache (FNV-1a
// over the words). Leaves key on contentRevision + transform directly.
std::uint64_t subtreeRevision(const core::Layer& layer) {
    std::uint64_t h = 1469598103934665603ull;
    const auto fold = [&h](std::uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    fold(layer.id());
    fold(layer.contentRevision());
    fold(layer.maskRevision());    // the fold changes the composited pixels like content does
    fold(layer.effectsRevision()); // ... and so does a child's shadow / glow / overlay: a group
                                   // composites its children THROUGH applyEffects, so the group's
                                   // thumbnail shows effects its own key would otherwise miss
    fold(static_cast<std::uint64_t>(layer.visible()));
    fold(static_cast<std::uint64_t>(layer.clipToBelow()));
    fold(static_cast<std::uint64_t>(layer.blendMode()));
    fold(std::bit_cast<std::uint32_t>(layer.opacity()));
    const common::Affine2D& t = layer.transform();
    for (const double d : {t.m00, t.m01, t.m02, t.m10, t.m11, t.m12})
        fold(std::bit_cast<std::uint64_t>(d));
    // A NESTED ADJUSTMENT has no contentRevision() of its own (it inherits the base's 0), so
    // without this a group holding a Levels layer froze the instant its sliders moved -- for ever,
    // since nothing about the group would ever drift again.
    if (const auto* adj = layer.as<core::AdjustmentLayer>())
        foldAdjustmentParams(h, *adj);
    // A DESCENDANT text/texture layer's pixels are a renderer-filled cache that no revision here
    // tracks; the leaf key carries it (cachedThumbnail), so the subtree key has to as well, or a
    // group containing text keeps the thumbnail it had before the text was first rendered.
    const PixelCacheKey cache = pixelCacheKey(layer);
    fold(static_cast<std::uint64_t>(cache.present));
    fold(cache.w);
    fold(cache.h);
    if (const auto* g = layer.as<core::GroupLayer>())
        for (const auto& child : g->children())
            fold(subtreeRevision(*child));
    return h;
}

// Everything an ADJUSTMENT's preview thumbnail depends on (S32): its own knobs (params bag, kind,
// opacity, mask, visibility, clip flag) plus the whole affected scope — every sibling below it in
// its parent group, each folded as its full subtree. The dock recomputes the preview exactly when
// this drifts, so an edit anywhere under the adjustment refreshes it and nothing else does.
std::uint64_t adjustmentScopeRevision(const core::AdjustmentLayer& adj) {
    std::uint64_t h = 1469598103934665603ull;
    const auto fold = [&h](std::uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    fold(adj.id());
    fold(adj.maskRevision());
    fold(adj.effectsRevision()); // an effect styles the graded result exactly as it does a group's
    fold(static_cast<std::uint64_t>(adj.visible()));
    fold(static_cast<std::uint64_t>(adj.clipToBelow()));
    fold(std::bit_cast<std::uint32_t>(adj.opacity()));
    foldAdjustmentParams(h, adj); // kind + params bag (shared with subtreeRevision)
    if (const core::GroupLayer* parent = adj.parent()) {
        for (std::size_t i = 0; i < parent->childCount(); ++i) {
            const core::Layer& child = parent->child(i);
            if (child.id() == adj.id())
                break;
            fold(subtreeRevision(child));
        }
    }
    return h;
}
} // namespace

const common::Image& LayerPanel::cachedThumbnail(const core::Layer& layer, bool* rebuilt) {
    ThumbEntry& e = m_thumbCache[layer.id()];
    const std::uint64_t rev = [&] {
        if (layer.kind() == core::LayerKind::Group)
            return subtreeRevision(layer);
        if (const auto* adj = layer.as<core::AdjustmentLayer>())
            return adjustmentScopeRevision(*adj); // the preview depends on everything below it
        return layer.contentRevision();
    }();
    // The WORLD transform is the cache key: transforming a group must refresh every child's
    // thumbnail even though the children's own transforms never moved.
    const common::Affine2D world = core::worldTransform(layer);
    const PixelCacheKey cache = pixelCacheKey(layer); // see ThumbEntry::textCache
    // The CANVAS size is a key too (see ThumbEntry::docW): the render below frames the doc rect for
    // an adjustment preview and for any layer with no contentBounds, and rasterizes group / vector
    // / adjustment content at doc resolution -- and a crop or canvas resize moves no layer revision
    // and no world transform, so nothing else in this key would ever notice one.
    const std::uint32_t docW = m_doc != nullptr ? m_doc->width() : 0;
    const std::uint32_t docH = m_doc != nullptr ? m_doc->height() : 0;

    const bool stale = e.image.empty() || e.contentRev != rev || !(e.transform == world) ||
                       e.textCache != cache.ptr || e.textCacheW != cache.w ||
                       e.textCacheH != cache.h || e.docW != docW || e.docH != docH;
    if (stale) {
        // Scoped because this 34x34 px thumbnail is built by rendering the layer at DOCUMENT
        // resolution first: a group goes through render::compositeGroup(group, docW, docH) -- a
        // full composite of the whole subtree -- and a vector layer through
        // rasterizeObjectF(obj, docW, docH). On a 39.8 MP canvas that is a ~34,000:1 ratio between
        // what is computed and what is drawn, and it is why the per-kind walk rows overshoot the
        // composite rows they should nest inside.
        MOSAIC_PERF_SCOPE("Layer panel thumbnail", mosaic::common::Lane::Cpu);
        e.image = layerThumbnail(layer, kThumb, docW, docH);
        e.contentRev = rev;
        e.transform = world;
        e.textCache = cache.ptr;
        e.textCacheW = cache.w;
        e.textCacheH = cache.h;
        e.docW = docW;
        e.docH = docH;
    }
    if (rebuilt != nullptr)
        *rebuilt = stale;
    return e.image;
}

const common::Image& LayerPanel::cachedMaskThumbnail(const core::Layer& layer, bool* rebuilt) {
    static const common::Image kNoMaskThumb; // the maskless answer (rows draw nothing for it)
    ThumbEntry& e = m_thumbCache[layer.id()];
    const std::uint64_t rev = layer.maskRevision();
    if (!layer.hasMask()) {
        const bool had = !e.maskImage.empty();
        e.maskImage = common::Image{};
        e.maskRev = rev;
        if (rebuilt != nullptr)
            *rebuilt = had;
        return kNoMaskThumb;
    }
    const bool stale = e.maskImage.empty() || e.maskRev != rev;
    if (stale) {
        e.maskImage = maskThumbnail(*layer.mask(), kThumb);
        e.maskRev = rev;
    }
    if (rebuilt != nullptr)
        *rebuilt = stale;
    return e.maskImage;
}

void LayerPanel::modifiersChanged() {
    for (LayerRow* row : m_rows)
        row->modifiersChanged(); // each row no-ops unless the pointer is on it
}

void LayerPanel::refresh() {
    if (m_doc != nullptr && m_active != core::kInvalidLayerId && m_doc->find(m_active) == nullptr)
        selectTopLayer(); // the active layer was removed (e.g. by undo)
    rebuildRows();
}

void LayerPanel::refreshThumbnails() {
    // Re-fetch each row's thumbnail (cachedThumbnail only rebuilds the one whose contentRevision /
    // transform / text pixel-cache actually changed) without recreating the row widgets -- light
    // enough to call per keystroke so a TextLayer's thumbnail updates live as you type (fixlist
    // #10), and after every composite so a freshly-opened document's text layers stop showing the
    // blank placeholder the moment the renderer has drawn them.
    //
    // It is ALSO the settle step every panel-LOCAL edit owes (visibility, blend, opacity, the mask
    // flags). Those push their command and then redrawList() -- and a redraw only re-blits the
    // common::Image the row is already holding, because LayerRow::draw() paints m_thumb and never
    // consults this cache. So without a call here, clicking a child layer's eye inside a group left
    // the GROUP's composite thumbnail frozen until some unrelated command happened to rebuild the
    // rows. Cheap on a no-op: the expensive compositeGroup / adjustmentPreview renders inside
    // cachedThumbnail only run when a key actually drifted.
    if (m_doc == nullptr)
        return;
    bool anyChanged = false;
    for (LayerRow* row : m_rows) {
        core::Layer* l = m_doc->find(row->layerId());
        if (l == nullptr)
            continue;
        std::string name = displayName(*l); // a Text layer's name tracks its content live (#5)
        bool rebuilt = false;
        const common::Image& thumb = cachedThumbnail(*l, &rebuilt);
        bool maskRebuilt = false;
        const common::Image& mthumb = cachedMaskThumbnail(*l, &maskRebuilt); // live mask paint (S31)
        if (!rebuilt && !maskRebuilt && name == row->name())
            continue; // nothing moved: do not damage the row (this runs on every composite)
        row->setName(std::move(name));
        if (rebuilt)
            row->setThumbnail(thumb);
        if (maskRebuilt) {
            row->setMaskState(l->hasMask());
            row->setMaskThumbnail(mthumb);
        }
        anyChanged = true;
        if (!renaming())
            row->redraw();
    }
    if (anyChanged && renaming())
        redraw(); // a row-only repaint would paint across the floating rename editor
}

void LayerPanel::reapplyTheme() {
    Panel::reapplyTheme(); // panel ground; rows/dropdown/slider/buttons draw live (redraw repaints)
    if (m_scroll != nullptr)
        m_scroll->color(toFl(activePalette().panelBg)); // Fl_Scroll paints its own cached bg
    if (m_history != nullptr)
        m_history->reapplyTheme();
    if (m_channels != nullptr)
        m_channels->reapplyTheme();
    redraw();
}

void LayerPanel::rebuildRows() {
    // An editor floating over a row that is about to be deleted has nothing left to rename. (Reached
    // from commitRename() too -- harmlessly, since that clears m_renameId before refreshing.)
    cancelRename();
    m_rows.clear();
    m_scroll->scroll_to(0, 0);
    m_scroll->clear(); // delete existing LayerRow children (keeps the scrollbars)
    if (m_doc != nullptr) {
        m_scroll->begin();
        int ry = m_scroll->y();
        // Walk the tree top-of-stack first, indenting by depth and skipping collapsed subtrees.
        const std::function<void(core::GroupLayer&, int)> emit = [&](core::GroupLayer& g, int depth) {
            for (std::size_t i = g.childCount(); i-- > 0;) {
                core::Layer& layer = g.child(i);
                auto* row = new LayerRow(m_scroll->x(), ry, m_scroll->w(), kRowH, this, layer.id());
                row->setName(displayName(layer));
                row->setDepth(depth);
                row->setPastedMarker(layer.pastedMarker());
                row->setHasEffects(layer.hasEffects() && !layer.effects().empty());
                row->setTypeBadge(typeBadgeFor(layer)); // shape / path / gradient / text / ...
                row->setThumbnail(cachedThumbnail(layer));
                row->setMaskState(layer.hasMask()); // the second thumbnail + chain (S31)
                if (layer.hasMask())
                    row->setMaskThumbnail(cachedMaskThumbnail(layer));
                m_rows.push_back(row);
                ry += kRowH;
                if (auto* grp = layer.as<core::GroupLayer>(); grp != nullptr && grp->expanded())
                    emit(*grp, depth + 1);
            }
        };
        emit(m_doc->root(), 0);
        m_scroll->end();
        // Deleted layers leave cache entries behind; prune so the map tracks the document.
        std::erase_if(m_thumbCache,
                      [this](const auto& kv) { return m_doc->find(kv.first) == nullptr; });
    }
    updateScrollGutter(); // the row count just changed: so may the scrollbar's presence
    redrawList();
    syncProperties();
    redraw(); // header + properties strip + empty-state
}

void LayerPanel::openEffectsFor(core::LayerId id) {
    setActive(id);
    if (m_onOpenEffects)
        m_onOpenEffects(id);
}

void LayerPanel::openTextureFor(core::LayerId id) {
    setActive(id); // makes the texture layer active first, so the generator dialog opens in edit mode
    if (m_onOpenTexture)
        m_onOpenTexture(id);
}

void LayerPanel::notifyChanged() {
    if (m_onChange)
        m_onChange();
}

// The bottom strip's two structural buttons follow the same rule the context menu's items do: a
// locked layer greys them out. Silently ignoring the click (which is what deleteActive/groupActive
// do) reads as a dead button; greying says why. Delete/Group also need SOMETHING active to act on.
void LayerPanel::syncActionButtons() {
    if (m_deleteButton == nullptr || m_groupButton == nullptr)
        return;
    const bool haveActive =
        m_doc != nullptr && m_active != core::kInvalidLayerId && m_doc->find(m_active) != nullptr;
    const bool allowStructural = haveActive && !activeLayerLocked();
    for (IconButton* button : {m_deleteButton, m_groupButton}) {
        if (allowStructural)
            button->activate();
        else
            button->deactivate();
    }
    // Add is always available: it makes a new layer, it does not touch the locked one.
    if (m_addButton != nullptr) {
        if (m_doc != nullptr)
            m_addButton->activate();
        else
            m_addButton->deactivate();
    }
}

void LayerPanel::syncProperties() {
    syncActionButtons();
    if (m_blendChoice == nullptr || m_opacitySlider == nullptr)
        return;
    // "Disabled" AND "All selected layers" both present the WHOLE selection rather than one layer:
    // the blend dropdown shows the mixed state ("Normal, Multiply", with a flyout dot on every mode
    // present), and the slider + "~NN%" readout (draw()) show the selection's AVERAGE opacity. They
    // differ only in editability -- Disabled is inert (the user single-selects to edit one), All
    // keeps the strip live and batch-edits every selected layer (onBlend/Opacity see editsAllSelected
    // and coalesce into one undo step). "Active layer only" + the single-selection case fall through
    // to the active-layer path below.
    if (multiSelectActive() && (m_multiMode == MultiSelectMode::Disabled ||
                                m_multiMode == MultiSelectMode::All)) {
        const bool editable = (m_multiMode == MultiSelectMode::All);
        m_blendChoice->setOverrideText(mixedBlendLabel());     // single name once the set is uniform
        m_blendChoice->setMarkedItems(distinctBlendIndices()); // dot every present mode in the flyout
        // Anchor value() to the active layer's mode so the flyout opens aligned to it; in All mode a
        // pick from the list then applies to the whole set (onBlendChanged).
        if (const core::Layer* a = (m_doc != nullptr) ? m_doc->find(m_active) : nullptr)
            m_blendChoice->value(static_cast<int>(a->blendMode()));
        float sum = 0.0f;
        int n = 0;
        for (const core::LayerId id : m_moveSelection)
            if (const core::Layer* l = (m_doc != nullptr) ? m_doc->find(id) : nullptr) {
                sum += l->opacity();
                ++n;
            }
        if (n > 0)
            m_opacitySlider->value(sum / static_cast<double>(n));
        if (editable) {
            m_blendChoice->activate();
            m_opacitySlider->activate();
        } else {
            m_blendChoice->deactivate();
            m_opacitySlider->deactivate();
        }
        m_blendChoice->redraw();
        m_opacitySlider->redraw();
        redraw();
        return;
    }
    m_blendChoice->setOverrideText({}); // back to showing the selected mode
    m_blendChoice->setMarkedItems({});  // plain single-select dot
    core::Layer* l = (m_doc != nullptr && m_active != core::kInvalidLayerId) ? m_doc->find(m_active)
                                                                             : nullptr;
    if (l == nullptr) {
        m_blendChoice->value(0);
        m_blendChoice->deactivate();
        m_opacitySlider->value(1.0);
        m_opacitySlider->deactivate();
    } else {
        m_blendChoice->value(static_cast<int>(l->blendMode()));
        m_blendChoice->activate();
        m_opacitySlider->value(l->opacity());
        m_opacitySlider->activate();
    }
    m_blendChoice->redraw();
    m_opacitySlider->redraw();
    redraw(); // the "NN%" readout + captions
}

void LayerPanel::setMultiSelectionMode(MultiSelectMode mode) {
    if (mode == m_multiMode)
        return;
    m_multiMode = mode;
    syncProperties(); // enable/disable + relabel the strip live if a multi-selection is active now
}

void LayerPanel::setMoveSelection(std::vector<core::LayerId> selection) {
    if (selection == m_moveSelection)
        return;
    m_moveSelection = std::move(selection);
    if (m_scroll != nullptr)
        redrawList(); // rows read isInMoveSelection() in draw()
    syncProperties();       // gate / restore the blend + opacity strip
}

bool LayerPanel::isInMoveSelection(core::LayerId id) const {
    return std::find(m_moveSelection.begin(), m_moveSelection.end(), id) != m_moveSelection.end();
}

std::string LayerPanel::mixedBlendLabel() const {
    if (m_doc == nullptr)
        return {};
    std::string out;
    std::vector<core::BlendMode> seen;
    for (const core::LayerId id : m_moveSelection) {
        const core::Layer* l = m_doc->find(id);
        if (l == nullptr)
            continue;
        const core::BlendMode m = l->blendMode();
        if (std::find(seen.begin(), seen.end(), m) != seen.end())
            continue; // each distinct mode once
        seen.push_back(m);
        if (!out.empty())
            out += ", ";
        out += core::blendModeName(m);
    }
    return out;
}

std::vector<int> LayerPanel::distinctBlendIndices() const {
    std::vector<int> out;
    if (m_doc == nullptr)
        return out;
    for (const core::LayerId id : m_moveSelection) {
        const core::Layer* l = m_doc->find(id);
        if (l == nullptr)
            continue;
        const int idx = static_cast<int>(l->blendMode());
        if (std::find(out.begin(), out.end(), idx) == out.end())
            out.push_back(idx); // each distinct mode once, in stack order (mirrors mixedBlendLabel)
    }
    return out;
}

bool LayerPanel::selectionOpacitiesMixed() const {
    if (m_doc == nullptr)
        return false;
    bool have = false;
    float first = 0.0f;
    for (const core::LayerId id : m_moveSelection)
        if (const core::Layer* l = m_doc->find(id)) {
            if (!have) {
                first = l->opacity();
                have = true;
            } else if (std::abs(l->opacity() - first) > 0.005f) { // ~half a 1% slider step
                return true;
            }
        }
    return false;
}

// S15-e: while a multi-selection is active, "All selected layers" mode applies an edit across the
// whole set (the active layer included); "Active layer only" and the single-selection case edit just
// the active layer; "Disabled" makes the strip inert (handled by syncProperties deactivating it, but
// guard here too since a programmatic value change could still fire).
bool LayerPanel::editsAllSelected() const {
    return multiSelectActive() && m_multiMode == MultiSelectMode::All;
}

void LayerPanel::onBlendChanged() {
    if (m_doc == nullptr || m_active == core::kInvalidLayerId)
        return;
    if (multiSelectActive() && m_multiMode == MultiSelectMode::Disabled)
        return; // the strip is inert on a multi-selection in Disabled mode
    const int idx = m_blendChoice->value();
    if (idx < 0 || idx >= core::kBlendModeCount)
        return;
    const auto mode = static_cast<core::BlendMode>(idx);
    if (editsAllSelected()) { // one undo step setting every selected layer's blend mode
        auto composite = std::make_unique<core::CompositeCommand>("Set Blend Mode");
        for (const core::LayerId id : m_moveSelection)
            composite->add(std::make_unique<core::SetBlendModeCommand>(id, mode));
        m_doc->commands().push(std::move(composite));
        notifyChanged();
        refreshThumbnails(); // a blend change restyles every enclosing group's composite thumb
        syncProperties(); // the set is now uniform: collapse "Normal, Multiply" + dots to the one mode
        return;
    }
    m_doc->commands().push(std::make_unique<core::SetBlendModeCommand>(m_active, mode));
    notifyChanged();
    refreshThumbnails(); // ... likewise
}

void LayerPanel::onOpacityChanged() {
    if (m_doc == nullptr || m_active == core::kInvalidLayerId)
        return;
    if (multiSelectActive() && m_multiMode == MultiSelectMode::Disabled)
        return;
    if (Fl::event() == FL_RELEASE) {
        ++m_opacityCoalesce; // end the gesture: the next drag is a separate undo step
        // Re-derive on the SETTLE, not per tick: a grouped layer's opacity is in its group's
        // thumbnail key, and re-running compositeGroup once per drag frame would stall the drag for
        // a picture nobody can read mid-gesture. Same deal the host gives the text and adjustment
        // thumbnails (app_window's m_textThumbDirty / m_adjustThumbDirty settle timers).
        refreshThumbnails();
        return;
    }
    const auto value = static_cast<float>(m_opacitySlider->value());
    if (editsAllSelected()) { // set the whole selection to the slider value, coalescing the drag
        std::vector<core::SetOpacitiesCommand::Entry> entries;
        entries.reserve(m_moveSelection.size());
        for (const core::LayerId id : m_moveSelection)
            entries.push_back({id, value});
        m_doc->commands().push(
            std::make_unique<core::SetOpacitiesCommand>(std::move(entries), m_opacityCoalesce));
    } else {
        m_doc->commands().push(
            std::make_unique<core::SetOpacityCommand>(m_active, value, m_opacityCoalesce));
    }
    notifyChanged();
    redraw(); // live "NN%" readout
}

void LayerPanel::setActive(core::LayerId id) {
    if (m_active == id)
        return;
    m_active = id;
    m_maskTarget = false; // a fresh row aims at pixels first (click its mask thumb to retarget)
    redrawList(); // rows read activeLayer() in draw()
    syncProperties();
}

void LayerPanel::toggleVisible(core::LayerId id) {
    if (m_doc == nullptr)
        return;
    const core::Layer* l = m_doc->find(id);
    if (l == nullptr)
        return;
    m_doc->commands().push(std::make_unique<core::SetVisibleCommand>(id, !l->visible()));
    notifyChanged();      // recomposite
    refreshThumbnails();  // hiding a CHILD re-derives its group's composite thumb (see it there)
    redrawList(); // eye glyph + name muting
}

void LayerPanel::toggleLocked(core::LayerId id) {
    if (m_doc == nullptr)
        return;
    const core::Layer* l = m_doc->find(id);
    if (l == nullptr)
        return;
    m_doc->commands().push(std::make_unique<core::SetLockedCommand>(id, !l->locked()));
    // Locking changes no pixels, so there is nothing to recomposite -- but the command stack moved,
    // so the History tab must hear about it exactly as the visibility toggle's does.
    notifyChanged();
    if (id == m_active)
        syncActionButtons(); // Delete/Group grey out (or come back) with the active layer's lock
    redrawList();            // the padlock glyph
}

bool LayerPanel::activeLayerLocked() const {
    if (m_doc == nullptr || m_active == core::kInvalidLayerId)
        return false;
    const core::Layer* l = m_doc->find(m_active);
    return l != nullptr && l->locked();
}

// ---- inline rename -----------------------------------------------------------------------------

void LayerPanel::beginRename(core::LayerId id) {
    if (m_doc == nullptr || m_renameEditor == nullptr || m_tab != DockTab::Layers)
        return;
    const core::Layer* layer = m_doc->find(id);
    if (layer == nullptr)
        return;
    if (renaming())
        commitRename(); // renaming a second row while one editor is live: land the first edit
    setActive(id);

    const LayerRow* row = nullptr;
    for (const LayerRow* r : m_rows)
        if (r->layerId() == id) {
            row = r;
            break;
        }
    if (row == nullptr)
        return;
    // Bring a partially-scrolled row fully into view first, so the editor always floats over the
    // whole row rather than half of one (or, worse, over the viewport's edge).
    int dy = 0;
    if (row->y() < m_scroll->y())
        dy = row->y() - m_scroll->y();
    else if (row->y() + kRowH > m_scroll->y() + m_scroll->h())
        dy = row->y() + kRowH - (m_scroll->y() + m_scroll->h());
    if (dy != 0)
        m_scroll->scroll_to(0, std::max(0, m_scroll->yposition() + dy)); // moves the row widgets

    // Float the editor over the row's name cell.
    const int ex = row->x() + kEyeW + kLockW + row->depth() * kIndent + kTriW + kThumb + 6;
    const int ey = row->y() + (kRowH - 24) / 2;
    const int ew = std::max(40, row->x() + row->w() - 8 - ex);
    m_renameEditor->resize(ex, ey, ew, 24);
    // The STORED name, not the displayed one: a Text layer's row shows its content while auto-named,
    // and seeding the editor with that would silently turn the caption into a manual name.
    m_renameEditor->value(layer->name().c_str());
    m_renameId = id;
    m_renameEditor->show();
    m_renameEditor->take_focus();
    // Select the whole name: the first keystroke replaces it, Right/End keeps it to edit.
    m_renameEditor->insert_position(0, static_cast<int>(layer->name().size()));
    redraw();
}

// MUST NOT delete any row widget. A pending edit is landed from inside a LayerRow's own handle()
// -- pressing another row, double-clicking a second name, right-clicking for the menu -- and a
// rebuildRows() there would free the widget the call is still running in. (The same rule the
// History panel's same-count refresh path documents.) So the renamed row is patched in place.
void LayerPanel::commitRename() {
    if (!renaming() || m_renameCommitting)
        return; // hide() below re-enters through the editor's FL_UNFOCUS
    m_renameCommitting = true;
    const core::LayerId id = m_renameId;
    std::string next = m_renameEditor->value() != nullptr ? m_renameEditor->value() : "";
    m_renameId = core::kInvalidLayerId;
    m_renameEditor->hide();
    m_renameCommitting = false;

    const core::Layer* layer = m_doc != nullptr ? m_doc->find(id) : nullptr;
    // A blank name would leave a row with nothing to click or read; an unchanged one is not an edit.
    // Either way: no command, so renaming never litters the History tab with no-op steps.
    if (layer == nullptr || next.empty() || next == layer->name()) {
        redraw();
        return;
    }
    m_doc->commands().push(std::make_unique<core::SetNameCommand>(id, std::move(next)));
    notifyChanged(); // the recomposite is deferred to the frame loop; nothing rebuilds here
    for (LayerRow* row : m_rows) {
        if (row->layerId() != id)
            continue;
        row->setName(displayName(*layer));         // the command may have cleared TextLayer auto-naming
        row->setPastedMarker(layer->pastedMarker()); // ... and the paste badge ("naming adopts it")
        row->redraw();
        break;
    }
    redraw();
}

void LayerPanel::cancelRename() {
    if (m_renameEditor == nullptr || !renaming())
        return;
    m_renameId = core::kInvalidLayerId;
    m_renameCommitting = true; // the hide()'s FL_UNFOCUS must not resurrect the edit as a commit
    m_renameEditor->hide();
    m_renameCommitting = false;
    redraw();
}

// ---- row context menu --------------------------------------------------------------------------

void LayerPanel::showRowMenu(core::LayerId id) {
    if (m_doc == nullptr)
        return;
    commitRename(); // right-clicking elsewhere is clicking away: land the edit (deletion-free)
    setActive(id);
    const core::Layer* layer = m_doc->find(id);
    if (layer == nullptr)
        return;
    ContextMenu* menu = contextMenuFor(top_window());
    if (menu == nullptr)
        return; // no themed host -> no menu (there is no stock fallback for a custom widget)

    const bool locked = layer->locked();
    const bool visible = layer->visible();
    std::vector<ContextAction> actions;
    actions.push_back({_("Rename..."), [this, id] { beginRename(id); }});
    actions.push_back({_("Duplicate Layer"), [this] { duplicateActive(); }});
    // Structural edits are what a lock forbids; the menu says so by greying them rather than
    // accepting the click and doing nothing.
    actions.push_back({_("Delete Layer"), [this] { deleteActive(); }, !locked, /*divider=*/true});
    actions.push_back({_("Group Layers"), [this] { groupActive(); }, !locked});
    // Merge Down stays enabled: the host command explains each refusal in the status bar (no layer
    // below / an adjustment layer below / hidden / locked / a blend mode that cannot be baked),
    // which beats a silently greyed item. Since S36 the KIND of the layer below is no longer a
    // refusal on its own -- every pair that has pixels merges, so there is nothing here to gate on.
    actions.push_back(
        {_("Merge Down"), [this] { if (m_onMergeDown) m_onMergeDown(); }, true, /*divider=*/true});
    actions.push_back({visible ? _("Hide Layer") : _("Show Layer"),
                       [this, id] { toggleVisible(id); }});
    actions.push_back({locked ? _("Unlock Layer") : _("Lock Layer"),
                       [this, id] { toggleLocked(id); }, true, /*divider=*/true});
    // Rasterize / Convert to Path. Each item appears only for the kinds it can act on -- a greyed
    // item is a promise, and there is nothing to promise here: a raster layer is already rasterized,
    // and a magic layer or a group has no outline to convert. Both are destructive (one undo step).
    const bool canRasterize = layer->kind() != core::LayerKind::Raster &&
                              layer->kind() != core::LayerKind::Adjustment;
    const bool canConvertToPath = layer->as<core::TextLayer>() != nullptr ||
                                  (layer->as<core::VectorLayer>() != nullptr &&
                                   layer->as<core::VectorLayer>()->hasObject());
    if (canRasterize) {
        actions.push_back({_("Rasterize"),
                           [this, id] { if (m_onRasterize) m_onRasterize(id); },
                           !locked, /*divider=*/!canConvertToPath});
    }
    if (canConvertToPath) {
        actions.push_back({_("Convert to Path"),
                           [this, id] { if (m_onConvertToPath) m_onConvertToPath(id); },
                           !locked, /*divider=*/true});
    }
    // Layer masks (S31). Add appears only on maskless rows whose kind supports one (a text
    // layer's pixel cache grid moves with the text -- no mask there yet); the management trio
    // appears only with a mask to manage -- absent beats greyed, as with Rasterize above.
    // Add/Delete change document pixels' visibility wholesale and respect the lock; the two flag
    // flips are visibility-like toggles and stay live on a locked layer, like the eye.
    if (!layer->hasMask()) {
        if (layer->kind() != core::LayerKind::Text)
            actions.push_back(
                {_("Add Mask"), [this, id] { addMaskTo(id); }, !locked, /*divider=*/true});
    } else {
        const core::RasterMask* mk = layer->mask();
        actions.push_back({_("Delete Mask"), [this, id] { deleteMask(id); }, !locked});
        actions.push_back({mk->enabled ? _("Disable Mask") : _("Enable Mask"),
                           [this, id] { toggleMaskEnabled(id); }});
        actions.push_back({mk->linked ? _("Unlink Mask") : _("Link Mask"),
                           [this, id] { toggleMaskLinked(id); }, true, /*divider=*/true});
    }
    actions.push_back({_("Layer Effects..."), [this, id] { openEffectsFor(id); }});

    // Anchor at the cursor in the menu's top-level coords (add each sub-window offset up the chain,
    // exactly as the canvas and slider menus do).
    int hx = Fl::event_x();
    int hy = Fl::event_y();
    for (Fl_Window* win = window(); win != nullptr && win != menu->window(); win = win->window()) {
        hx += win->x();
        hy += win->y();
    }
    menu->openWith(hx, hy, std::move(actions));
}

void LayerPanel::toggleExpanded(core::LayerId id) {
    if (m_doc == nullptr)
        return;
    core::Layer* l = m_doc->find(id);
    auto* g = l != nullptr ? l->as<core::GroupLayer>() : nullptr;
    if (g == nullptr)
        return;
    g->setExpanded(!g->expanded()); // view state only -- not an undoable document edit
    rebuildRows();                   // rows of a collapsed subtree appear/disappear
}

namespace {
// The alpha channel of a DOCUMENT-SPACE, document-sized image as a Selection. The group and
// adjustment composites below already land in that space, so this is a straight channel lift.
core::Selection coverageOfDocImage(const common::Image& img, std::uint32_t docW,
                                   std::uint32_t docH) {
    core::Selection s(docW, docH);
    if (img.width != docW || img.height != docH)
        return s; // a refused/short render selects nothing (the caller reports it)
    auto* out = s.data().data();
    for (std::size_t i = 0, n = s.data().size(); i < n; ++i)
        out[i] = img.rgba[i * 4 + 3];
    return s;
}

// The alpha of a document-sized PARENT-LOCAL bake (what render::rasterizeLayer produces) carried
// into document space through the layer's ancestors -- the same nearest inverse-sample the
// compositor's leaf walk and core::selectionFromLayerPixels use, so the coverage lands exactly
// where the canvas draws the layer. A top-level layer's parent transform is the identity, which is
// the fast path.
core::Selection coverageOfParentLocalImage(const common::Image& img,
                                           const common::Affine2D& parentToDoc,
                                           std::uint32_t docW, std::uint32_t docH) {
    if (parentToDoc == common::Affine2D::identity())
        return coverageOfDocImage(img, docW, docH);
    core::Selection s(docW, docH);
    const std::optional<common::Affine2D> inv = parentToDoc.inverse();
    if (!inv || img.empty())
        return s; // a singular ancestor transform shows nothing, like the compositor's leaf walk
    for (std::uint32_t y = 0; y < docH; ++y) {
        auto* row = s.data().data() + static_cast<std::size_t>(y) * docW;
        for (std::uint32_t x = 0; x < docW; ++x) {
            const common::Vec2 p = inv->apply({x + 0.5, y + 0.5});
            const long sx = static_cast<long>(std::floor(p.x));
            const long sy = static_cast<long>(std::floor(p.y));
            if (sx >= 0 && sy >= 0 && sx < static_cast<long>(img.width) &&
                sy < static_cast<long>(img.height))
                row[x] = img.rgba[(static_cast<std::size_t>(sy) * img.width + sx) * 4 + 3];
        }
    }
    return s;
}
} // namespace

bool LayerPanel::layerHasSelectablePixels(core::LayerId id) const {
    if (m_doc == nullptr)
        return false;
    const core::Layer* l = m_doc->find(id);
    if (l == nullptr)
        return false;
    // CHEAP QUESTIONS ONLY. This runs from LayerRow::updateCursor -- once per pointer move over a
    // row, and again on every modifier keydown -- so it may never composite. It decides whether the
    // row offers the hand cursor and the marching-ants thumbnail preview; whether the gesture then
    // finds any coverage is layerPixelCoverage()'s answer, narrated on the click.
    if (l->as<core::RasterLayer>() != nullptr || l->as<core::MagicLayer>() != nullptr)
        return true;
    if (l->kind() == core::LayerKind::Group)
        return l->contentBounds().has_value(); // a group offers its whole composited subtree
    if (const auto* vl = l->as<core::VectorLayer>())
        return vl->hasObject(); // a layer with no object has no outline to fill
    if (const auto* tl = l->as<core::TextLayer>()) // ... nor has unrendered text any glyphs
        return tl->cachedImage() != nullptr && !tl->cachedImage()->empty();
    if (const auto* xl = l->as<core::TextureLayer>())
        return (xl->cachedImage() != nullptr && !xl->cachedImage()->empty()) ||
               (xl->cachedImageF() != nullptr && !xl->cachedImageF()->empty());
    // An ADJUSTMENT's "pixels" are the backdrop it grades, and whether that scope is empty is a
    // whole composite away. The affordance promises; the click answers honestly.
    return l->as<core::AdjustmentLayer>() != nullptr;
}

std::optional<core::Selection> LayerPanel::layerPixelCoverage(core::LayerId id) const {
    if (m_doc == nullptr)
        return std::nullopt;
    const core::Layer* layer = m_doc->find(id);
    if (layer == nullptr)
        return std::nullopt;
    const std::uint32_t docW = m_doc->width();
    const std::uint32_t docH = m_doc->height();
    if (docW == 0 || docH == 0)
        return std::nullopt;

    // ONE routing table, and every arm of it is a picture the compositor ALREADY produces for this
    // layer's row -- no per-kind rasterizer is written here, because a second one would be a second
    // thing to keep in step with the canvas.
    //
    // Raster / Magic keep the S13 contract exactly as it was: their own alpha through the world
    // transform, with the layer MASK deliberately left out (clicking the mask thumbnail is the S31
    // gesture, and folding it here would make the two gestures indistinguishable).
    if (layer->as<core::RasterLayer>() != nullptr || layer->as<core::MagicLayer>() != nullptr)
        return core::selectionFromLayerPixels(*layer, docW, docH);

    // A GROUP selects its composited subtree's coverage -- the same pixels the canvas shows for it
    // (children + group + ancestor transforms applied). This arm predates the others and is what
    // the rest were modelled on: compositeGroup is the group thumbnail's own source.
    if (const auto* group = layer->as<core::GroupLayer>())
        return coverageOfDocImage(render::compositeGroup(*group, docW, docH), docW, docH);

    // An ADJUSTMENT owns no pixels of its own; what it has is a SCOPE -- the accumulated backdrop
    // beneath it, graded. That composite is exactly the picture its dock thumbnail shows
    // (layerThumbnail's adjustment arm), just rendered at document resolution instead of preview
    // resolution, so the gesture selects precisely the pixels the row is a portrait of. An
    // adjustment with nothing under it composites to nothing and is reported, not silently applied.
    if (const auto* adj = layer->as<core::AdjustmentLayer>())
        return coverageOfDocImage(render::adjustmentPreview(*adj, docW, docH, docW, docH), docW,
                                  docH);

    // Everything else -- Text (3D and warped included), Vector shape / path / gradient, Texture --
    // goes through the ONE rasterizer there is. render::rasterizeLayer bakes the layer's transform,
    // its mask and its layer effects into a document-sized PARENT-LOCAL image (the same contract
    // Rasterize and Merge Down bake against), so a text layer answers from its rendered cache and a
    // vector layer from its rasterized coverage without either growing a bespoke path here. Auto
    // filter: this is a commit, not a live-drag preview, so vector edges keep their analytic AA.
    const common::Image flat =
        render::rasterizeLayer(*layer, docW, docH, render::ResampleFilter::Auto);
    return coverageOfParentLocalImage(flat, core::parentWorldTransform(*layer), docW, docH);
}

void LayerPanel::status(std::string message) {
    if (m_onStatus)
        m_onStatus(std::move(message));
}

void LayerPanel::shiftClickThumbnail(core::LayerId id) {
    // Shift-click a layer thumbnail -> select that layer's pixels (S13). Ctrl/Alt on top of the
    // Shift trigger choose the boolean op against the current selection (S14-b; thumbnailSelectOp).
    setActive(id);
    if (m_doc == nullptr)
        return;
    const core::Layer* layer = m_doc->find(id);
    if (layer == nullptr)
        return;
    const std::optional<core::Selection> sel = layerPixelCoverage(id);
    if (!sel) {
        uiLog().debug("shift-click layer {}: no document/canvas to select against", id);
        return;
    }
    if (!sel->anySelected()) {
        // HONEST, NOT SILENT. Every kind is wired now, so reaching here means the layer really is
        // empty -- an adjustment with nothing beneath it, a vector layer with no object, an
        // unrendered text layer, a fully transparent raster -- and a gesture that quietly does
        // nothing reads as a broken gesture (it used to be a debug log nobody sees).
        uiLog().debug("shift-click layer {}: '{}' layer covers no pixels", id,
                      core::layerKindName(layer->kind()));
        status(_("This layer has no pixels to select"));
        return; // contributing nothing: don't clobber (or pointlessly combine) the selection
    }
    const auto state = Fl::event_state();
    const core::SelectOp op =
        thumbnailSelectOp((state & FL_CTRL) != 0, (state & FL_ALT) != 0);
    core::Selection combined = core::Selection::combine(m_doc->selection(), *sel, op);
    if (!combined.anySelected())
        combined = core::Selection{}; // an all-zero active mask never lands (S14 semantics)
    if (combined == m_doc->selection())
        return; // a no-op combine isn't worth an undo step
    m_doc->commands().push(std::make_unique<core::SetSelectionCommand>(std::move(combined)));
    notifyChanged(); // the host re-syncs the canvas (selection mask -> marching ants)
}

// ---- layer masks (S31) ---------------------------------------------------------------------

bool LayerPanel::maskEditTarget() const {
    if (!m_maskTarget || m_doc == nullptr)
        return false;
    const core::Layer* l = m_doc->find(m_active);
    return l != nullptr && l->hasMask(); // the mask may have been deleted under the flag
}

void LayerPanel::targetMask(core::LayerId id) {
    setActive(id); // a row change re-aims at pixels; the flag below re-aims at the mask
    const core::Layer* l = m_doc != nullptr ? m_doc->find(id) : nullptr;
    if (l == nullptr || !l->hasMask())
        return;
    if (!m_maskTarget) {
        m_maskTarget = true;
        redrawList(); // the target ring moves thumbnails
    }
}

void LayerPanel::targetPixels(core::LayerId id) {
    setActive(id);
    if (m_maskTarget) {
        m_maskTarget = false;
        redrawList();
    }
}

void LayerPanel::addMaskTo(core::LayerId id) {
    if (m_doc == nullptr)
        return;
    commitRename();
    core::Layer* l = m_doc->find(id);
    if (l == nullptr || l->hasMask() || l->locked())
        return; // the menu offers Add only maskless + unlocked; belt and braces
    // Photoshop's add-mask semantics: an active selection seeds the mask, none = reveal-all.
    const core::Selection& sel = m_doc->selection();
    core::RasterMask mask = sel.isEmpty()
                                ? core::revealAllMask(*l, m_doc->width(), m_doc->height())
                                : core::maskFromSelection(*l, sel, m_doc->width(), m_doc->height());
    m_doc->commands().push(
        std::make_unique<core::SetLayerMaskCommand>(id, std::move(mask), "Add Mask"));
    setActive(id);
    m_maskTarget = true; // a fresh mask becomes the edit target (paint lands on it)
    rebuildRows();
    notifyChanged();
}

void LayerPanel::deleteMask(core::LayerId id) {
    if (m_doc == nullptr)
        return;
    commitRename();
    core::Layer* l = m_doc->find(id);
    if (l == nullptr || !l->hasMask() || l->locked())
        return;
    m_doc->commands().push(
        std::make_unique<core::SetLayerMaskCommand>(id, std::nullopt, "Delete Mask"));
    m_maskTarget = false;
    rebuildRows();
    notifyChanged();
}

void LayerPanel::toggleMaskEnabled(core::LayerId id) {
    if (m_doc == nullptr)
        return;
    const core::Layer* l = m_doc->find(id);
    const core::RasterMask* m = l != nullptr ? l->mask() : nullptr;
    if (m == nullptr)
        return;
    m_doc->commands().push(std::make_unique<core::SetMaskEnabledCommand>(id, !m->enabled));
    notifyChanged();     // recomposite: the mask just switched on/off
    refreshThumbnails(); // ... and the group above shows the masked result (see that note)
    redrawList();    // the X overlay (rows read the flag live)
}

void LayerPanel::toggleMaskLinked(core::LayerId id) {
    if (m_doc == nullptr)
        return;
    const core::Layer* l = m_doc->find(id);
    const core::RasterMask* m = l != nullptr ? l->mask() : nullptr;
    if (m == nullptr)
        return;
    m_doc->commands().push(std::make_unique<core::SetMaskLinkedCommand>(id, !m->linked));
    notifyChanged();     // recomposite: linkage changes the fold on a transformed layer
    refreshThumbnails(); // ... which is a different picture in the group above it too
    redrawList();    // the chain glyph
}

void LayerPanel::shiftClickMaskThumbnail(core::LayerId id) {
    // Shift-click the MASK thumbnail -> select the mask's coverage, placed exactly where the
    // compositor folds it (S31); Ctrl/Alt choose the boolean op like the pixel gesture.
    setActive(id);
    if (m_doc == nullptr)
        return;
    const core::Layer* layer = m_doc->find(id);
    if (layer == nullptr)
        return;
    const std::optional<core::Selection> sel =
        core::selectionFromLayerMask(*layer, m_doc->width(), m_doc->height());
    if (!sel) {
        uiLog().debug("shift-click mask of layer {}: no mask to select", id);
        status(_("This layer has no mask to select"));
        return;
    }
    if (!sel->anySelected()) {
        uiLog().debug("shift-click mask of layer {}: coverage-free, nothing to select", id);
        status(_("This mask hides everything — there is no coverage to select"));
        return; // contributing nothing: don't clobber (or pointlessly combine) the selection
    }
    const auto state = Fl::event_state();
    const core::SelectOp op = thumbnailSelectOp((state & FL_CTRL) != 0, (state & FL_ALT) != 0);
    core::Selection combined = core::Selection::combine(m_doc->selection(), *sel, op);
    if (!combined.anySelected())
        combined = core::Selection{}; // an all-zero active mask never lands (S14 semantics)
    if (combined == m_doc->selection())
        return; // a no-op combine isn't worth an undo step
    m_doc->commands().push(std::make_unique<core::SetSelectionCommand>(std::move(combined)));
    notifyChanged(); // the host re-syncs the canvas (selection mask -> marching ants)
}

void LayerPanel::addRasterLayer() {
    if (m_doc == nullptr)
        return;
    commitRename(); // acting on the panel while renaming lands the edit; it does not throw it away
    auto layer = m_doc->makeRaster("Layer " + std::to_string(m_doc->layerCount() + 1));
    const core::LayerId id = layer->id();
    m_doc->commands().push(std::make_unique<core::AddLayerCommand>(
        m_doc->root().id(), m_doc->root().childCount(), std::move(layer)));
    m_active = id; // a freshly added layer becomes active
    rebuildRows();
    notifyChanged();
}

void LayerPanel::deleteActive() {
    if (m_doc == nullptr || m_active == core::kInvalidLayerId || m_doc->find(m_active) == nullptr)
        return;
    if (activeLayerLocked())
        return; // a lock forbids structural edits (the menus check first, so they can explain why)
    cancelRename(); // the layer is about to go away: naming it first would be a pointless undo step
    m_doc->commands().push(std::make_unique<core::RemoveLayerCommand>(m_active));
    selectTopLayer();
    rebuildRows();
    notifyChanged();
}

void LayerPanel::duplicateActive() {
    if (m_doc == nullptr || m_active == core::kInvalidLayerId)
        return;
    commitRename(); // the clone should carry the name the user just typed
    const core::Layer* src = m_doc->find(m_active);
    const std::optional<core::Document::Location> loc = m_doc->locate(m_active);
    if (src == nullptr || !loc)
        return;
    auto clone = m_doc->duplicateLayer(*src);
    const core::LayerId newId = clone->id();
    m_doc->commands().push(std::make_unique<core::AddLayerCommand>(
        loc->parent->id(), loc->index + 1, std::move(clone))); // sits just above the original
    m_active = newId;
    rebuildRows();
    notifyChanged();
}

void LayerPanel::groupActive() {
    if (m_doc == nullptr || m_active == core::kInvalidLayerId)
        return;
    if (activeLayerLocked())
        return; // grouping reparents the layer -- a structural edit (see deleteActive)
    commitRename();
    const std::optional<core::Document::Location> loc = m_doc->locate(m_active);
    if (!loc)
        return;
    auto group = m_doc->makeGroup(_("Group"));
    const core::LayerId groupId = group->id();
    const core::LayerId parentId = loc->parent->id();
    const std::size_t index = loc->index;
    // One undo step: add the (empty) group just above the active layer, then move the active layer
    // into it. After the move the group occupies the active layer's original stack slot.
    auto composite = std::make_unique<core::CompositeCommand>("Group Layers");
    composite->add(std::make_unique<core::AddLayerCommand>(parentId, index + 1, std::move(group)));
    composite->add(std::make_unique<core::MoveLayerCommand>(m_active, groupId, 0));
    m_doc->commands().push(std::move(composite));
    m_active = groupId; // select the new group
    rebuildRows();
    notifyChanged();
}

int LayerPanel::rowIndexOf(core::LayerId id) const {
    if (id == core::kInvalidLayerId)
        return -1;
    for (std::size_t i = 0; i < m_rows.size(); ++i)
        if (m_rows[i]->layerId() == id)
            return static_cast<int>(i);
    return -1;
}

std::vector<core::LayerId> LayerPanel::rowsBetween(core::LayerId fromId, core::LayerId toId) const {
    std::vector<core::LayerId> out;
    const int a = rowIndexOf(fromId);
    const int b = rowIndexOf(toId);
    if (a < 0 || b < 0)
        return out;
    // Walk FROM the anchor TO the clicked row, so `toId` is always out.back() -- see the header:
    // back() is the primary the canvas mirrors into this panel's active row.
    const int step = a <= b ? 1 : -1;
    out.reserve(static_cast<std::size_t>(std::abs(b - a)) + 1);
    for (int i = a;; i += step) {
        out.push_back(m_rows[static_cast<std::size_t>(i)]->layerId());
        if (i == b)
            break;
    }
    return out;
}

void LayerPanel::selectRow(core::LayerId id, RowClick how) {
    if (m_doc == nullptr || m_doc->find(id) == nullptr)
        return;
    std::vector<core::LayerId> next = m_moveSelection;
    // Ids of layers that have since been deleted can linger in a set pushed from the canvas
    // (moveTargets() is documented as raw); a grammar that then "toggled" one of them would be
    // editing a ghost, so the set is validated before every edit.
    std::erase_if(next, [this](core::LayerId l) { return m_doc->find(l) == nullptr; });
    core::LayerId active = id;
    switch (how) {
    case RowClick::Replace:
        next = {id};
        m_selectAnchor = id;
        break;
    case RowClick::Toggle: {
        const auto it = std::find(next.begin(), next.end(), id);
        if (it != next.end()) {
            next.erase(it);
            // Dropping a row hands the ACTIVE slot to whatever is left on top of the set --
            // VulkanCanvas::toggleMoveTarget's own rule, so the two surfaces agree. Dropping the
            // last one leaves the row active but unselected: the single active layer is a separate,
            // always-present thing, not the one-element case of the selection.
            if (!next.empty())
                active = next.back();
        } else {
            next.push_back(id); // appended, so back() is the row you just added
        }
        m_selectAnchor = id;
        break;
    }
    case RowClick::Extend: {
        // The anchor is where the last plain/Ctrl press landed; if that row is no longer displayed
        // (a collapsed group, an undone delete) the active row stands in, and failing that the
        // press degenerates to a plain pick -- never to "select everything".
        core::LayerId anchor = m_selectAnchor;
        if (rowIndexOf(anchor) < 0)
            anchor = m_active;
        if (rowIndexOf(anchor) < 0)
            anchor = id;
        next = rowsBetween(anchor, id);
        if (next.empty())
            next = {id}; // the clicked row is not displayed either: fall back to itself
        break;                  // the anchor deliberately does NOT move: successive Shift-clicks
    }                           // re-sweep from the same row, as every list in the world does
    }

    const bool changed = next != m_moveSelection;
    setMoveSelection(std::move(next)); // rows re-tint + the blend/opacity strip re-gates
    setActive(active);
    if (changed && m_onSelectionChanged)
        m_onSelectionChanged(m_moveSelection); // the host mirrors it onto the canvas's move targets
}

void LayerPanel::rowPressed(core::LayerId id) {
    // Pressing another row is "clicking away": land the pending edit rather than throw it out. (The
    // editor keeps FLTK focus through a click on a non-focusable row, so its FL_UNFOCUS never fires
    // and cannot be relied on here. commitRename() is deletion-free, so this is safe from a row.)
    commitRename();
    // The modifiers decide what the press does to the SELECTION; selectRow also settles the active
    // row (normally `id`, which is the immediate feedback a press has always given, and the set's
    // remaining primary when a Ctrl-press was what dropped `id` out of it).
    const auto state = Fl::event_state();
    const RowClick how = rowClickFor((state & FL_SHIFT) != 0, (state & FL_COMMAND) != 0);
    selectRow(id, how);
    m_dragging = false;
    m_dragOverPlus = false;
    if (how != RowClick::Replace) {
        // A modifier press is a selection EDIT, not a pick-up. Arming the reorder drag here would
        // turn "Ctrl-click to drop a row from the set" into an accidental reparent on the slightest
        // wobble -- and there is no multi-layer MoveLayerCommand for a swept range to issue anyway.
        m_dragId = core::kInvalidLayerId;
        return;
    }
    if (activeLayerLocked()) {
        m_dragId = core::kInvalidLayerId; // a locked layer selects, but never reorders or reparents
        return;
    }
    m_dragId = id;
    m_pressY = Fl::event_y();
    m_dragY = m_pressY;
    m_dragX = Fl::event_x();
}

void LayerPanel::rowDragged() {
    if (m_dragId == core::kInvalidLayerId)
        return;
    m_dragY = Fl::event_y();
    m_dragX = Fl::event_x();
    if (!m_dragging && std::abs(m_dragY - m_pressY) > kDragThreshold) {
        m_dragging = true;
        captureDragGhost(); // build the floating chip once, the moment the drag actually starts
    }
    if (!m_dragging)
        return;
    const bool overPlus = overPlusButton();
    if (overPlus != m_dragOverPlus && m_addButton != nullptr) {
        m_dragOverPlus = overPlus;
        // Filled solid green = "release to clone here". The plus must then read against the green,
        // so it leaves the palette until the drag ends.
        m_addButton->color(overPlus ? fl_rgb_color(56, 150, 92) : toFl(activePalette().controlBg));
        m_addButton->setInk(overPlus ? std::optional<common::Color8>{{255, 255, 255, 255}}
                                     : std::nullopt);
        m_addButton->redraw();
    }
    redraw(); // the drop indicator
}

void LayerPanel::rowReleased() {
    const bool wasDragging = m_dragging;
    const bool toPlus = m_dragOverPlus;
    const int releaseY = m_dragY;
    m_dragging = false; // reset before acting (the actions rebuild the rows)
    m_dragOverPlus = false;
    if (m_addButton != nullptr) {
        m_addButton->color(toFl(activePalette().controlBg));
        m_addButton->setInk(std::nullopt); // back to the palette ink
        m_addButton->redraw();
    }
    if (wasDragging) {
        if (toPlus) {
            duplicateActive(); // dropped on the "+": clone the dragged layer in place
        } else if (m_doc != nullptr && m_dragId != core::kInvalidLayerId) {
            const DropPlan plan = planDrop(releaseY);
            const std::optional<core::Document::Location> loc = m_doc->locate(m_dragId);
            if (plan.valid && loc) {
                const bool sameParent = loc->parent->id() == plan.parentId;
                const std::size_t newIndex = moveIndexFor(plan.endIndex, sameParent, loc->index);
                if (!(sameParent && newIndex == loc->index)) { // not a drop back where it started
                    m_doc->commands().push(std::make_unique<core::MoveLayerCommand>(
                        m_dragId, plan.parentId, newIndex));
                    rebuildRows();
                    notifyChanged();
                }
            }
        }
    }
    m_dragId = core::kInvalidLayerId;
    m_ghostImg.reset(); // release the captured chip (the Fl_RGB_Image views m_ghostChip: order!)
    m_ghostChip = common::Image{};
    m_ghostCardW = m_ghostCardH = 0;
    redraw();
}

namespace {
constexpr int kGhostShadowOff = 3; // the shadow's offset == the image's bottom/right margin
constexpr std::uint8_t kGhostShadowAlpha = 70;
} // namespace

void LayerPanel::captureDragGhost() {
    m_ghostImg.reset();
    m_ghostChip = common::Image{};
    m_ghostCardW = m_ghostCardH = 0;
    if (m_doc == nullptr)
        return;
    const core::Layer* layer = m_doc->find(m_dragId);
    if (layer == nullptr)
        return;

    const Palette& pal = activePalette();
    const std::string name = displayName(*layer);
    // A content-width chip, capped well short of the dock width: even with the drop line drawn on
    // top, a full-width card would blanket the row it is about to land between.
    const int avail = (m_scroll != nullptr ? m_scroll->w() : w() - 2) - 8;
    fl_font(FL_HELVETICA, 13);
    const int content = 6 + kThumb + 10 + static_cast<int>(fl_width(name.c_str())) + 12;
    const int cardW = std::clamp(content, kThumb + 40, std::max(kThumb + 40, (avail * 3) / 5));
    const int cardH = kThumb + 8;
    const int imgW = cardW + kGhostShadowOff;
    const int imgH = cardH + kGhostShadowOff;

    // Paint the card offscreen. FLTK's fl_* primitives are opaque, so the only way to get a chip
    // that the drop line can be drawn over (and that the row beneath shows through) is to render it
    // to an image and blit it with an alpha channel of our own making.
    Fl_Image_Surface surface(imgW, imgH);
    Fl_Surface_Device::push_current(&surface);
    fl_color(toFl(ghostCardColor(pal)));
    fl_rectf(0, 0, cardW, cardH); // card ground
    fl_color(toFl(pal.accent));
    fl_rect(0, 0, cardW, cardH); // accent edge => "picked up"
    const int tx = 6;
    const int ty = (cardH - kThumb) / 2;
    if (const common::Image& thumb = cachedThumbnail(*layer); !thumb.empty())
        fl_draw_image(thumb.rgba.data(), tx, ty, kThumb, kThumb, 4, 0);
    fl_color(toFl(blend8(pal.border, pal.textMuted, 0.45))); // same mild frame as the rows
    fl_rect(tx - 1, ty - 1, kThumb + 2, kThumb + 2);
    fl_color(toFl(pal.text));
    fl_font(FL_HELVETICA, 13);
    const int nx = tx + kThumb + 10;
    fl_push_clip(0, 0, cardW, cardH);
    fl_draw(name.c_str(), nx, 0, cardW - nx - 6, cardH, FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    fl_pop_clip();
    Fl_Surface_Device::pop_current();

    std::unique_ptr<Fl_RGB_Image> rgb(surface.image()); // RGB (no alpha); ours to delete
    if (!rgb || rgb->w() < imgW || rgb->h() < imgH || rgb->array == nullptr)
        return; // an offscreen we cannot read: skip the chip rather than draw garbage
    // On a scaled screen the offscreen comes back at DEVICE resolution, not the imgW x imgH we asked
    // for. Walk the raster's own pixels and map each back to the logical card/shadow regions --
    // reading only the top-left imgW x imgH of a 2x raster would blit a magnified corner of the card.
    const int rasterW = rgb->w();
    const int rasterH = rgb->h();
    const double sx = static_cast<double>(rasterW) / imgW;
    const double sy = static_cast<double>(rasterH) / imgH;
    const int depth = rgb->d();
    const int lineBytes = rgb->ld() != 0 ? rgb->ld() : rasterW * depth;

    common::Image chip(static_cast<std::uint32_t>(rasterW), static_cast<std::uint32_t>(rasterH));
    for (int py = 0; py < rasterH; ++py) {
        for (int px = 0; px < rasterW; ++px) {
            const std::size_t dp = (static_cast<std::size_t>(py) * rasterW + px) * 4;
            const double lx = px / sx; // this device pixel's logical position in the chip
            const double ly = py / sy;
            const bool onCard = lx < cardW && ly < cardH;
            const bool onShadow =
                !onCard && lx >= kGhostShadowOff && ly >= kGhostShadowOff;
            if (onCard) {
                const unsigned char* s = rgb->array + static_cast<std::size_t>(py) * lineBytes +
                                         static_cast<std::size_t>(px) * depth;
                chip.rgba[dp + 0] = s[0];
                chip.rgba[dp + 1] = s[1];
                chip.rgba[dp + 2] = s[2];
                chip.rgba[dp + 3] = kGhostAlpha;
            } else if (onShadow) {
                chip.rgba[dp + 0] = chip.rgba[dp + 1] = chip.rgba[dp + 2] = 0;
                chip.rgba[dp + 3] = kGhostShadowAlpha; // the offset shadow reads as "floating"
            } else {
                chip.rgba[dp + 3] = 0; // the L-shaped notch above/left of the shadow
            }
        }
    }
    m_ghostChip = std::move(chip);
    m_ghostCardW = cardW; // the CARD's size stays logical: it positions the chip and the knob test
    m_ghostCardH = cardH;
    m_ghostImg = std::make_unique<Fl_RGB_Image>(m_ghostChip.rgba.data(), rasterW, rasterH, 4);
    // Draw the (possibly high-res) raster at its logical size, so it stays crisp on a scaled screen.
    m_ghostImg->scale(imgW, imgH, /*proportional=*/0, /*can_expand=*/1);
}

LayerPanel::PixelRect LayerPanel::ghostCardRect() const {
    // Top-left anchored to the cursor, nudged clear of the arrow's hotspot; clamped to the dock,
    // which is also the only place a drop can land, so confinement costs the gesture nothing.
    const int gx = std::clamp(m_dragX + kGhostCursorDX, x() + 4, x() + w() - 4 - m_ghostCardW);
    const int gy = std::clamp(m_dragY + kGhostCursorDY, y() + 2, y() + h() - 2 - m_ghostCardH);
    return {gx, gy, m_ghostCardW, m_ghostCardH};
}

// The knob's anti-aliased edge has to know what it sits on. Inside the chip that is the card seen
// through kGhostAlpha; elsewhere it is the row's own fill (rows paint their whole rect).
common::Color8 LayerPanel::colorUnderKnob(int px, int py) const {
    const Palette& pal = activePalette();
    common::Color8 beneath = pal.panelBg;
    for (const LayerRow* row : m_rows)
        if (py >= row->y() && py < row->y() + kRowH) {
            // Same ramp the row itself paints (hover is irrelevant mid-drag: the pointer is on the
            // ghost, and a row under it is not tracking FL_ENTER anyway).
            beneath = layerRowBackground(pal, /*enabled=*/true, row->layerId() == m_active,
                                         multiSelectActive() && isInMoveSelection(row->layerId()),
                                         /*hover=*/false);
            break;
        }
    if (m_ghostImg) {
        const PixelRect card = ghostCardRect();
        if (px >= card.x && px < card.x + card.w && py >= card.y && py < card.y + card.h)
            return ghostOverColor(ghostCardColor(pal), beneath);
    }
    return beneath;
}

void LayerPanel::drawDragGhost() {
    if (!m_ghostImg)
        return; // capture found no document/layer/offscreen: nothing to float
    const PixelRect card = ghostCardRect();
    m_ghostImg->draw(card.x, card.y);
}

bool LayerPanel::isSelfOrDescendant(core::LayerId ancestor, core::LayerId node) const {
    if (m_doc == nullptr)
        return false;
    for (core::Layer* l = m_doc->find(node); l != nullptr; l = l->parent())
        if (l->id() == ancestor)
            return true;
    return false;
}

LayerPanel::DropPlan LayerPanel::planDrop(int eventY) const {
    DropPlan plan;
    if (m_doc == nullptr || m_scroll == nullptr)
        return plan;
    const int n = static_cast<int>(m_rows.size());
    const core::LayerId rootId = m_doc->root().id();
    // `groupRow` is the group the layer would JOIN (invalid => the top level): the draw rings it,
    // so in-group vs out-of-group is never ambiguous. A position line is always drawn too.
    const auto setGroupRow = [&](core::LayerId parent) {
        plan.groupRow = parent != rootId ? parent : core::kInvalidLayerId;
    };

    // 1) Mid-band over a group row (not the dragged subtree) => drop INTO it, ON TOP of its children.
    for (int i = 0; i < n; ++i) {
        const LayerRow* row = m_rows[i];
        if (eventY < row->y() || eventY >= row->y() + kRowH)
            continue;
        const core::Layer* l = m_doc->find(row->layerId());
        const auto* g = l != nullptr ? l->as<core::GroupLayer>() : nullptr;
        const bool midBand = eventY >= row->y() + kRowH / 4 && eventY < row->y() + (kRowH * 3) / 4;
        if (g != nullptr && midBand && !isSelfOrDescendant(m_dragId, row->layerId())) {
            plan.valid = true;
            plan.parentId = row->layerId();
            plan.endIndex = g->childCount();    // on top of the group's children
            plan.groupRow = row->layerId();     // ring the group => "joins this group"
            plan.lineDepth = row->depth() + 1;  // line sits at the top of the children
            plan.lineY = row->y() + kRowH;
            return plan;
        }
        break; // over a non-group row (or an edge band): fall through to gap insertion
    }

    // 2) Insert at the gap nearest the cursor.
    int gap = 0;
    for (const LayerRow* row : m_rows)
        if (row->y() + kRowH / 2 < eventY)
            ++gap;
    const int aboveDepth = gap > 0 ? m_rows[gap - 1]->depth() : -1;
    const int belowDepth = gap < n ? m_rows[gap]->depth() : 0;

    // A gap at a group EXIT (the row above is nested deeper than the row below) is a CHOICE: drop at
    // the bottom of the deeper group, or step out one or more levels to land just below the group(s)
    // -- both were otherwise hard to reach. The cursor's horizontal indent picks the target depth,
    // and the indented line + ringed group make the choice legible as you slide left/right.
    if (gap > 0 && aboveDepth > belowDepth) {
        const int cursorDepth = std::max(0, (Fl::event_x() - (m_scroll->x() + 2)) / kIndent);
        const int targetDepth = std::clamp(cursorDepth, belowDepth, aboveDepth);
        std::optional<core::Document::Location> loc = m_doc->locate(m_rows[gap - 1]->layerId());
        int d = aboveDepth;
        while (loc && d > targetDepth) { // step out a level: the parent group becomes the anchor
            loc = m_doc->locate(loc->parent->id());
            --d;
        }
        if (loc && !isSelfOrDescendant(m_dragId, loc->parent->id())) {
            plan.parentId = loc->parent->id();
            // Deepest target: the bottom of the anchor's own parent group. Stepped out: just below
            // the group we stepped out of, in ITS parent (the group keeps its place, we land under).
            plan.endIndex = d == aboveDepth ? 0 : loc->index;
            plan.lineDepth = targetDepth;
            plan.lineY = m_rows[gap - 1]->y() + kRowH;
            plan.valid = true;
            setGroupRow(plan.parentId);
            return plan;
        }
    }

    // 3) A plain gap: just above the row below it (in that row's parent), or the root bottom past the end.
    if (gap < n) {
        const std::optional<core::Document::Location> loc = m_doc->locate(m_rows[gap]->layerId());
        if (loc) {
            plan.parentId = loc->parent->id();
            plan.endIndex = loc->index + 1; // just above `below` in the stack
            plan.lineDepth = m_rows[gap]->depth();
            plan.lineY = m_rows[gap]->y();
            plan.valid = true;
        }
    } else {
        plan.parentId = rootId;
        plan.endIndex = 0; // below everything: the bottom of the root stack
        plan.lineDepth = 0;
        plan.lineY = n > 0 ? m_rows[n - 1]->y() + kRowH : m_scroll->y();
        plan.valid = true;
    }
    if (plan.valid && isSelfOrDescendant(m_dragId, plan.parentId))
        plan.valid = false; // can't drop a layer into itself or its own subtree
    if (plan.valid)
        setGroupRow(plan.parentId);
    return plan;
}

bool LayerPanel::overPlusButton() const {
    if (m_addButton == nullptr)
        return false;
    const int ex = Fl::event_x();
    const int ey = Fl::event_y();
    return ex >= m_addButton->x() && ex < m_addButton->x() + m_addButton->w() &&
           ey >= m_addButton->y() && ey < m_addButton->y() + m_addButton->h();
}

void LayerPanel::draw() {
    Panel::draw(); // themed panel fill + border + children (controls, scroll, buttons)

    const Palette& pal = activePalette();
    // These captions/labels are drawn straight onto the panel ground, so they may ONLY be drawn
    // when that ground was just cleared -- i.e. on a full redraw. Fl_Group::draw() clears the box
    // only when damage is more than FL_DAMAGE_CHILD; a child-only redraw (a row hover, a thumbnail
    // refresh mid-drag) would otherwise re-stamp the text over itself, anti-aliasing it heavier
    // each pass (the "labels get bolder" report -- the same class as the old scrolling-label bug).
    const bool fullRedraw = (damage() & ~FL_DAMAGE_CHILD) != 0;

    if (fullRedraw) {
        // The tab strip (S16-b): Layers | History. The active tab gets full-strength text and the
        // accent underline; the other is muted and clickable (LayerPanel::handle).
        fl_font(FL_HELVETICA_BOLD, 13);
        const char* tabLabels[3] = {_("Layers"), _("History"), _("Channels")};
        const DockTab tabIds[3] = {DockTab::Layers, DockTab::History, DockTab::Channels};
        for (int i = 0; i < 3; ++i) {
            const auto [x0, x1] = tabSpan(i);
            const bool active = tabIds[i] == m_tab;
            fl_color(toFl(active ? pal.text : pal.textMuted));
            fl_draw(tabLabels[i], x0, y(), x1 - x0 + 4, kHeaderH, FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
            if (active) {
                fl_color(toFl(pal.accent));
                fl_line(x0, y() + kHeaderH - 2, x1, y() + kHeaderH - 2);
            }
        }
    }

    if (m_tab != DockTab::Layers)
        return; // the History child draws its own body; no Layers chrome

    if (fullRedraw) {
        // Properties-strip captions + the live opacity readout (the controls are real widgets).
        const bool haveActive = m_doc != nullptr && m_active != core::kInvalidLayerId &&
                                m_doc->find(m_active) != nullptr;
        fl_font(FL_HELVETICA, 12);
        fl_color(toFl(haveActive ? pal.textMuted : pal.border));
        fl_draw(_("Blend"), x() + 10, y() + kHeaderH + 8, kCaptionW - 6, 22,
                FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        fl_draw(_("Opacity"), x() + 10, y() + kHeaderH + 8 + 28, kCaptionW - 6, 18,
                FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        // The readout sits in the kReadoutW column, vertically aligned to the opacity slider's
        // centre (sliderY/sliderH mirror the m_opacitySlider geometry), centred horizontally.
        const int rx = x() + w() - kReadoutW - 6;
        const int sliderY = y() + kHeaderH + 8 + 28; // == m_opacitySlider Y
        const int sliderH = 20;                       // == m_opacitySlider H
        const bool showMixed = multiSelectActive() && (m_multiMode == MultiSelectMode::Disabled ||
                                                        m_multiMode == MultiSelectMode::All);
        if (showMixed) {
            // Disabled + All show the selection's AVERAGE opacity. A leading "~" (ASCII, so no host-
            // font glyph risk) marks it approximate while the layers' opacities differ; once they
            // match (e.g. after an All-mode edit) the "~" drops and it reads as an exact value. The
            // slider sits at the average (syncProperties). Disabled is inert (muted readout); All is
            // live, so its readout reads as a normal value (text colour).
            char pct[12];
            std::snprintf(
                pct, sizeof(pct), "%s%d%%", selectionOpacitiesMixed() ? "~" : "",
                static_cast<int>(std::lround(
                    (m_opacitySlider != nullptr ? m_opacitySlider->value() : 1.0) * 100.0)));
            fl_color(toFl(m_multiMode == MultiSelectMode::All ? pal.text : pal.textMuted));
            fl_draw(pct, rx, sliderY, kReadoutW, sliderH, FL_ALIGN_CENTER);
        } else if (haveActive && m_opacitySlider != nullptr) {
            char pct[8];
            std::snprintf(pct, sizeof(pct), "%d%%",
                          static_cast<int>(std::lround(m_opacitySlider->value() * 100.0)));
            fl_color(toFl(pal.text));
            fl_draw(pct, rx, sliderY, kReadoutW, sliderH, FL_ALIGN_CENTER);
        }

        if (m_rows.empty() && m_scroll != nullptr) {
            fl_color(toFl(pal.textMuted));
            fl_draw(m_doc != nullptr ? _("No layers") : _("No document"), m_scroll->x(),
                    m_scroll->y(), m_scroll->w(), m_scroll->h(), FL_ALIGN_CENTER);
        }
    }

    // Drag feedback. The New Layer button is a clone drop-target for the whole gesture, so it gets a
    // green rectangular outline the instant the drag begins; once the pointer is actually over it the
    // button fills solid green (set in rowDragged) and the now-redundant outline is dropped.
    // Otherwise we show where a reorder/reparent would land: two tied-together accent cues -- a
    // RING around the group the layer would join (none => the top level), and an indented POSITION
    // line with a round knob at its start, so "in the group vs just outside it" is unmistakable.
    if (m_dragging) {
        if (m_addButton != nullptr && !m_dragOverPlus) {
            fl_color(fl_rgb_color(56, 150, 92));
            const int bx = m_addButton->x() - 2;
            const int by = m_addButton->y() - 2;
            fl_rect(bx, by, m_addButton->w() + 4, m_addButton->h() + 4);
            fl_rect(bx + 1, by + 1, m_addButton->w() + 2, m_addButton->h() + 2);
        }
        // ORDER MATTERS: the chip goes down FIRST and the drop cues on top of it. The chip is
        // translucent, so the line and knob stay legible where they cross it, and the line no
        // longer has to dodge the card (the old narrow-ghost compromise) to be seen at all.
        drawDragGhost();
        if (!m_dragOverPlus && m_scroll != nullptr && !m_rows.empty()) {
            const DropPlan plan = planDrop(m_dragY);
            if (plan.valid) {
                const int sx = m_scroll->x();
                const int sw = m_scroll->w();
                const Fl_Color accent = toFl(pal.accent);
                // (a) Membership: a SINGLE 1-px accent border on the group being joined -- a quiet
                //     "lands in here" (absent => the top level). Subtler than the old double ring.
                if (plan.groupRow != core::kInvalidLayerId) {
                    for (const LayerRow* row : m_rows) {
                        if (row->layerId() != plan.groupRow)
                            continue;
                        const int ry = std::clamp(row->y(), m_scroll->y(),
                                                  m_scroll->y() + m_scroll->h() - kRowH);
                        fl_color(accent);
                        fl_rect(sx + 2, ry, sw - 4, kRowH);
                        break;
                    }
                }
                // (b) Position: a slim 2-px accent line at the target indent, with a round START
                //     KNOB at its left end (restored: it is what makes the indent -- and therefore
                //     the target group -- readable at a glance). AA'd against whatever it lands on.
                const int x0 = sx + 4 + plan.lineDepth * kIndent;
                const int lineY = std::clamp(plan.lineY, m_scroll->y() + 1,
                                             m_scroll->y() + m_scroll->h() - 2);
                fl_color(accent);
                fl_rectf(x0, lineY, sx + sw - 4 - x0, 2);
                constexpr double kKnobR = 4.0;
                const int kcx = x0;
                const int kcy = lineY + 1;
                drawAAPrims(kcx - 5, kcy - 5, 11, 11,
                            [this](int px, int py) { return colorUnderKnob(px, py); },
                            {{static_cast<double>(kcx), static_cast<double>(kcy), kKnobR, 0.0,
                              pal.accent}});
            }
        }
    }
}

namespace {
void cbBlend(Fl_Widget*, void* p) { static_cast<LayerPanel*>(p)->onBlendChanged(); }
void cbOpacity(Fl_Widget*, void* p) { static_cast<LayerPanel*>(p)->onOpacityChanged(); }
} // namespace

} // namespace mosaic::ui
