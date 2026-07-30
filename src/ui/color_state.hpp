#pragma once

#include "common/image.hpp" // Color8

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The editor's active foreground / background colours (PLAN S11-d). Photoshop / Affinity-style: the
// painting and fill tools (S19+) read the *foreground*; the two-diagonal-square swatch widget edits
// it and the picker popover writes into it. Kept deliberately FLTK-free so it can be unit-tested and
// owned by non-UI code; the main window holds one and shares it by reference.
namespace mosaic::ui {

class ColorState {
public:
    ColorState() = default;

    [[nodiscard]] common::Color8 foreground() const noexcept { return m_fg; }
    [[nodiscard]] common::Color8 background() const noexcept { return m_bg; }

    // Each mutator fires the observers (so the swatch redraws and an open picker re-syncs). The
    // alpha channel is carried through but the picker keeps it at 255 for now (full alpha is S12).
    void setForeground(common::Color8 c);
    void setBackground(common::Color8 c);
    void swap();  // exchange fg <-> bg (the swatch's back-square click and the "X" shortcut)
    void reset(); // the default-reset corner: foreground = black, background = white

    // Observers fire after any change. Append-only and intentionally un-removable: the only
    // observers are app-lifetime (the swatch now; the colour-consuming tools later), so there is no
    // dangling concern. An open picker is refreshed *by* the swatch's observer, not as one itself.
    void addObserver(std::function<void()> cb) { m_observers.push_back(std::move(cb)); }

private:
    void notify();

    common::Color8 m_fg{0, 0, 0, 255};       // black
    common::Color8 m_bg{255, 255, 255, 255}; // white
    std::vector<std::function<void()>> m_observers;
};

// Hex <-> Color8 helpers for the picker's hex field (pure, unit-tested). RGB only -- alpha is left
// at 255. parseHexColor accepts "#RGB" / "#RRGGBB", with or without the '#' and surrounding spaces,
// case-insensitive; std::nullopt for anything malformed. hexString formats uppercase "#RRGGBB".
[[nodiscard]] std::optional<common::Color8> parseHexColor(std::string_view text);
[[nodiscard]] std::string hexString(common::Color8 c);

} // namespace mosaic::ui
