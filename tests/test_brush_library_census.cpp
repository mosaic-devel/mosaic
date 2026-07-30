// The shipped-bundle census: the CC-0 default set (data/brushes/, docs/brushes.md §4) replayed
// end-to-end through the real chain -- zip -> bundle -> kpp -> mapper -> library -- with every
// number pinned against docs/brushes.md §3.9/§3.10 and the session's independent hand-derivation
// (the six masking diameters were recomputed from the raw XML and raw tip headers before being
// written here; they are not the library's own output echoed back).
//
// This is the bundle-level corpus replay as a PERMANENT test: any reader change that loses a
// preset, misresolves a tip, or moves an absolute size fails here, not in a user's dock.

#include "io/brush/library.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

using namespace mosaic::io::brush;
namespace cb = mosaic::core::brush;

namespace {

const PresetLibrary& shipped() {
    static const PresetLibrary lib = [] {
        PresetLibrary l;
        std::string error;
        const int n = l.addBundleFile(
            std::string(MOSAIC_SHIPPED_DATA_DIR) + "/brushes/Krita_4_Default_Resources.bundle",
            &error);
        REQUIRE_MESSAGE(n == 117, error);
        return l;
    }();
    return lib;
}

[[nodiscard]] const LibraryPreset* byName(const PresetLibrary& lib, std::string_view name) {
    for (const LibraryPreset& p : lib.presets())
        if (p.preset.name == name)
            return &p;
    return nullptr;
}

} // namespace

TEST_CASE("census: 117 presets, all usable, the counters say nothing was lost") {
    const PresetLibrary& lib = shipped();
    const LibraryCounters& c = lib.counters();
    CHECK(c.presetsLoaded == 117);
    CHECK(c.presetsFailed == 0);
    // The whole 2.2 corpus carries no tip md5sum, so every referenced tip resolves by filename:
    // 54 references across all paintops (47 of them in the pixel-brush family, below).
    CHECK(c.tipsResolvedByMd5 == 0);
    CHECK(c.tipsResolvedByFilename == 54);
    CHECK(c.tipsFallback == 0);
    CHECK(c.tipDecodeFailures == 0);
    CHECK(c.scanBudgetSkips == 0);

    // The §4 licence facts, read back from the artifact itself.
    REQUIRE(lib.sources().size() == 1);
    CHECK(lib.sources()[0].meta.license == "CC-0");
    CHECK(lib.sources()[0].meta.author.rfind("Deevad", 0) == 0);
}

TEST_CASE("census: the §3.9 paintop histogram reproduces to the unit") {
    std::map<std::string, int> ops;
    for (const LibraryPreset& p : shipped().presets())
        ops[p.preset.provenance.sourcePaintop]++;

    const std::map<std::string, int> expected = {
        {"paintbrush", 82},   {"colorsmudge", 15},   {"spraybrush", 4},
        {"deformbrush", 3},   {"sketchbrush", 2},    {"filter", 2},
        {"curvebrush", 1},    {"duplicate", 1},      {"experimentbrush", 1},
        {"gridbrush", 1},     {"hairybrush", 1},     {"hatchingbrush", 1},
        {"particlebrush", 1}, {"roundmarker", 1},    {"tangentnormal", 1},
    };
    CHECK(ops == expected);
}

TEST_CASE("census: the 82 pixel-brush presets -- tips, hoses, accumulators") {
    int bitmapTips = 0, hoses = 0, uniform = 0, colored = 0, eraserModes = 0;
    for (const LibraryPreset& p : shipped().presets()) {
        if (p.preset.provenance.sourcePaintop != "paintbrush")
            continue;
        if (p.tip != nullptr) {
            ++bitmapTips;
            if (p.tip->hose().dim > 0)
                ++hoses;
        }
        if (p.preset.accumulator == cb::StrokeAccumulator::Uniform)
            ++uniform;
        else
            ++colored;
        if (p.preset.eraserMode)
            ++eraserModes;
    }
    CHECK(bitmapTips == 47); // §3.10: 27 gbr refs + 19 png + 1 svg, all resolved
    CHECK(hoses == 23);      // 23 of the 27 "gbr" references are really .gih
    CHECK(eraserModes == 3); // the three a)_Eraser_* presets: CompositeOp "erase"

    // Every bitmap tip in the set is a grayscale alpha mask, so Uniform covers all but ONE
    // preset: v)_Texture_Impressionism carries BOTH of §3.10's h/v colour-dynamics singletons,
    // and colour dynamics need the Colored accumulator (the mapper's pinned rule).
    CHECK(uniform == 81);
    CHECK(colored == 1);
    const LibraryPreset* impressionism = byName(shipped(), "v)_Texture_Impressionism");
    REQUIRE(impressionism != nullptr);
    CHECK(impressionism->preset.accumulator == cb::StrokeAccumulator::Colored);
    CHECK(impressionism->preset.colorDynamicsActive);

    // The one svg tip: z)_Stamp_Leaves, rendered 1000 px wide at its own (square) aspect.
    const LibraryPreset* leaves = byName(shipped(), "z)_Stamp_Leaves");
    REQUIRE(leaves != nullptr);
    CHECK(leaves->tipFileName == "leaves-scattered.svg");
    REQUIRE(leaves->tip != nullptr);
    CHECK(leaves->tip->frameCount() == 1);
    CHECK(leaves->tip->frameWidth(0) == 1000);
    CHECK(leaves->tip->frameHeight(0) == 1000);
}

TEST_CASE("census: the six masking presets, at hand-derived absolute sizes, with REAL tips") {
    // Each diameter below was recomputed by hand from the raw preset XML and the raw tip file
    // headers: coeff x (auto diameter | post-BrushVersion scale x max(w, h)). §3.10's op split
    // is the 3 + 3 it documents: linear_dodge on the charcoals, subtract on the rest.
    //
    // The tip kinds and the two bitmap frames were read from the raw nested brush_definitions and
    // the raw tip file headers (2026-07-14): four AUTO circles (the three charcoals plus
    // Waterpaint_Soft's softness-curve `soft` generator) and two BITMAPS -- Dry_Bristles_Eroded's
    // eroded-debris .gbr at 153 x 64 (NOT square -- the masking cadence's ellipse is not a
    // formality) and Waterpaint_Hard's rough-square .png at 454 x 448.
    struct Expect {
        const char* name;
        cb::MaskingOp op;
        double diameter;
        bool procedural;
        std::uint32_t frameW; // bitmap only
        std::uint32_t frameH;
    };
    const Expect expected[] = {
        {"h)_Charcoal_pencil_large", cb::MaskingOp::LinearDodge, 16.0, true, 0, 0}, // 0.5 x 32
        {"h)_Charcoal_Pencil_Medium", cb::MaskingOp::LinearDodge, 60.0 / 11.0, true, 0,
         0},                                                                    // 0.4545… x 12
        {"h)_Charcoal_Pencil_Thin", cb::MaskingOp::LinearDodge, 3.0, true, 0, 0}, // 0.5 x 6
        {"g)_Dry_Bristles_Eroded", cb::MaskingOp::Subtract, 120.0, false, 153,
         64}, // 1 x 0.46875 x 256
        {"j)_Waterpaint_Hard_Edges", cb::MaskingOp::Subtract, 77.18, false, 454,
         448}, // 0.7718 x 0.390625 x 256
        {"j)_Waterpaint_Soft_Edges", cb::MaskingOp::Subtract, 110.0, true, 0,
         0}, // 1.1458… x 0.32 x 300
    };

    int maskingOn = 0;
    for (const LibraryPreset& p : shipped().presets())
        if (p.preset.provenance.sourcePaintop == "paintbrush" && p.masking.enabled)
            ++maskingOn;
    CHECK(maskingOn == 6);

    for (const Expect& e : expected) {
        CAPTURE(e.name);
        const LibraryPreset* p = byName(shipped(), e.name);
        REQUIRE(p != nullptr);
        REQUIRE(p->masking.enabled);
        CHECK(p->masking.op == e.op);
        CHECK(p->masking.diameter == doctest::Approx(e.diameter));

        // Every shipped masking brush carries its REAL nested tip -- none falls back to the
        // analytic disc.
        REQUIRE(p->masking.tip != nullptr);
        CHECK(p->masking.tip->isProcedural() == e.procedural);
        if (!e.procedural) {
            REQUIRE(p->masking.tip->bitmap() != nullptr);
            CHECK(p->masking.tip->bitmap()->frameWidth(0) == e.frameW);
            CHECK(p->masking.tip->bitmap()->frameHeight(0) == e.frameH);
        }
    }

    // The two bitmap references resolve; nothing fell back. Counted apart from the primary tips'
    // four counters, whose hand-derived numbers (54 by filename, above) keep meaning what they say.
    CHECK(shipped().counters().maskingTipsResolved == 2);
    CHECK(shipped().counters().maskingTipsFallback == 0);

    // The TEXTURE option's own pair (§6.6h), counted apart for the same reason. All 21 textured
    // presets EMBED their pattern (a PNG in every case), so every one bakes and none falls back.
    // The per-preset assertions live in test_brush_texture.cpp.
    CHECK(shipped().counters().texturesResolved == 21);
    CHECK(shipped().counters().texturesFallback == 0);
}

TEST_CASE("census: fidelity spread and provenance discipline") {
    int exact = 0, approximated = 0, substituted = 0;
    for (const LibraryPreset& p : shipped().presets()) {
        switch (p.preset.provenance.fidelity) {
        case PresetFidelity::Exact: ++exact; break;
        case PresetFidelity::Approximated: ++approximated; break;
        case PresetFidelity::Substituted: ++substituted; break;
        }
        CHECK(p.preset.provenance.sourceFormat == "bundle");
        // Anything short of Exact must SAY why -- fidelity honesty, §6.4.
        if (p.preset.provenance.fidelity != PresetFidelity::Exact) {
            CAPTURE(p.preset.name);
            CHECK(!p.preset.provenance.droppedOptions.empty());
        }
    }
    // 13 = the paintops Mosaic has no engine family for (§6.5/§6.6).
    //
    // ⚠ THE SPREAD MOVED ON 2026-07-12, AND NOT BECAUSE A READER CHANGED: it was 55/49/13 while the
    // mapper's "supported options" list still named Scatter, Mirror and Spacing -- three options the
    // dab pipeline has never driven -- and while an Opacity with a live sensor counted as honoured.
    // At 11/93/13 the engine drove the option's STATIC strength as the stroke's ceiling and nothing
    // more, because a per-dab opacity is a per-dab CEILING and one wash coverage channel could not
    // hold both a per-dab ceiling and the flow build-up.
    //
    // ⚠ IT MOVED AGAIN ON 2026-07-14, THE DAY THE ENGINE EARNED IT: the wash accumulation now takes
    // the transcribed per-dab-ceiling step (washAlphaDarkenAlpha), so a live-sensor Opacity under
    // WASH is honoured end-to-end and costs nothing. 11 -> 42 Exact: Opacity alone was worth 31.
    //
    // The 42/41 pixel-family split was re-derived INDEPENDENTLY from the raw preset XML (not echoed
    // from the library): of the 83 pixel-family presets (82 paintbrush + 1 roundmarker), 41 remain
    // Approximated, and the reason histogram over them reproduces §3.10's own option counts to the
    // unit -- Mirror 11, Scatter 8, Sharpness 3, Spacing 2, h 1, v 1, real Texture 16, Airbrush 7,
    // plus the 2 BUILDUP presets whose live Opacity curve keeps the static ceiling
    // (c)_Pencil-5_Tilted, f)_Bristles-5_Flat -- the direct path's own per-dab composite is not
    // transcribed yet, and the other 2 Buildup presets author OpacityUseCurve=false).
    //
    // ⚠ IT MOVED A THIRD TIME ON 2026-07-14, WHEN THE SMUDGE ENGINE LANDED (§6.6c): colorsmudge
    // maps to a real engine now, so its 15 pay only for what they actually drop. Re-derived from
    // the raw XML, 6 go Exact -- i)_Wet_Circle, i)_Wet_Knife, j)_Watercolor_Fringe,
    // k)_Blender_Knife_Edge, k)_Blender_Rake, k)_Blender_Smear -- and 9 stay Approximated on real
    // remaining drops (Scatter 3: Wet_Bristles, Wet_Bristles_Rough, Blender_Blur; Spacing 2:
    // Wet_Bristles again, Wet_Smear; Texture 5: Wet_Paint, Wet_Paint_Details, Wet_Textured_Soft,
    // Blender_Basic, Blender_Textured_Soft). 42 + 6 = 48 Exact; 41 + 9 + the 6 no-engine-family
    // (spraybrush 4, filter 2) = 56 Approximated; 13 Substituted. 48 + 56 + 13 = 117.
    //
    // ⚠ AND A FOURTH TIME, SAME DAY, WHEN SCATTER AND MIRROR WERE TRANSCRIBED (§6.6d). Re-derived
    // from the raw XML (scratch script, PNG preset chunks -> mapper-reason replay): of the 11
    // Scatter carriers and 11 Mirror carriers (3 presets carry both: v)_Texture_Impressionism,
    // y)_Texture_Spines, y)_Texture_Spray), 15 had NO other drop and flip to Exact --
    // c)_Pencil-1_Hard, c)_Pencil-2, f)_Charcoal_Rock_Soft, i)_Wet_Bristles_Rough,
    // j)_Waterpaint_Hard_Edges, k)_Blender_Blur, t)_Shapes_Square, y)_Texture_Large_Splat,
    // y)_Texture_Noise, y)_Texture_Random_Particles, y)_Texture_Snow_Pile, y)_Texture_Spines,
    // z)_Stamp_Bokeh, z)_Stamp_Sparkles, z)_Stamp_Vegetal. That left 63 Exact / 41 Approximated / 13
    // Substituted, with a histogram over the 35 pixel+smudge Approximated of Texture 21, Airbrush 7,
    // SmudgeRate-on-paintbrush 5, Spacing 4, Sharpness 3, Opacity-under-Buildup 2, h 1, v 1.
    // No shipped colorsmudge preset carries Mirror, so the reference-faithful drop-on-smudge rule
    // is pinned by a unit test (test_brush_preset_brush.cpp), not exercised here.
    //
    // ⚠ AND A FIFTH TIME ON 2026-07-18, WHEN THE SPACING OPTION WAS TRANSCRIBED (§6.6e): the per-dab
    // cadence scale is honoured end-to-end now (brush_engine.cpp's m_dabSpacingScale), on the paint
    // AND smudge walks. Of the 4 Spacing carriers, 3 had NO other remaining drop and flip to Exact --
    // i)_Wet_Bristles and i)_Wet_Smear (colorsmudge) and h)_Chalk_Grainy (paintbrush); the 4th,
    // y)_Texture_Spray, keeps its Airbrush drop. That left 66 Exact / 38 Approximated / 13
    // Substituted, with a histogram over the 32 pixel+smudge Approximated of Texture 21, Airbrush 7,
    // SmudgeRate-on-paintbrush 5, Sharpness 3, Opacity-under-Buildup 2, h 1, v 1.
    //
    // ⚠ AND A SIXTH TIME, SAME DAY, WHEN THE SHARPNESS OPTION WAS TRANSCRIBED (§6.6e): the alpha
    // threshold AND the pixel-grid coordinate snap are honoured now (brush_engine.cpp's
    // sharpnessThreshold + applySharpnessSnap). Of the 3 Sharpness carriers, 2 had NO other drop and
    // flip to Exact -- u)_Pixel_Art and u)_Pixel_Art_Fill; the 3rd, u)_Pixel_Art_Dithering, keeps its
    // Texture drop. 66 + 2 = 68 Exact; 38 - 2 = 36 Approximated; 13 Substituted. The 30 remaining
    // pixel+smudge Approximated histogram as Texture 21, Airbrush 7, SmudgeRate-on-paintbrush 5,
    // Opacity-under-Buildup 2, h 1, v 1 (Spacing 0, Sharpness 0), plus the 6 no-engine-family
    // (spraybrush 4, filter 2). No shipped colorsmudge preset carries Sharpness, and colorsmudge
    // installs no sharpness option at all, so the drop-on-smudge rule is faithful and badge-free like
    // Mirror (pinned by a unit test, not exercised here). 68 + 36 + 13 = 117.
    //
    // ⚠ AND A SEVENTH TIME ON 2026-07-18, WHEN THE HSV COLOUR DYNAMICS LANDED (§6.6f): h/s/v adjust
    // the Colored accumulator's per-dab paint colour now (brush_engine.cpp's applyColorDynamics). The
    // ONE preset that drives any of them, v)_Texture_Impressionism (h + v, both fuzzy sensors; s, Mix
    // and Darken are unused across the WHOLE shipped set), had NO other remaining drop -- its Size,
    // Rotation, Scatter and Mirror all landed already and its Texture/Strength/ is inert
    // (Texture/Pattern/Enabled=false) -- so it flips to Exact. 68 + 1 = 69 Exact; 36 - 1 = 35
    // Approximated; 13 Substituted. The 29 remaining pixel+smudge Approximated histogram as Texture
    // 21, Airbrush 7, SmudgeRate-on-paintbrush 5, Opacity-under-Buildup 2 (h 0, v 0 now), plus the 6
    // no-engine-family (spraybrush 4, filter 2). Mix and Darken stay unsupported, and no shipped
    // preset drives either -- their drop-on-smudge caveat is a mapper rule, not exercised here. 69 +
    // 35 + 13 = 117.
    //
    // ⚠ AND AN EIGHTH TIME ON 2026-07-27, WHEN THE SECOND ENGINE KIND LANDED (§6.6g): `sketchbrush`
    // and `hairybrush` map to real `StrokePainter`s now, so their 3 presets stop being substituted
    // with a pixel brush and pay only for what they actually drop. Re-derived INDEPENDENTLY from the
    // raw preset XML (a scratch probe replaying the mapper's rules over the bundle's PNG preset
    // chunks; ⚠ THE ATTRIBUTE ORDER INSIDE `<param>` IS NOT FIXED IN THE CORPUS -- some files write
    // `type=` before `name=` and some after, and a probe that pins one order silently loses 28
    // presets' tips. The probe reproduces 69/35/13 exactly with the painters switched off, which is
    // what makes its 70/37/10 with them on worth anything):
    //   * `d)_Ink-8_Sumi-e` (hairybrush) has NO remaining drop -- its Size and Rotation options ride
    //     the painter, its Opacity is static (`OpacityUseCurve=false`) under Wash, its ink depletion
    //     is off and it authors neither saturation depletion nor soaked ink -- so it flips straight
    //     to EXACT. Substituted 13 -> 12, Exact 69 -> 70.
    //   * `v)_Sketching-1_Chrome_Thin` and `v)_Sketching-2_Chrome_Large` (sketchbrush) each keep
    //     exactly ONE drop -- a dynamic Opacity under BUILDUP (`PaintOpAction=1`), the same caveat
    //     the 2 Buildup pixel-brush presets carry, because the direct path's own per-mark opacity
    //     composite is still untranscribed. Both author simple mode and no anti-aliasing, so neither
    //     of the sketch painter's own two badges fires. Substituted 12 -> 10, Approximated 35 -> 37.
    // 70 + 37 + 10 = 117. The 31 Approximated presets that DO have an engine family now histogram as
    // Texture 21, Airbrush 7, SmudgeRate-on-paintbrush 5, Opacity-under-Buildup 4 (2 pixel + the 2
    // sketch), plus the 6 no-engine-family (spraybrush 4, filter 2). The 10 Substituted are the
    // paintops with no engine at all: deformbrush 3, curvebrush, duplicate, experimentbrush,
    // gridbrush, particlebrush, tangentnormal, and hatchingbrush (§6.6b).
    // ⚠ AND A NINTH TIME, SAME DAY, WHEN THE LAST FOUR EXOTICS WERE BUILT (2026-07-27, §6.6g):
    // everything here is transcribed from the reference's published GPL source, so the remaining
    // three §6.6(b) painters AND `hatchingbrush` ship enabled.
    // Re-derived with the same attribute-order-safe probe, which still reproduces 69/35/13 with all
    // of it switched off:
    //   * `t)_Shapes_Fill` (experimentbrush) and `v)_Experimental_Webs` (particlebrush) have NO
    //     remaining drop and go straight to EXACT. Exact 70 -> 72.
    //   * `v)_Sketching-3_Leaky` (curvebrush) keeps the one Buildup-Opacity caveat, like the two
    //     sketch presets.
    //   * `y)_Screentone_Moire` (hatchingbrush -- a DAB engine, not a painter) keeps two: the
    //     Buildup-Opacity caveat and its antialiased hatch line, whose dedicated varying-width Wu
    //     rasterizer is not transcribed.
    // 72 + 39 + 6 = 117. The 33 Approximated presets with an engine family histogram as Texture 21,
    // Airbrush 7, Opacity-under-Buildup 6, SmudgeRate-on-paintbrush 5, Hatching anti-aliasing 1;
    // plus the 6 no-engine-family (spraybrush 4, filter 2). The 6 that remain SUBSTITUTED are the
    // paintops with no engine at all and a home elsewhere on the plan: deformbrush 3 (Liquify),
    // duplicate 1 (Clone Stamp), gridbrush 1 (Tier 5) and tangentnormal 1 (Tier 3).
    //
    // ⚠ AND A TENTH TIME ON 2026-07-28, WHEN THE TEXTURE OPTION AND THE AIRBRUSH LANDED (§6.6h) --
    // the two biggest remaining drop reasons, and by a wide margin. Re-derived with the SAME
    // attribute-order-safe probe (which still reproduces the pinned 72/39/6 with both switched off,
    // which is what makes its 94/17/6 with them on worth anything):
    //   * TEXTURE: 21 carriers, and 20 of them had NO other remaining drop -- 15 paintbrush
    //     (c)_Pencil-3_Large_4B, d)_Ink-7_Brush_Rough, f)_Dry_Roller, g)_Dry_Brushing,
    //     g)_Dry_Textured_Creases, h)_Chalk_Details, h)_Chalk_Soft, h)_Charcoal_Pencil_Medium,
    //     h)_Charcoal_Pencil_Thin, h)_Charcoal_pencil_large, u)_Pixel_Art_Dithering,
    //     y)_Screentone_Pressure, y)_Screentones_Regular, y)_Texture_Reptile,
    //     y)_Texture_Wood_Fiber) and 5 colorsmudge (i)_Wet_Paint, i)_Wet_Paint_Details,
    //     i)_Wet_Textured_Soft, k)_Blender_Basic, k)_Blender_Textured_Soft -- the texture option
    //     rides the SMUDGE walk too, because the reference installs it on the brush-based paintop
    //     base colorsmudge derives from). The 21st, c)_Pencil-5_Tilted, keeps its
    //     Opacity-under-Buildup caveat. All 21 author TexturingMode 0 or 1 -- the two Mosaic
    //     transcribes -- and all 21 EMBED their pattern, so none falls back.
    //   * AIRBRUSH: 10 carriers, of which 2 had no other drop and flip -- e)_Marker_Details and
    //     y)_Texture_Spray. b)_Airbrush_Soft and the four l)_Adjust_* keep a SmudgeRate drop (an
    //     active smudge option on a paintbrush), and the 3 spraybrush ones keep their paintop.
    // 72 + 20 + 2 = 94 Exact; 39 - 22 = 17 Approximated; 6 Substituted. The 11 Approximated presets
    // that DO have an engine family histogram as Opacity-under-Buildup 6, SmudgeRate-on-paintbrush
    // 5, Hatching anti-aliasing 1 (Texture 0, Airbrush 0 now); plus the 6 no-engine-family
    // (spraybrush 4, filter 2). 94 + 17 + 6 = 117.
    //
    // ⚠ AND AN ELEVENTH TIME ON 2026-07-28 (§6.6i) -- the two reasons that were left, and they were
    // NOT one kind of thing. Both sets were re-derived from the raw preset XML with the same
    // attribute-order-safe probe:
    //   * OPACITY-UNDER-BUILDUP was a real missing mechanism, and it is transcribed now: BUILDUP is
    //     the reference's DIRECT painting, where the per-dab opacity rides each dab's own composite
    //     instead of being the ceiling the WASH accumulation strives toward (brush_engine's
    //     buildCap). Exactly 6 shipped presets are Buildup with a dynamic Opacity --
    //     c)_Pencil-5_Tilted, f)_Bristles-5_Flat, v)_Sketching-1_Chrome_Thin,
    //     v)_Sketching-2_Chrome_Large, v)_Sketching-3_Leaky and y)_Screentone_Moire -- and 5 of them
    //     had NO other drop and flip. (The other 4 Buildup presets author `OpacityUseCurve=false`
    //     and were never badged; the 6th, y)_Screentone_Moire, keeps its antialiased-hatch-line
    //     drop and stays Approximated on that alone.)
    //   * SMUDGERATE-ON-A-PAINTBRUSH was never a missing mechanism at all -- it was a badge that
    //     should not have fired. The reference's own pixel brush constructs NO smudge option, so
    //     `SmudgeRate` on a `paintbrush` is read by nothing there either and Mosaic's stroke MATCHES
    //     it exactly; the drop is faithful and badge-free, like Mirror on a colorsmudge preset. The
    //     5 carriers are b)_Airbrush_Soft and the four l)_Adjust_* -- Krita-2-era files still
    //     wearing stale keys (they are the same five whose dead `AirbrushOption/*` spelling used to
    //     be honoured, which is what froze the program; §6.6h) -- and all 5 flip.
    // 94 + 5 + 5 = 104 Exact; 17 - 10 = 7 Approximated; 6 Substituted. What remains Approximated is
    // exactly ONE preset with an engine family (y)_Screentone_Moire, hatching anti-aliasing) plus
    // the 6 with none (spraybrush 4, filter 2). 104 + 7 + 6 = 117.
    CHECK(exact == 104);
    CHECK(approximated == 7);
    CHECK(substituted == 6);

    // ⚠ THE HISTOGRAM, NOT JUST THE TOTAL. A total can be reached by two mistakes that cancel; the
    // remaining Approximated set is small enough now to name outright, so name it.
    std::vector<std::string> stillBadged;
    for (const LibraryPreset& p : shipped().presets())
        if (p.preset.provenance.fidelity == PresetFidelity::Approximated)
            stillBadged.push_back(p.preset.name);
    std::sort(stillBadged.begin(), stillBadged.end());
    const std::vector<std::string> expectedBadged{
        "v)_Texture_Pointillism", "x)_Filter_Blur",         "x)_Filter_Sharpen",
        "y)_Screentone_Moire",    "y)_Texture_Starfield",   "z)_Stamp_Hearts",
        "z)_Stamp_Shoujo_Bubbles"};
    CHECK(stillBadged == expectedBadged);
}

TEST_CASE("census: every exotic paintop that has an engine gets one, and only those") {
    // The engine-kind half of §6.6's one-list rule, over the corpus: a preset gets a painter iff its
    // paintop is one the engine has a painter for. The unit-level biconditional (does the MAPPING
    // agree with the LIST?) lives in test_brush_preset_brush.cpp; this is the corpus checking that
    // the promise reaches the shipped set and stops there.
    std::map<int, int> kinds;
    int hatching = 0;
    for (const LibraryPreset& p : shipped().presets()) {
        CAPTURE(p.preset.name);
        const cb::StrokePainterKind kind = p.preset.painter.kind;
        CHECK(kind == cb::painterKindForPaintop(p.preset.provenance.sourcePaintop));
        kinds[static_cast<int>(kind)]++;
        // ⚠ Hatching is the DAB engine of the exotics: it gets no painter and its own gate instead,
        // and the two must agree preset by preset (`hatchingbrush` <=> the lattice).
        const bool isHatching = p.preset.provenance.sourcePaintop == "hatchingbrush";
        CHECK(p.preset.hatching.enabled == isHatching);
        if (isHatching)
            ++hatching;
    }
    CHECK(kinds[static_cast<int>(cb::StrokePainterKind::Sketch)] == 2);
    CHECK(kinds[static_cast<int>(cb::StrokePainterKind::Hairy)] == 1);
    CHECK(kinds[static_cast<int>(cb::StrokePainterKind::Curve)] == 1);
    CHECK(kinds[static_cast<int>(cb::StrokePainterKind::Particle)] == 1);
    CHECK(kinds[static_cast<int>(cb::StrokePainterKind::Experiment)] == 1);
    CHECK(hatching == 1);

    // ⚠ The transcribed property blocks reach the preset -- these are the values the raw XML
    // carries, read back through the whole chain rather than re-derived from the reader's defaults.
    const LibraryPreset* sumi = byName(shipped(), "d)_Ink-8_Sumi-e");
    REQUIRE(sumi != nullptr);
    const cb::HairyPainterParams& h = sumi->preset.painter.hairy;
    CHECK(h.densityFactor == doctest::Approx(34.0));
    CHECK(h.randomFactor == doctest::Approx(7.47));
    CHECK(h.scaleFactor == doctest::Approx(1.03));
    CHECK(h.shearFactor == doctest::Approx(0.0));
    CHECK(h.antialias);
    CHECK_FALSE(h.useCompositing);
    CHECK_FALSE(h.connectedPath);
    CHECK_FALSE(h.inkDepletionEnabled); // so the whole depletion block is inert on this preset
    CHECK(sumi->preset.provenance.fidelity == PresetFidelity::Exact);

    // The sketch preset that randomizes its connection colour needs the Colored accumulator -- one
    // coverage channel cannot carry a colour per mark (§6.1's rule, third input).
    const LibraryPreset* thin = byName(shipped(), "v)_Sketching-1_Chrome_Thin");
    REQUIRE(thin != nullptr);
    CHECK(thin->preset.painter.sketch.randomRgb);
    CHECK(thin->preset.painter.sketch.simpleMode);
    CHECK(thin->preset.painter.sketch.lineWidth == 1);
    CHECK(thin->preset.accumulator == cb::StrokeAccumulator::Colored);

    const LibraryPreset* large = byName(shipped(), "v)_Sketching-2_Chrome_Large");
    REQUIRE(large != nullptr);
    CHECK_FALSE(large->preset.painter.sketch.randomRgb);
    CHECK(large->preset.painter.sketch.distanceOpacity);
    CHECK(large->preset.painter.sketch.lineWidth == 4);
    CHECK(large->preset.accumulator == cb::StrokeAccumulator::Uniform);

    // The other three painters' blocks, read back through the whole chain.
    const LibraryPreset* leaky = byName(shipped(), "v)_Sketching-3_Leaky");
    REQUIRE(leaky != nullptr);
    CHECK(leaky->preset.painter.curve.strokeHistorySize == 12);
    CHECK(leaky->preset.painter.curve.lineWidth == 4);
    CHECK(leaky->preset.painter.curve.smoothing);
    CHECK(leaky->preset.painter.curve.makeConnection);

    const LibraryPreset* webs = byName(shipped(), "v)_Experimental_Webs");
    REQUIRE(webs != nullptr);
    CHECK(webs->preset.painter.particle.count == 65);
    CHECK(webs->preset.painter.particle.iterations == 15);
    CHECK(webs->preset.painter.particle.gravity == doctest::Approx(0.906));
    CHECK(webs->preset.painter.particle.weight == doctest::Approx(0.06));
    CHECK(webs->preset.provenance.fidelity == PresetFidelity::Exact);

    const LibraryPreset* shapes = byName(shipped(), "t)_Shapes_Fill");
    REQUIRE(shapes != nullptr);
    CHECK(shapes->preset.painter.experiment.windingFill);
    CHECK_FALSE(shapes->preset.painter.experiment.hardEdge);
    CHECK(shapes->preset.provenance.fidelity == PresetFidelity::Exact);

    // ⚠ The moiré preset: a DAB engine with the lattice on, and the style really is Moire -- which
    // it only is because the file explicitly clears `bool_nocrosshatching`, whose default is TRUE.
    const LibraryPreset* moire = byName(shipped(), "y)_Screentone_Moire");
    REQUIRE(moire != nullptr);
    CHECK(moire->preset.painter.kind == cb::StrokePainterKind::None);
    CHECK(moire->preset.hatching.enabled);
    CHECK(moire->preset.hatching.style == cb::CrosshatchingStyle::Moire);
    CHECK(moire->preset.hatching.angle == doctest::Approx(45.0));
    CHECK(moire->preset.hatching.separation == doctest::Approx(18.0));
    CHECK(moire->preset.hatching.thickness == doctest::Approx(9.0));
    CHECK(moire->preset.hatching.subpixelPrecision);
    CHECK(moire->preset.hatching.antialias);
}


