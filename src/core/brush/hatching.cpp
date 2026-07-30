#include "core/brush/hatching.hpp"

#include <algorithm>
#include <cmath>

namespace mosaic::core::brush {

namespace {

constexpr double kPi = 3.14159265358979323846;

// The reference's `myround`: round half UP (toward +inf), not away from zero. It is used only to
// drop the sub-pixel precision, and it is not `std::round`.
[[nodiscard]] double myRound(double x) noexcept {
    return (x - std::floor(x)) >= 0.5 ? std::ceil(x) : std::floor(x);
}

// One hatch line into the stencil. ⚠ THE ANTIALIASED BRANCH IS AN APPROXIMATION AND IT IS BADGED:
// the reference draws it with a dedicated varying-width Wu line, which is not transcribed; this is
// the transcribed distance-field thick line (stroke_painter.hpp) with its antialiasing on, which
// differs only in the edge ramp. The NON-antialiased branch IS that same reference call, exactly.
//
// Lines combine by MAX rather than by the reference's source-over: parallel lines a separation
// apart do not touch at all, and a crosshatch pass crosses another at full alpha, where max and
// over agree everywhere but on the antialiased rim of the crossing.
void drawHatchLine(common::Vec2 a, common::Vec2 b, double thickness, bool antialias, int width,
                   int height, std::vector<LinePixel>& scratch, std::vector<std::uint8_t>& out) {
    const LineClip clip{0, 0, width, height};
    rasterizeThickLine(a, b, thickness, antialias, clip, scratch);
    for (const LinePixel& p : scratch) {
        std::uint8_t& dst = out[static_cast<std::size_t>(p.y) * static_cast<std::size_t>(width) +
                                static_cast<std::size_t>(p.x)];
        const auto v = static_cast<std::uint8_t>(
            std::clamp(static_cast<int>(std::lround(p.weight * 255.0)), 0, 255));
        dst = std::max(dst, v);
    }
}

// The reference's `iterateLines`: walk the lattice outward from the line that passes nearest the
// dab, in both directions, clipping each to the dab's box by intersecting it with the four edges.
//
// `forward`/`lineIndex`/`oneLine` are the reference's own three-call idiom -- forward, the single
// line between the two directions, then backward -- kept verbatim because the middle call is what
// draws the line the phase actually landed on.
void iterateLines(bool forward, int lineIndex, bool oneLine, double slope, double hotIntercept,
                  double dy, double thickness, bool antialias, bool subpixel, int width,
                  int height, std::vector<LinePixel>& scratch, std::vector<std::uint8_t>& out) {
    const auto w = static_cast<double>(width);
    const auto h = static_cast<double>(height);
    double xdraw[2] = {0.0, 0.0};
    double ydraw[2] = {0.0, 0.0};
    bool remaining = true;
    // The reference's loop is unbounded in principle; a degenerate `dy` would spin it. `dy` is a
    // separation over |cos| and the separation is floored below, so this backstop is against a
    // non-finite angle rather than a small one.
    int guard = 0;
    constexpr int kMaxLines = 100000;

    while (remaining && guard++ < kMaxLines) {
        int append = 0;
        remaining = false;
        const double scanIntercept =
            forward ? hotIntercept + dy * lineIndex : hotIntercept - dy * lineIndex;
        ++lineIndex;

        // ⚠ ONLY TWO OF THE FOUR EDGES MAY INCLUDE THEIR LIMITS. The reference is explicit about
        // this: with all four inclusive a corner-to-corner line intersects all four and still
        // counts as an inner line. The <=/< split below is its own.
        if (scanIntercept >= 0.0 && scanIntercept <= h) {
            xdraw[append] = 0.0;
            ydraw[append] = scanIntercept;
            remaining = true;
            ++append;
        }
        if (slope * w + scanIntercept <= h && slope * w + scanIntercept >= 0.0) {
            xdraw[append] = w;
            ydraw[append] = scanIntercept + slope * w;
            remaining = true;
            ++append;
        }
        if (append < 2 && -scanIntercept / slope > 0.0 && -scanIntercept / slope < w) {
            xdraw[append] = -scanIntercept / slope;
            ydraw[append] = 0.0;
            remaining = true;
            ++append;
        }
        if (append < 2 && (h - scanIntercept) / slope > 0.0 && (h - scanIntercept) / slope < w) {
            xdraw[append] = (h - scanIntercept) / slope;
            ydraw[append] = h;
            remaining = true;
            ++append;
        }

        if (!remaining)
            break;
        if (!subpixel) {
            xdraw[0] = myRound(xdraw[0]);
            xdraw[1] = myRound(xdraw[1]);
            ydraw[0] = myRound(ydraw[0]);
            ydraw[1] = myRound(ydraw[1]);
        }
        // ⚠ ONE intersection is a CORNER, and the reference deliberately draws nothing there:
        // "floating point calculations not being quite in sync with algebra".
        if (append != 2)
            continue;

        drawHatchLine(common::Vec2{xdraw[0], ydraw[0]}, common::Vec2{xdraw[1], ydraw[1]}, thickness,
                      antialias, width, height, scratch, out);
        if (oneLine)
            break;
    }
}

void iterateVerticalLines(bool forward, int lineIndex, bool oneLine, double hotX, double separation,
                          double thickness, bool antialias, bool subpixel, int width, int height,
                          std::vector<LinePixel>& scratch, std::vector<std::uint8_t>& out) {
    const auto w = static_cast<double>(width);
    const double yTop = 0.0;
    const double yBottom = static_cast<double>(height);
    int guard = 0;
    constexpr int kMaxLines = 100000;

    while (guard++ < kMaxLines) {
        const double scanX = forward ? hotX + separation * lineIndex : hotX - separation * lineIndex;
        ++lineIndex;
        if (scanX < 0.0 || scanX > w)
            break; // the reference's "no more inner lines" exit

        double x = scanX;
        double bottom = yBottom;
        if (!subpixel) {
            x = myRound(x);
            bottom = myRound(bottom);
        }
        drawHatchLine(common::Vec2{x, yTop}, common::Vec2{x, bottom}, thickness, antialias, width,
                      height, scratch, out);
        if (oneLine)
            break;
    }
}

// One hatch pass at `angleDeg`, into the stencil.
void hatchPass(const HatchingParams& params, double angleDeg, double separation, double thickness,
               double dabX, double dabY, int width, int height, std::vector<LinePixel>& scratch,
               std::vector<std::uint8_t>& out) {
    // `dy` is the vertical distance between two neighbouring lines' intercepts: the separation over
    // |cos(angle)|. Absolute, to keep a negative angle from inverting the walk.
    double dy = std::fabs(separation / std::cos(angleDeg * kPi / 180.0));
    if (!std::isfinite(dy) || dy < 1e-6)
        dy = std::max(separation, 1e-6);
    if (!params.subpixelPrecision)
        dy = std::floor(dy);
    if (dy < 1.0)
        dy = 1.0; // the reference's modf can leave 0 here, which would not advance the walk

    if (angleDeg == 90.0 || angleDeg == -90.0) {
        // A vertical line has no tangent, so the whole intercept machinery is replaced by an x
        // phase. Its own three-call idiom, verbatim.
        const double hotX = std::fmod(params.originX - dabX, separation);
        iterateVerticalLines(true, 1, false, hotX, separation, thickness, params.antialias,
                             params.subpixelPrecision, width, height, scratch, out);
        iterateVerticalLines(true, 0, true, hotX, separation, thickness, params.antialias,
                             params.subpixelPrecision, width, height, scratch, out);
        iterateVerticalLines(false, 1, false, hotX, separation, thickness, params.antialias,
                             params.subpixelPrecision, width, height, scratch, out);
        return;
    }

    // ⚠ THE PHASE IS THE WHOLE POINT. `baseLineIntercept` is the lattice's own line through the
    // DOCUMENT origin point; `cursorLineIntercept` is the line through the dab's top-left corner;
    // the remainder between them is where this dab's nearest lattice line falls inside it. That is
    // what phase-locks the pattern to the document instead of to the dab.
    const double slope = std::tan(angleDeg * kPi / 180.0);
    const double baseLineIntercept = params.originY - slope * params.originX;
    const double cursorLineIntercept = dabY - slope * dabX;
    const double hotIntercept = std::fmod(baseLineIntercept - cursorLineIntercept, dy);

    iterateLines(true, 1, false, slope, hotIntercept, dy, thickness, params.antialias,
                 params.subpixelPrecision, width, height, scratch, out);
    iterateLines(true, 0, true, slope, hotIntercept, dy, thickness, params.antialias,
                 params.subpixelPrecision, width, height, scratch, out);
    iterateLines(false, 1, false, slope, hotIntercept, dy, thickness, params.antialias,
                 params.subpixelPrecision, width, height, scratch, out);
}

} // namespace

double hatchSeparationForParameter(double parameter, double separation, int numIntervals) {
    if (numIntervals < 2 || numIntervals > 7)
        return separation; // the reference complains and passes through; so does this

    const double sizeInterval = 1.0 / static_cast<double>(numIntervals);
    double lowerLimit = 0.0;
    double upperLimit = 0.0;
    int baseFactor = numIntervals / 2;
    // "Make the base separation factor tend to greater instead of lesser numbers when numintervals
    // is even" -- the reference, verbatim.
    if (numIntervals % 2 == 0)
        --baseFactor;

    for (int interval = 0; interval < numIntervals; ++interval) {
        lowerLimit = upperLimit;
        upperLimit += sizeInterval;
        if (interval == numIntervals - 1)
            upperLimit = 1.0;
        if (parameter >= lowerLimit && parameter <= upperLimit)
            return separation * std::pow(2.0, static_cast<double>(baseFactor - interval));
    }
    return separation;
}

double hatchSpinAngle(double baseAngleDeg, double spinDeg) {
    double tempAngle = baseAngleDeg + spinDeg;
    double factor = 1.0;
    if (tempAngle < 0.0)
        factor = -1.0;
    tempAngle = std::fabs(std::fmod(tempAngle, 180.0));
    if (tempAngle >= 0.0 && tempAngle <= 90.0)
        return factor * tempAngle;
    if (tempAngle > 90.0 && tempAngle <= 180.0)
        return factor * -(180.0 - tempAngle);
    return 0.0; // unreachable except on NaN, exactly as the reference notes
}

void hatchStencil(const HatchingParams& params, const HatchingDabValues& values, double dabX,
                  double dabY, int width, int height, std::vector<std::uint8_t>& out) {
    out.assign(static_cast<std::size_t>(std::max(0, width)) *
                   static_cast<std::size_t>(std::max(0, height)),
               static_cast<std::uint8_t>(0));
    if (width <= 0 || height <= 0)
        return;

    // The two derived quantities, in the reference's own order and arithmetic. ⚠ The thickness is
    // ROUNDED to a whole pixel and floored at 1 -- a hatch line is never thinner than a pixel,
    // whatever the sensor says.
    const double thickness =
        std::max(1.0, std::round(params.thickness * values.thickness));
    double separation = params.separation;
    if (values.separationChecked) {
        separation = hatchSeparationForParameter(values.separation, params.separation,
                                                 params.separationIntervals);
    }
    if (!(separation > 0.05))
        separation = 0.05; // a non-positive separation is not a lattice, it is a loop

    std::vector<LinePixel> scratch;

    // ⚠ THE PASS ORDER IS THE REFERENCE'S AND IT IS LOAD-BEARING: the crosshatch block first, then
    // the Angle option's pass, and the "base" pass ONLY when the style is not moiré and the Angle
    // option is unchecked. So a moiré preset with its Crosshatching option checked lays exactly ONE
    // pass, at a sensor-driven angle -- which is what makes the pattern beat against itself as the
    // sensor moves, rather than two fixed passes crossing.
    if (values.crosshatchingChecked) {
        switch (params.style) {
        case CrosshatchingStyle::Perpendicular:
            if (values.crosshatching > 0.5)
                hatchPass(params, hatchSpinAngle(params.angle, 90.0), separation, thickness, dabX,
                          dabY, width, height, scratch, out);
            break;
        case CrosshatchingStyle::MinusThenPlus:
            if (values.crosshatching > 0.33)
                hatchPass(params, hatchSpinAngle(params.angle, -45.0), separation, thickness, dabX,
                          dabY, width, height, scratch, out);
            if (values.crosshatching > 0.67)
                hatchPass(params, hatchSpinAngle(params.angle, 45.0), separation, thickness, dabX,
                          dabY, width, height, scratch, out);
            break;
        case CrosshatchingStyle::PlusThenMinus:
            if (values.crosshatching > 0.33)
                hatchPass(params, hatchSpinAngle(params.angle, 45.0), separation, thickness, dabX,
                          dabY, width, height, scratch, out);
            if (values.crosshatching > 0.67)
                hatchPass(params, hatchSpinAngle(params.angle, -45.0), separation, thickness, dabX,
                          dabY, width, height, scratch, out);
            break;
        case CrosshatchingStyle::Moire:
            hatchPass(params, hatchSpinAngle(params.angle, values.crosshatching * 360.0), separation,
                      thickness, dabX, dabY, width, height, scratch, out);
            break;
        case CrosshatchingStyle::None:
            break;
        }
    } else {
        switch (params.style) {
        case CrosshatchingStyle::Perpendicular:
            hatchPass(params, hatchSpinAngle(params.angle, 90.0), separation, thickness, dabX, dabY,
                      width, height, scratch, out);
            break;
        case CrosshatchingStyle::MinusThenPlus:
            hatchPass(params, hatchSpinAngle(params.angle, -45.0), separation, thickness, dabX, dabY,
                      width, height, scratch, out);
            hatchPass(params, hatchSpinAngle(params.angle, 45.0), separation, thickness, dabX, dabY,
                      width, height, scratch, out);
            break;
        case CrosshatchingStyle::PlusThenMinus:
            hatchPass(params, hatchSpinAngle(params.angle, 45.0), separation, thickness, dabX, dabY,
                      width, height, scratch, out);
            hatchPass(params, hatchSpinAngle(params.angle, -45.0), separation, thickness, dabX, dabY,
                      width, height, scratch, out);
            break;
        case CrosshatchingStyle::Moire:
            hatchPass(params, hatchSpinAngle(params.angle, -10.0), separation, thickness, dabX, dabY,
                      width, height, scratch, out);
            break;
        case CrosshatchingStyle::None:
            break;
        }
    }

    // ⚠ `spinAngle` ALREADY ADDS the preset's own angle, so this call adds it twice. That is the
    // reference's arithmetic at this exact call site, reproduced rather than corrected.
    if (values.angleChecked) {
        hatchPass(params, hatchSpinAngle(params.angle, values.angle * 360.0 + params.angle),
                  separation, thickness, dabX, dabY, width, height, scratch, out);
    }

    // The base pass -- unless moiré or a driven angle already laid one.
    if (params.style != CrosshatchingStyle::Moire && !values.angleChecked) {
        hatchPass(params, params.angle, separation, thickness, dabX, dabY, width, height, scratch,
                  out);
    }
}

} // namespace mosaic::core::brush
