#include "ui/crop_gesture.hpp"

#include <algorithm>
#include <cmath>

namespace mosaic::ui {

bool CropGesture::begin(CropMode mode, int handle, common::Vec2 docPt,
                        const common::Rect& rect) {
    m_mode = CropMode::None;
    if (mode == CropMode::None)
        return false;
    if (mode == CropMode::Resize && (handle < 0 || handle > 7))
        return false;
    if (mode != CropMode::Draw && rect.empty())
        return false;
    m_mode = mode;
    m_handle = handle;
    m_base = rect;
    m_startDoc = docPt;
    return true;
}

namespace {

// Snap `v` onto whichever canvas edge coordinate (`lo`/`hi`) lies within `tol` (nearest wins).
double snapEdge(double v, double lo, double hi, double tol) {
    if (tol <= 0.0)
        return v;
    const double dLo = std::abs(v - lo);
    const double dHi = std::abs(v - hi);
    if (dLo <= tol && dLo <= dHi)
        return lo;
    if (dHi <= tol)
        return hi;
    return v;
}

} // namespace

common::Rect CropGesture::rectFor(common::Vec2 docPt, double ratio, bool shift, bool alt,
                                  double docW, double docH, double snapTol) const {
    using common::Rect;
    using common::Vec2;
    if (!active())
        return m_base;
    const double W = std::max(docW, 1.0);
    const double H = std::max(docH, 1.0);
    // The safety envelope: how far outside the canvas anything may reach (S16-f expansion).
    const double out = kCropOutsetFactor * std::max(W, H);
    // The cursor is bounded by the envelope, then snapped onto a nearby canvas edge so plain
    // crops land exactly on the bounds they used to be clamped to.
    const Vec2 p{snapEdge(std::clamp(docPt.x, -out, W + out), 0.0, W, snapTol),
                 snapEdge(std::clamp(docPt.y, -out, H + out), 0.0, H, snapTol)};

    if (m_mode == CropMode::Move) {
        Vec2 d = docPt - m_startDoc; // unclamped: the rect, not the cursor, is what bounds
        if (shift) {                 // lock to the dominant axis (the Move tool's convention)
            if (std::abs(d.x) >= std::abs(d.y))
                d.y = 0.0;
            else
                d.x = 0.0;
        }
        // Move keeps the size: bound the origin by the envelope, then snap by translation —
        // origin 0 aligns the leading edge to the canvas, origin W-w / H-h the trailing one.
        const double x = snapEdge(std::clamp(m_base.x + d.x, -out, W + out - m_base.w), 0.0,
                                  W - m_base.w, snapTol);
        const double y = snapEdge(std::clamp(m_base.y + d.y, -out, H + out - m_base.h), 0.0,
                                  H - m_base.h, snapTol);
        return {x, y, m_base.w, m_base.h};
    }

    // ---- Draw / Resize: anchor-and-extend ----
    double r = ratio > 0.0 ? ratio : 0.0;
    if (r <= 0.0 && shift) // free ratio + Shift: square for a fresh Draw, the rect's own aspect
        r = m_mode == CropMode::Draw ? 1.0 : (m_base.h > 1e-9 ? m_base.w / m_base.h : 0.0);

    // Edge mids drive one axis (4 T / 6 B resize height; 5 R / 7 L width); everything else both.
    const bool drivesX = m_mode == CropMode::Draw || (m_handle != 4 && m_handle != 6);
    const bool drivesY = m_mode == CropMode::Draw || (m_handle != 5 && m_handle != 7);

    // The fixed point the rect extends away from: Draw anchors at the press; a Resize handle at
    // its opposite corner/edge. Alt swaps the anchor for the centre (Draw: the press point).
    Vec2 a = m_startDoc;
    if (m_mode == CropMode::Resize) {
        switch (m_handle) {
        case 0: a = {m_base.right(), m_base.bottom()}; break; // TL grows from BR
        case 1: a = {m_base.x, m_base.bottom()}; break;       // TR from BL
        case 2: a = {m_base.x, m_base.y}; break;              // BR from TL
        case 3: a = {m_base.right(), m_base.y}; break;        // BL from TR
        case 4: a = {m_base.x + m_base.w * 0.5, m_base.bottom()}; break; // T from the bottom
        case 5: a = {m_base.x, m_base.y + m_base.h * 0.5}; break;        // R from the left
        case 6: a = {m_base.x + m_base.w * 0.5, m_base.y}; break;        // B from the top
        default: a = {m_base.right(), m_base.y + m_base.h * 0.5}; break; // L from the right
        }
    }
    const Vec2 c = m_mode == CropMode::Draw ? m_startDoc : m_base.center();

    const double sx = drivesX && p.x < a.x ? -1.0 : 1.0; // the cursor's side of the anchor
    const double sy = drivesY && p.y < a.y ? -1.0 : 1.0;

    double w = drivesX ? (alt ? 2.0 * std::abs(p.x - c.x) : std::abs(p.x - a.x)) : m_base.w;
    double h = drivesY ? (alt ? 2.0 * std::abs(p.y - c.y) : std::abs(p.y - a.y)) : m_base.h;
    w = std::max(w, 1.0);
    h = std::max(h, 1.0);

    // Per-axis placement: centred (Alt; or the cross axis of a ratio'd edge resize, which
    // re-centres on the rect) vs anchored vs untouched. The available extent follows the rule —
    // measured to the safety ENVELOPE, not the canvas (S16-f: past the canvas = expansion).
    const bool centredX = alt || (r > 0.0 && !drivesX);
    const bool centredY = alt || (r > 0.0 && !drivesY);
    const double availX = centredX ? 2.0 * std::min(c.x + out, W + out - c.x)
                                   : (drivesX ? (sx > 0.0 ? W + out - a.x : a.x + out)
                                              : W + 2.0 * out);
    const double availY = centredY ? 2.0 * std::min(c.y + out, H + out - c.y)
                                   : (drivesY ? (sy > 0.0 ? H + out - a.y : a.y + out)
                                              : H + 2.0 * out);

    if (r > 0.0) {
        if (!drivesX)
            w = h * r;
        else if (!drivesY)
            h = w / r;
        else if (w / r >= h) // both driven: the dominant dimension wins
            h = w / r;
        else
            w = h * r;
        if (w > availX) { // clamp to the envelope WITH the ratio held (shrink both)
            w = availX;
            h = w / r;
        }
        if (h > availY) {
            h = availY;
            w = h * r;
        }
    } else {
        w = std::min(w, availX);
        h = std::min(h, availY);
    }

    double x = centredX ? c.x - w * 0.5 : (drivesX ? (sx > 0.0 ? a.x : a.x - w) : m_base.x);
    double y = centredY ? c.y - h * 0.5 : (drivesY ? (sy > 0.0 ? a.y : a.y - h) : m_base.y);
    x = std::clamp(x, -out, std::max(-out, W + out - w)); // numeric backstop (envelope)
    y = std::clamp(y, -out, std::max(-out, H + out - h));
    return {x, y, w, h};
}

common::Vec2 cropFrameToDoc(common::Vec2 p, double angle, common::Vec2 pivot) {
    if (angle == 0.0)
        return p;
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    const common::Vec2 d{p.x - pivot.x, p.y - pivot.y};
    return {pivot.x + c * d.x - s * d.y, pivot.y + s * d.x + c * d.y};
}

common::Vec2 docToCropFrame(common::Vec2 p, double angle, common::Vec2 pivot) {
    return cropFrameToDoc(p, -angle, pivot);
}

std::array<common::Vec2, 4> cropBoxCorners(const common::Rect& r, double angle,
                                           common::Vec2 pivot) {
    return {cropFrameToDoc(r.topLeft(), angle, pivot),
            cropFrameToDoc({r.right(), r.y}, angle, pivot),
            cropFrameToDoc({r.right(), r.bottom()}, angle, pivot),
            cropFrameToDoc({r.x, r.bottom()}, angle, pivot)};
}

double cropRatioForOptions(int choiceIndex, bool swap, double docW, double docH) {
    double r = 0.0;
    switch (choiceIndex) {
    case 1: r = docH > 0.0 ? docW / docH : 0.0; break; // Original
    case 2: r = 1.0; break;
    case 3: r = 4.0 / 3.0; break;
    case 4: r = 16.0 / 9.0; break;
    case 5: r = 3.0 / 2.0; break;
    default: return 0.0; // Free
    }
    return swap && r > 0.0 ? 1.0 / r : r;
}

double customCropRatio(double ratioW, double ratioH, bool swap) {
    const double r = (ratioW > 0.0 && ratioH > 0.0) ? ratioW / ratioH : 0.0;
    return swap && r > 0.0 ? 1.0 / r : r;
}

common::Rect conformCropRect(const common::Rect& r, double ratio, double docW, double docH) {
    if (ratio <= 0.0)
        return r;
    const double W = std::max(docW, 1.0);
    const double H = std::max(docH, 1.0);
    // A rect fully inside the canvas conforms INSIDE it (entry / reset / ratio switches keep
    // their historical behaviour); one already staging an expansion only respects the safety
    // envelope, so a ratio change never silently discards the outset.
    const bool inside = r.x >= 0.0 && r.y >= 0.0 && r.right() <= W && r.bottom() <= H;
    const double out = inside ? 0.0 : kCropOutsetFactor * std::max(W, H);
    const double boundW = W + 2.0 * out;
    const double boundH = H + 2.0 * out;
    const double area = std::max(r.w * r.h, 1.0);
    double w = std::sqrt(area * ratio);
    double h = w / ratio;
    if (w > boundW) {
        w = boundW;
        h = w / ratio;
    }
    if (h > boundH) {
        h = boundH;
        w = h * ratio;
    }
    return {std::clamp(r.x + (r.w - w) * 0.5, -out, W + out - w),
            std::clamp(r.y + (r.h - h) * 0.5, -out, H + out - h), w, h};
}

CropPixels snapCropRect(const common::Rect& r, std::uint32_t docW, std::uint32_t docH) {
    if (docW == 0 || docH == 0)
        return {};
    // Round the ORIGIN and the SIZE independently (not the two edges independently): a pure
    // translate keeps r.w/r.h fixed, so the snapped size is translation-invariant and a Move can
    // never flicker the reported W/H by ±1px (S16-h). Draw/Resize still snap because r.w/r.h
    // genuinely change there; the only trade is that a left/top-edge resize may wobble its pinned
    // far edge by 1px, which is far less noticeable than the move jitter this replaces.
    // Bounds are the S16-f safety envelope, not the canvas: a beyond-canvas rect is a staged
    // EXPANSION and must round-trip through here (display, status bar and apply all share this).
    const long out = static_cast<long>(kCropOutsetFactor) *
                     static_cast<long>(std::max(docW, docH));
    const long w = std::clamp(std::lround(r.w), 1L, static_cast<long>(docW) + 2 * out);
    const long h = std::clamp(std::lround(r.h), 1L, static_cast<long>(docH) + 2 * out);
    const long x0 = std::clamp(std::lround(r.x), -out, static_cast<long>(docW) + out - w);
    const long y0 = std::clamp(std::lround(r.y), -out, static_cast<long>(docH) + out - h);
    return {x0, y0, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)};
}

} // namespace mosaic::ui
