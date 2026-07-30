#pragma once

#include "core/brush/curve.hpp"  // the ONE curve type -- S34's Curves reuses it (docs §2.1)
#include "core/layer.hpp"
#include "core/vector/paint.hpp"  // the ONE gradient type -- S34-a's Gradient Map reuses it (§2.6)

#include <map>
#include <span>
#include <string>
#include <string_view>

// The S32 typed parameter system for adjustment layers (docs/adjustment-layers.md). The storage
// stays the AdjustmentLayer name->double params bag -- that is what docio round-trips and what the
// compositor reads -- but every parameter an adjustment kind understands is DECLARED here: key,
// human label, range, default, UI step. The schema is the single source of truth three consumers
// share: the compositor reads through it (absent keys fall back to the declared default, present
// ones clamp to the declared range, so a hostile .mosaic file can never feed the math garbage),
// the Filter-menu insert seeds a fresh layer from it, and the adjustment editor generates its
// controls from it -- adding a parameter to a kind is one table row, not four call sites.
namespace mosaic::core {

// The type picks the editor CONTROL (and nothing else -- every kind is stored/read as the same
// name->double bag). Scalar covers everything continuous (a ScrubSlider in the editor); Toggle is a
// 0/1 flag (a CheckBox; read through adjustmentParamValue like any scalar, >= 0.5 means on); Choice
// is a small enumeration (a Dropdown; the bag stores the option INDEX as a double, docio-compatible,
// and the read clamps to [0, count-1] like any range); Angle is a continuous value that reads better
// as a rotary knob (a ui::Dial) -- it is a plain scalar to every other consumer (compositor, docio,
// seeding, adjustmentParamValue all treat it exactly like Scalar), only the editor renders it round.
// There is deliberately still NO curve-valued param type after S34: a Curves layer's four tone
// curves live in the SAME name->double bag as indexed knots (adjustmentCurve/setAdjustmentCurve
// below), so undo, coalescing and .mosaic round-tripping needed no new machinery at all
// (docs/adjustment-layers.md §2.1 records the decision and why the string-bag alternative lost).
enum class AdjustmentParamType { Scalar, Toggle, Choice, Angle };

struct AdjustmentParamDesc {
    const char* key;    // the params-bag key (stable; serialized by docio)
    const char* label;  // human label for the editor row (ASCII -- FLTK label rule)
    double min = 0.0;
    double max = 1.0;
    double def = 0.0;
    double step = 0.01;        // editor increment
    const char* suffix = "";   // editor unit tag ("", " EV", " deg", "%")
    AdjustmentParamType type = AdjustmentParamType::Scalar;
    // Choice rows only: the option labels, index order = the stored value. min/max are derived
    // from the count by the schema author (0 .. count-1) so the clamped read stays uniform.
    const char* const* choices = nullptr;
    int choiceCount = 0;
    // Choice rows only, both optional and EDITOR-ONLY (the compositor, docio and seeding never
    // read them -- the stored value is always the option index). They let a dropdown be GROUPED
    // and REORDERED for display without moving the stored index, so a saved document is untouched.
    // `choiceOrder` (length choiceCount) maps a dropdown POSITION to the stored value it shows;
    // null = identity (position == stored value). `choiceDivider` (length choiceCount, indexed by
    // dropdown POSITION) draws a group separator below that row; null = no dividers. Grayscale
    // groups its methods this way (docs/adjustment-layers.md §2).
    const int* choiceOrder = nullptr;
    const bool* choiceDivider = nullptr;
};

// The declared parameters of `kind`, in editor display order. Empty for kinds with no knobs
// (Invert). Curves declares exactly one row -- the channel the editor is on; its knots are not
// schema rows (see the curve helpers at the bottom of this header).
[[nodiscard]] std::span<const AdjustmentParamDesc> adjustmentParamSchema(AdjustmentKind kind);

// The descriptor for `key` under `kind`, or nullptr when the kind declares no such parameter.
[[nodiscard]] const AdjustmentParamDesc* adjustmentParamDesc(AdjustmentKind kind,
                                                             std::string_view key);

// The value of `desc` on `layer`: the bag's entry clamped to [desc.min, desc.max], or desc.def
// when the bag has no entry. Every schema-declared read (compositor + editor) funnels here.
[[nodiscard]] double adjustmentParamValue(const AdjustmentLayer& layer,
                                          const AdjustmentParamDesc& desc);

// Fill `layer`'s bag with every declared default (existing entries are overwritten). The
// Filter-menu insert seeds new layers with this so the editor opens on honest values; it is NOT
// called by Document::makeAdjustment -- programmatic creators (the S55 sky commit, tests, docio)
// own their bags.
void seedAdjustmentDefaults(AdjustmentLayer& layer);

// Whether the compositor has real math for `kind` today. The Filter menu offers only implemented
// kinds (a menu item that inserts a do-nothing layer is a broken promise); every kind answers
// true since S34 closed Curves. PhotometricMatch is implemented but created by the S55 sky flow,
// not the menu.
[[nodiscard]] bool adjustmentImplemented(AdjustmentKind kind);

// The Grayscale "method" choice order (the bag stores the index; docs/adjustment-layers.md §2).
// Luma is the default -- the pre-S32 Grayscale formula, so an old document's bag (or an empty
// one) renders byte-identically. The first cut also offered Average/Lightness/Value; the user
// found the set confusing and near-indistinguishable (2026-07-17), so it is now the two
// principled projections plus the photographer's mono-filter channel reads.
enum class GrayscaleMethod {
    NoChrominance, // zero the chroma, keep the photometric luminance (Rec 709 linear, re-encoded)
    Luma,          // 0.30/0.59/0.11 on encoded values (the classic weights; the old behavior)
    Red,           // the red channel as mono (a deep red filter: darkens skies, lifts skin)
    Green,
    Blue,
    // The three later additions. Dithered + AdaptiveThreshold are SPATIAL (they read/diffuse
    // across neighbours, not per pixel) -- the per-INSTANCE adjustmentIsSpatial overload below
    // reports it and the compositor routes them through its whole-buffer branch
    // (docs/adjustment-layers.md §2). MaxChannel is a pure per-pixel projection.
    Dithered,          // Luma, then 1-bit Floyd-Steinberg error-diffusion dither (whole-raster)
    MaxChannel,        // per-pixel max(R,G,B)
    AdaptiveThreshold, // binarize against a local-window mean minus a small bias (per region)
    // Appended (index 8) so the older indices stay byte-stable in the stored bag -- do NOT
    // reorder. MinChannel is the pure per-pixel complement of MaxChannel (a dark, low-key mono).
    MinChannel, // per-pixel min(R,G,B)
};

// The S33 blur family is SPATIAL: it reads the backdrop's neighborhood (and diffuses alpha)
// instead of mapping pixels independently. The compositor routes spatial kinds through its blur
// branch and grows region/group buffers by their reach (docs/blur-filters.md §4-§5); everything
// else (schema, editor, docio, dock) treats them as ordinary adjustment kinds.
[[nodiscard]] bool adjustmentIsSpatial(AdjustmentKind kind);

// Per-INSTANCE spatiality: some kinds are spatial only for certain parameter choices, so the
// by-kind predicate above is not enough. The Grayscale kind is per-pixel for its projection
// methods but SPATIAL for Dithered (Floyd-Steinberg error diffusion, a whole-raster serial pass)
// and Adaptive threshold (a local-window read); every other kind matches its by-kind answer.
// Callers with a layer in hand (the compositor's application + reach walk) use this overload.
[[nodiscard]] bool adjustmentIsSpatial(const AdjustmentLayer& layer);

// ---- Curves (S34) -----------------------------------------------------------------------------
// A Curves layer carries FOUR tone curves: a COMPOSITE curve applied to all three channels plus
// one curve per channel (the shape every editor has shipped since Photoshop 1.0, 1990). The
// compositor composes them into one 256-entry LUT per channel -- per-channel curve first, then
// the composite on its result (docs/adjustment-layers.md §2.1).
//
// STORAGE DECISION (S34). The curves live in the ordinary name->double params bag as INDEXED
// KNOTS, not in a second string-valued bag:
//   <prefix>_n      knot count      <prefix> = "curve_rgb" | "curve_r" | "curve_g" | "curve_b"
//   <prefix>_<i>_x  knot i's x      (both clamped into [0,1] on the way back out)
//   <prefix>_<i>_y  knot i's y
//   <prefix>_<i>_c  1.0 when knot i is a spline CORNER; ABSENT means smooth
// An IDENTITY curve writes no keys at all, so a freshly inserted Curves layer carries only its
// `channel` row and composites byte-identically to no layer. Every consumer therefore needed
// exactly zero new code: SetAdjustmentParamsCommand still stores the whole bag (undo/coalescing
// unchanged), .mosaic still serializes the bag as a JSON number map (round-trip exact -- doubles
// print round-trippably, where core::brush::Curve's own "x,y;" spelling is six-significant-digit
// %g), and a document written before S34 (no curve keys) loads as the identity.
// The curve TYPE is still core::brush::Curve -- the same natural-cubic-spline-with-corners the
// brush dynamics and ui::CurveEditor already speak, so the editor edits the stored object itself.
enum class CurveChannel { Composite, Red, Green, Blue };
inline constexpr int kCurveChannelCount = 4;

// The most knots one channel keeps. A tone curve with more than this is not a thing anyone
// authors; the cap bounds the bag (and a hostile file's decode loop) rather than the editor.
inline constexpr int kMaxCurvePoints = 64;

// `ch`'s bag-key prefix ("curve_rgb" / "curve_r" / "curve_g" / "curve_b").
[[nodiscard]] std::string_view adjustmentCurveKey(CurveChannel ch);

// The curve `ch` stored in `bag` (or on `layer`), or the identity when it holds none. Knots with
// a missing/non-finite coordinate are skipped and the rest are clamped into the unit square, so a
// corrupt or hostile document degrades to a sane curve instead of feeding the LUT garbage.
[[nodiscard]] brush::Curve adjustmentCurve(const std::map<std::string, double>& bag,
                                           CurveChannel ch);
[[nodiscard]] brush::Curve adjustmentCurve(const AdjustmentLayer& layer, CurveChannel ch);

// Write `c` into `bag` under `ch`, replacing whatever was there (a shorter curve must not leave
// the longer one's tail behind). An IDENTITY curve writes nothing -- absent IS the identity.
void setAdjustmentCurve(std::map<std::string, double>& bag, CurveChannel ch,
                        const brush::Curve& c);

// Drop `ch`'s knots (or every channel's -- what the editor's Reset does, since the knots are not
// schema rows and so are not re-seeded by seedAdjustmentDefaults).
void clearAdjustmentCurve(std::map<std::string, double>& bag, CurveChannel ch);
void clearAdjustmentCurves(std::map<std::string, double>& bag);

// True when all four of `layer`'s curves are the identity -- the compositor's byte-level no-op.
[[nodiscard]] bool adjustmentCurvesIdentity(const AdjustmentLayer& layer);

// The Matte Removal "mode" choice order (the bag stores the index; docs/adjustment-layers.md
// §2.4). RemoveBlack and Unpremultiply are the SAME algebra (C/a) under the two names the trade
// uses for it -- both ship because both are what someone reaches for.
enum class MatteMode { RemoveWhite, RemoveBlack, Unpremultiply, Premultiply };

// ---- Gradient Map (S34-a) ---------------------------------------------------------------------
// A Gradient Map layer carries ONE colour ramp: the backdrop's luma indexes it, and the sampled
// colour replaces the pixel (Photoshop 4, 1996). The ramp is a core::vec::Gradient -- the SAME
// type the vector paint model, the gradient tool and ui::GradientFlyout already speak -- so the
// editor edits the stored object itself and the ramp maths has exactly one implementation
// (core/vector/raster.cpp sampleStops, midpoints included).
//
// STORAGE DECISION (S34-a): the ramp lives in the ordinary name->double params bag as INDEXED
// STOPS, exactly the way S34's Curves stores its knots (docs/adjustment-layers.md §2.1 records
// why the string-bag alternative lost, and every word of it applies here):
//   gm_n            stop count      (decode clamps to [0, kMaxGradientMapStops])
//   gm_<i>_t        stop i's offset along the ramp, clamped into [0,1] on decode
//   gm_<i>_r/_g/_b/_a   stop i's colour, each clamped into [0,1]
//   gm_<i>_m        stop i's blend MIDPOINT; ABSENT means 0.5 (the straight linear blend)
// Only the STOPS are stored: a gradient map has no geometry, so the type/transform/spread/dither
// of the vec::Gradient are fixed (Linear, identity, Pad, None) and are not written. Dither in
// particular must never be enabled here -- it is keyed to the DESTINATION pixel, so a dirty-rect
// recomposite would disagree with the full composite (region == crop(full) is not negotiable).
//
// ABSENT IS THE DEFAULT RAMP (black -> white), not an identity: a gradient map has no identity
// setting, so this kind is "inherently visible" in the Threshold/Posterize sense. Writing the
// default ramp erases the keys, so a freshly inserted layer carries only its `reverse` row and a
// pre-S34-a document (no gm_* keys) loads as the classic black-to-white map.
inline constexpr int kMaxGradientMapStops = 32;

// The default ramp: opaque black at 0 -> opaque white at 1 (a luma map).
[[nodiscard]] vec::Gradient defaultGradientMap();

// The ramp stored in `bag` (or on `layer`), or defaultGradientMap() when it holds none. A stop
// with a missing/non-finite field is skipped and the rest are clamped into range, and fewer than
// two surviving stops reads as the default ramp -- a corrupt file must not silently flatten the
// image to one colour. Stops come back sorted by offset (sampleStops requires it).
[[nodiscard]] vec::Gradient adjustmentGradientMap(const std::map<std::string, double>& bag);
[[nodiscard]] vec::Gradient adjustmentGradientMap(const AdjustmentLayer& layer);

// Write `g`'s STOPS into `bag`, replacing whatever was there (a shorter ramp must not leave the
// longer one's tail behind). The default ramp writes nothing -- absent IS the default.
void setAdjustmentGradientMap(std::map<std::string, double>& bag, const vec::Gradient& g);
void clearAdjustmentGradientMap(std::map<std::string, double>& bag);

// ---- Photo Filter (S34-a) ---------------------------------------------------------------------
// The "filter" choice order (the bag stores the index). The named presets are the standard
// photographic filter designations (the Wratten numbering the trade has used since the 1910s);
// Custom takes the colour from the color_r/color_g/color_b rows. Do NOT reorder -- the schema
// indexes its label table by the stored value.
enum class PhotoFilterPreset {
    Warming85,
    Warming81,
    Cooling80,
    Cooling82,
    Red,
    Orange,
    Yellow,
    Green,
    Cyan,
    Blue,
    Violet,
    Magenta,
    Sepia,
    Underwater,
    Custom,
};
inline constexpr int kPhotoFilterPresetCount = 15;

// `p`'s filter colour in the encoded working space (sRGB). Custom returns the Warming (85) colour
// -- the schema's own custom default -- so a caller that forgets to substitute the bag's
// color_r/g/b still gets a sane filter rather than black.
[[nodiscard]] common::Color8 photoFilterPresetColor(PhotoFilterPreset p);

// ---- The S35 stylize choices (docs/filters-stylize.md §1) ------------------------------------

// The Add Noise "distribution" choice order (the bag stores the index). Uniform is scaled to the
// SAME variance as Gaussian, so the Amount slider means one thing in both modes.
enum class NoiseDistribution { Gaussian, Uniform };

// The Wave "mode" choice order: a directional sine displacement, or concentric rings about the
// layer's own centre.
enum class WaveMode { Wave, Ripple };

// The Radial Blur "mode" choice order (the bag stores the index).
enum class RadialBlurMode { Spin, Zoom };

// The Depth of Field "bokeh" choice order: Soft = Gaussian levels, Iris = hexagonal aperture
// levels (docs/blur-filters.md §3).
enum class DofBokeh { Soft, Iris };

// The Color Balance wheels' plane mapping (the S32 pro controls): the three slider axes --
// cyan-red, magenta-green, yellow-blue -- are the R/G/B hue directions 120 degrees apart in the
// chroma plane, so a 2D wheel-puck offset projects onto them and back exactly. The MEAN (an
// equal shift of all three, which no wheel position can express) is preserved by threading it
// through colorBalanceFromPlane, so a hand-authored bag round-trips through a wheel drag with
// its achromatic component intact. Plane convention: +x toward red, +y toward the green side
// (mathematical y-up; the widget flips for screen pixels).
struct ColorBalancePoint {
    double x = 0.0;
    double y = 0.0;
};
struct ColorBalanceTriple {
    double cr = 0.0;
    double mg = 0.0;
    double yb = 0.0;
};
[[nodiscard]] ColorBalancePoint colorBalanceToPlane(const ColorBalanceTriple& v);
[[nodiscard]] ColorBalanceTriple colorBalanceFromPlane(const ColorBalancePoint& p, double mean);

} // namespace mosaic::core
