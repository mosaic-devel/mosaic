#pragma once

#include <cstdint>
#include <variant>
#include <vector>

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "core/vector/pattern.hpp"  // vec::Pattern (procedural + image)

// Vector paint & stroke (S25; docs/vector-model.md §2.3-2.4). A Paint says HOW a region is
// coloured; a Stroke says how its outline is drawn. Colours are common::ColorF (float RGBA),
// not Color8 -- vectors are precision-independent, and float stops keep gradients band-free.
// This is the SVG paint model (linear/radial gradients + spreadMethod, cap/join/dash) plus two
// modern extras flagged below, both of which degrade gracefully on SVG export.
namespace mosaic::core::vec {

using common::ColorF;

// ---------------------------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------------------------
struct GradientStop {
    double offset = 0.0;  // along the gradient, in [0,1]
    ColorF color;
    // The BLEND CURVE (S22): where, within the segment to the NEXT stop, this stop's colour and the
    // next are mixed 50/50 -- the classic gradient "midpoint" of Photoshop's diamond markers and the
    // CSS `<color-hint>`. In (0,1) of the segment; 0.5 = a straight linear blend (the default, so old
    // gradients are unchanged). Values off 0.5 bias the mix toward one end (a non-linear ramp per
    // segment). The LAST stop's midpoint is unused (no following segment). A very old idea; see
    // docs/gradient-tool.md.
    double midpoint = 0.5;
    bool operator==(const GradientStop&) const = default;
};

enum class GradientType { Linear, Radial, Conic };       // Conic = our extra (no SVG 1.1 export)
enum class SpreadMethod { Pad, Repeat, Reflect };        // == SVG spreadMethod

// How a gradient's ramp is DITHERED before anything downstream quantises it (S22). A ramp spread
// over more pixels than it has 8-bit steps bands visibly; perturbing the sampled colour by a
// fraction of ONE quantisation step breaks each band edge into texture the eye integrates away.
// The kind rides the PAINT, not the tool, so it round-trips through the document and re-renders
// identically -- exactly like SpreadMethod above.
//
// Every kind here is a POINT function of the destination pixel, which is what lets the compositor,
// the layer-effects overlays and the region fill share one evaluation (see sampleAt). Error
// diffusion (Floyd-Steinberg, 1976) is deliberately ABSENT: it is a sequential neighbourhood
// filter, not a point function -- see docs/gradient-tool.md §7 for the full reasoning.
//
// Technique lineage, all published and decades old: B.E. Bayer, "An optimum method for two-level
// rendition of continuous-tone pictures", IEEE ICC 1973 (the ordered matrix); Holladay's
// screen-cell tiling, 1978; Adobe PostScript halftone screens, 1985; R. Ulichney, "Digital
// Halftoning", MIT Press 1987 (the white/blue-noise analysis) and "The
// void-and-cluster method for dither array generation", Proc. SPIE 1913, 1993 (the blue-noise
// tile generator we build).
enum class DitherKind {
    None,       // the exact ramp -- bit-identical to a pre-dither Mosaic (the default)
    Ordered,    // Bayer 8x8 threshold matrix: deterministic, tiling, a fine cross-hatch texture
    BlueNoise,  // void-and-cluster 64x64 threshold tile: high-frequency noise, no readable pattern
    Noise,      // per-channel triangular-PDF white noise: structureless, flat spectrum
};

struct Gradient {
    GradientType type = GradientType::Linear;
    std::vector<GradientStop> stops;
    common::Affine2D transform = common::Affine2D::identity();  // gradient unit-space -> object-local
    SpreadMethod spread = SpreadMethod::Pad;
    DitherKind dither = DitherKind::None;  // banding control (S22); None renders the exact ramp
    bool operator==(const Gradient&) const = default;
};

struct SolidPaint {
    ColorF color;
    bool operator==(const SolidPaint&) const = default;
};

// An explicit "no paint" alternative so an unpainted fill/stroke is a first-class state (not a
// magic transparent colour).
struct NoPaint {
    bool operator==(const NoPaint&) const = default;
};

using Paint = std::variant<NoPaint, SolidPaint, Gradient, Pattern>;

// The single colour that best stands for a paint -- what a consumer that can only take ONE colour
// per region should use. Solid is exact; a gradient answers with the average of its stops, which
// is the honest summary of a ramp; a pattern and NoPaint answer transparent (nothing to stand for).
//
// Its caller is 3D (docs/type-tool.md §10.4): a solid's surface shades with one albedo per run, so
// an extruded gradient run has to pick one. A gradient that must stay a gradient ON the face is
// what the §12 Layer-Effects overlay is for -- it replaces the albedo per fragment -- and that
// path is unaffected by this.
[[nodiscard]] inline ColorF representativeColor(const Paint& p) noexcept {
    if (const auto* solid = std::get_if<SolidPaint>(&p))
        return solid->color;
    if (const auto* g = std::get_if<Gradient>(&p)) {
        if (g->stops.empty())
            return ColorF{0.0f, 0.0f, 0.0f, 0.0f};
        float r = 0.0f, gg = 0.0f, b = 0.0f, a = 0.0f;
        for (const GradientStop& st : g->stops) {
            r += st.color.r;
            gg += st.color.g;
            b += st.color.b;
            a += st.color.a;
        }
        const float n = static_cast<float>(g->stops.size());
        return ColorF{r / n, gg / n, b / n, a / n};
    }
    return ColorF{0.0f, 0.0f, 0.0f, 0.0f}; // NoPaint / Pattern: no ink of its own to name
}

// Is this paint a gradient? The one predicate that tells a GRADIENT layer from a plain shape layer
// (docs/vector-model.md §1: both are a VectorLayer, they differ only in the paint), so the Gradient
// tool and the Shape tool can each bind the objects they can actually express.
[[nodiscard]] inline bool isGradient(const Paint& p) noexcept {
    return std::holds_alternative<Gradient>(p);
}

// The DESTINATION pixel a paint sample lands on -- the dither key, and nothing else. Dithering is a
// device-space operation (its whole point is scattering quantisation error across neighbouring
// OUTPUT pixels), so it cannot be derived from `localPt`: the rasteriser passes layer-local px
// there, while the layer-effects renderer and the region fill pass the content box normalised to
// [0,1]^2. Callers that own a pixel grid pass theirs; the default ("no pixel") evaluates the exact
// ramp, which is what point queries want -- hit tests, the flyout's single-colour probes, tests.
struct SamplePixel {
    std::int32_t x = 0;
    std::int32_t y = 0;
    bool valid = false;
};

// The signed dither offset for destination pixel (px,py) and channel `ch` (0..3 = R,G,B,A), in
// units of ONE 8-bit quantisation step (1/255). Pure, deterministic and tiling, so a re-render --
// on another thread, at another time, or of one dirty tile -- reproduces it exactly.
// Ordered/BlueNoise return the SAME threshold for every channel (a luminance-only dither: the
// prepress convention, and no chroma speckle on a grey ramp) and live in [-0.5, 0.5); Noise
// returns an independent triangular-PDF sample per channel in [-1, 1] (common::ditherTPDF, the
// formula already shipping in the sky renderer's banding fix). None returns exactly 0.0.
[[nodiscard]] double ditherOffsetLsb(DitherKind kind, std::int32_t px, std::int32_t py, int ch);

// Evaluate a Paint at a point in its object-local coordinate space: constant for SolidPaint,
// transparent (0,0,0,0) for NoPaint, the gradient colour (its transform + spread applied) for
// Gradient, and the tiled pattern colour for Pattern (procedural kinds tile with period = pattern
// scale, in the SAME coordinate units the point is given). Shared by the vector rasteriser
// (raster.cpp) and the layer-effects overlays / patterned strokes, so a paint samples identically
// everywhere. NOTE: a Pattern's coordinate space is layer px (not normalised), so the effect
// renderer must feed it real box-relative px, not the [0,1] it uses for gradients. `antialias` (the
// document-wide AA setting) only affects Pattern edges; solids/gradients ignore it.
// `pixel` is the destination-pixel DITHER key (see SamplePixel): pass it wherever a pixel grid
// exists, so a dithered gradient dithers identically in the compositor, the layer-effects overlays
// and the region fill. Omitting it evaluates the exact ramp -- and a DitherKind::None gradient is
// bit-identical either way.
[[nodiscard]] ColorF sampleAt(const Paint& paint, common::Vec2 localPt, bool antialias = true,
                              SamplePixel pixel = {});

// ---------------------------------------------------------------------------------------------
// Stroke
// ---------------------------------------------------------------------------------------------
enum class LineCap { Butt, Round, Square };
enum class LineJoin { Miter, Round, Bevel };
// SVG strokes are centre-aligned only; Inside/Outside (our extra, like Affinity/Figma) are
// realised by coverage-clipping a double-width centred stroke against the fill (raster.cpp), and
// exported by outlining the stroke to a fill / clipPath. Open paths have no inside -> Center.
enum class StrokeAlign { Center, Inside, Outside };

struct Stroke {
    Paint paint = NoPaint{};        // a stroke can be a gradient too
    double width = 1.0;
    double miterLimit = 4.0;
    double dashOffset = 0.0;        // a distance along the path (arc-length; see samplePathAt)
    LineCap cap = LineCap::Butt;
    LineJoin join = LineJoin::Miter;
    StrokeAlign align = StrokeAlign::Center;
    std::vector<double> dashArray;  // empty == solid
    bool enabled = false;

    bool operator==(const Stroke&) const = default;
};

}  // namespace mosaic::core::vec
