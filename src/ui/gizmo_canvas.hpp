#pragma once

#include "common/geometry3d.hpp" // common::Vec2
#include "common/image.hpp"

#include <cstdint>
#include <vector>

// A tiny software AA canvas for viewport gizmos, EXTRACTED from type3d_panel.cpp for the Texture
// Generator's preview pane (S55-f; the GlyphButton-to-widgets precedent). Round-2 3D-popup
// feedback stands: fl_line's 1px aliased rings read jagged and unprofessional, so every primitive
// is coverage-rasterized from its signed distance with a 1px feather, over an opaque ground --
// the owner then blits ONE composed image. Bounded per-primitive bboxes keep a full recompose ~a
// millisecond at panel size.
namespace mosaic::ui {

class GizmoCanvas {
public:
    GizmoCanvas(int w, int h, common::Color8 ground);

    // Straight-alpha source-over of a rendered preview at (ox, oy).
    void blitImage(const common::Image& img, int ox, int oy);

    // The transparency checkerboard over [x0, x1) x [y0, y1) (drawn UNDER an alpha-carrying
    // preview blit), phase-locked to the rect's origin so it does not swim when the rect moves.
    void checker(int x0, int y0, int x1, int y1, common::Color8 a, common::Color8 b, int cell = 8);

    void stroke(common::Vec2 a, common::Vec2 b, double width, common::Color8 c, float alpha);
    void fillDisc(common::Vec2 ctr, double r, common::Color8 c, float alpha);
    void strokeCircle(common::Vec2 ctr, double r, double width, common::Color8 c, float alpha);
    void fillSquare(common::Vec2 ctr, double half, common::Color8 c, float alpha);
    void fillDiamond(common::Vec2 ctr, double r, common::Color8 c, float alpha);

    [[nodiscard]] const unsigned char* data() const { return m_px.data(); }
    [[nodiscard]] int width() const { return m_w; }
    [[nodiscard]] int height() const { return m_h; }

private:
    static double segDist(common::Vec2 p, common::Vec2 a, common::Vec2 b);
    template <typename SdCov>
    void forBox(double x0, double y0, double x1, double y1, SdCov inside, common::Color8 c,
                float alpha);

    int m_w, m_h;
    std::vector<std::uint8_t> m_px;
};

} // namespace mosaic::ui
