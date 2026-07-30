#pragma once

#include "common/image.hpp"      // Color8
#include "core/vector/paint.hpp" // core::vec::Paint

#include <FL/Fl_Widget.H>
#include <functional>

// A shared, clickable paint-preview chip + the paint-kind helpers behind it (promoted out of
// layer_effects_dialog.cpp so the Fill dialog can reuse the same control). PaintChip renders any
// core::vec::Paint -- a solid swatch + hex, a gradient ramp over a checker, a live pattern
// mini-preview, or "None" -- with a hover highlight + "Edit..." + chevron so it reads as a button.
// Clicking it is up to the host (open the matching flyout). The kName / kindIndex / seedColor /
// setKind helpers are the "type owned by the parent control" logic a kind dropdown drives.
namespace mosaic::ui {

// Paint-kind names for a kind dropdown, and the gradient-type names (Linear/Radial/Conic).
extern const char* const kPaintKindNames[5];
extern const char* const kGradTypeNames[3];
inline constexpr int kPatternKindIndex = 4;

// The kind dropdown index for a paint: 0=Solid, 1=Linear, 2=Radial, 3=Conic, 4=Pattern.
[[nodiscard]] int paintKindIndex(const core::vec::Paint& p);

// The colour to seed a kind switch from: the solid colour, the gradient's first stop, the pattern's
// foreground, or `fallback` (NoPaint / empty gradient).
[[nodiscard]] common::Color8 paintSeedColor(const core::vec::Paint& p, common::Color8 fallback);

// Switch a paint's kind (a kPaintKindNames index): Solid adopts the seed; a gradient kind keeps
// existing stops (only retyping + re-placing the transform) or seeds a two-stop seed->transparent
// fade; Pattern keeps an existing pattern or seeds a default procedural one (fg = seed).
void setPaintKind(core::vec::Paint& p, int kind, common::Color8 seed);

// A clickable paint preview (a solid swatch + hex, a gradient ramp over a checker, a live pattern
// mini-preview, or "None") with a hover highlight + chevron so it reads as a button. Opening it is
// the host's job (setOnClick); the host hands the current paint to the right flyout.
class PaintChip : public Fl_Widget {
public:
    PaintChip(int X, int Y, int W, int H);
    void setPaint(const core::vec::Paint& p);
    void setGroundColor(common::Color8 c) { m_ground = c; }
    void setOnClick(std::function<void()> f) { m_onClick = std::move(f); }
    [[nodiscard]] const core::vec::Paint& paint() const { return m_paint; }

protected:
    int handle(int e) override;
    void draw() override;

private:
    core::vec::Paint m_paint;
    common::Color8 m_ground{0, 0, 0, 255};
    bool m_hover = false;
    std::function<void()> m_onClick;
};

} // namespace mosaic::ui
