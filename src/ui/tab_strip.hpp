#pragma once

#include <FL/Fl_Widget.H>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

// The open-document tab strip (PLAN S49). It sits directly under the tool options bar and above the
// canvas -- the position PLAN §9 S11 reserved for it -- and lists one tab per open document with its
// unsaved marker and an X to close.
//
// **The strip is hidden outright while a single document is open** (user, 2026-07-09): a lone tab
// says nothing the window title does not already say, and it would steal a row of canvas for ever.
// The host asks tabStripHeight() for the row to reserve, so appearing/disappearing is a layout
// change, not a paint.
//
// The strip owns no documents and knows no core types. It renders labels and reports indices; the
// host maps those onto its sessions. Layout is a pure function (fitTabWidths) so the shrink-then-
// scroll behaviour is unit-tested headlessly.
namespace mosaic::ui {

// Tab metrics, in logical px.
inline constexpr int kTabStripHeight = 30;
inline constexpr int kTabMinWidth = 92;  // below this a filename is unreadable; the strip scrolls
inline constexpr int kTabMaxWidth = 220; // a lone tab must not stretch across the window

// The widths `natural` tabs actually get inside `availW`. Everything fits -> the natural widths,
// untouched. Otherwise they shrink proportionally, but never past kTabMinWidth -- so the returned
// sum MAY still exceed availW, and the strip scrolls the overflow. Pure; unit-tested.
[[nodiscard]] std::vector<int> fitTabWidths(const std::vector<int>& natural, int availW);

class TabStrip : public Fl_Widget {
public:
    struct TabItem {
        std::string label;    // the document's file name, or its untitled title
        bool dirty = false;   // unsaved changes: a filled accent dot rides before the X
        bool readOnly = false; // opened while another Mosaic holds the lock (a muted padlock)
    };

    TabStrip(int X, int Y, int W, int H);

    // Replace the whole list. `active` is an index into `tabs` (ignored when empty). Cheap to call
    // every time anything changes -- it early-outs when nothing actually differs, so the host can
    // drive it from its per-frame sync without churning the display.
    void setTabs(std::vector<TabItem> tabs, std::size_t active);

    void setOnSelect(std::function<void(std::size_t)> cb) { m_onSelect = std::move(cb); }
    void setOnClose(std::function<void(std::size_t)> cb) { m_onClose = std::move(cb); }
    // S50: files dropped on the STRIP open as documents (dropped on the canvas they would become
    // magic layers). The strip highlights while a drag is over it, because that difference is
    // otherwise invisible. Unset = the strip refuses the drag.
    void setOnFilesDropped(std::function<void(const std::vector<std::string>&)> cb) {
        m_onFilesDropped = std::move(cb);
    }

    [[nodiscard]] std::size_t count() const noexcept { return m_tabs.size(); }

    void draw() override;
    int handle(int event) override;

private:
    // Where each tab lands, in widget-local x (already scrolled). Recomputed on demand from the
    // current label widths -- the font is a draw-time concern, so this sets the font itself.
    struct Slot {
        int x = 0;
        int w = 0;
    };
    [[nodiscard]] std::vector<Slot> slots() const;
    [[nodiscard]] int contentWidth() const; // total width the tabs want
    void clampScroll();
    // The tab under a widget-local x, and whether the point is over its close X.
    [[nodiscard]] std::size_t tabAt(int localX, bool* overClose = nullptr) const;

    std::vector<TabItem> m_tabs;
    std::size_t m_active = 0;
    std::size_t m_hover = static_cast<std::size_t>(-1);
    bool m_hoverClose = false;
    int m_scrollX = 0; // px of overflow scrolled off the left edge (0 when everything fits)
    bool m_dropHot = false;         // a file drag is hovering: draw the "drop to open" cue
    bool m_expectDropPaste = false; // an accepted drop's FL_PASTE payload is on its way
    std::function<void(std::size_t)> m_onSelect;
    std::function<void(std::size_t)> m_onClose;
    std::function<void(const std::vector<std::string>&)> m_onFilesDropped;
};

} // namespace mosaic::ui
