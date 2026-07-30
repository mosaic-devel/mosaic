#include "ui/clone_stamp_gesture.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace mosaic::ui {

core::CloneSampleSource cloneSampleForChoice(int index) noexcept {
    switch (index) {
    case 1:
        return core::CloneSampleSource::CurrentAndBelow;
    case 2:
        return core::CloneSampleSource::AllLayers;
    default:
        return core::CloneSampleSource::CurrentLayer;
    }
}

std::vector<common::Vec2> cloneMarkerPolyline(common::Vec2 center, double radius) {
    constexpr int kSegments = 24; // a circle this small needs no more; 24 divides by 4 for the
                                  // diamond's corners to land exactly on circle vertices
    const double r = std::max(2.0, radius);
    std::vector<common::Vec2> pts;
    pts.reserve(kSegments + 1 + 4);
    for (int i = 0; i <= kSegments; ++i) {
        const double a = 2.0 * std::numbers::pi * static_cast<double>(i) / kSegments;
        pts.push_back({center.x + r * std::cos(a), center.y + r * std::sin(a)});
    }
    // The inscribed diamond, walked from the circle's own end point (angle 0) through the three
    // other cardinal points and back to it -- four chords, no segment drawn twice.
    pts.push_back({center.x, center.y - r});
    pts.push_back({center.x - r, center.y});
    pts.push_back({center.x, center.y + r});
    pts.push_back({center.x + r, center.y});
    return pts;
}

} // namespace mosaic::ui
