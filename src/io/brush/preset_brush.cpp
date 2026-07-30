#include "io/brush/preset_brush.hpp"

#include "core/brush/brush_tip.hpp"

#include <memory>
#include <utility>

namespace mosaic::io::brush {

namespace cb = core::brush;

namespace {

[[nodiscard]] double clamp01(double v) noexcept {
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

// The slot each driven base fills. A base with no slot is a base the engine does not read, and
// `kDrivenOptions` is what says which is which -- so this switch and that list are the same fact
// spelled twice, and the test pins them against each other.
[[nodiscard]] std::optional<cb::CurveOption>* slotFor(cb::BrushOptions& o, std::string_view base) {
    if (base == "Size")
        return &o.size;
    if (base == "Flow")
        return &o.flow;
    if (base == "Ratio")
        return &o.ratio;
    if (base == "Rotation")
        return &o.rotation;
    if (base == "Softness")
        return &o.softness;
    if (base == "Opacity")
        return &o.opacity;
    if (base == "SmudgeRate")
        return &o.smudgeRate;
    if (base == "ColorRate")
        return &o.colorRate;
    if (base == "SmudgeRadius")
        return &o.smudgeRadius;
    // Spacing is a plain CurveOption slot (no gates beside it -- Spacing/Isotropic is a static
    // BrushParams flag, not part of the option): the loop below wires it like any other. §6.6e.
    if (base == "Spacing")
        return &o.spacing;
    // The three HSV colour-dynamics channels (§6.6f) are plain CurveOption slots too, like Spacing:
    // no gates beside them, so the loop below wires each like any other. The engine reads them only
    // on the Colored accumulator (BrushParams::colorDynamicsActive gates it).
    if (base == "h")
        return &o.hue;
    if (base == "s")
        return &o.saturation;
    if (base == "v")
        return &o.value;
    // The sketch engine's three (§6.6g) are plain CurveOption slots too. They reach only the sketch
    // StrokePainter -- the dab walk has no use for any of them -- exactly as the smudge trio reaches
    // only the smudge walk; the mapper owns the badge that keeps that honest.
    if (base == "Density")
        return &o.density;
    if (base == "Line width")
        return &o.lineWidth;
    if (base == "Offset scale")
        return &o.offsetScale;
    // The curve engine's own opacity, and the hatching engine's four (§6.6g). Same shape: plain
    // slots, single-consumer, badged by the mapper anywhere else.
    if (base == "Curves opacity")
        return &o.curvesOpacity;
    if (base == "Angle")
        return &o.hatchAngle;
    if (base == "Crosshatching")
        return &o.crosshatching;
    if (base == "Separation")
        return &o.separation;
    if (base == "Thickness")
        return &o.thickness;
    // The texture strength and the airbrush rate (§6.6h). Plain slots too; each has exactly one
    // consumer (the texture composite / the timed cadence) and, unlike the smudge and sketch
    // families, an active one with its consumer off is the reference's own no-op, so the mapper
    // badges neither.
    if (base == "Texture/Strength/")
        return &o.textureStrength;
    if (base == "Rate")
        return &o.rate;
    return nullptr;
}

} // namespace

cb::BrushParams presetBrushParams(const LibraryPreset& lp) {
    const BrushPreset& p = lp.preset;
    cb::BrushParams bp;

    // The tip's absolute geometry. `masterDiameter` is resolved by the library, because a predefined
    // tip's size is only knowable once its file is decoded (scale x max(w, h) of the first frame) --
    // and re-deriving it here would be a second copy of a rule that has already moved once.
    bp.diameter = lp.masterDiameter;
    bp.angleRad = p.tip.angle; // radians in the file; summed straight into the dab's rotation
    bp.spacing = p.tip.spacing;
    bp.useAutoSpacing = p.tip.useAutoSpacing;
    bp.autoSpacingCoeff = p.tip.autoSpacingCoeff;
    // The spacing MODE is a paintop property, not a tip attribute -- so it comes off the preset and
    // not off `p.tip`, unlike the three above.
    bp.isotropicSpacing = p.isotropicSpacing;

    if (lp.tip != nullptr) {
        // A BITMAP tip. Its ratio is 1: the frame's own aspect is preserved INSIDE the dab's envelope
        // (bitmapDabShape), so squashing it here would squash it twice. `Ratio` -- the option -- still
        // squashes it, which is what the option means.
        bp.tip = cb::makeTip(lp.tip);
        bp.ratio = 1.0;
    } else {
        // A PROCEDURAL tip. The generator's own diameter and ratio are its authored geometry, and the
        // dab overrides both per dab (brush_tip.hpp) -- so they are copied into the params, where the
        // Size and Ratio options can scale them, rather than left to speak for themselves.
        const cb::MaskGeneratorParams& gen = p.tip.autoTip.generator;
        bp.ratio = gen.ratio;
        bp.tip = cb::makeTip(gen);
    }

    bp.paintMode = p.paintMode;
    bp.strokeMode = p.eraserMode ? cb::StrokeMode::Erase : cb::StrokeMode::Paint;
    bp.blendMode = p.blendMode;
    bp.accumulator = p.accumulator; // the library's finalized verdict, post tip-resolution
    bp.colorDynamicsActive = p.colorDynamicsActive; // §6.6f: gates the engine's HSV colour dynamics
    bp.masking = lp.masking;        // resolved against the master size
    bp.smudge = p.smudge;           // the smudge engine (§6.6c); begin() normalizes the axes
    // The second engine kind (§6.6g). A SPEC, not a painter: begin() builds one per stroke, so two
    // strokes of the same preset never share a point history or a bristle field. `kind == None` --
    // every pixel-brush, eraser and smudge preset -- leaves the dab walk exactly as it was.
    bp.painter = p.painter;
    // The hatching engine (§6.6g). A dab engine: begin() gates it on a real tip and on neither the
    // smudge walk nor a painter running, exactly as it gates those two against each other.
    bp.hatching = p.hatching;
    // The TEXTURE option (§6.6h) -- the LIBRARY's resolved form, because the pattern's pixels are
    // only knowable once the embedded payload is decoded and baked, exactly like a predefined tip's.
    // It rides both the paint and the smudge walk; begin() gates it on a real tip.
    bp.texture = lp.texture;
    // The AIRBRUSH (§6.6h): the stroke's second, TIME-driven dab cadence, read straight off the
    // preset's timing knobs. The per-dab `Rate` option rides the option table below like any other.
    bp.airbrush.enabled = p.airbrush.enabled;
    bp.airbrush.rate = p.airbrush.rate;
    bp.airbrush.ignoreSpacing = p.airbrush.ignoreSpacing;

    // The stroke's ceiling: the Opacity option's STATIC strength -- and ONLY the strength. The
    // option itself rides the pipeline below (its slot in BrushOptions), and the engine evaluates
    // it WITHOUT the strength (`useStrength=false`): the reference's indirect painting splits
    // Opacity exactly there -- strength = the whole stroke's ceiling, sensors = the per-dab one --
    // and folding the strength into both would SQUARE it, the same trap `flow` documents below
    // from the other side.
    if (const cb::CurveOptionData* op = p.option("Opacity"); op != nullptr)
        bp.opacity = clamp01(op->strength);

    // ⚠ `flow` STAYS AT 1. The Flow option is always-on and carries the preset's `FlowValue` as its
    // own strength, and the dab pipeline multiplies the two (`base.flow * sizeLikeValue`). Seeding the
    // base with the same number as well would SQUARE it -- a preset authored at 0.5 flow would paint
    // at 0.25. A preset with no Flow option at all keeps the full 1.0, which is what "no option" means
    // everywhere else in the pipeline.
    bp.flow = 1.0;

    // The options themselves. An option the preset never mentioned stays ABSENT -- not disabled, not
    // defaulted: absent, contributing exactly the identity. That is the hard rule of §6.2, and it is
    // what keeps a preset with no options byte-for-byte the stroke the engine laid before options
    // existed. An option the preset DOES carry is handed over whole, checked or not: the pipeline's
    // own gate decides what an unchecked one contributes (the identity, never its strength).
    auto options = std::make_shared<cb::BrushOptions>();
    bool any = false;
    for (const cb::CurveOptionData& data : p.options) {
        std::optional<cb::CurveOption>* slot = slotFor(*options, data.name);
        if (slot == nullptr)
            continue; // not a base the dab pipeline reads; the import already badged it
        slot->emplace(data);
        any = true;
    }

    // The two wrapped slots (§6.6d): Scatter and Mirror carry their axis gates beside the option,
    // so they cannot ride slotFor's plain-CurveOption path. Handed over whole like everything
    // else -- checked or not, the pipeline's own gate decides.
    if (const cb::CurveOptionData* sc = p.option("Scatter"); sc != nullptr) {
        options->scatter.emplace(
            cb::ScatterOption{cb::CurveOption(*sc), p.scatterAxisX, p.scatterAxisY});
        any = true;
    }
    // ⚠ NEVER on a smudge preset: the reference's colorsmudge engine ignores its Mirror option
    // entirely (the settings widget offers one; the paintop constructs none), so dropping it here
    // IS the faithful stroke -- and it costs no badge for the same reason (dab.hpp's caveat).
    if (!p.smudge.enabled) {
        if (const cb::CurveOptionData* mi = p.option("Mirror"); mi != nullptr) {
            options->mirror.emplace(
                cb::MirrorOption{cb::CurveOption(*mi), p.mirrorHorizontal, p.mirrorVertical});
            any = true;
        }
        // Sharpness wraps alignOutline + softness beside its curve (§6.6e). ⚠ NEVER on a smudge
        // preset -- the reference's colorsmudge installs NO sharpness option at all, so dropping it
        // here IS the faithful stroke, badge-free, exactly like Mirror.
        if (const cb::CurveOptionData* sh = p.option("Sharpness"); sh != nullptr) {
            options->sharpness.emplace(cb::SharpnessOption{cb::CurveOption(*sh),
                                                           p.sharpnessAlignOutline,
                                                           p.sharpnessSoftness});
            any = true;
        }
    }
    // A preset that drives nothing gets a NULL option table rather than an empty one -- the engine
    // reads null as "no options at all" and skips the pipeline entirely, which is the path every
    // golden was laid on.
    if (any)
        bp.options = std::move(options);

    return bp;
}

} // namespace mosaic::io::brush
