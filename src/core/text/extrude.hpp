#pragma once

#include <cstddef>
#include <map>
#include <vector>

#include "common/geometry3d.hpp"  // Vec3 / Quat (orientation, light directions)
#include "common/image.hpp"       // ColorF

// The Type tool's 3D extrusion parameters (docs/type-tool.md §10.1, S30-c). A TextBlock carries
// `std::optional<Extrude>` -- nullopt is the flat 2D default, so every existing block is
// untouched. The block extrudes as ONE watertight solid with one shared depth/bevel/orientation
// (§1.2: per-run depth was rejected -- coplanar per-run solids z-fight); materials MAY vary per
// run via the sparse override map (§10.4). Pure data, FLTK/FreeType-free; consumed by
// extrude_mesh (geometry) and extrude_render (the CPU/Vulkan lanes).
namespace mosaic::core::text {

using common::ColorF;
using common::Quat;
using common::Vec3;

// The profile cut into the front/back edges of the solid (§10.2 step 4). `size` = 0 disables the
// bevel (a sharp 90-degree edge); `segments` is the ring count a curved profile is built from
// (Flat ignores it -- a chamfer is one ring by construction).
struct Bevel {
    enum class Profile { Flat, Round, Convex, Concave } profile = Profile::Round;
    float size = 0.0f;  // layer units, measured diagonally into the edge
    int segments = 3;   // rings per curved profile (clamped >= 1 by the mesher)

    bool operator==(const Bevel&) const = default;
};

// A PBR-lite surface FINISH (§10.3): metalness + roughness, shaded by the toggle-gated
// Blinn-Phong lane. A run with no entry in Extrude::runMaterials inherits Extrude::material.
//
// ⚠ NO COLOUR HERE, deliberately. Albedo used to live on this struct, and that made a 3D layer's
// colour a second, parallel piece of state beside the colour the layer already had -- a text run's
// paint, a shape's fill. The two could not be kept in sync and were not: a shape seeded its albedo
// from its fill ONCE when 3D was switched on and then ignored every later colour change, and a
// text block ignored its run colours entirely and started at a flat grey. Worse, 2D text carries a
// colour PER RUN and this carried one per layer, so going 3D silently flattened a multi-coloured
// block.
//
// Colour is now supplied per render, by run, in an ExtrudePalette the caller fills from the layer
// itself (see below). Finish stays here because it has no 2D equivalent -- there is no "roughness"
// of a flat fill -- so it is the only thing 3D genuinely adds to a surface.
struct Material {
    float metalness = 0.0f;
    float roughness = 0.5f;

    bool operator==(const Material&) const = default;
};

// One directional light. `direction` is the direction the light TRAVELS (from the lamp into the
// scene), in the solid's model space -- so a light at the viewer's upper-left shines along
// {+x-ish, +y-ish... see kDefaultKeyLight}. Normalized by the render lane, not stored normalized.
struct Light {
    Vec3 direction{0.45, 0.55, -0.7};
    ColorF color{1.0f, 1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;

    bool operator==(const Light&) const = default;
};

// The default soft key light (§10.3: "one soft key light by default so enabling 3D looks good
// immediately"): from the viewer's upper-left, shining right/down/into the screen. Model space is
// the layer's frame (x right, y DOWN, +z toward the viewer), hence +y = shining downward.
inline constexpr Light kDefaultKeyLight{{0.45, 0.55, -0.7}, {1.0f, 1.0f, 1.0f, 1.0f}, 0.9f};

struct Extrude {
    float depth = 20.0f;  // layer units, shared by the whole block (no per-run depth -- §1.2)
    Bevel bevelFront;     // the edge the viewer sees...
    Bevel bevelBack;      // ...and its far twin (independent profiles)
    Material material;    // the per-layer default; runMaterials overrides per run (§10.4)

    // Orientation of the solid: a quaternion (any-axis, gimbal-free -- settled 2026-06-25). The
    // layer's 2D transform still positions/scales the RENDERED RESULT in the document; this
    // orients the solid itself (§10.3 -- no "3D vs 2D transform" mode confusion).
    Quat orientation = Quat::identity();

    // Vertical camera FOV in DEGREES. Small = the near-orthographic default (calm, graphic,
    // logo-right); larger = drama on demand; -> 0 = true orthographic (§10.3).
    float perspective = 10.0f;

    // The lighting toggle: off => flat self-lit faces (clean, fast, predictable); on => the
    // Blinn-Phong / PBR-lite lane over `lights` + `ambient`, plus the procedural studio
    // environment metals mirror (chrome needs SOMETHING to reflect -- a lone key light reads as
    // dark plastic; user feedback 2026-07-03).
    bool lightingEnabled = true;
    std::vector<Light> lights{kDefaultKeyLight};
    ColorF ambient{0.25f, 0.25f, 0.25f, 1.0f};

    // Metallic reflections mirror the CANVAS CONTENT behind the solid (the composite below this
    // layer) instead of only the studio gradient: the render lanes intersect each reflected ray
    // with the canvas plane at the solid's back cap and sample a snapshot the app provides
    // (ExtrudeEnv). Off by default -- the studio look is calmer and snapshot-free.
    bool reflectCanvas = false;
    // Restrict the canvas mirror to the extruded SIDES (walls + bevels): the caps -- the face the
    // text reads on -- keep the studio finish. A head-on face mirroring the artwork reads odd at
    // some angles (user 2026-07-03); the sides are where the effect earns its keep. Only
    // meaningful while reflectCanvas is set.
    bool reflectSidesOnly = false;

    // Sparse per-run material overrides (§10.4): run index -> material. Empty by default -- the
    // simple case shows ONE material control and draws one pass. The mesh partitions into
    // per-material index ranges of the same vertex buffer; geometry never changes, so no
    // z-fighting and no seams.
    std::map<std::size_t, Material> runMaterials;

    // Layer-Effects overlays on 3D text (§12, S30-e): a colour/gradient/pattern overlay is
    // evaluated in the glyph's 2D design space and applied to the FRONT FACE (the face the design
    // reads on); the side walls and bevels keep the shaded base material -- never a re-projected
    // smear over the whole rendered rectangle. This opt-in wraps the overlay onto the sides too
    // (each wall/bevel texel takes the design colour of the outline point it extrudes from). Off
    // by default per the settled design.
    bool overlayWrapSides = false;

    bool operator==(const Extrude&) const = default;
};

// The colour each run's surface paints with (§10.4), indexed by runIndex -- the 3D half of the
// SAME colour the layer draws with in 2D, resolved by the caller because only the caller knows
// what the layer is: a text block's per-run paint, a shape's fill (run 0) and stroke (run 1).
//
// It is a render input rather than stored state, and that is what makes 3D colour track 2D colour
// with nothing to keep in sync -- there is no second copy to go stale. It also restores per-run
// colour to 3D text, which the single per-layer albedo had flattened.
//
// A run's ALPHA is its coverage: an entry with alpha 0 draws nothing, which is how a NoPaint fill
// (and a run that contributes no ink) reports itself, and both render lanes already skip a range
// whose alpha is zero.
struct ExtrudePalette {
    std::vector<ColorF> runs;                // index = runIndex
    ColorF fallback{0.8f, 0.8f, 0.8f, 1.0f}; // a run the caller named no colour for

    [[nodiscard]] ColorF forRun(std::size_t runIndex) const {
        return runIndex < runs.size() ? runs[runIndex] : fallback;
    }
};

// Resolve the §10.4 material for a run: the sparse override, or the shared default. One
// definition shared by the CPU lane, the Vulkan lane and the S30-e overlay-map builder.
[[nodiscard]] inline const Material& materialForRun(const Extrude& params, std::size_t runIndex) {
    const auto it = params.runMaterials.find(runIndex);
    return it != params.runMaterials.end() ? it->second : params.material;
}

}  // namespace mosaic::core::text
