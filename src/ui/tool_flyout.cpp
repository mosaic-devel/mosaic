#include "ui/tool_flyout.hpp"

#include "common/image.hpp"
#include "ui/icon_pack.hpp" // renderIconSource: vector or raster pack art
#include "ui/theme.hpp"
#include "ui/tool.hpp"

#include <FL/Enumerations.H>
#include <FL/Fl.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/Fl_Widget.H>
#include <FL/fl_draw.H>

#include <memory>
#include <string>
#include <vector>

namespace mosaic::ui {
namespace {

constexpr int kRowH = 30;    // height of one variant row
constexpr int kFlyoutW = 188; // flyout width (icon + name)
constexpr int kPad = 5;      // inset around the rows
constexpr int kIconPx = 20;  // rasterized glyph size
constexpr int kIconGap = 9;  // gap between the icon and the name

Fl_Color toFl(common::Color8 c) {
    return fl_rgb_color(c.r, c.g, c.b);
}

// One variant row: the tool's (already-coloured) icon + its name, hover-highlit, with the active
// variant marked by an accent fill. Clicking the row activates that variant and closes the flyout.
class FlyoutRow : public Fl_Widget {
public:
    FlyoutRow(int X, int Y, int W, int H, ToolManager& tools, const Tool& tool, ToolFlyout* flyout)
        : Fl_Widget(X, Y, W, H), m_tools(tools), m_id(tool.id()), m_name(tool.name()),
          m_flyout(flyout) {
        std::string err;
        m_icon = renderIconSource(tool.icon(), kIconPx, &err); // vector or raster pack art
        if (!m_icon.empty())
            m_iconImg = std::make_unique<Fl_RGB_Image>(m_icon.rgba.data(),
                                                       static_cast<int>(m_icon.width),
                                                       static_cast<int>(m_icon.height), 4);
    }

protected:
    void draw() override {
        const Palette& pal = activePalette();
        const bool active = m_tools.active() == m_id;
        const common::Color8 bg = active ? pal.accent : (m_hover ? pal.controlHover : pal.panelBg);
        fl_color(toFl(bg));
        fl_rectf(x(), y(), w(), h());
        if (m_iconImg != nullptr)
            m_iconImg->draw(x() + kPad, y() + (h() - kIconPx) / 2);
        fl_color(toFl(pal.text));
        fl_font(FL_HELVETICA, 12);
        fl_draw(m_name.c_str(), x() + kPad + kIconPx + kIconGap, y(), w() - kPad - kIconPx - kIconGap,
                h(), FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    }

    int handle(int event) override {
        switch (event) {
        case FL_ENTER:
            m_hover = true;
            redraw();
            return 1; // claim ENTER so FLTK also delivers LEAVE
        case FL_LEAVE:
            m_hover = false;
            redraw();
            return 1;
        case FL_PUSH:
            return 1; // act on release, so a press that started elsewhere can't fire us by accident
        case FL_RELEASE:
            m_tools.setActive(m_id); // picks this variant; the slot's button now shows it
            m_flyout->hide();
            return 1;
        default:
            return Fl_Widget::handle(event);
        }
    }

private:
    ToolManager& m_tools;
    ToolId m_id;
    std::string m_name;
    ToolFlyout* m_flyout;
    common::Image m_icon; // backing store for m_iconImg
    std::unique_ptr<Fl_RGB_Image> m_iconImg;
    bool m_hover = false;
};

} // namespace

ToolFlyout::ToolFlyout(ToolManager& tools)
    : Popover(kFlyoutW + (Popover::bubbleSupported() ? Popover::kBubbleTri : 0), kRowH),
      m_tools(tools) {
    enableBubble(); // the same comic-book pointer as the colour picker (Popover draws it)
}

void ToolFlyout::showForSlot(ToolSlot slot, const Fl_Widget* anchor) {
    const std::vector<Tool*> variants = m_tools.toolsInSlot(slot);
    // Reserve the triangle's left margin (when the backend supports the bubble) so the rows clear it.
    const int dx = Popover::bubbleSupported() ? Popover::kBubbleTri : 0;
    setBaseSize(kFlyoutW + dx, kPad * 2 + static_cast<int>(variants.size()) * kRowH);

    clear(); // drop the previous slot's rows, then rebuild for this slot
    begin();
    int ry = kPad;
    for (Tool* t : variants) {
        new FlyoutRow(dx + kPad, ry, kFlyoutW - 2 * kPad, kRowH, m_tools, *t, this);
        ry += kRowH;
    }
    end();
    resizable(nullptr); // rows are fixed; clear() had reset this to the group

    showAnchored(anchor);
}

} // namespace mosaic::ui
