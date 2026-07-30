#pragma once

#include "core/brush/curve.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

// The six procedural brush tips (docs/brushes.md §3.4): `default | soft | gauss` falloff crossed
// with a `circle | rect` shape. 35 of the 82 pixel-brush presets in the CC-0 default set use one.
//
// The whole point of this file is that dab coverage is a PURE FUNCTION of the offset from the dab
// centre and a block of precomputed coefficients:
//
//     coverage = coverageAt(x, y)      // depends on nothing but x, y and the coefficients
//
// so the same formula can later be mirrored line-for-line in GLSL with the coefficients in a UBO and
// the softness curve in a 1D texture -- the CPU/GPU parity-lane idiom already proven by
// `extrude_render` vs `extrude_raster.comp` (docs/brushes.md §6.3). Nothing here reads the stroke,
// allocates per dab, or depends on FLTK or Vulkan.
//
// Coverage is 1 at the centre and 0 outside the tip. (The format's own generators are inverted --
// 0 means fully painted -- which is a trap when transcribing their formulas.)
namespace mosaic::core::brush {

// A linear falloff from an opaque centre to a clear rim. Stands in when a `soft` generator arrives
// without the `softness_curve` attribute it is supposed to carry.
inline constexpr std::string_view kDefaultSoftnessCurve = "0,1;1,0;";

enum class MaskShape : std::uint8_t { Circle, Rect };

enum class MaskFalloff : std::uint8_t {
    Default, // an analytic shoulder; `hFade`/`vFade` set where it starts
    Soft,    // the shoulder is an arbitrary user curve
    Gauss,   // an erf profile: a disc convolved with a Gaussian
};

// The serialized `id` and `type` attributes of `<MaskGenerator>`.
[[nodiscard]] std::string_view maskFalloffName(MaskFalloff f) noexcept;
[[nodiscard]] std::optional<MaskFalloff> maskFalloffFromName(std::string_view name) noexcept;
[[nodiscard]] std::string_view maskShapeName(MaskShape s) noexcept;
[[nodiscard]] std::optional<MaskShape> maskShapeFromName(std::string_view name) noexcept;

struct MaskGeneratorParams {
    MaskShape shape = MaskShape::Circle;
    MaskFalloff falloff = MaskFalloff::Default;

    double diameter = 24.0; // the tip's width in document px; its height is diameter * ratio
    double ratio = 1.0;     // height / width. 0 makes the tip empty, which is legal and paints nothing
    double hFade = 1.0;     // [0,1]; the ATTRIBUTE value, not the halved form the reference stores
    double vFade = 1.0;
    int spikes = 2;             // 2 is an ordinary tip; >2 folds it into a rotationally-repeated wedge
    bool antialiasEdges = true; // a ~1 px linear ramp at the rim, on top of whatever the falloff does

    // The live `Softness` curve option, 1 = as authored. It scales the fade coefficients of the
    // `Default` falloff and re-shapes the `Soft` falloff's curve.
    //
    // ⚠ It does NOTHING to a `Gauss` tip. That is faithful, not an oversight: the reference's gauss
    // generators do not override the softness hook at all, so a preset pairing an enabled Softness
    // option with a gauss mask is silently static. An importer should report it (§6.4).
    double softness = 1.0;

    // `Soft` only, and always present on a well-formed one (the `softness_curve` attribute of §3.4).
    // It maps the normalized SQUARED radius (circle) or the normalized axis distance (rect) DIRECTLY
    // to coverage -- so it must DESCEND: 1 at the centre, 0 at the rim. Every softness curve in the
    // shipped set does.
    //
    // The identity curve would therefore build an inside-out tip, opaque at the rim and clear at the
    // centre. That is why the default here is an explicit falloff rather than `Curve{}`: a `soft`
    // generator whose attribute went missing should degrade to a plain linear tip, not to a ring.
    Curve softnessCurve = Curve::fromString(kDefaultSoftnessCurve);
};

class MaskGenerator {
public:
    explicit MaskGenerator(MaskGeneratorParams params);

    // Coverage in [0,1] at `(x, y)` px from the dab centre, in the TIP's own frame: the caller has
    // already undone the dab's rotation and scatter. Pure, total, and finite for every input --
    // including the degenerate parameter sets a hostile preset can ask for.
    [[nodiscard]] double coverageAt(double x, double y) const noexcept;

    // A zero diameter or a zero ratio. Paints nothing rather than dividing by zero.
    [[nodiscard]] bool isEmpty() const noexcept { return m_empty; }

    [[nodiscard]] const MaskGeneratorParams& params() const noexcept { return m_params; }
    [[nodiscard]] double width() const noexcept { return m_width; }   // diameter
    [[nodiscard]] double height() const noexcept { return m_height; } // diameter * ratio

private:
    // Fold `(x, y)` into a single angular wedge when `spikes > 2`, so one falloff evaluation serves
    // all the spikes. Operates on the half-plane y >= 0.
    void fixRotation(double& x, double& y) const noexcept;

    [[nodiscard]] double circleDefault(double x, double y) const noexcept;
    [[nodiscard]] double rectDefault(double x, double y) const noexcept;
    [[nodiscard]] double circleGauss(double x, double y) const noexcept;
    [[nodiscard]] double rectGauss(double x, double y) const noexcept;
    [[nodiscard]] double circleSoft(double x, double y) const noexcept;
    [[nodiscard]] double rectSoft(double x, double y) const noexcept;

    // Raw profiles, before the antialiasing rim. Named so the rim can sample them at its own start.
    [[nodiscard]] double gaussCircleProfile(double dist) const noexcept;
    [[nodiscard]] double softCurve(double t) const noexcept;
    [[nodiscard]] double softRectAxis(double t) const noexcept;

    MaskGeneratorParams m_params;

    // Everything below is the "UBO": derived once, read-only, and the sole state `coverageAt` sees.
    bool m_empty = false;
    double m_width = 0.0;
    double m_height = 0.0;

    double m_spikeCos = 0.0; // cos/sin of one spike step, and the half-wedge angle
    double m_spikeSin = 0.0;
    double m_spikeAngle = 0.0;

    double m_xCoef = 0.0; // 2 / width, 2 / height -- normalize to the unit disc/square
    double m_yCoef = 0.0;
    double m_fadeX = 0.0; // the fade coefficients, already scaled by softness
    double m_fadeY = 0.0;

    // Gauss.
    double m_gaussYCoef = 0.0;
    double m_gaussCenter = 0.0;
    double m_gaussAlpha = 0.0;
    double m_gaussDist = 0.0;
    double m_gaussRadius = 0.0;
    double m_rectGaussXFade = 0.0;
    double m_rectGaussYFade = 0.0;
    double m_rectGaussHalfW = 0.0;
    double m_rectGaussHalfH = 0.0;
    double m_rectGaussAlpha = 0.0;

    // Soft. The LUT is the 1D texture the GLSL lane would bind.
    std::vector<float> m_softLut;
    double m_softFadeStart = 0.0; // in squared-normalized radius, for the circle

    // Half-extents and antialiasing band, for the two rect falloffs that fade in x and y separately.
    double m_halfWidth = 0.0;
    double m_halfHeight = 0.0;
};

} // namespace mosaic::core::brush
