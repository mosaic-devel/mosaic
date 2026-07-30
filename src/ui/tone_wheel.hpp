#pragma once

#include "common/image.hpp"

#include <FL/Fl_Widget.H>

#include <functional>
#include <vector>

// A chromatic offset wheel (S32 pro adjustment controls; docs/adjustment-layers.md §5): the
// colorist's tone-band wheel — a hue disc, neutral at the centre and vivid at the rim, with a
// draggable puck expressing a 2D chroma shift. The Color Balance panel shows three (shadows /
// midtones / highlights); the puck's plane position maps onto the cyan-red / magenta-green /
// yellow-blue slider axes via core::colorBalanceToPlane/FromPlane. Software-rendered with AA
// (the GizmoCanvas discipline: the affordance is drawn from the same math that hit-tests it).
namespace mosaic::ui {

// Hue (turns, red at 0) at `sat` (0 = white, 1 = vivid), HSV full value: the ramp the wheel disc
// samples, exported so the Hue/Saturation slider tracks draw the same spectrum.
[[nodiscard]] common::Color8 wheelHue(double turns, double sat);

class ToneWheel : public Fl_Widget {
public:
    ToneWheel(int X, int Y, int W, int H);

    // The puck position on the unit disc, mathematical y-UP (+x toward red, +y toward the green
    // side — the core::ColorBalancePoint convention divided by 100). setValue clamps to the disc
    // and never fires onChange.
    void setValue(double x, double y);
    [[nodiscard]] double valueX() const noexcept { return m_x; }
    [[nodiscard]] double valueY() const noexcept { return m_y; }
    // Fired per drag motion with the new unit-disc position; double-click recentres (0,0).
    void setOnChange(std::function<void(double x, double y)> cb) { m_onChange = std::move(cb); }
    // Programmatic drag (the headless test hook): clamps to the disc and fires onChange exactly
    // like a pointer drag landing there.
    void dragToValue(double x, double y);
    void setGroundColor(common::Color8 c) { m_ground = c; }

protected:
    void draw() override;
    int handle(int event) override;

private:
    [[nodiscard]] int radius() const; // disc radius in px (fits the widget box)
    void dragTo(int ex, int ey);      // pointer -> clamped unit position + onChange
    void rebuildDisc();               // re-render the cached AA disc (size / theme change)

    double m_x = 0.0;
    double m_y = 0.0;
    common::Color8 m_ground{0, 0, 0, 255};
    // Cached AA hue disc, RGB TRIPLES over the ground colour -- depth 3 on purpose: a depth-4
    // fl_draw_image is interpreted inconsistently across drawing surfaces (channel misreads).
    std::vector<unsigned char> m_disc;
    int m_discFor = 0;       // the radius the cache was rendered for
    common::Color8 m_discGround{0, 0, 0, 0}; // the ground it was rendered over (theme key)
    std::function<void(double, double)> m_onChange;
};

} // namespace mosaic::ui
