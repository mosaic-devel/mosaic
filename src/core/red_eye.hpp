#pragma once

#include "common/image.hpp"   // Image / ColorF (the pixels in, the patch out)
#include "core/selection.hpp" // Selection (the user-brushed scope)

#include <cstdint>

// The eye retouch engine (S38-b, docs/red-eye-tool.md §3.1/§3.2): the two SHIPPING tiers of the
// eye tool, both of them masked colour transforms over a region the USER pointed at.
//
//   Tier 1 -- flash red-eye. The pupil's correct appearance is a known near-constant (a dark,
//             near-neutral disc with a specular catchlight), so removal is artifact DELETION over
//             a constant target: gate the glow by a fixed redness ramp, collapse its chroma while
//             preserving luminance, then pull the luminance toward a dark-pupil target everywhere
//             except the highlight. Nothing is invented (§3.1). The gate is a HYSTERESIS pair --
//             a strict ramp finds the glow's core, a permissive one is admitted only where the
//             core vouches for it -- because a single ramp always leaves the glow's dimmer rim
//             half-corrected, which is what a red ring around a black pupil IS (§9.8).
//   Tier 2 -- sclera de-redding / vein suppression. The white of the eye has NO constant target,
//             so this is HARMONIZATION: pull the brushed pixels toward the white the region ITSELF
//             demonstrates -- per pixel, the local tone of its own least-red, brightest, unvascular
//             neighbours -- never toward a fixed white. A vessel is REPLACED by that local white in
//             proportion to how much redder than it the pixel is; the diffuse pinkness left over is
//             then graded toward it. Every whitening step is scaled by how plausible that tone is
//             as an eye white at all, so a solid blood patch (which demonstrates no white anywhere)
//             is barely touched instead of being turned a convincing grey (§3.2, §9.8). The
//             vascularity floor clamps all of it, so the tool cannot whiten an eye to a dead,
//             plastic white. Under-correcting is the house default, not a timid setting.
//
// Tier 3 (procedural eye synthesis for a destroyed eye) is NOT here and has no stub: it is
// deferred research, deliberately unbuilt and deliberately non-ML when it is built.
//
// FLTK- and render-free so it is unit-tested headlessly (tests/test_red_eye.cpp). The two spatial
// filters both tiers need (the hysteresis support field and the local-white field) are
// re-implemented here rather than borrowed from render/fx because `mosaic_render` DEPENDS ON
// `mosaic_core`, not the other way round -- the same call the Select-menu morphology makes when it
// carries its own small separable Gaussian + EDT instead of reaching into render
// (core/selection.hpp §"the Select-menu morphology ops"). Both are the same repeated separable box
// pass (three passes approximate a Gaussian to well under a quantization step); divergence is on
// purpose and documented.
//
// ---------------------------------------------------------------------------------------------
// Technique lineage -- every mechanism here descends from long-published work, cited so the
// record of where each piece came from is explicit:
//   * Tier 1 correction -- the classical photofinishing recipe: build a redness mask, reduce the
//     chrominance toward zero while PRESERVING LUMINANCE so the specular catchlight survives.
//     That is exactly the shape of correctFlashGlow(): setLum onto a chroma-free triple.
//   * Tier 1 replacement value -- the classical soft, probability-weighted replacement of the red
//     channel toward the green/blue average, R' = (1-p)R + p*avg(G,B). Here avg(G,B) supplies the
//     pupil's honest luminance, so the glow's own inflated red never sets how bright the corrected
//     pupil is.
//   * Tier 1 rim -- the strict/permissive threshold PAIR, where the permissive one is admitted
//     only in the strict one's spatial neighbourhood, is hysteresis thresholding: J. Canny, "A
//     Computational Approach to Edge Detection", IEEE PAMI 8(6), 1986 -- published 40 years ago
//     and foundational. Growing a correction region out from a thresholded seed is the same
//     early-1990s photofinishing move at its core.
//   * Tier 2 whitening -- the classical region-customized enhancement that brightens and de-chromas
//     the whites of the eyes, in its MANUAL-region form: the operator supplies the region and the
//     algorithm supplies the grade. That is the direct ancestor of a brush-scoped whitener.
//   * Tier 2b local white -- estimating a signal's background as a weighted local mean in which
//     each sample carries its own confidence is normalized convolution: H. Knutsson & C-F. Westin,
//     "Normalized and Differential Convolution", CVPR 1993. Here the confidences are the fixed
//     ramps below (bright enough, no redder than the scope's own reference, not warm), so the
//     estimate is the local mean of the region's OWN unvascular pixels. Suppressing a thin dark
//     structure by pulling it toward such a background is textbook grey-level morphology
//     (J. Serra, "Image Analysis and Mathematical Morphology", 1982; S. Sternberg's rolling-ball
//     background, 1983) -- decades published, and none of it eye-specific.
//   * W3C "Compositing and Blending Level 1" non-separable colour toolbox (lum/setLum/sat/setSat)
//     -- core/blend_math.hpp, already shipped.
//
// ⚠⚠ THE STANDING INVARIANTS of this module. They are load-bearing, they are deliberate, and
// they are marked again below where each one binds. None of them is an oversight; do not
// "improve" any of them away:
//   1. USER-SCOPED, NO AUTOMATIC DETECTION. Every entry point takes a Selection the user painted.
//        Nothing here searches the image for an eye, a face, a pupil, an iris, a sclera or an
//        iris/sclera border; there is no whole-image candidate scan of any kind. The user invokes
//        the tool and the user says where -- the tool never volunteers, and it never runs unasked.
//        The Tier-1 hysteresis and the Tier-2 local-white field are both spatial filters run
//        STRICTLY INSIDE that painted scope: neither ever grows past it, neither identifies a
//        region for the user, and neither is offered anything to seed from but the user's own
//        stroke and the fixed ramps below.
//   2. FIXED-THRESHOLD redness gate. The ramps below are compile-time constants -- BOTH ramps
//        of the hysteresis pair, and the support level at which the permissive one is admitted. No
//        trained classifier, no learned false-positive rejection, no degradation-class routing, and
//        NO threshold derived by analysing the image.
//   3. No YUV chroma-vs-luma inequality-count classification, and no intensity-peak GLINT
//        DETECTOR. The catchlight is PRESERVED by a fixed luminance rolloff; it is never used to
//        find anything.
//   4. The vein handling is suppression for appearance only. The sclera is never segmented to
//        extract a vessel pattern, and no vessel structure is ever stored or matched. "How much
//        redder is this pixel than the local tone" is a per-pixel weight consumed and discarded in
//        the same expression that computes it -- there is no vessel map, no connected structure, no
//        template, and nothing that outlives the call.
//   5. The vascularity floor is a hard ceiling on the whole effect, not only a quality dial: the
//        tool cannot fully whiten, and it never inserts a glint at a detected pupil border. The
//        whiteness licence below is a SECOND such limit and a strictly tighter one: every whitening
//        step is scaled by how plausible the region's own reference tone is as an eye white, so on
//        an eye that demonstrates no white the tool declines to invent one. It harmonizes what is
//        there; it never reconstructs (that is Tier 3, deliberately not built).
namespace mosaic::core {

// The two shipping modes of the one eye tool (docs/red-eye-tool.md §4).
enum class RedEyeMode : std::uint8_t {
    Flash,  // Tier 1: kill the flash red-eye glow inside the brushed pupil
    Sclera, // Tier 2: de-redden / vein-suppress the brushed white of the eye
};

struct RedEyeParams {
    RedEyeMode mode = RedEyeMode::Flash;

    // ---- Tier 1 (Flash) --------------------------------------------------------------------
    // Global scale on the correction: 1 = the full masked transform, 0 = a no-op.
    double strength = 1.0;
    // How far the corrected pupil's luminance is pulled toward the dark-pupil target. The default
    // deliberately stops short of the floor: §4's "leaves the catchlight and a hint of pupil
    // structure".
    double darken = 0.65;
    // Hold the darkening back over the specular highlight (the chroma collapse still applies, so a
    // red-tinted glare becomes a white one). That is the whole point of the tier; on by default.
    bool keepCatchlight = true;
    // How far the glow's core may vouch for its own dimmer rim, in image px: the reach of the
    // hysteresis support field. A glow does not end at a threshold, it FADES into the iris over a
    // pixel or two, and those transition pixels are exactly the ones a single ramp scores near
    // zero -- so without this a corrected pupil keeps a red ring at the radius where the glow met
    // the iris. Derived from the tip, not exposed. 0 disables the pass, leaving the bare gate.
    double rimReach = 6.0;

    // ---- Tier 2 (Sclera) -------------------------------------------------------------------
    // How hard the harmonization pulls. 0 = a no-op; 1 = all the way to the floor, never past it.
    double amount = 0.55;
    // The vascularity floor (§3.2, a first-class control): the residual fraction of the pixel's
    // own saturation, and of how far a vessel may be replaced by the white around it -- that
    // ALWAYS survives, whatever the amount. High by default: the tool under-corrects, and it
    // cannot produce a dead white eye.
    double vascularityFloor = 0.35;
    // Run the structural sub-op (§3.2b: replace vessels with the local white) as well as the
    // harmonization (§3.2a). This is the tool's "method" control: off = harmonize only.
    bool suppressVeins = true;
    // Damp the effect on warm (orange/amber) pixels -- the canthus tint, the lid margin and any
    // skin a slipped brush caught. Scleral injection reads red-to-magenta (blue-white sclera under
    // the blood) while skin reads red-to-orange, so a fixed G-B window separates them well enough
    // to be worth having. Not a detector: it is a per-pixel hue window, like the redness ramp.
    bool protectCornerWarmth = true;
    // The vein suppression's spatial scale, in image px. Roughly "how thick is a vessel"; the
    // local-white field reaches kWhiteFieldReachScale times further, which is what lets a pixel in
    // the middle of a dense vessel mat still see white. The default suits a face-filling
    // portrait's eye; derived from the tip, not exposed.
    double veinRadius = 6.0;
};

// ---- The redness metrics (fixed thresholds; invariant 2) --------------------------------------
//
// Both scores ride the same "red excess" axis: R - (0.70*G + 0.30*B). The weighting is what makes
// it work on real photographs -- a retinal reflection is often MAGENTA rather than pure red (the
// iris pigment and the camera's own processing tint it), so the textbook R - max(G,B) collapses to
// nothing exactly on the tinted cases, while the green channel stays reliably crushed. Verified
// against real flash-red-eye photographs before these numbers were set.

// The absolute red-excess ramp for a flash glow.
inline constexpr float kFlashRedExcessLo = 0.16f;
inline constexpr float kFlashRedExcessHi = 0.38f;
// The PURITY ramp: red excess as a fraction of the pixel's own red. Warm skin carries a real red
// excess but spends most of its red on being bright (purity ~0.2-0.3); a retinal glow spends it on
// being red (purity 0.5-0.95). This second factor is what keeps a brush that slips onto an eyelid
// from bleaching it -- the absolute term alone does not separate them.
inline constexpr float kFlashPurityLo = 0.30f;
inline constexpr float kFlashPurityHi = 0.55f;
// The PERMISSIVE half of the hysteresis pair (§9.8). These ramps alone would bleach a warm iris,
// which is why they are only ever consulted where the STRICT pair above has already fired nearby:
// a glow's rim is a mixture of the glow and the iris, so it reads at a fraction of the core's red
// excess but keeps most of the core's purity, and purity is what still separates it from a brown
// iris (~0.2-0.4) at the same excess. Measured across the flash corpus: a corrected pupil's last
// red pixel sits at excess 0.15-0.16 with purity 0.7-0.9, one pixel outside which the iris proper
// reads excess 0.04 at purity 0.28.
inline constexpr float kFlashWeakExcessLo = 0.05f;
inline constexpr float kFlashWeakExcessHi = 0.15f;
inline constexpr float kFlashWeakPurityLo = 0.28f;
inline constexpr float kFlashWeakPurityHi = 0.50f;
// The scope is a GATE in this mode, not a weight. A soft brush edge multiplied into the correction
// leaves a partially-corrected annulus wherever the ring's own shoulder crosses live glow -- and
// since the ring IS the scope, sizing the brush to the pupil (which is what the tool asks you to
// do) puts that shoulder exactly on the glow's edge. The result is a bright red ring drawn by the
// TOOL, not left by it. Above this coverage the correction is full, so the only soft pixels are the
// sub-pixel ones at the very boundary; below it there is none. Half-corrected red is never the
// answer here -- the corrected pupil is idempotent, so a crisp boundary costs nothing.
// (The sclera mode keeps coverage as a true weight: there the soft edge is what blends a graded
// retouch into the rest of the white, and there is no artifact to half-remove.)
inline constexpr float kFlashScopeGate = 0.35f;
// How much core has to be in the neighbourhood before the permissive ramp is admitted at all. The
// support field is the strict score blurred over `rimReach`, so this is "a few percent of my
// neighbourhood is definitely glow" -- low, because the rim is by construction right next to the
// core, and the purity ramp above is doing the real discrimination.
inline constexpr float kFlashSupportLo = 0.02f;
inline constexpr float kFlashSupportHi = 0.25f;

// ---- The local iris, and the rim measured against it -------------------------------------------
//
// The permissive ramp above is an ABSOLUTE reading, and absolute readings cannot separate a glow's
// tail from a warm iris -- they measure the same red excess. This one asks the question that can be
// answered: how much redder is this pixel than the iris immediately around it? A glow's tail is
// +0.15..+0.35 above its iris; an iris is, by construction, zero above itself, whatever its colour.
// Measured on the corpus: the 2 px band left ringing a corrected pupil sat at excess +0.02..+0.03
// against an iris of -0.105 -- a step of 0.13, which is exactly what the eye reads as a drawn
// circle even though the band's own excess is nearly neutral.
inline constexpr float kRimAboveIrisLo = 0.05f;
inline constexpr float kRimAboveIrisHi = 0.15f;
// How far out the iris tone is sampled, as a multiple of rimReach -- far enough to clear the fade
// and reach real iris, near enough to stay this eye's iris and not the lid.
inline constexpr float kIrisFieldReachScale = 2.0f;
// ... and how much of that vote must accumulate before the iris tone is trusted. A glow larger than
// the field can see past has no iris in reach, and there the correction falls back to neutral --
// the shipped behaviour, which is right when there is nothing better to aim at.
inline constexpr float kIrisEvidenceLo = 0.02f;
inline constexpr float kIrisEvidenceHi = 0.10f;
// Which pixels may vote for it: only those clear of the glow's influence, by the same support
// field. The fade is the thing being MEASURED against this tone, so if it votes it drags the
// reference toward itself and the rim survives at half strength -- measured, exactly that.
inline constexpr float kIrisVoteSupportLo = 0.02f;
inline constexpr float kIrisVoteSupportHi = 0.30f;

// The catchlight rolloff: above kCatchlightHi luminance the darkening is fully withheld.
inline constexpr float kCatchlightLo = 0.55f;
inline constexpr float kCatchlightHi = 0.80f;
// The dark-pupil target: a floor plus a fraction of the pupil's own (red-free) luminance, so the
// disc keeps a hint of its structure instead of flattening to one constant grey.
inline constexpr float kPupilFloorLum = 0.06f;
inline constexpr float kPupilStructureKeep = 0.25f;

// The sclera ramp is far lower: conjunctival injection lives at a red excess a flash glow would
// not even register at.
inline constexpr float kScleraRedExcessLo = 0.06f;
inline constexpr float kScleraRedExcessHi = 0.30f;
// The RELATIVE luminance gate, as a fraction of the scope's own reference tone. An iris, a lash
// and a pupil are all much darker than the sclera around them, whatever the photo's exposure --
// which an absolute threshold cannot express and this can.
inline constexpr float kScleraRelLumLo = 0.45f;
inline constexpr float kScleraRelLumHi = 0.72f;
// The corner-warmth window on G - B, and how much of the effect it can take away.
inline constexpr float kWarmProtectLo = 0.05f;
inline constexpr float kWarmProtectHi = 0.16f;
inline constexpr float kWarmProtectMax = 0.75f;

// ---- The local white (§3.2b) ------------------------------------------------------------------
//
// Per pixel, the mean of its neighbours that are plausibly unvascular sclera. A neighbour votes
// with the product of three fixed ramps, and the vote is what "plausibly" means -- there is no
// segmentation, no label, and nothing kept (invariant 4).

// Bright enough to be the white of an eye rather than an iris, a lash or a lid shadow. Stated as a
// FRACTION of the scope's own reference luminance, not as a level: sclera luminance runs 0.22-0.55
// across the corpus depending only on exposure, so no absolute threshold can hold.
inline constexpr float kWhiteFieldLumLo = 0.45f;
inline constexpr float kWhiteFieldLumHi = 0.70f;
// No redder than the scope's own reference tone, plus a little. Also relative -- to the reference's
// red excess, so a mildly pink eye's own baseline counts as its white while its vessels do not.
inline constexpr float kWhiteFieldRedLo = 0.02f;
inline constexpr float kWhiteFieldRedHi = 0.10f;
// How far the field reaches, as a multiple of veinRadius. A vessel has to be crossable, and in a
// dense injection the white survives only as islands BETWEEN vessels -- reaching only one vessel
// width would find nothing but more vessel, which shows up as whole patches of a severely injected
// sclera going uncorrected while the rest cleans up. Measured on the worst frame in the corpus:
// at 2x, patches of the vessel mat still had no white in reach; at 4x they do, and at 6x the
// sclera starts losing its own shading to an estimate that has become a regional average.
inline constexpr float kWhiteFieldReachScale = 4.0f;
// How much of that vote has to accumulate before the local estimate is trusted at all. Below this
// the field fades back to the scope-wide reference and the structural half of the effect stands
// down: deep inside an iris there is no white in reach, and inventing one is exactly the failure
// this gate exists to prevent.
inline constexpr float kWhiteEvidenceLo = 0.01f;
inline constexpr float kWhiteEvidenceHi = 0.06f;

// A vessel is a pixel materially redder than the surface it sits on. The ramp is on that DIFFERENCE
// (pixel minus its local white), which is why it reads ~0 on clean sclera and ~0 on an iris -- on
// both, the local white is the pixel itself -- and 0.16-0.36 on a conjunctival vessel.
inline constexpr float kVeinExcessLo = 0.03f;
inline constexpr float kVeinExcessHi = 0.14f;
// ... and still keeping most of that white's RED, which is what says it is a vessel rather than an
// iris, a lash or a pupil. Blood absorbs green and blue and reflects red, so a vessel is dark in
// luminance while its red channel stays near the sclera's; an iris or a lash is dark in every
// channel. Measured against the local white, the red ratio separates them four times as well as
// luminance does (vessels 0.77-1.21 at the 10th-90th percentile against irises' 0.08-0.51, a gap of
// +0.27; by luminance the same populations sit at 0.54 and 0.47, a gap of +0.07 -- which is why
// gating on luminance left the darkest vessel cores uncorrected as red speckle while the rest of
// the vessel went white).
inline constexpr float kVeinRedKeepLo = 0.55f;
inline constexpr float kVeinRedKeepHi = 0.75f;

// ---- The whitening licence (§9.8, and a hard limit -- invariant 5) -----------------------------
//
// How plausible the region's own reference tone is as the white of an eye, from two absolute
// ramps: a sclera is barely saturated and barely red. Every whitening step -- the saturation
// ceiling, the luminance lift, the hue nudge -- is scaled by it. A conjunctivitis reference
// (sat 0.08, excess -0.02) scores 1.00 and the tool corrects hard; a subconjunctival hemorrhage's
// (sat 0.26, excess +0.24) scores 0.00 and the tool declines, because there is no white in that
// photograph to harmonize toward and a confident grey is worse than an honest under-correction.
inline constexpr float kWhitenessSatLo = 0.10f;
inline constexpr float kWhitenessSatHi = 0.30f;
inline constexpr float kWhitenessExcessLo = 0.05f;
inline constexpr float kWhitenessExcessHi = 0.20f;
// The plausible-sclera saturation the harmonization aims at when it is fully licensed. Without a
// ceiling the target is the region's OWN least-red tone, which on a thoroughly injected eye is
// itself pink -- the tool then converges to pink however many times it is applied, which is
// precisely the "it only ever makes the veins a duller colour" failure (§9.8).
inline constexpr float kScleraSatCeiling = 0.075f;
// The luminance lift is weaker than the desaturation -- raising the whole white of an eye to its
// brightest tone is the flat, dead look §3.2 exists to avoid -- and it is scaled by
// (1 - vascularityFloor) like every other half of the effect, so the floor is ONE honest control
// over the WHOLE correction: at a floor of 1 the tool is an exact no-op, and it can therefore never
// fully whiten an eye however hard Amount is pushed (§3.2's vascularity floor, invariant 5).
inline constexpr float kScleraLift = 0.90f;

// How much of a flash glow is in this colour, in [0,1]. Zero for anything neutral -- which is why
// a fully corrected pixel is an exact fixed point of the Tier-1 transform.
[[nodiscard]] float flashGlowScore(common::ColorF c) noexcept;

// How red this colour is on the sclera's much gentler scale, in [0,1]. `refLum` is the scope's
// reference luminance (see scleraReference) and drives the relative-luminance gate; pass 0 to skip
// that gate entirely.
[[nodiscard]] float scleraRednessScore(common::ColorF c, float refLum) noexcept;

// ---- The result ------------------------------------------------------------------------------

// A localized pixel correction: the patched rectangle and where it belongs on the layer's own
// grid. This is exactly what the region-scoped SetLayerPixelsCommand overload takes, so one
// gesture is one undo step costing its bounding box rather than the whole layer (§2.5).
struct RetouchPatch {
    common::Image pixels; // region-sized RGBA; EMPTY means "nothing changed, push no command"
    long originX = 0;
    long originY = 0;
    [[nodiscard]] bool empty() const noexcept { return pixels.empty(); }
};

// The scope's own reference tone: the mean colour of the LEAST-RED pixels among its BRIGHTER HALF
// (§3.2a, "the local sclera mean sampled just outside the reddest pixels"). The brightness half of
// that sentence is load-bearing and was missing until §9.8: red excess falls to zero on anything
// dark, so "least red" alone elects the lashes, the iris and the lid shadow, and on a real
// conjunctivitis photograph it returned a mid grey (luminance 0.51) for an eye whose sclera
// measures 0.87. Everything downstream is relative to this tone, so that one error made the whole
// tier under-correct. This is a summary statistic of the region the user brushed -- not a
// segmentation, not a localization, and it never looks outside the scope. Returns a fully
// transparent colour when the scope covers nothing usable.
[[nodiscard]] common::ColorF scleraReference(const common::Image& src, const Selection& scope);

// How plausible `tone` is as the white of an eye, in [0,1] -- the whitening licence described
// above. Pure, and a fixed-threshold function of one colour: it asks nothing about the image.
[[nodiscard]] float scleraWhiteness(common::ColorF tone) noexcept;

// Run the eye retouch over `scope` (which must be `src`-sized: the caller resamples the document
// selection onto the layer's own grid). Returns the changed rectangle, or an empty patch when the
// scope is empty, degenerate, or the correction turned out to be a no-op (an image with nothing
// red in it lands no undo step at all).
//
// Pure: `src` is not modified, no global state is read, and the result is deterministic.
[[nodiscard]] RetouchPatch retouchEye(const common::Image& src, const Selection& scope,
                                      const RedEyeParams& params);

} // namespace mosaic::core
