#include "ui/brush_editor.hpp"

#include "common/i18n.hpp"
#include "common/log.hpp"
#include "core/brush/stroke_preview.hpp"
#include "io/brush/preset_brush.hpp"
#include "common/settings.hpp"      // dataDir(): where a user preset lives, and where Export starts
#include "platform/file_dialog.hpp" // the app's ONE picker: portal / kdialog / native (see doImport)
#include "ui/ask_or_tell_dialog.hpp" // the delete confirmation
#include "ui/brush_preset_panel.hpp" // presetDisplayName + presetStrokeStyle (one preview, one style)
#include "ui/brush_presets.hpp"
#include "ui/curve_editor.hpp"
#include "ui/scrub_slider.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <FL/Enumerations.H>
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/Fl_Scroll.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>

namespace mosaic::ui {

namespace cb = core::brush;

namespace {

// ---- Geometry ---------------------------------------------------------------------------------
// Pinned constants, because the dialog is a WINDOW and an unshown Fl_Window renders BLACK to an
// Fl_Image_Surface -- so this layout cannot be screenshot headlessly and the only way a test can
// hold it still is by asserting on the numbers themselves (tests/test_brush_editor.cpp).
constexpr int kWinW = 1040;
constexpr int kWinH = 640;
constexpr int kHeaderH = 68;
constexpr int kFooterH = 52;
constexpr int kRailW = 250;
constexpr int kPreviewW = 320;
constexpr int kPad = 12;
constexpr int kRowH = 24;
constexpr int kRowGap = 6;
constexpr int kLabelW = 96;   // the control caption column on an option page
constexpr int kFieldW = 216;  // ... and the control beside it
constexpr int kThumbPx = 52;  // the header's stroke thumbnail (a square)
constexpr int kListRowH = 22; // one row of the OPTION list

// ⭐ THE PRESET LIST'S ROW CARRIES THE BRUSH'S OWN STROKE (feedback round 1: "Presets have no
// previews unlike the dock"). A name is not what a brush looks like -- that is the finding the dock's
// Cards mode was built on (§8.2), and a browse list inside the EDITOR had no business restating the
// question the dock already answered. Same renderer, same rules, one level down.
constexpr int kPresetRowH = 46;
constexpr int kPresetNameH = 15;  // the name line over the strip
constexpr int kPresetStripH = 26; // ... and the strip itself
constexpr int kPresetStripPad = 6;
// The strip's own diameter ceiling, at the dock card's ratio (28 px of nib in a 58 px strip). A
// ceiling near the strip's own height leaves the S-curve no room to swing and flattens it into a fat
// straight sausage (§8.2).
constexpr double kPresetStripMaxDiameter = 12.0;

// The rail is two lists stacked: presets on top, options below, with a caption over each.
constexpr int kPresetListH = 190;
constexpr int kCaptionH = 18;

// The fidelity badge's inks, the dock's exactly (brush_preset_panel.cpp): one fact, two surfaces.
constexpr common::Color8 kApproxInk{214, 158, 46, 255};
constexpr common::Color8 kSubstInk{198, 84, 72, 255};

// The preview surface's own diameter ceiling. A preset may author a 1000 px nib, and a 1000 px tip
// rasterizes a megapixel mask PER DAB into a 320 px pane that a big blob and a bigger blob look
// identical in -- the dock's card rule (stroke_preview.hpp), at this pane's scale.
constexpr double kPreviewMaxDiameter = 160.0;

// One coalescing tick. Long enough that a slider drag rebuilds once per frame rather than once per
// motion event, short enough that letting go feels immediate.
constexpr double kPreviewCoalesceS = 1.0 / 30.0;

Fl_Color toFl(common::Color8 c) {
    return fl_rgb_color(c.r, c.g, c.b);
}

// ⚠ Fl_Menu_::add() parses '/' as a submenu path, '&' as a mnemonic and '\' as its own escape, so a
// translated label carrying any of them silently becomes a submenu (or loses a character). Every
// label this file puts in a Dropdown goes through here first.
[[nodiscard]] std::string menuSafe(std::string_view label) {
    std::string out;
    out.reserve(label.size() + 4);
    for (const char c : label) {
        if (c == '/' || c == '\\' || c == '&')
            out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

struct Binding {
    std::function<void()> fn;
};
void controlThunk(Fl_Widget*, void* b) {
    if (auto* bb = static_cast<Binding*>(b))
        bb->fn();
}

// ---- The rail's two lists ----------------------------------------------------------------------
//
// ONE widget each, not one per row -- the preset grid's rule (§8.2) at a smaller scale, and for the
// same two reasons: a widget per row would rebuild the whole child list every time the working copy
// changes, and it would hand "which row is under the cursor" to FLTK's clipping instead of to
// arithmetic a test can drive.

class EditorPresetList : public Fl_Widget {
public:
    EditorPresetList(int X, int Y, int W, int H) : Fl_Widget(X, Y, W, H) {}

    void setStore(const BrushPresetStore* store) {
        if (store == m_store)
            return;
        m_store = store;
        dropStrokes();
    }
    void setItems(std::vector<std::string> labels, std::vector<int> indices) {
        m_labels = std::move(labels);
        m_indices = std::move(indices);
        redraw();
    }
    void setActive(int presetIndex) {
        m_active = presetIndex;
        redraw();
    }
    void setOnPick(std::function<void(int)> f) { m_onPick = std::move(f); }
    [[nodiscard]] int rowAt(int localY) const {
        if (localY < 0)
            return -1;
        const int row = localY / kPresetRowH;
        return row < static_cast<int>(m_labels.size()) ? row : -1;
    }
    [[nodiscard]] int contentHeight() const {
        return static_cast<int>(m_labels.size()) * kPresetRowH;
    }
    // Scroll the active row into view within `view` (the ScrollView the list lives in).
    [[nodiscard]] int activeRowTop() const {
        for (std::size_t i = 0; i < m_indices.size(); ++i)
            if (m_indices[i] == m_active)
                return static_cast<int>(i) * kPresetRowH;
        return -1;
    }

    // ⚠⚠ EVENTS, NEVER A CACHE SIZE. A re-render refills the cache to exactly the size it had, so a
    // size assertion cannot witness one -- the §8.2 lesson, already paid for twice in the dock.
    [[nodiscard]] std::size_t strokeRenders() const noexcept { return m_renders; }

    // The store changed under the list (a save, an import, a delete) or the theme did: every cached
    // stroke is now for the wrong preset index or the wrong paper. Dropped WHOLE, deliberately --
    // a per-index fixup would have to know exactly where the insertion happened, which is the same
    // rule in two places (the dock's refreshStore makes the identical choice).
    void dropStrokes() {
        m_strokes.clear();
        m_params.clear();
        m_stripW = 0;
        redraw();
    }

protected:
    void draw() override {
        const Palette& p = activePalette();
        fl_color(toFl(p.panelBg));
        fl_rectf(x(), y(), w(), h());
        for (std::size_t i = 0; i < m_labels.size(); ++i) {
            const int ry = y() + static_cast<int>(i) * kPresetRowH;
            // ⚠ THE LAZY-RENDER GATE, and it is what makes this affordable at all: only the four
            // rows the 190 px list can actually show ask for a stroke, at ~1.0-1.7 ms each. A list
            // that rendered all 114 on open would stall the editor for two tenths of a second.
            if (!fl_not_clipped(x(), ry, w(), kPresetRowH))
                continue;
            drawRow(static_cast<int>(i), ry, p);
        }
    }
    int handle(int e) override {
        switch (e) {
        case FL_ENTER:
            return 1;
        case FL_MOVE: {
            const int row = rowAt(Fl::event_y() - y());
            if (row != m_hover) {
                m_hover = row;
                redraw();
            }
            return 1;
        }
        case FL_LEAVE:
            m_hover = -1;
            redraw();
            return 1;
        case FL_PUSH:
            return 1; // claim the PUSH so the RELEASE below is ours (the PUSH/RELEASE pair rule)
        case FL_RELEASE: {
            const int row = rowAt(Fl::event_y() - y());
            if (row >= 0 && m_onPick)
                m_onPick(m_indices[static_cast<std::size_t>(row)]);
            return 1;
        }
        default:
            break;
        }
        return Fl_Widget::handle(e);
    }

private:
    void drawRow(int row, int ry, const Palette& p) {
        const auto i = static_cast<std::size_t>(row);
        const int index = m_indices[i];
        const bool sel = index == m_active;
        common::Color8 ground = p.panelBg;
        if (sel) {
            ground = p.controlActive;
        } else if (row == m_hover) {
            ground = p.controlHover;
        }
        if (sel || row == m_hover) {
            fl_color(toFl(ground));
            fl_rectf(x(), ry, w(), kPresetRowH);
        }

        // ⭐ THE USER BADGE, the dock's mark at the rail's scale (feedback round 1, item 3): an
        // accent RING, never the fidelity dot's amber/red filled disc. Same ink, same shape, same
        // meaning in both places -- one convention, two surfaces.
        int textX = x() + kPresetStripPad;
        const bool mine = m_store != nullptr && m_store->isUserPreset(index);
        if (mine) {
            const double bcx = x() + kPresetStripPad + 5.0;
            const double bcy = ry + 1.0 + kPresetNameH / 2.0;
            const auto under = [ground](int, int) { return ground; };
            drawAAPrims(static_cast<int>(bcx) - 6, static_cast<int>(bcy) - 6, 13, 13, under,
                        {{bcx, bcy, 3.6, 1.8, p.accent}});
            textX += 14;
        }

        fl_font(FL_HELVETICA, 11);
        fl_color(toFl(sel ? p.text : p.textMuted));
        const int textW = x() + w() - kPresetStripPad - textX;
        const std::string text = ellipsizeToWidth(m_labels[i], textW);
        fl_draw(text.c_str(), textX, ry + 1, textW, kPresetNameH, FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

        // ⚠ RENDERED WIDE, DRAWN CROPPED -- never scaled (§8.2). The strip is rendered at the next
        // 32 px bucket up and the MIDDLE of it is blitted in. A scaled stroke lies about the brush's
        // size, which is the one thing a preview exists to be honest about; and bucketing is what
        // keeps a re-layout from re-running the engine, at ~1.0-1.7 ms a row.
        const int sx = x() + kPresetStripPad;
        const int sy = ry + 1 + kPresetNameH + 1;
        const int sw = std::max(0, w() - 2 * kPresetStripPad);
        if (sw <= 0)
            return;
        const int renderW = presetStrokeRenderWidth(sw);
        if (Fl_RGB_Image* img = strokeFor(index, renderW); img != nullptr)
            img->draw(sx, sy, sw, kPresetStripH, (renderW - sw) / 2, 0);
        fl_color(toFl(p.border)); // the strip is a little canvas: frame it, like the dock's
        fl_rect(sx, sy, sw, kPresetStripH);
    }

    // Build-once params for a preset. ⚠ `presetBrushParams` MINTS THE TIP'S RASTER ID, and a fresh
    // id is a permanently cold dab cache -- so this happens once per preset, never once per render
    // and certainly never once per draw().
    [[nodiscard]] const cb::BrushParams* paramsFor(int index) {
        if (const auto it = m_params.find(index); it != m_params.end())
            return &it->second;
        if (m_store == nullptr)
            return nullptr;
        const io::brush::LibraryPreset* lp = m_store->presetAt(index);
        if (lp == nullptr)
            return nullptr;
        const auto [it, inserted] = m_params.emplace(index, io::brush::presetBrushParams(*lp));
        return &it->second;
    }

    [[nodiscard]] Fl_RGB_Image* strokeFor(int index, int renderWidth) {
        if (renderWidth <= 0)
            return nullptr;
        if (renderWidth != m_stripW) { // the size is part of the KEY, not part of the entry
            m_strokes.clear();
            m_stripW = renderWidth;
        }
        if (const auto it = m_strokes.find(index); it != m_strokes.end())
            return it->second.img.get();

        const cb::BrushParams* params = paramsFor(index);
        if (params == nullptr)
            return nullptr;
        cb::StrokePreviewStyle style =
            presetStrokeStyle(activePalette(), params->strokeMode == cb::StrokeMode::Erase);
        style.maxDiameter = kPresetStripMaxDiameter;

        Stroke entry;
        entry.pixels = cb::renderStrokePreview(*params, renderWidth, kPresetStripH, style);
        ++m_renders; // an EVENT, and the only thing a test can honestly assert on
        entry.img = std::make_unique<Fl_RGB_Image>(entry.pixels.rgba.data(),
                                                   static_cast<int>(entry.pixels.width),
                                                   static_cast<int>(entry.pixels.height), 4);
        Fl_RGB_Image* raw = entry.img.get();
        m_strokes.emplace(index, std::move(entry));
        return raw;
    }

    struct Stroke {
        common::Image pixels;
        std::unique_ptr<Fl_RGB_Image> img; // views `pixels`: stored and dropped together
    };

    const BrushPresetStore* m_store = nullptr;
    std::vector<std::string> m_labels;
    std::vector<int> m_indices;
    std::function<void(int)> m_onPick;
    std::unordered_map<int, cb::BrushParams> m_params;
    std::unordered_map<int, Stroke> m_strokes;
    int m_stripW = 0;          // the render width m_strokes were laid at (0 = nothing cached)
    std::size_t m_renders = 0; // EVENTS, never reset: see strokeRenders()
    int m_active = -1;
    int m_hover = -1;
};

// The CHECKABLE option list. A row's checkbox is exactly the format's `Pressure{X}` bit -- "this
// option is enabled" -- which the engine already reads through CurveOption::isChecked(); the box
// writes nothing new into the model, it exposes a bit that was always there (§3.2).
class EditorOptionList : public Fl_Widget {
public:
    EditorOptionList(int X, int Y, int W, int H) : Fl_Widget(X, Y, W, H) {}

    void setRows(std::vector<BrushEditorRow> rows) {
        m_rows = std::move(rows);
        redraw();
    }
    void setPage(std::string_view base) {
        m_page.assign(base);
        redraw();
    }
    void setOnPick(std::function<void(const std::string&)> f) { m_onPick = std::move(f); }
    void setOnCheck(std::function<void(const std::string&, bool)> f) { m_onCheck = std::move(f); }
    [[nodiscard]] int contentHeight() const { return static_cast<int>(m_rows.size()) * kListRowH; }
    [[nodiscard]] int rowAt(int localY) const {
        if (localY < 0)
            return -1;
        const int row = localY / kListRowH;
        return row < static_cast<int>(m_rows.size()) ? row : -1;
    }
    // The checkbox's square, in widget-local coords. Exposed so a test can hit it without guessing.
    [[nodiscard]] static int checkBoxSize() { return 13; }
    [[nodiscard]] static int checkBoxLeft() { return 8; }

protected:
    void draw() override {
        const Palette& p = activePalette();
        fl_color(toFl(p.panelBg));
        fl_rectf(x(), y(), w(), h());
        for (std::size_t i = 0; i < m_rows.size(); ++i) {
            const BrushEditorRow& r = m_rows[i];
            const int ry = y() + static_cast<int>(i) * kListRowH;
            if (!fl_not_clipped(x(), ry, w(), kListRowH))
                continue;
            if (r.header) {
                fl_font(FL_HELVETICA_BOLD, 10);
                fl_color(toFl(p.textMuted));
                fl_draw(brushOptionGroupLabel(r.group).c_str(), x() + 6, ry + 2, w() - 12,
                        kListRowH - 2, FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
                continue;
            }
            const bool sel = r.base == m_page;
            if (sel) {
                fl_color(toFl(p.controlActive));
                fl_rectf(x(), ry, w(), kListRowH);
            } else if (static_cast<int>(i) == m_hover) {
                fl_color(toFl(p.controlHover));
                fl_rectf(x(), ry, w(), kListRowH);
            }
            const int box = checkBoxSize();
            const int bx = x() + checkBoxLeft();
            const int by = ry + (kListRowH - box) / 2;
            if (r.checkable) {
                fl_color(toFl(r.checked ? p.accent : p.controlBg));
                fl_rectf(bx, by, box, box);
                fl_color(toFl(p.border));
                fl_rect(bx, by, box, box);
                if (r.checked) {
                    fl_color(toFl(p.onAccent));
                    fl_line_style(FL_SOLID, 2);
                    fl_line(bx + 3, by + box / 2, bx + box / 2 - 1, by + box - 4);
                    fl_line(bx + box / 2 - 1, by + box - 4, bx + box - 3, by + 3);
                    fl_line_style(0);
                }
            }
            fl_font(FL_HELVETICA, 11);
            fl_color(toFl(sel ? p.text : (r.checked || !r.checkable ? p.text : p.textMuted)));
            const int tx = bx + box + 6;
            const std::string label = r.base == kBrushTipPage ? std::string(_("Tip & spacing"))
                                                              : brushOptionLabel(r.base);
            fl_draw(ellipsizeToWidth(label, x() + w() - 6 - tx).c_str(), tx, ry,
                    x() + w() - 6 - tx, kListRowH, FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        }
    }
    int handle(int e) override {
        switch (e) {
        case FL_ENTER:
            return 1;
        case FL_MOVE: {
            const int row = rowAt(Fl::event_y() - y());
            const int hover = row >= 0 && !m_rows[static_cast<std::size_t>(row)].header ? row : -1;
            if (hover != m_hover) {
                m_hover = hover;
                redraw();
            }
            return 1;
        }
        case FL_LEAVE:
            m_hover = -1;
            redraw();
            return 1;
        case FL_PUSH:
            return 1;
        case FL_RELEASE: {
            const int row = rowAt(Fl::event_y() - y());
            if (row < 0)
                return 1;
            const BrushEditorRow& r = m_rows[static_cast<std::size_t>(row)];
            if (r.header)
                return 1;
            const int lx = Fl::event_x() - x();
            // The checkbox toggles; anywhere else on the row opens the page. Two targets on one row
            // rather than a row that both selects AND toggles: switching pages must not be able to
            // switch an option off by accident.
            if (r.checkable && lx >= checkBoxLeft() && lx < checkBoxLeft() + checkBoxSize()) {
                if (m_onCheck)
                    m_onCheck(r.base, !r.checked);
            } else if (m_onPick) {
                m_onPick(r.base);
            }
            return 1;
        }
        default:
            break;
        }
        return Fl_Widget::handle(e);
    }

private:
    std::vector<BrushEditorRow> m_rows;
    std::string m_page;
    std::function<void(const std::string&)> m_onPick;
    std::function<void(const std::string&, bool)> m_onCheck;
    int m_hover = -1;
};

// ---- The preview surface -----------------------------------------------------------------------
//
// ONE canvas, two jobs (§8.3): it shows the AUTO STROKE that `core::brush::stroke_preview` lays --
// the same renderer the dock's cards and the chip use, because two previews of one brush that
// disagreed would be worse than no preview at all -- and it is PAINTABLE, so the brush being edited
// can be tried on something before the artwork is.
//
// ⚠ It is a plain CPU `common::Image`. It never becomes a layer, never enters the undo stack and
// never reaches the document; the only thing that leaves this widget is what the eye takes off it.
class ScratchCanvas : public Fl_Widget {
public:
    ScratchCanvas(int X, int Y, int W, int H) : Fl_Widget(X, Y, W, H) {}
    ~ScratchCanvas() override {
        // A stroke still in flight when the dialog closes is simply dropped -- there is nothing
        // downstream of this surface for it to reach.
        if (m_engine.active())
            m_engine.restore();
    }

    // What the pen is doing right now (feedback round 1, item 1). Null = mouse.
    void setTabletReader(std::function<BrushEditorTabletSample()> f) { m_tablet = std::move(f); }
    // Did the last stroke on this surface come from a real device? The tooltip says so, because
    // "why is my pressure curve not doing anything" has exactly two answers and this is the one the
    // user cannot see.
    [[nodiscard]] bool lastStrokeWasPen() const noexcept { return m_lastStrokeWasPen; }

    // New params. Re-renders the auto stroke ONLY while the surface still IS the auto stroke: once
    // the user has painted on it, it is a scratchpad, and throwing their marks away on every tick of
    // a slider drag would make the scratchpad useless exactly when it is most wanted. `Reset` is how
    // the auto stroke comes back, and it comes back rendered with the settings as they now stand.
    void setParams(std::shared_ptr<const cb::BrushParams> params) {
        m_params = std::move(params);
        if (!m_painted)
            renderAuto();
    }
    void reset() {
        m_painted = false;
        renderAuto();
    }
    [[nodiscard]] bool painted() const noexcept { return m_painted; }
    [[nodiscard]] std::size_t renders() const noexcept { return m_renders; }

    void resize(int X, int Y, int W, int H) override {
        const bool sizeChanged = W != w() || H != h();
        Fl_Widget::resize(X, Y, W, H);
        if (sizeChanged) {
            m_painted = false; // the surface's pixels do not survive a resize; the stroke does
            renderAuto();
        }
    }

protected:
    void draw() override {
        const Palette& p = activePalette();
        if (m_img.empty() || m_rgb.empty()) {
            fl_color(toFl(p.panelBg));
            fl_rectf(x(), y(), w(), h());
        } else {
            // ⚠ DEPTH 3, ALWAYS. An RGBA fl_draw_image is read inconsistently across backends (the
            // standing "never blit depth 4" rule); the paper is opaque anyway, so the alpha byte is
            // padding and packing it out costs one pass over a buffer that only changes when the
            // pixels do.
            fl_draw_image(m_rgb.data(), x(), y(), static_cast<int>(m_img.width),
                          static_cast<int>(m_img.height), 3, 0);
        }
        fl_color(toFl(p.border));
        fl_rect(x(), y(), w(), h());
    }

    int handle(int e) override {
        switch (e) {
        case FL_ENTER:
            fl_cursor(FL_CURSOR_CROSS);
            return 1;
        case FL_LEAVE:
            fl_cursor(FL_CURSOR_DEFAULT);
            return 1;
        case FL_PUSH:
            if (Fl::event_button() != FL_LEFT_MOUSE || m_params == nullptr || m_img.empty())
                return 1;
            beginStroke(Fl::event_x() - x(), Fl::event_y() - y());
            return 1;
        case FL_DRAG:
            if (m_engine.active())
                extendStroke(Fl::event_x() - x(), Fl::event_y() - y());
            return 1;
        case FL_RELEASE:
            if (m_engine.active())
                endStroke();
            return 1;
        default:
            break;
        }
        return Fl_Widget::handle(e);
    }

private:
    // ⭐ THE SCRATCHPAD READS THE PEN (feedback round 1, item 1). It used to pin pressure at 1 and
    // nothing else, which made the one surface that exists to SHOW a preset's dynamics the one
    // surface that could not: most of the corpus is pressure-driven, and every one of those
    // previewed as a dead constant under a stylus that was reporting perfectly well two windows
    // over.
    //
    // ⚠ THE POSITION IS FLTK'S, THE DYNAMICS ARE THE DEVICE'S -- deliberately, and they are not the
    // same question. FLTK has already routed this event to this widget (on Wayland the tablet wiring
    // SYNTHESIZES that routing, docs/tablet.md §4), so the event's x/y is the one coordinate that is
    // certainly right for this canvas; the sample's own `pos` is surface-local to whatever surface
    // it came from and would need the whole gate the canvas has. Pressure, tilt and rotation have no
    // such ambiguity: they are properties of the nib, not of a window.
    //
    // ⚠ ONE SAMPLE PER EVENT, not the whole ~200 Hz segment the canvas drains. A scratchpad
    // interpolates between FLTK motion events like a mouse stroke does; what it must not do is
    // paint at a pressure the pen is not making.
    [[nodiscard]] cb::StrokeInput sampleAt(int px, int py) {
        cb::StrokeInput s;
        s.pos = {static_cast<double>(px), static_cast<double>(py)};
        // A mouse has no pressure, and pretending otherwise would preview a lie -- but the lie has
        // to be 1 and never 0 (§3.2), or size/flow dynamics collapse every mouse stroke to nothing.
        s.pressure = 1.0;
        if (m_tablet) {
            const BrushEditorTabletSample t = m_tablet();
            if (t.valid) {
                s.pressure = std::clamp(t.pressure, 0.0, 1.0);
                s.xTilt = t.xTilt;
                s.yTilt = t.yTilt;
                s.rotation = t.rotation;
                s.tangentialPressure = t.tangentialPressure;
                m_lastStrokeWasPen = true;
            }
        }
        s.timeUs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        return s;
    }

    void beginStroke(int px, int py) {
        cb::BrushParams params = *m_params;
        // ⭐ THE SCRATCHPAD PAINTS IN THE PREVIEW'S INK, NOT THE APP'S FOREGROUND (feedback round 1,
        // item 4 -- the ruling, written where it bites). The two halves of this surface share ONE
        // paper, so they have to share one ink:
        //   * the paper follows the THEME (§8.2). In the dark theme it is the panel's ground and the
        //     ink is the muted text colour -- a foreground of #1a1a1a paints an invisible stroke on
        //     it, and a foreground that happens to equal panelBg paints nothing at all.
        //   * an ERASER'S PAPER IS A SLAB OF THE INK ITSELF (§8.2), because what shows through the
        //     hole it bites is the dock. Painting that slab in some other colour makes the carve and
        //     the paint two unrelated pictures.
        //   * and the FALLBACK swaps BOTH paper and ink for the five presets that cannot mark the
        //     first pair. m_ink is what the render actually CAME BACK IN, so the scratchpad follows
        //     it into the grey-and-red fallback rather than contradicting it there.
        // The editor never touches the document, so "which colour will I paint with" is not the
        // question this surface answers; "what mark does this brush make" is.
        params.color = m_ink;
        // A LIVE stroke must vary between taps (§6.6i) -- the preview's pinned seed is the opposite
        // requirement, and this half of the surface is live.
        params.seedFromFirstSample = true;
        params = cb::previewCapped(std::move(params), kPreviewMaxDiameter);
        m_painted = true;
        m_engine.begin(m_img.width, m_img.height, m_img, params, cb::BrushDynamics{},
                       sampleAt(px, py));
        m_engine.composite();
        packRgb();
        redraw();
    }
    void extendStroke(int px, int py) {
        m_engine.extendTo(sampleAt(px, py));
        m_engine.composite();
        packRgb();
        redraw();
    }
    void endStroke() {
        m_engine.end(); // flushes the tail span: the dab walk lags the sample stream by one sample
        m_engine.composite(); // ... and the tail has to be composited, or the stroke ends short
        packRgb();
        redraw();
    }

    void renderAuto() {
        if (w() <= 0 || h() <= 0)
            return;
        // ⚠ THE ENGINE HOLDS A REFERENCE TO `m_img` FOR THE LIFE OF A STROKE, and the line below
        // REPLACES it. Every caller today reaches here only with no stroke in flight, but "today"
        // is not a guarantee a use-after-free should rest on -- so an in-flight stroke is dropped
        // here rather than left pointing at a buffer that is about to move.
        if (m_engine.active())
            m_engine.end();
        const Palette& p = activePalette();
        cb::StrokePreviewStyle style =
            presetStrokeStyle(p, m_params != nullptr && m_params->strokeMode == cb::StrokeMode::Erase);
        style.maxDiameter = kPreviewMaxDiameter;
        if (m_params == nullptr) {
            m_img = common::Image(static_cast<std::uint32_t>(w()), static_cast<std::uint32_t>(h()));
            m_img.fill(style.paper);
            m_ink = style.ink;
        } else {
            // ⚠ THE RESOLVED FORM, because the style that was ASKED for is not always the one that
            // was used: a brush that cannot mark the first pair comes back on the fallback paper
            // under the fallback ink. The scratchpad has to paint in whatever the picture beneath it
            // actually is (see beginStroke).
            cb::StrokePreviewRender r = cb::renderStrokePreviewResolved(*m_params, w(), h(), style);
            m_img = std::move(r.image);
            m_ink = r.ink;
        }
        ++m_renders;
        packRgb();
        redraw();
    }

    // The opaque RGB mirror the blit reads. Rebuilt only when the pixels change -- not per draw().
    void packRgb() {
        m_rgb.resize(m_img.pixelCount() * 3);
        for (std::size_t i = 0, o = 0; i + 3 < m_img.rgba.size(); i += 4, o += 3) {
            m_rgb[o] = m_img.rgba[i];
            m_rgb[o + 1] = m_img.rgba[i + 1];
            m_rgb[o + 2] = m_img.rgba[i + 2];
        }
    }

    common::Image m_img;
    std::vector<std::uint8_t> m_rgb;
    cb::BrushEngine m_engine;
    std::shared_ptr<const cb::BrushParams> m_params;
    std::function<BrushEditorTabletSample()> m_tablet;
    // The ink the surface's own picture came back in -- what a scratch stroke is laid in, so the
    // two halves of one canvas agree (see beginStroke). The light theme's default until a render.
    common::Color8 m_ink{0, 0, 0, 255};
    bool m_painted = false;
    bool m_lastStrokeWasPen = false;
    std::size_t m_renders = 0;
};

} // namespace

// ---- The taxonomy (pure) -----------------------------------------------------------------------

BrushOptionGroup brushOptionGroupOf(std::string_view base) noexcept {
    // General: what the mark IS -- how big, how opaque, how often, how sharp.
    if (base == "Size" || base == "Opacity" || base == "Flow" || base == "Spacing" ||
        base == "Rate" || base == "Sharpness")
        return BrushOptionGroup::General;
    // Colour: the three HSV channels the paint colour is put through, and the smudge trio, which is
    // the engine moving colour that is ALREADY on the canvas rather than laying any of its own.
    if (base == "h" || base == "s" || base == "v" || base == "ColorRate" ||
        base == "SmudgeRate" || base == "SmudgeRadius")
        return BrushOptionGroup::Colour;
    if (base == "Texture/Strength/")
        return BrushOptionGroup::Texture;
    // Tip: the nib's own geometry, where a dab lands, and the mark geometry of the second engine
    // kind (the sketch/curve/hatching families) -- which is a tip in everything but name.
    return BrushOptionGroup::Tip;
}

std::string brushOptionGroupLabel(BrushOptionGroup group) {
    switch (group) {
    case BrushOptionGroup::General: return _("General");
    case BrushOptionGroup::Colour: return _("Colour");
    case BrushOptionGroup::Texture: return _("Texture");
    case BrushOptionGroup::Tip: return _("Tip");
    }
    return _("Tip");
}

std::string brushOptionLabel(std::string_view base) {
    // ⚠ The base names are WIRE names (§3.2), not labels: `h`/`s`/`v` are single letters, the smudge
    // trio is spelled for the reference's own code rather than for a person, and
    // `Texture/Strength/` really does end in a slash. This is the one place the two are reconciled.
    if (base == "Size") return _("Size");
    if (base == "Opacity") return _("Opacity");
    if (base == "Flow") return _("Flow");
    if (base == "Ratio") return _("Ratio");
    if (base == "Rotation") return _("Rotation");
    if (base == "Softness") return _("Softness");
    if (base == "Scatter") return _("Scatter");
    if (base == "Mirror") return _("Mirror");
    if (base == "Spacing") return _("Spacing");
    if (base == "Sharpness") return _("Sharpness");
    if (base == "Rate") return _("Airbrush rate");
    if (base == "SmudgeRate") return _("Smudge length");
    if (base == "ColorRate") return _("Colour rate");
    if (base == "SmudgeRadius") return _("Smudge radius");
    if (base == "h") return _("Hue");
    if (base == "s") return _("Saturation");
    if (base == "v") return _("Value");
    if (base == "Density") return _("Density");
    if (base == "Line width") return _("Line width");
    if (base == "Offset scale") return _("Offset scale");
    if (base == "Curves opacity") return _("Curve opacity");
    if (base == "Angle") return _("Hatch angle");
    if (base == "Crosshatching") return _("Crosshatching");
    if (base == "Separation") return _("Separation");
    if (base == "Thickness") return _("Thickness");
    if (base == "Texture/Strength/") return _("Texture strength");
    return std::string(base); // a base from a newer preset: shown verbatim rather than hidden
}

std::string brushSensorLabel(cb::SensorId id) {
    switch (id) {
    case cb::SensorId::Pressure: return _("Pressure");
    case cb::SensorId::PressureIn: return _("Pressure (on tap)");
    case cb::SensorId::TangentialPressure: return _("Tangential pressure");
    case cb::SensorId::DrawingAngle: return _("Drawing angle");
    case cb::SensorId::XTilt: return _("X tilt");
    case cb::SensorId::YTilt: return _("Y tilt");
    // ⚠ The wire names are `ascension` and `declination`; a person calls them tilt direction and
    // tilt elevation (sensors.hpp). Nothing serializes from here, so the UI spelling is safe.
    case cb::SensorId::Ascension: return _("Tilt direction");
    case cb::SensorId::Declination: return _("Tilt elevation");
    case cb::SensorId::Rotation: return _("Barrel rotation");
    case cb::SensorId::Fuzzy: return _("Random (per dab)");
    case cb::SensorId::FuzzyStroke: return _("Random (per stroke)");
    case cb::SensorId::Speed: return _("Speed");
    case cb::SensorId::Fade: return _("Fade");
    case cb::SensorId::Distance: return _("Distance");
    case cb::SensorId::Time: return _("Time");
    case cb::SensorId::Perspective: return _("Perspective");
    }
    return _("Pressure");
}

std::vector<BrushEditorRow> brushEditorRows(const io::brush::BrushPreset& preset) {
    constexpr std::array<BrushOptionGroup, 4> kOrder{BrushOptionGroup::General,
                                                     BrushOptionGroup::Colour,
                                                     BrushOptionGroup::Texture,
                                                     BrushOptionGroup::Tip};
    std::vector<BrushEditorRow> rows;
    for (const BrushOptionGroup group : kOrder) {
        std::vector<BrushEditorRow> body;
        if (group == BrushOptionGroup::Tip) {
            // The tip is not an option and never was, but it is the first thing anyone reaches for.
            BrushEditorRow tip;
            tip.base = std::string(kBrushTipPage);
            tip.group = group;
            body.push_back(std::move(tip));
        }
        for (const cb::CurveOptionData& d : preset.options) {
            if (brushOptionGroupOf(d.name) != group)
                continue;
            BrushEditorRow row;
            row.base = d.name;
            row.group = group;
            // ⚠ Opacity and Flow are ALWAYS ON. Their `Pressure{X}` bit is written to shipped files
            // and the reader forces both on regardless (§3.2), so a checkbox there would be a
            // control that changes nothing -- which is worse than no control.
            row.checkable = d.checkable;
            row.checked = !d.checkable || d.checked;
            body.push_back(std::move(row));
        }
        if (body.empty())
            continue; // a caption over an empty group advertises an empty room
        BrushEditorRow header;
        header.group = group;
        header.header = true;
        rows.push_back(std::move(header));
        for (BrushEditorRow& r : body)
            rows.push_back(std::move(r));
    }
    return rows;
}

// ---- The dialog ---------------------------------------------------------------------------------

struct BrushEditorDialog::Ui {
    // Header
    Fl_Widget* thumb = nullptr; // a ScratchCanvas-free square: the brush's own stroke, tiny
    TextInput* name = nullptr;
    Fl_Box* engine = nullptr;
    Fl_Box* status = nullptr;
    FlatButton* save = nullptr;
    FlatButton* saveAs = nullptr;
    FlatButton* importBtn = nullptr;
    FlatButton* exportBtn = nullptr;
    FlatButton* deleteBtn = nullptr;
    // Rail
    ScrollView* presetScroll = nullptr;
    EditorPresetList* presets = nullptr;
    ScrollView* optionScroll = nullptr;
    EditorOptionList* options = nullptr;
    // Centre
    ScrollView* pageScroll = nullptr;
    Fl_Group* page = nullptr;
    CurveEditor* curve = nullptr;
    Dropdown* sensor = nullptr;
    // Preview
    ScratchCanvas* canvas = nullptr;
    // Footer
    CheckBox* sizeTie = nullptr;
    CheckBox* presetTie = nullptr;

    std::vector<std::unique_ptr<Binding>> bindings; // cleared on every rebuildPage()
    cb::SensorId editing = cb::SensorId::Pressure;  // whose curve the CurveEditor is showing
};

namespace {

// The header's little stroke square. A separate widget rather than a second ScratchCanvas: it is
// never painted on, and it draws the SAME renderer's output at a thumbnail's scale.
//
// ⚠ §8.1's thumbnail rule says a MODIFIED preset shows a generated stroke rather than its embedded
// PNG. Inside the editor every preset is modified by definition -- that is what the editor is -- so
// this one is always the stroke, and there is no icon-decode path here to keep in step with the
// dock's.
class HeaderThumb : public Fl_Widget {
public:
    HeaderThumb(int X, int Y, int W, int H) : Fl_Widget(X, Y, W, H) {}
    void setPixels(common::Image img) {
        m_img = std::move(img);
        m_rgb.resize(m_img.pixelCount() * 3);
        for (std::size_t i = 0, o = 0; i + 3 < m_img.rgba.size(); i += 4, o += 3) {
            m_rgb[o] = m_img.rgba[i];
            m_rgb[o + 1] = m_img.rgba[i + 1];
            m_rgb[o + 2] = m_img.rgba[i + 2];
        }
        redraw();
    }

protected:
    void draw() override {
        const Palette& p = activePalette();
        if (m_img.empty() || m_rgb.empty()) {
            fl_color(toFl(p.controlBg));
            fl_rectf(x(), y(), w(), h());
        } else {
            fl_draw_image(m_rgb.data(), x(), y(), static_cast<int>(m_img.width),
                          static_cast<int>(m_img.height), 3, 0); // depth 3: never blit depth 4
        }
        fl_color(toFl(p.border));
        fl_rect(x(), y(), w(), h());
    }

private:
    common::Image m_img;
    std::vector<std::uint8_t> m_rgb;
};

} // namespace

BrushEditorDialog::BrushEditorDialog(BrushPresetStore* store, BrushEditorHost host)
    : Fl_Double_Window(kWinW, kWinH, _("Brush Editor")), m_store(store), m_host(std::move(host)),
      m_ui(std::make_unique<Ui>()) {
    color(toFl(activePalette().windowBg));
    begin(); // keep `this` current through build() AND the sub-window creation below
    build();

    // A modal is its own TOP LEVEL, so its dropdown list and its precision-ruler HUD have to be ITS
    // children -- created here, before show(), because a sub-window added to an already-shown parent
    // is promoted to a stray top-level window.
    (new DropdownPopup())->hide();
    (new ContextMenu())->hide(); // the name field's right-click menu is a sub-window too
    // ScrubSlider::updateRuler bails unless slider.top_window() == ruler.top_window(), so the main
    // window's shared ruler would never appear over this dialog (the recurring "ruler doesn't show
    // in a new window" bug).
    m_ruler = new ScrubRuler();
    m_ruler->hide();

    end();
    size_range(kWinW, kWinH, kWinW, kWinH); // fixed: every page is laid out against these numbers
    set_modal();
    callback([](Fl_Widget*, void* v) { static_cast<BrushEditorDialog*>(v)->doClose(); }, this);
}

BrushEditorDialog::~BrushEditorDialog() {
    // ⚠ A widget must remove its timeouts in its destructor, or a coalesced rebuild fires into a
    // freed dialog. Both of them.
    Fl::remove_timeout(previewTimer, this);
    Fl::remove_timeout(pageRebuildTimer, this);
    // ⚠ AND THE TABLET REGISTRATION, HERE TOO. ~Fl_Window calls hide(), but from a base destructor,
    // where our override is already gone -- so a dialog destroyed while still shown would leave the
    // wiring holding a native handle that is about to be freed. This is the last moment it is ours.
    unwatchTablet();
}

void BrushEditorDialog::requestPageRebuild() {
    if (m_pageRebuildPending)
        return;
    m_pageRebuildPending = true;
    Fl::add_timeout(0.0, pageRebuildTimer, this);
}

void BrushEditorDialog::pageRebuildTimer(void* self) {
    auto* d = static_cast<BrushEditorDialog*>(self);
    d->m_pageRebuildPending = false;
    d->rebuildPage();
}

void BrushEditorDialog::build() {
    const Palette& pal = activePalette();
    // NB: no begin()/end() here -- the ctor keeps `this` the current group so the sub-windows it
    // creates after build() are children of the dialog rather than leaked top-levels.

    // ---- Header ----------------------------------------------------------------------------
    auto* th = new HeaderThumb(kPad, kPad, kThumbPx, kThumbPx);
    m_ui->thumb = th;

    const int hx = kPad + kThumbPx + kPad;
    auto* nameField = new TextInput(hx, kPad + 2, 280, kRowH);
    nameField->textsize(13);
    nameField->tooltip(_("The name this brush is saved and found under"));
    m_ui->name = nameField;
    m_ui->bindings.push_back(std::make_unique<Binding>());
    m_ui->bindings.back()->fn = [this] {
        if (m_seeding)
            return;
        markDirty();
        syncHeader();
    };
    nameField->when(FL_WHEN_CHANGED);
    nameField->callback(controlThunk, m_ui->bindings.back().get());

    auto* eng = new Fl_Box(hx, kPad + 2 + kRowH + 2, 420, 18);
    eng->box(FL_NO_BOX);
    eng->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    eng->labelfont(FL_HELVETICA);
    eng->labelsize(11);
    eng->labelcolor(toFl(pal.textMuted));
    m_ui->engine = eng;

    // The action row, laid out RIGHT to LEFT so Save keeps the corner it has always had:
    //   [Delete] [Import…] [Export…] | [Save As…] [Save]
    // Delete sits furthest from Save on purpose -- it is the one button in the row that cannot be
    // undone, and a mis-click that lands on the wrong neighbour should land on something harmless.
    const int bh = 28;
    const int by = kPad + 2;
    const int gap = 6;
    int bx = kWinW - kPad;
    const auto button = [&](int width, const char* label, Fl_Callback* cbf, bool filled) {
        bx -= width;
        FlatButton* b = filled ? new FilledButton(bx, by, width, bh, label)
                               : new FlatButton(bx, by, width, bh, label);
        b->callback(cbf, this);
        bx -= gap;
        return b;
    };
    m_ui->save = button(
        78, _("Save"),
        [](Fl_Widget*, void* v) { (void)static_cast<BrushEditorDialog*>(v)->doSave(false); }, true);
    m_ui->saveAs = button(
        92, _("Save As…"),
        [](Fl_Widget*, void* v) { (void)static_cast<BrushEditorDialog*>(v)->doSave(true); }, false);
    m_ui->exportBtn = button(
        88, _("Export…"), [](Fl_Widget*, void* v) { static_cast<BrushEditorDialog*>(v)->doExport(); },
        false);
    m_ui->exportBtn->tooltip(_("Write this brush to a .mbp file you can keep or share"));
    m_ui->importBtn = button(
        88, _("Import…"), [](Fl_Widget*, void* v) { static_cast<BrushEditorDialog*>(v)->doImport(); },
        false);
    m_ui->deleteBtn = button(
        76, _("Delete"),
        [](Fl_Widget*, void* v) { (void)static_cast<BrushEditorDialog*>(v)->doDelete(false); },
        false);
    m_ui->deleteBtn->tooltip(_("Remove this brush from your brushes folder. Only your own."));

    auto* st = new Fl_Box(hx + 288, kPad + 2, bx + gap - (hx + 288) - 8, kRowH);
    st->box(FL_NO_BOX);
    st->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
    st->labelfont(FL_HELVETICA);
    st->labelsize(11);
    st->labelcolor(toFl(pal.textMuted));
    m_ui->status = st;

    auto* rule = new Fl_Box(0, kHeaderH, kWinW, 1);
    rule->box(FL_FLAT_BOX);
    rule->color(toFl(pal.border));

    // ---- Left rail: the preset list over the checkable option list -------------------------
    const int railTop = kHeaderH + 1;
    const int bodyH = kWinH - kFooterH - railTop;
    auto* rail = new Panel(0, railTop, kRailW, bodyH);
    rail->borderEdges(Panel::EdgeRight);
    rail->begin();

    const auto caption = [&pal](int cx, int cy, int cw, const char* text) {
        auto* box = new Fl_Box(cx, cy, cw, kCaptionH, text);
        box->box(FL_NO_BOX);
        box->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        box->labelfont(FL_HELVETICA_BOLD);
        box->labelsize(11);
        box->labelcolor(toFl(pal.text));
        return box;
    };
    caption(10, railTop + 6, kRailW - 20, _("Presets"));
    auto* psv = new ScrollView(6, railTop + 6 + kCaptionH, kRailW - 12, kPresetListH);
    psv->type(Fl_Scroll::VERTICAL);
    psv->box(FL_NO_BOX);
    psv->color(toFl(pal.panelBg));
    psv->begin();
    auto* plist = new EditorPresetList(6, railTop + 6 + kCaptionH, kRailW - 12 - 16, kPresetListH);
    plist->setStore(m_store); // it renders each row's own stroke, and needs the resolved preset
    psv->end();
    m_ui->presetScroll = psv;
    m_ui->presets = plist;
    plist->setOnPick([this](int index) {
        // Browsing is a RE-SEED, and a dirty working copy is dropped by it. That is the same
        // bargain the Close button strikes and it is announced the same way: the dirty dot is the
        // warning, and Save is one click away from it.
        seed(index);
    });

    const int optTop = railTop + 6 + kCaptionH + kPresetListH + 10;
    caption(10, optTop, kRailW - 20, _("Options"));
    auto* osv = new ScrollView(6, optTop + kCaptionH, kRailW - 12,
                               railTop + bodyH - (optTop + kCaptionH) - 6);
    osv->type(Fl_Scroll::VERTICAL);
    osv->box(FL_NO_BOX);
    osv->color(toFl(pal.panelBg));
    osv->begin();
    auto* olist = new EditorOptionList(6, optTop + kCaptionH, kRailW - 12 - 16, 10);
    osv->end();
    m_ui->optionScroll = osv;
    m_ui->options = olist;
    olist->setOnPick([this](const std::string& base) { selectPage(base); });
    olist->setOnCheck([this](const std::string& base, bool on) {
        if (cb::CurveOptionData* d = option(base); d != nullptr && d->checkable) {
            d->checked = on;
            markDirty();
            rebuildRail();
        }
    });

    rail->end();

    // ---- Centre: the option page stack -----------------------------------------------------
    const int pageX = kRailW;
    const int pageW = kWinW - kRailW - kPreviewW;
    auto* sv = new ScrollView(pageX, railTop, pageW, bodyH);
    sv->type(Fl_Scroll::VERTICAL);
    sv->box(FL_FLAT_BOX);
    sv->color(toFl(pal.windowBg));
    sv->begin();
    auto* page = new Fl_Group(pageX, railTop, pageW - 16, 10);
    page->resizable(nullptr); // Fl_Group::clear() resets this to `this`; see rebuildPage()
    page->box(FL_NO_BOX);
    page->end();
    sv->end();
    m_ui->pageScroll = sv;
    m_ui->page = page;

    // ---- Right: the preview surface + Reset ------------------------------------------------
    const int prevX = kWinW - kPreviewW;
    auto* prev = new Panel(prevX, railTop, kPreviewW, bodyH);
    prev->borderEdges(Panel::EdgeLeft);
    prev->begin();
    caption(prevX + 10, railTop + 6, kPreviewW - 20, _("Preview & scratchpad"));
    const int canvasY = railTop + 6 + kCaptionH;
    const int canvasH = bodyH - (canvasY - railTop) - 6 - 30 - 6;
    auto* canvas = new ScratchCanvas(prevX + 10, canvasY, kPreviewW - 20, canvasH);
    canvas->setTabletReader([this]() -> BrushEditorTabletSample {
        return m_host.tabletReading ? m_host.tabletReading() : BrushEditorTabletSample{};
    });
    canvas->tooltip(_("Draw here to try the brush -- with your tablet's pressure and tilt. "
                      "Nothing here ever reaches your artwork."));
    m_ui->canvas = canvas;
    auto* resetBtn = new FlatButton(prevX + kPreviewW - 10 - 90, canvasY + canvasH + 6, 90, 30,
                                    _("Reset"));
    resetBtn->callback(
        [](Fl_Widget*, void* v) {
            auto* self = static_cast<BrushEditorDialog*>(v);
            if (self->m_ui->canvas != nullptr)
                self->m_ui->canvas->reset();
        },
        this);
    resetBtn->tooltip(_("Wipe the scratchpad and lay the sample stroke again"));
    prev->end();

    // ---- Footer: the two eraser ties, and Close --------------------------------------------
    auto* frule = new Fl_Box(0, kWinH - kFooterH, kWinW, 1);
    frule->box(FL_FLAT_BOX);
    frule->color(toFl(pal.border));
    const int fy = kWinH - kFooterH + (kFooterH - 22) / 2;
    auto* sizeTie = new CheckBox(kPad, fy, 250, 22, _("Eraser size follows the brush"),
                                 [this](bool on) {
                                     if (m_seeding)
                                         return;
                                     if (m_host.setEraserSizeFollowsBrush)
                                         m_host.setEraserSizeFollowsBrush(on);
                                 });
    sizeTie->setGroundColor(pal.windowBg);
    m_ui->sizeTie = sizeTie;
    auto* presetTie = new CheckBox(kPad + 260, fy, 260, 22, _("Eraser preset follows the brush"),
                                   [this](bool on) {
                                       if (m_seeding)
                                           return;
                                       if (m_host.setEraserPresetFollowsBrush)
                                           m_host.setEraserPresetFollowsBrush(on);
                                   });
    presetTie->setGroundColor(pal.windowBg);
    m_ui->presetTie = presetTie;

    auto* close = new FlatButton(kWinW - kPad - 88, kWinH - kFooterH + (kFooterH - 30) / 2, 88, 30,
                                 _("Close"));
    close->callback([](Fl_Widget*, void* v) { static_cast<BrushEditorDialog*>(v)->doClose(); },
                    this);
}

// ---- Seeding ------------------------------------------------------------------------------------

bool BrushEditorDialog::seed(int presetIndex) {
    if (m_store == nullptr)
        return false;
    const io::brush::LibraryPreset* lp = m_store->presetAt(presetIndex);
    if (lp == nullptr)
        return false; // -1 ("Default round") included: the ABSENCE of a preset has nothing to edit

    m_seeding = true;
    m_index = presetIndex;
    // THE EDIT MODEL (§8.3): a mutable COPY of the preset -- params, options and the tip reference
    // together -- beside the copy it was seeded from. Nothing here is applied anywhere, so a cancel
    // is a close and the store is byte-identical by construction rather than by a revert path that
    // could get one field wrong.
    m_working = *lp;
    m_original = *lp;
    m_dirty = false;
    m_page.clear();
    m_ui->editing = cb::SensorId::Pressure;
    setStatus(std::string());

    if (m_ui->name != nullptr)
        m_ui->name->value(m_working.preset.name.c_str());

    // The page is chosen BEFORE the rail is built, because the rail draws the selection.
    m_rows = brushEditorRows(m_working.preset);
    for (const BrushEditorRow& r : m_rows)
        if (!r.header) {
            m_page = r.base;
            break;
        }
    rebuildRail();
    m_seeding = false;
    rebuildPage();
    syncHeader();
    // Seeding rebuilds the params NOW rather than on the timeout: the dialog is about to be shown
    // and an empty preview surface for a frame reads as a broken editor.
    rebuildParams();
    if (m_ui->canvas != nullptr)
        m_ui->canvas->reset();
    return true;
}

void BrushEditorDialog::seedEraserTies(bool sizeFollows, bool presetFollows) {
    m_seeding = true;
    if (m_ui->sizeTie != nullptr)
        m_ui->sizeTie->setChecked(sizeFollows);
    if (m_ui->presetTie != nullptr)
        m_ui->presetTie->setChecked(presetFollows);
    m_seeding = false;
}

cb::CurveOptionData* BrushEditorDialog::option(std::string_view base) {
    for (cb::CurveOptionData& d : m_working.preset.options)
        if (d.name == base)
            return &d;
    return nullptr;
}

void BrushEditorDialog::rebuildRail() {
    m_rows = brushEditorRows(m_working.preset);
    if (m_ui->options != nullptr) {
        m_ui->options->setRows(m_rows);
        m_ui->options->setPage(m_page);
        const int need = std::max(10, m_ui->options->contentHeight());
        m_ui->options->size(m_ui->options->w(), need);
        if (m_ui->optionScroll != nullptr)
            m_ui->optionScroll->redraw();
    }
    if (m_ui->presets != nullptr && m_store != nullptr) {
        // The BROWSE surface: every preset in the same corpus as the one being edited, so reaching
        // for a neighbour never silently crosses the Brush|Eraser split the dock keeps (§8.2).
        std::vector<std::string> labels;
        std::vector<int> indices;
        const bool eraser = m_working.preset.eraserMode;
        const std::vector<io::brush::LibraryPreset>& all = m_store->presets();
        for (std::size_t i = 0; i < all.size(); ++i) {
            if (all[i].preset.eraserMode != eraser)
                continue;
            labels.push_back(presetDisplayName(all[i].preset.name));
            indices.push_back(static_cast<int>(i));
        }
        m_ui->presets->setItems(std::move(labels), std::move(indices));
        m_ui->presets->setActive(m_index);
        const int need = std::max(10, m_ui->presets->contentHeight());
        m_ui->presets->size(m_ui->presets->w(), need);
        if (m_ui->presetScroll != nullptr) {
            const int top = m_ui->presets->activeRowTop();
            if (top >= 0)
                m_ui->presetScroll->scroll_to(0, std::max(0, top - kPresetListH / 2));
            m_ui->presetScroll->redraw();
        }
    }
    syncActionButtons();
}

void BrushEditorDialog::syncActionButtons() {
    // Save is always live -- on a shipped preset it creates a user preset beside it (§8.3 ②), which
    // is the whole point. Delete and Export are not: one needs a file of the user's OWN to remove,
    // and the other needs a preset at all.
    const bool haveWorking = m_store != nullptr && m_store->presetAt(m_index) != nullptr;
    if (m_ui->exportBtn != nullptr) {
        if (haveWorking)
            m_ui->exportBtn->activate();
        else
            m_ui->exportBtn->deactivate();
    }
    if (m_ui->deleteBtn != nullptr) {
        if (canDeleteWorking())
            m_ui->deleteBtn->activate();
        else
            m_ui->deleteBtn->deactivate();
    }
}

bool BrushEditorDialog::canDeleteWorking() const {
    return m_store != nullptr && m_store->canDeletePreset(m_index);
}

std::size_t BrushEditorDialog::presetListStrokeRenders() const {
    return m_ui->presets != nullptr ? m_ui->presets->strokeRenders() : 0;
}

int BrushEditorDialog::presetListRowHeight() noexcept {
    return kPresetRowH;
}

Fl_Widget* BrushEditorDialog::presetListForTest() const {
    return m_ui->presets;
}

void BrushEditorDialog::selectPage(std::string_view base) {
    if (m_page == base)
        return;
    // A page key that is not in the rail selects nothing rather than emptying the centre: the caller
    // is a test or a stale row, and an editor with a blank middle reads as broken.
    const bool known =
        base == kBrushTipPage ||
        std::any_of(m_rows.begin(), m_rows.end(),
                    [base](const BrushEditorRow& r) { return !r.header && r.base == base; });
    if (!known)
        return;
    m_page.assign(base);
    m_ui->editing = cb::SensorId::Pressure;
    if (m_ui->options != nullptr)
        m_ui->options->setPage(m_page);
    rebuildPage();
}

// ---- The option pages ---------------------------------------------------------------------------

void BrushEditorDialog::rebuildPage() {
    const Palette& pal = activePalette();
    Fl_Group* page = m_ui->page;
    if (page == nullptr)
        return;
    const bool wasSeeding = m_seeding;
    m_seeding = true;

    page->clear();
    // Fl_Group::clear() resets resizable_ to `this`, so a later size() would PROPORTIONALLY scale
    // every child (the "controls get thicker" bug).
    page->resizable(nullptr);
    m_ui->curve = nullptr;
    m_ui->sensor = nullptr;
    // The name field's binding is the FIRST one and must survive a page rebuild; everything after it
    // belongs to the page.
    if (m_ui->bindings.size() > 1)
        m_ui->bindings.erase(m_ui->bindings.begin() + 1, m_ui->bindings.end());

    page->begin();
    const int px = page->x() + kPad;
    const int pw = page->w() - 2 * kPad;
    int y = page->y() + kPad;

    const auto bind = [this](std::function<void()> fn) {
        m_ui->bindings.push_back(std::make_unique<Binding>());
        m_ui->bindings.back()->fn = std::move(fn);
        return m_ui->bindings.back().get();
    };
    // ⚠ copy_label, never label: an Fl_Widget STORES the pointer it is handed, and a title built
    // from `brushOptionLabel(...)` is a temporary std::string that is gone by the first draw().
    const auto title = [&](const std::string& text) {
        auto* box = new Fl_Box(px, y, pw, 20);
        box->copy_label(text.c_str());
        box->box(FL_NO_BOX);
        box->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        box->labelfont(FL_HELVETICA_BOLD);
        box->labelsize(13);
        box->labelcolor(toFl(pal.text));
        y += 20 + kRowGap;
    };
    const auto caption = [&](const char* text, int lines) {
        auto* box = new Fl_Box(px, y, pw, 16 * lines);
        box->copy_label(text);
        box->box(FL_NO_BOX);
        box->align(FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
        box->labelfont(FL_HELVETICA);
        box->labelsize(11);
        box->labelcolor(toFl(pal.textMuted));
        y += 16 * lines + kRowGap;
    };
    const auto rowLabel = [&](const char* text) {
        auto* box = new Fl_Box(px, y, kLabelW, kRowH);
        box->copy_label(text);
        box->box(FL_NO_BOX);
        box->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        box->labelfont(FL_HELVETICA);
        box->labelsize(11);
        box->labelcolor(toFl(pal.textMuted));
    };
    const auto slider = [&](const char* label, double lo, double hi, double step, double value,
                            const char* suffix, std::function<void(double)> onChange) {
        rowLabel(label);
        auto* s = new ScrubSlider(px + kLabelW, y, kFieldW, kRowH);
        s->range(lo, hi);
        s->step(step);
        s->value(value);
        s->setSuffix(suffix != nullptr ? suffix : "");
        s->setCellColor(pal.windowBg);
        s->setRuler(m_ruler);
        s->when(FL_WHEN_CHANGED);
        s->callback(controlThunk, bind([this, s, cbf = std::move(onChange)] {
                        if (m_seeding)
                            return;
                        cbf(s->value());
                        markDirty();
                    }));
        y += kRowH + kRowGap;
        return s;
    };
    const auto check = [&](const char* label, bool value, std::function<void(bool)> onToggle) {
        auto* c = new CheckBox(px, y, pw, 22, nullptr);
        c->copy_label(label);
        c->setGroundColor(pal.windowBg);
        c->setChecked(value);
        c->setOnToggle([this, cbf = std::move(onToggle)](bool on) {
            if (m_seeding)
                return;
            cbf(on);
            markDirty();
        });
        y += 22 + kRowGap;
        return c;
    };

    if (m_page == kBrushTipPage) {
        title(_("Tip & spacing"));
        caption(_("The nib itself: how wide it is, which way it is turned, and how densely it lays "
                  "dabs along the stroke. A preset's dynamic options scale these; this is what they "
                  "scale."),
                3);
        slider(_("Diameter"), 1.0, 1000.0, 0.1, m_working.masterDiameter, "px", [this](double v) {
            m_working.masterDiameter = v;
            // ⚠ AN AUTO TIP CARRIES ITS OWN DIAMETER, and it is what a reload reads (the generator
            // is the whole tip). Setting only the resolved master size would paint right this
            // session and load small the next.
            if (m_working.preset.tip.kind == io::brush::TipXml::Kind::Auto)
                m_working.preset.tip.autoTip.generator.diameter = v;
            // ⚠⚠ AND THE MASKING BRUSH IS PART OF THE BRUSH: its size is authored as a COEFFICIENT
            // of the master size and only resolved to an absolute at load (§6.2). Re-resolve it
            // through the same pure function the library uses, or a resized nib wears a mask
            // several times too big for it.
            m_working.masking = io::brush::resolveMasking(m_working.preset.masking,
                                                          m_working.masterDiameter);
        });
        rowLabel(_("Angle"));
        {
            auto* dial = new Dial(px + kLabelW, y, 64, 64);
            dial->range(0.0, 360.0);
            dial->step(1.0);
            dial->setShowReadout(true);
            dial->setCellColor(pal.windowBg);
            // The file stores radians; the knob speaks degrees, measured the way a knob is read.
            double deg = m_working.preset.tip.angle * 180.0 / 3.14159265358979323846;
            deg = std::fmod(deg, 360.0);
            if (deg < 0.0)
                deg += 360.0;
            dial->value(deg);
            dial->when(FL_WHEN_CHANGED);
            dial->callback(controlThunk, bind([this, dial] {
                               if (m_seeding)
                                   return;
                               m_working.preset.tip.angle =
                                   dial->value() * 3.14159265358979323846 / 180.0;
                               markDirty();
                           }));
            y += 64 + kRowGap;
        }
        slider(_("Spacing"), 0.01, 2.0, 0.01, m_working.preset.tip.spacing, nullptr,
               [this](double v) { m_working.preset.tip.spacing = v; });
        check(_("Automatic spacing"), m_working.preset.tip.useAutoSpacing,
              [this](bool on) { m_working.preset.tip.useAutoSpacing = on; });
        slider(_("Auto coefficient"), 0.05, 4.0, 0.01, m_working.preset.tip.autoSpacingCoeff,
               nullptr, [this](double v) { m_working.preset.tip.autoSpacingCoeff = v; });
        check(_("Same spacing in every direction"), m_working.preset.isotropicSpacing,
              [this](bool on) { m_working.preset.isotropicSpacing = on; });
        caption(_("Spacing is an ellipse, not a number: the interval follows both of the dab's "
                  "extents, so a long thin nib steps further along its blade than across it. "
                  "\"Same in every direction\" opts out of that."),
                3);
    } else if (cb::CurveOptionData* d = option(m_page); d != nullptr) {
        title(brushOptionLabel(d->name));
        if (!d->checkable)
            caption(_("Always on. This option's enable bit is written to preset files and ignored "
                      "by every reader, so there is nothing here to switch off."),
                    2);

        slider(_("Strength"), d->strengthMin, d->strengthMax,
               (d->strengthMax - d->strengthMin) > 4.0 ? 0.01 : 0.001, d->strength, nullptr,
               [d](double v) { d->strength = v; });

        check(_("Let the sensors drive it"), d->useCurve, [d](bool on) { d->useCurve = on; });
        caption(_("Off collapses the option to its constant strength -- the sensors are not "
                  "consulted at all, which is what the preset format means by this bit even though "
                  "its name says otherwise."),
                3);

        // The sensor picker. ONE dropdown over all sixteen, with the ACTIVE ones dotted in the open
        // list (Dropdown::setMarkedItems, which exists for exactly this shape of question), plus an
        // "Active" box that adds/removes the one on show.
        rowLabel(_("Sensor"));
        auto* dd = new Dropdown(px + kLabelW, y, kFieldW, kRowH);
        std::vector<int> marked;
        int show = 0;
        for (std::size_t i = 0; i < cb::kSensorCount; ++i) {
            const auto id = static_cast<cb::SensorId>(i);
            dd->add(menuSafe(brushSensorLabel(id)).c_str(), 0, nullptr, nullptr, 0);
            if (d->sensors.has(id))
                marked.push_back(static_cast<int>(i));
            if (id == m_ui->editing)
                show = static_cast<int>(i);
        }
        dd->setMarkedItems(std::move(marked));
        dd->value(show);
        dd->callback(controlThunk, bind([this, dd] {
                         m_ui->editing = static_cast<cb::SensorId>(std::max(0, dd->value()));
                         // A different sensor is a different curve and a different set of extras.
                         requestPageRebuild();
                     }));
        m_ui->sensor = dd;
        y += kRowH + kRowGap;

        const bool active = d->sensors.has(m_ui->editing);
        check(_("Active"), active, [this, d](bool on) {
            const cb::SensorId id = m_ui->editing;
            if (on) {
                if (!d->sensors.has(id))
                    d->sensors.sensors.push_back(cb::Sensor::withDefaults(id));
            } else {
                // ⚠ NEVER TO ZERO. An option whose sensor list is empty is not "a sensor turned
                // off" -- optionIsDynamic reads it as a STATIC option, which is what the "Let the
                // sensors drive it" box above says in words. Leaving one sensor standing keeps the
                // two controls from meaning the same thing in two different ways.
                if (d->sensors.sensors.size() <= 1) {
                    setStatus(_("An option keeps at least one sensor -- switch the sensors off "
                                "instead."));
                    return;
                }
                const auto it =
                    std::remove_if(d->sensors.sensors.begin(), d->sensors.sensors.end(),
                                   [id](const cb::Sensor& s) { return s.id == id; });
                d->sensors.sensors.erase(it, d->sensors.sensors.end());
            }
            requestPageRebuild();
        });

        if (d->sensors.sensors.size() > 1) {
            rowLabel(_("Combine"));
            auto* mode = new Dropdown(px + kLabelW, y, kFieldW, kRowH);
            const char* modes[] = {_("Multiply"), _("Add"), _("Largest"), _("Smallest"),
                                   _("Difference")};
            for (const char* m : modes)
                mode->add(menuSafe(m).c_str(), 0, nullptr, nullptr, 0);
            mode->value(static_cast<int>(d->combineMode));
            mode->callback(controlThunk, bind([this, d, mode] {
                               if (m_seeding)
                                   return;
                               d->combineMode = cb::combineModeFromInt(mode->value());
                               markDirty();
                           }));
            y += kRowH + kRowGap;
            caption(_("How several active sensors fold together. It touches the scaling sensors "
                      "only -- a tilt offset always adds and an absolute angle always overwrites."),
                    2);
            check(_("One curve for every sensor"), d->useSameCurve,
                  [this, d](bool on) {
                      d->useSameCurve = on;
                      requestPageRebuild(); // the curve below is a different curve now
                  });
        }

        // The response curve. ⚠ ui::CurveEditor edits a core::brush::Curve ITSELF rather than its
        // own point type, which is what makes an imported preset's curve survive a visit here
        // byte-exactly ("x,y;x,y;", six-significant-digit %g, corner flags and all).
        const bool shared = d->useSameCurve || d->sensors.sensors.size() <= 1;
        rowLabel(shared ? _("Curve (shared)") : _("Curve"));
        y += kRowH - 2;
        auto* ce = new CurveEditor(px, y, std::min(pw, 260), 200);
        ce->setCellColor(pal.windowBg);
        cb::Sensor* sensor = nullptr;
        for (cb::Sensor& s : d->sensors.sensors)
            if (s.id == m_ui->editing)
                sensor = &s;
        ce->setCurve(shared || sensor == nullptr ? d->commonCurve : sensor->curve);
        ce->onChanged([this, d, shared, id = m_ui->editing](const cb::Curve& c) {
            if (m_seeding)
                return;
            if (shared) {
                d->commonCurve = c;
            } else {
                for (cb::Sensor& s : d->sensors.sensors)
                    if (s.id == id)
                        s.curve = c;
            }
            markDirty();
        });
        m_ui->curve = ce;
        y += 200 + kRowGap;

        // Per-sensor extras, and they are per SENSOR, not per option: only three sensors carry a
        // length and only one carries a fan.
        //
        // ⚠ These callbacks look their sensor up by ID rather than capturing a `Sensor*`. The
        // "Active" box above PUSHES INTO the very vector those pointers would point into, and a
        // vector that reallocates leaves every captured element pointer dangling -- for exactly as
        // long as it takes the deferred page rebuild to replace these widgets. An id costs one
        // linear scan over at most sixteen entries and cannot dangle at all.
        const cb::SensorId editing = m_ui->editing;
        const auto sensorById = [this, base = d->name, editing]() -> cb::Sensor* {
            cb::CurveOptionData* opt = option(base);
            if (opt == nullptr)
                return nullptr;
            for (cb::Sensor& s : opt->sensors.sensors)
                if (s.id == editing)
                    return &s;
            return nullptr;
        };
        if (sensor != nullptr && cb::sensorHasRange(editing)) {
            const char* unit = editing == cb::SensorId::Distance ? "px"
                               : editing == cb::SensorId::Time   ? "ms"
                                                                 : nullptr;
            slider(_("Length"), 1.0, 10000.0, 1.0, sensor->range.length, unit,
                   [sensorById](double v) {
                       if (cb::Sensor* s = sensorById(); s != nullptr)
                           s->range.length = std::max(1, static_cast<int>(v));
                   });
            check(_("Repeat"), sensor->range.periodic, [sensorById](bool on) {
                if (cb::Sensor* s = sensorById(); s != nullptr)
                    s->range.periodic = on;
            });
            caption(_("Fade counts dabs, Distance counts pixels and Time counts milliseconds -- "
                      "three sensors, three units. \"Repeat\" saws back to zero instead of holding "
                      "at one."),
                    3);
        }
        if (sensor != nullptr && editing == cb::SensorId::DrawingAngle) {
            rowLabel(_("Offset"));
            auto* dial = new Dial(px + kLabelW, y, 64, 64);
            dial->range(-180.0, 180.0);
            dial->step(1.0);
            dial->setDefaultValue(0.0);
            dial->setShowReadout(true);
            dial->setCellColor(pal.windowBg);
            dial->value(std::clamp(sensor->fan.angleOffset, -180.0, 180.0));
            dial->when(FL_WHEN_CHANGED);
            dial->callback(controlThunk, bind([this, sensorById, dial] {
                               if (m_seeding)
                                   return;
                               if (cb::Sensor* s = sensorById(); s != nullptr)
                                   s->fan.angleOffset = dial->value();
                               markDirty();
                           }));
            y += 64 + kRowGap;
            check(_("Lock the angle at the stroke's start"), sensor->fan.lockedAngleMode,
                  [sensorById](bool on) {
                      if (cb::Sensor* s = sensorById(); s != nullptr)
                          s->fan.lockedAngleMode = on;
                  });
        }
    } else {
        title(_("Nothing to edit"));
        caption(_("This preset carries no options in that group."), 2);
    }

    page->end();
    page->size(page->w(), std::max(10, y - page->y() + kPad));
    if (m_ui->pageScroll != nullptr) {
        m_ui->pageScroll->scroll_to(0, 0);
        m_ui->pageScroll->redraw();
    }
    m_seeding = wasSeeding;
}

// ---- Header, dirt, preview ----------------------------------------------------------------------

void BrushEditorDialog::syncHeader() {
    if (m_ui->name != nullptr) {
        const char* v = m_ui->name->value();
        m_working.preset.name = v != nullptr ? v : "";
    }
    if (m_ui->engine != nullptr) {
        const io::brush::PresetProvenance& p = m_working.preset.provenance;
        const bool exact = p.fidelity == io::brush::PresetFidelity::Exact;
        const bool approx = p.fidelity == io::brush::PresetFidelity::Approximated;
        const char* fidelity = exact ? _("Exact") : (approx ? _("Approximated") : _("Substituted"));
        const char* accum = m_working.preset.accumulator == cb::StrokeAccumulator::Colored
                                ? _("colour accumulation")
                                : _("alpha accumulation");
        std::string line =
            p.sourcePaintop.empty() ? std::string(_("pixel brush")) : p.sourcePaintop;
        line += " · ";
        line += fidelity;
        line += " · ";
        line += accum;
        if (m_dirty)
            line += std::string(" · ") + _("edited");
        m_ui->engine->copy_label(line.c_str());
        // The fidelity BADGE is the line's own ink -- the dock's two badge colours exactly, so a
        // preset that reads amber on a card reads amber here (§8.2).
        m_ui->engine->labelcolor(
            toFl(exact ? activePalette().textMuted : (approx ? kApproxInk : kSubstInk)));
        m_ui->engine->redraw();
    }
    redraw();
}

void BrushEditorDialog::setStatus(std::string text) {
    if (m_ui->status == nullptr)
        return;
    m_ui->status->copy_label(text.c_str());
    m_ui->status->redraw();
}

void BrushEditorDialog::markDirty() {
    if (m_seeding)
        return;
    if (!m_dirty) {
        m_dirty = true;
        syncHeader();
    }
    requestPreview();
}

void BrushEditorDialog::requestPreview() {
    if (m_previewPending)
        return;
    m_previewPending = true;
    Fl::add_timeout(kPreviewCoalesceS, previewTimer, this);
}

void BrushEditorDialog::previewTimer(void* self) {
    auto* d = static_cast<BrushEditorDialog*>(self);
    d->m_previewPending = false;
    d->rebuildParams();
}

void BrushEditorDialog::flushPreviewForTest() {
    if (m_previewPending) {
        Fl::remove_timeout(previewTimer, this);
        m_previewPending = false;
    }
    rebuildParams();
}

void BrushEditorDialog::rebuildParams() {
    // ⚠ THIS MINTS THE TIP'S RASTER ID (io/brush/preset_brush.hpp), so it runs once per COALESCED
    // settings change and never per stroke, per frame or per draw call. That is the same rule the
    // store keeps for a preset pick; the editor is simply a place where "the preset changed" can
    // happen more than once.
    m_params = std::make_shared<const cb::BrushParams>(io::brush::presetBrushParams(m_working));
    if (m_ui->canvas != nullptr)
        m_ui->canvas->setParams(m_params);
    if (auto* th = static_cast<HeaderThumb*>(m_ui->thumb); th != nullptr) {
        // §8.1's rule: a MODIFIED preset shows a generated stroke, not its embedded PNG -- and
        // inside the editor every preset is modified by definition. Same renderer as the pane
        // beside it, at a thumbnail's scale.
        cb::StrokePreviewStyle style = presetStrokeStyle(
            activePalette(), m_working.preset.eraserMode);
        style.maxDiameter = kThumbPx / 2.0;
        th->setPixels(cb::renderStrokePreview(*m_params, kThumbPx, kThumbPx, style));
    }
}

std::size_t BrushEditorDialog::previewRenders() const noexcept {
    return m_ui->canvas != nullptr ? m_ui->canvas->renders() : 0;
}

void BrushEditorDialog::setWorkingName(std::string name) {
    if (m_ui->name != nullptr)
        m_ui->name->value(name.c_str());
    m_working.preset.name = std::move(name);
    markDirty();
    syncHeader();
}

// ---- Commit -------------------------------------------------------------------------------------

int BrushEditorDialog::doSave(bool saveAs) {
    if (m_store == nullptr)
        return -1;
    syncHeader(); // take whatever is in the name field before anything is written

    // Where a save LANDS (docs/brushes.md §8.3): `dataDir()/brushes`, in Mosaic's own `.mbp`
    // container. A shipped bundle is read-only -- there is no `.bundle` writer and there must not be
    // one that edits the set Mosaic ships -- so editing a shipped preset ALWAYS creates a user
    // preset beside it, under a name made unique against the whole corpus.
    const bool overwrite = !saveAs && m_store->isUserPreset(m_index);
    if (!overwrite) {
        const std::string unique = m_store->uniqueName(m_working.preset.name);
        if (unique != m_working.preset.name) {
            m_working.preset.name = unique;
            if (m_ui->name != nullptr)
                m_ui->name->value(unique.c_str());
        }
    }

    // The preset is the user's own now, whatever it was imported from. The source format changes
    // with the container it is about to live in; the PAINTOP and the dropped-option list do not --
    // they are what the import actually cost, and a save must not launder that away.
    m_working.preset.provenance.sourceFormat = "mbp";

    std::string error;
    const int index =
        m_store->writeUserPreset(m_working, common::Image{}, overwrite ? m_index : -1, &error);
    if (index < 0) {
        setStatus(error.empty() ? std::string(_("The preset could not be saved.")) : error);
        common::log::category("brush")->warn("brush editor: save failed: {}", error);
        return -1;
    }

    m_index = index;
    m_original = m_working;
    m_dirty = false;
    setStatus(_("Saved."));
    syncHeader();
    // ⚠ A WRITE RE-NUMBERS THE CORPUS -- and since the user's run leads it (brush_presets.hpp), a
    // save can move every shipped preset too. The rail's stroke cache is keyed by index, so it goes
    // whole; a per-index fixup would be the same rule in two places.
    if (m_ui->presets != nullptr)
        m_ui->presets->dropStrokes();
    rebuildRail();
    if (m_host.onSaved)
        m_host.onSaved(index);
    return index;
}

void BrushEditorDialog::doImport() {
    if (m_store == nullptr)
        return;
    // ⚠⚠ THE APP'S OWN PICKER, NOT A BARE `Fl_Native_File_Chooser` (feedback round 1: "import
    // doesn't glob"). The brace list `*.{mbp,kpp,bundle}` is not the bug -- FLTK's own
    // `fl_filename_match` understands braces, and its kdialog/zenity drivers expand them. WHICH
    // BACKEND ANSWERS is the bug: `platform::initNativeFileDialog()` turns FLTK's kdialog driver on
    // for the whole process on KDE, and that driver hands kdialog a filter of the form
    // `"Name (*.a *.b)\nAll files (*)"` -- newline-separated Qt-style entries, which is not a filter
    // syntax kdialog accepts, so nothing matched and the list came up empty.
    //
    // `platform::showOpenDialog` is the seam File▸Open and Export already go through: the XDG
    // portal first, Mosaic's OWN kdialog command second (`<globs>|<label>`, the syntax kdialog
    // actually reads), and the native chooser only as a last resort. It also takes globs as a LIST,
    // so all three extensions are three globs rather than one pattern a backend has to parse.
    platform::FileDialogRequest req;
    req.title = _("Import a brush preset");
    req.parent = this;
    req.startFolder = (common::dataDir() / "brushes").string();
    req.filters = {{_("Brush presets"), {"*.mbp", "*.kpp", "*.bundle"}, {}},
                   {_("All files"), {"*"}, {}}};
    const std::optional<std::string> picked = platform::showOpenDialog(req);
    if (!picked || picked->empty())
        return; // cancelled: not an error worth a line

    std::string error;
    const int index = m_store->importPresetFile(std::filesystem::path(*picked), &error);
    if (index < 0) {
        setStatus(error.empty() ? std::string(_("That file is not a brush preset.")) : error);
        return;
    }
    if (m_ui->presets != nullptr)
        m_ui->presets->dropStrokes(); // the corpus was re-numbered
    if (m_host.onSaved)
        m_host.onSaved(index); // the dock has to re-read the store either way
    seed(index);               // ... and the editor opens on what was just imported
    setStatus(_("Imported."));  // seed() clears the status line, so this says it AFTER, not before
}

void BrushEditorDialog::doExport() {
    // ⚠ MOSAIC WRITES `.mbp` AND NOTHING ELSE (§8.3 ②). Export is not a `.kpp` writer in disguise:
    // round-tripping one means a PNG-chunk writer plus an XML serializer for 94 params whose
    // defaults, spellings and two prefixing rules all have to be reproduced exactly, or the file
    // loads DIFFERENTLY somewhere else -- which is a worse outcome than not offering it.
    if (m_store == nullptr || m_store->presetAt(m_index) == nullptr)
        return;
    syncHeader(); // whatever is in the name field is what gets written

    platform::FileDialogRequest req;
    req.title = _("Export brush preset");
    req.acceptLabel = _("Export");
    req.parent = this;
    req.suggestedName = presetFileStem(m_working.preset.name) + ".mbp";
    req.startFolder = (common::dataDir() / "brushes").string();
    req.filters = {{_("Mosaic brush preset"), {"*.mbp"}, {}}};
    const std::optional<std::string> chosen = platform::showSaveDialog(req);
    if (!chosen || chosen->empty())
        return;

    // ⚠ HARD RULE: a file outside Mosaic's own data directory is written ONLY where the user just
    // pointed a save dialog, and only because they pointed it. Nothing here picks a path.
    if (exportForTest(*chosen))
        setStatus(_("Exported.")); // a refusal has already put its own reason on the line
}

bool BrushEditorDialog::exportForTest(const std::string& path) {
    // The extension is appended here rather than trusted from the dialog: the portal does not add
    // the filter's extension to a typed name, and a preset file that is not called `.mbp` is one
    // the next scan will not look at.
    const std::filesystem::path dest = withMbpExtension(std::filesystem::path(path));
    std::string error;
    // ONE writer, shared with the user-directory save (ui/brush_presets.hpp): an export that went
    // through a second serializer would drift from the file the app reads back.
    if (writePresetFile(m_working, common::Image{}, dest, &error))
        return true;
    common::log::category("brush")->warn("brush editor: export failed: {}", error);
    setStatus(error.empty() ? std::string(_("The preset could not be exported.")) : error);
    return false;
}

bool BrushEditorDialog::doDelete(bool confirmed) {
    if (m_store == nullptr)
        return false;
    if (!canDeleteWorking()) {
        // Say WHY. "Delete is greyed out" is not an answer to "why can I not delete this".
        const std::string why = m_store->deleteRefusal(m_index);
        setStatus(why.empty() ? std::string(_("That preset cannot be deleted.")) : why);
        return false;
    }

    if (!confirmed) {
        // ⚠ THE CONFIRMATION IS A SECOND MODAL OVER A MODAL, and `AskOrTellDialog` is built for it:
        // it is its own TOP-LEVEL window (never a sub-window of this one, so no promotion trap), it
        // calls set_modal() itself, and run() pumps Fl::wait() so the app keeps painting behind it.
        // The RIGHTMOST button is the accent default and Escape fires the leftmost, so "Cancel"
        // must be leftmost and Delete must be last.
        AskOrTellDialog::Stage stage;
        stage.icon = AskOrTellDialog::Icon::Warning;
        stage.title = _("Delete this brush?");
        // ONE translatable sentence with the name inside it, never two fragments glued around it:
        // a translator handed `"` and `" will be removed...` cannot put the quotes where their
        // language puts them, and several languages do not use these quotes at all.
        std::array<char, 512> body{};
        std::snprintf(body.data(), body.size(),
                      _("\"%s\" will be removed from your brushes folder. This cannot be undone."),
                      m_working.preset.name.c_str());
        stage.message = body.data();
        stage.buttons = {_("Cancel"), _("Delete")};
        AskOrTellDialog ask;
        if (ask.ask(stage, this) != 1)
            return false;
    }

    std::string error;
    int next = -1;
    if (!m_store->deleteUserPreset(m_index, &error, &next)) {
        setStatus(error.empty() ? std::string(_("The preset could not be deleted.")) : error);
        common::log::category("brush")->warn("brush editor: delete failed: {}", error);
        return false;
    }
    common::log::category("brush")->info("brush editor: deleted preset '{}'",
                                         m_working.preset.name);
    adoptStoreChange(next);
    return true;
}

bool BrushEditorDialog::deleteForTest() {
    return doDelete(true);
}

void BrushEditorDialog::adoptStoreChange(int index) {
    // ⚠ THREE THINGS HAVE TO END UP POINTING AT SOMETHING VALID, and the store is only one of them.
    // The store re-points its own selections by name (rebuildAll drops the params with the name, so
    // the tool cannot keep painting with a brush that no longer exists); the HOST re-points the dock
    // and the active tool; and this dialog re-seeds -- or closes, when the corpus has nothing left
    // to edit, because an editor showing a preset that is gone is worse than no editor.
    if (m_ui->presets != nullptr)
        m_ui->presets->dropStrokes(); // every cached stroke is keyed by an index that just moved
    if (m_host.onSaved)
        m_host.onSaved(index);
    if (index < 0 || !seed(index)) {
        doClose();
        return;
    }
    setStatus(_("Deleted."));
}

void BrushEditorDialog::doClose() {
    // Nothing was ever applied, so there is nothing to revert: the store is byte-identical to what
    // it was when the dialog opened unless a Save wrote to it. The working copy is simply dropped.
    hide();
}

int BrushEditorDialog::saveForTest(bool saveAs) {
    return doSave(saveAs);
}

void BrushEditorDialog::show() {
    Fl_Double_Window::show();
    // ⚠ THE PEN HAS TO BE READ OVER *THIS* WINDOW. Tablet delivery is per-window on both platforms,
    // so a backend brought up on the canvas sees nothing at all while the pen hovers this dialog --
    // which is the entire time the dialog is up, and is exactly why Settings → Tablet's test area
    // watches its own window too. Without this the scratchpad's pressure would be a constant 1.
    // `win` must already be SHOWN (both backends need its native handle), hence: after show().
    if (!m_tabletWatched && m_host.tabletWatchWindow) {
        m_host.tabletWatchWindow(this);
        m_tabletWatched = true;
    }
}

void BrushEditorDialog::hide() {
    // ⚠ UNWATCH BEFORE THE WINDOW GOES (ui::TabletInput::unwatch): the X11 backend holds the native
    // handle, and unwatching a destroyed window is a use-after-free. hide() is the last moment the
    // handle is certainly still ours.
    unwatchTablet();
    Fl_Double_Window::hide();
}

void BrushEditorDialog::unwatchTablet() {
    if (!m_tabletWatched)
        return;
    m_tabletWatched = false;
    if (m_host.tabletUnwatchWindow)
        m_host.tabletUnwatchWindow(this);
}

void BrushEditorDialog::reapplyTheme() {
    color(toFl(activePalette().windowBg));
    // The preview's paper and ink FOLLOW the theme (§8.2), so a re-theme is a re-render -- of the
    // auto stroke only; a scratchpad the user painted on keeps its pixels, because throwing away
    // their marks because the OS went dark would be a worse surprise than a mismatched paper.
    if (m_ui->canvas != nullptr && !m_ui->canvas->painted())
        m_ui->canvas->reset();
    // ... and the rail's strips were laid on the OLD paper, every one of them. Same rule the dock
    // pays on a re-theme, one level down.
    if (m_ui->presets != nullptr)
        m_ui->presets->dropStrokes();
    requestPageRebuild(); // a re-theme can arrive from inside another dialog's callback
    redraw();
}

int BrushEditorDialog::handle(int event) {
    if (event == FL_PUSH)
        dismissActiveDropdownPopupOnOutsideClick(Fl::event_x(), Fl::event_y());
    if (event == FL_KEYDOWN && Fl::event_key() == FL_Escape) {
        doClose();
        return 1;
    }
    return Fl_Double_Window::handle(event);
}

} // namespace mosaic::ui
