#include "core/red_eye.hpp"

#include "core/blend_math.hpp" // detail::lum / setLum / sat / setSat -- the W3C colour toolbox

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

// See red_eye.hpp for the design, the technique lineage and the standing invariants this file is
// written to. Everything below is arithmetic on colours over a region the user painted; nothing
// here searches the image for anything.
namespace mosaic::core {

using common::ColorF;
using common::Image;

namespace {

[[nodiscard]] float clamp01(float v) noexcept { return std::clamp(v, 0.0f, 1.0f); }

[[nodiscard]] float smoothstep(float e0, float e1, float x) noexcept {
    if (e1 <= e0)
        return x >= e1 ? 1.0f : 0.0f;
    const float t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}

[[nodiscard]] float mix(float a, float b, float t) noexcept { return a + (b - a) * t; }

// The shared redness axis (see the header): the green channel carries the signal, the blue channel
// only a third of a vote, so a MAGENTA retinal reflection -- where B rides nearly as high as R --
// still scores. The textbook R - max(G,B) reads ~0 on exactly those photographs.
[[nodiscard]] float redExcess(ColorF c) noexcept { return c.r - (0.70f * c.g + 0.30f * c.b); }

// The scope's coverage at a document/layer pixel, as [0,1].
[[nodiscard]] float coverageAt(const Selection& s, std::uint32_t x, std::uint32_t y) noexcept {
    return static_cast<float>(s.at(x, y)) / 255.0f;
}

[[nodiscard]] ColorF pixelAt(const Image& img, std::uint32_t x, std::uint32_t y) noexcept {
    const std::size_t i = (static_cast<std::size_t>(y) * img.width + x) * 4;
    return {img.rgba[i] / 255.0f, img.rgba[i + 1] / 255.0f, img.rgba[i + 2] / 255.0f,
            img.rgba[i + 3] / 255.0f};
}

void writePixel(Image& img, std::uint32_t x, std::uint32_t y, ColorF c) noexcept {
    const std::size_t i = (static_cast<std::size_t>(y) * img.width + x) * 4;
    const auto q = [](float v) {
        return static_cast<std::uint8_t>(clamp01(v) * 255.0f + 0.5f);
    };
    img.rgba[i] = q(c.r);
    img.rgba[i + 1] = q(c.g);
    img.rgba[i + 2] = q(c.b);
    img.rgba[i + 3] = q(c.a);
}

// The integer ROI of a scope's coverage, clamped to the image. Empty (x1<=x0) when there is none.
struct Roi {
    long x0 = 0;
    long y0 = 0;
    long x1 = 0;
    long y1 = 0;
    [[nodiscard]] bool empty() const noexcept { return x1 <= x0 || y1 <= y0; }
    [[nodiscard]] std::uint32_t w() const noexcept { return static_cast<std::uint32_t>(x1 - x0); }
    [[nodiscard]] std::uint32_t h() const noexcept { return static_cast<std::uint32_t>(y1 - y0); }
};

[[nodiscard]] Roi scopeRoi(const Image& src, const Selection& scope) {
    const auto b = scope.bounds();
    if (!b)
        return {};
    Roi r;
    r.x0 = std::max<long>(0, static_cast<long>(std::floor(b->x)));
    r.y0 = std::max<long>(0, static_cast<long>(std::floor(b->y)));
    r.x1 = std::min<long>(static_cast<long>(src.width), static_cast<long>(std::ceil(b->x + b->w)));
    r.y1 = std::min<long>(static_cast<long>(src.height), static_cast<long>(std::ceil(b->y + b->h)));
    return r;
}

// ---- The one spatial filter both tiers use ----------------------------------------------------
//
// A separable box pass run three times, which approximates a Gaussian closely enough that the
// difference is far below an 8-bit step -- and, unlike a single box, leaves no axis-aligned
// plateaus for the eye to find. Edge-clamped, so a filter that reaches past the ROI sees the
// border value repeated rather than a cliff of zeros (which would show up as a dark ring exactly
// at the edge of what the user painted). Serial on purpose: this only ever runs over an eye-sized
// ROI, and a serial loop is trivially deterministic.
constexpr int kBoxPasses = 3;

// One pass along a strided line: `step` is 1 for a row and the row stride for a column.
void boxPass(const std::vector<float>& src, std::vector<float>& dst, std::size_t start,
             std::size_t step, int len, int r, std::vector<double>& prefix) {
    prefix.assign(static_cast<std::size_t>(len) + 1, 0.0);
    for (int i = 0; i < len; ++i)
        prefix[static_cast<std::size_t>(i) + 1] =
            prefix[static_cast<std::size_t>(i)] + src[start + static_cast<std::size_t>(i) * step];
    const double first = src[start];
    const double last = src[start + static_cast<std::size_t>(len - 1) * step];
    const double inv = 1.0 / (2.0 * r + 1.0);
    for (int i = 0; i < len; ++i) {
        const int lo = i - r;
        const int hi = i + r;
        const int clampedLo = lo < 0 ? 0 : lo;
        const int clampedHi = hi > len - 1 ? len - 1 : hi;
        double sum = prefix[static_cast<std::size_t>(clampedHi) + 1] -
                     prefix[static_cast<std::size_t>(clampedLo)];
        if (lo < 0)
            sum += static_cast<double>(-lo) * first; // clamp-to-edge, not zero
        if (hi > len - 1)
            sum += static_cast<double>(hi - (len - 1)) * last;
        dst[start + static_cast<std::size_t>(i) * step] = static_cast<float>(sum * inv);
    }
}

// `radius` is the total reach; each of the three passes takes a third of it.
void boxBlurPlane(std::vector<float>& plane, int w, int h, float radius) {
    const int r = std::max(1, static_cast<int>(std::lround(radius / kBoxPasses)));
    if (w <= 0 || h <= 0)
        return;
    std::vector<float> tmp(plane.size());
    std::vector<double> prefix;
    for (int pass = 0; pass < kBoxPasses; ++pass) {
        for (int y = 0; y < h; ++y)
            boxPass(plane, tmp, static_cast<std::size_t>(y) * static_cast<std::size_t>(w), 1, w, r,
                    prefix);
        for (int x = 0; x < w; ++x)
            boxPass(tmp, plane, static_cast<std::size_t>(x), static_cast<std::size_t>(w), h, r,
                    prefix);
    }
}

// ---- The local tone -- the one estimator both tiers judge a pixel against ---------------------
//
// Per pixel, the tone of the neighbours that vote for being what this pixel OUGHT to look like,
// plus how much evidence there was for it. Normalized convolution (Knutsson & Westin 1993): every
// pixel of the scope contributes its colour weighted by `vote`, and the result is the weighted
// mean. Where the weights are thin the evidence falls and the tone fades back to `fallback`.
//
// Both tiers need this and for the same reason: an absolute threshold cannot tell "red" from "red
// FOR THIS EYE". Tier 2 asks its neighbours what the white of this eye looks like; Tier 1 asks
// what the iris around this pupil looks like. In both cases the pixel is then judged by how far it
// departs from its own surroundings, which is self-normalizing -- a brown iris and a bloodshot
// sclera are both "red" in absolute terms and neither departs from itself at all.
//
// Three colour planes rather than an ImageF: the alpha would be a wasted quarter, and this only
// ever runs over a region that can be as large as the user cares to paint.
struct LocalTone {
    std::vector<float> rgb[3];   // ROI-sized
    std::vector<float> evidence; // ROI-sized, [0,1]
    [[nodiscard]] ColorF at(std::size_t i) const noexcept {
        return {rgb[0][i], rgb[1][i], rgb[2][i], 1.0f};
    }
};

// `vote(c, sx, sy)` -> [0,1]: how much this pixel speaks for the tone being estimated, given its
// colour and its DOCUMENT position (so a vote may consult a field computed earlier, which is how
// Tier 1 keeps the glow's own fade out of the iris estimate it is about to be judged against).
//
// `pad` widens the rect the estimate READS from, without widening what is written: only `roi` is
// returned and only `roi` is ever corrected. This is the same "a read, never a write" the shipped
// Tier 2 took for its base-layer support, and Tier 1 needs it for the case that matters most --
// a brush sized to the pupil, where the only iris the tool could aim the fade at is just outside
// the ring the user drew. `inScopeOnly` keeps Tier 2 to the region the user actually painted (its
// white must be THIS eye's white, not the lid's), while Tier 1's iris may come from the pad.
template <class VoteFn>
[[nodiscard]] LocalTone localToneField(const Image& src, const Selection& scope, const Roi& roi,
                                       ColorF fallback, float radius, float evidenceLo,
                                       float evidenceHi, long pad, bool inScopeOnly, VoteFn vote) {
    const long rx0 = std::max<long>(0, roi.x0 - pad);
    const long ry0 = std::max<long>(0, roi.y0 - pad);
    const long rx1 = std::min<long>(static_cast<long>(src.width), roi.x1 + pad);
    const long ry1 = std::min<long>(static_cast<long>(src.height), roi.y1 + pad);
    const int rw = static_cast<int>(rx1 - rx0);
    const int rh = static_cast<int>(ry1 - ry0);
    const auto rn = static_cast<std::size_t>(rw) * static_cast<std::size_t>(rh);

    std::vector<float> plane[3] = {std::vector<float>(rn), std::vector<float>(rn),
                                   std::vector<float>(rn)};
    std::vector<float> weight(rn, 0.0f);
    for (int y = 0; y < rh; ++y) {
        for (int x = 0; x < rw; ++x) {
            const auto sx = static_cast<std::uint32_t>(rx0 + x);
            const auto sy = static_cast<std::uint32_t>(ry0 + y);
            if (inScopeOnly && coverageAt(scope, sx, sy) <= 0.0f)
                continue; // the estimate is made of the region the user painted
            const ColorF c = pixelAt(src, sx, sy);
            if (c.a <= 0.0f)
                continue;
            const float v = vote(c, sx, sy);
            if (v <= 0.0f)
                continue;
            const auto i = static_cast<std::size_t>(y) * static_cast<std::size_t>(rw) +
                           static_cast<std::size_t>(x);
            plane[0][i] = c.r * v;
            plane[1][i] = c.g * v;
            plane[2][i] = c.b * v;
            weight[i] = v;
        }
    }
    for (auto& p : plane)
        boxBlurPlane(p, rw, rh, radius);
    boxBlurPlane(weight, rw, rh, radius);

    // Hand back only the ROI: the pad was there to be looked at, never to be written.
    const int w = static_cast<int>(roi.w());
    const int h = static_cast<int>(roi.h());
    LocalTone out;
    for (auto& p : out.rgb)
        p.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    out.evidence.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    const float fb[3] = {fallback.r, fallback.g, fallback.b};
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const auto si = static_cast<std::size_t>(y + (roi.y0 - ry0)) *
                                static_cast<std::size_t>(rw) +
                            static_cast<std::size_t>(x + (roi.x0 - rx0));
            const auto di = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                            static_cast<std::size_t>(x);
            const float den = weight[si];
            const float ev = smoothstep(evidenceLo, evidenceHi, den);
            const float inv = den > 1e-6f ? 1.0f / den : 0.0f;
            // Thin evidence must not hand back a confident-looking local tone: fade to the fallback
            // exactly as fast as the evidence for a local one runs out.
            for (int ch = 0; ch < 3; ++ch)
                out.rgb[static_cast<std::size_t>(ch)][di] =
                    den > 1e-6f ? mix(fb[ch], plane[static_cast<std::size_t>(ch)][si] * inv, ev)
                                : fb[ch];
            out.evidence[di] = ev;
        }
    }
    return out;
}

// Tier 2's vote: bright enough to be sclera, no redder than the region's own reference, and not the
// warm orange of skin or a canthus. Three ramps, all fixed, all from the header.
[[nodiscard]] auto scleraVote(ColorF ref) {
    const float refLum = detail::lum({ref.r, ref.g, ref.b});
    const float refExcess = redExcess(ref);
    return [refLum, refExcess](ColorF c, std::uint32_t, std::uint32_t) {
        return smoothstep(kWhiteFieldLumLo * refLum, kWhiteFieldLumHi * refLum,
                          detail::lum({c.r, c.g, c.b})) *
               (1.0f - smoothstep(refExcess + kWhiteFieldRedLo, refExcess + kWhiteFieldRedHi,
                                  redExcess(c))) *
               (1.0f - kWarmProtectMax * smoothstep(kWarmProtectLo, kWarmProtectHi, c.g - c.b));
    };
}

// ---- Tier 1: the flash-red-eye correction (one pixel) -----------------------------------------
//
// `w` is the already-composed weight (scope coverage * glow score * strength). The two classical
// formulations the header cites meet here: one supplies the replacement value avg(G,B) -- which is
// what the pupil's luminance HONESTLY is once the glow's inflated red is discarded -- and the other
// supplies the policy: collapse the chroma, set the luminance, hold the darkening back over the
// specular highlight so the catchlight survives.
// `glow` is the STRICT score -- how much of this pixel is pupil rather than the iris the glow
// faded into -- and it chooses what the pixel is corrected TOWARD. A pupil's target is the known
// near-constant: dark and neutral. The fade at its edge is not a pupil at all, it is IRIS with a
// glow over it, and correcting that to neutral is what leaves a ring: a neutral band on a
// blue-green iris (excess -0.105 in the corpus) reads as warm even at excess exactly 0. So the
// target's chroma slides from the local iris tone to neutral as the pixel becomes pupil, and the
// darkening rides the same slider -- the disc goes dark, its edge only loses the red.
[[nodiscard]] ColorF correctFlashGlow(ColorF c, float w, float glow, ColorF irisTone,
                                      float irisEvidence, const RedEyeParams& p) noexcept {
    const float mid = 0.5f * (c.g + c.b); // the replacement value: R' toward the green/blue average
    const detail::Rgb replaced{mid, c.g, c.b};
    const float lBase = detail::lum(replaced);
    // The catchlight rolloff reads the pixel's ORIGINAL luminance: a specular highlight is bright
    // whatever colour the flash tinted it. It withholds the DARKENING only -- the chroma still
    // collapses there, so a red-tinted glare comes out white instead of pink (invariant 3: this
    // preserves a highlight, it never locates one).
    const float hold = p.keepCatchlight
                           ? smoothstep(kCatchlightLo, kCatchlightHi, detail::lum({c.r, c.g, c.b}))
                           : 0.0f;
    // The dark-pupil target keeps a quarter of the pupil's own (red-free) structure rather than
    // flattening the disc to one constant -- §4's "a hint of pupil structure". min() so the
    // correction can only ever darken: on an already-black pupil the floor must not brighten it.
    const float dark = std::min(lBase, kPupilFloorLum + kPupilStructureKeep * lBase);
    const float d = clamp01(static_cast<float>(p.darken)) * (1.0f - hold) * glow;
    // Neutral where the pixel is pupil, the iris beside it where it is not -- and neutral either
    // way when there is no iris in reach to have an opinion (a glow bigger than the field can see).
    const detail::Rgb neutral{lBase, lBase, lBase};
    const float toIris = (1.0f - glow) * irisEvidence;
    const detail::Rgb aim{mix(neutral.r, irisTone.r, toIris), mix(neutral.g, irisTone.g, toIris),
                          mix(neutral.b, irisTone.b, toIris)};
    const detail::Rgb corrected = detail::setLum(aim, mix(lBase, dark, d));
    return {mix(c.r, corrected.r, w), mix(c.g, corrected.g, w), mix(c.b, corrected.b, w), c.a};
}

// ---- Tier 2a: harmonize one pixel toward the white the region demonstrates ---------------------
//
// Everything the grade needs that is not the pixel: the local white it sits on, the scope-wide
// reference behind that, and the licence the region's own tone earns to be whitened at all.
struct ScleraGrade {
    ColorF tone;          // the local white (already faded toward the reference by its evidence)
    float toneLum = 0.0f;
    float refLum = 0.0f;
    float satRef = 0.0f;
    float licence = 0.0f; // scleraWhiteness(reference), 0..1
    float floorFrac = 0.0f;
};

[[nodiscard]] ColorF harmonizeSclera(ColorF c, float w, const ScleraGrade& g) noexcept {
    const detail::Rgb rgb{c.r, c.g, c.b};
    const float lPix = detail::lum(rgb);
    const float sPix = detail::sat(rgb);
    // The saturation the grade aims at: the region's own least-red tone, pulled down toward a
    // plausible sclera only as far as that tone earns it. On an eye whose every pixel is blood the
    // licence is 0 and the aim IS the region's own tone -- the shipped behaviour, which is the
    // honest one there. On a pink eye the licence is 1 and the aim is a white that the region's
    // own reference is otherwise too pink to name.
    const float satAim = mix(g.satRef, std::min(g.satRef, kScleraSatCeiling), g.licence);
    // Never past the vascularity floor and never UP: the tool de-reddens, it does not paint an eye
    // redder than it found it.
    const float aim = std::max(std::min(sPix, satAim), sPix * g.floorFrac);
    const float sNew = mix(sPix, aim, w);
    // The luminance lift only ever raises, and it is damped by the floor too, so "keep everything"
    // means exactly that (§3.2). It aims at the pixel's OWN local white, so a lid, a lash and any
    // skin a slipped brush caught -- whose local white is themselves -- cannot be bleached, and so
    // that the eyeball's shading toward the lids survives instead of flattening to one tone.
    // Desaturating without this is what turns a bloodshot eye GREY instead of white.
    const float lumAim = mix(lPix, g.toneLum, g.licence);
    const float lNew = mix(lPix, std::max(lPix, lumAim), w * kScleraLift * (1.0f - g.floorFrac));
    // With no licence and no local white to aim at -- a scope that is blood edge to edge -- every
    // term above collapses to the pixel's own value, and mix(a, a, t) is exactly a. Return early
    // rather than round-tripping through setSat/setLum for nothing: the difference is a 1/255
    // wobble, but a wobble is a non-empty patch, and that is a pointless undo step.
    const float hueW = w * g.licence * (1.0f - g.floorFrac);
    if (sNew == sPix && lNew == lPix && hueW <= 0.0f)
        return c;
    const detail::Rgb own = detail::setLum(detail::setSat(rgb, sNew), lNew);
    // The hue half (§3.2a's "nudge hue from red/magenta toward the sclera's neutral"), licensed by
    // the same evidence and spent at the same saturation and luminance: without it a thoroughly
    // injected patch desaturates along its OWN red hue and comes out beige rather than white.
    if (hueW <= 0.0f)
        return {own.r, own.g, own.b, c.a};
    const detail::Rgb toward =
        detail::setLum(detail::setSat({g.tone.r, g.tone.g, g.tone.b}, sNew), lNew);
    return {mix(own.r, toward.r, hueW), mix(own.g, toward.g, hueW), mix(own.b, toward.b, hueW),
            c.a};
}

} // namespace

// ---- The metrics -----------------------------------------------------------------------------

float flashGlowScore(ColorF c) noexcept {
    const float excess = redExcess(c);
    if (excess <= kFlashRedExcessLo)
        return 0.0f;
    // Purity: how much of the pixel's red is EXCESS red. Warm skin spends its red on being bright
    // (~0.2-0.3); a retinal reflection spends it on being red (0.5-0.95). Measured on real
    // flash-red-eye photographs -- the absolute ramp alone lets bright warm skin through.
    const float purity = excess / std::max(c.r, 1e-3f);
    return smoothstep(kFlashRedExcessLo, kFlashRedExcessHi, excess) *
           smoothstep(kFlashPurityLo, kFlashPurityHi, purity);
}

float scleraWhiteness(ColorF tone) noexcept {
    if (tone.a <= 0.0f)
        return 0.0f;
    const float s = detail::sat({tone.r, tone.g, tone.b});
    return (1.0f - smoothstep(kWhitenessSatLo, kWhitenessSatHi, s)) *
           (1.0f - smoothstep(kWhitenessExcessLo, kWhitenessExcessHi, redExcess(tone)));
}

float scleraRednessScore(ColorF c, float refLum) noexcept {
    const float redness = smoothstep(kScleraRedExcessLo, kScleraRedExcessHi, redExcess(c));
    if (redness <= 0.0f || refLum <= 0.0f)
        return redness;
    // The RELATIVE luminance gate: an iris, a lash and a pupil sit far below the sclera around
    // them at any exposure, so the ratio against the scope's own reference separates them where an
    // absolute threshold cannot (a night-flash photo's sclera is darker than a studio photo's
    // iris). Still a fixed threshold -- on a ratio, not on a level (invariants 1-2).
    const float rel = detail::lum({c.r, c.g, c.b}) / refLum;
    return redness * smoothstep(kScleraRelLumLo, kScleraRelLumHi, rel);
}

// ---- The scope's reference tone ---------------------------------------------------------------

ColorF scleraReference(const Image& src, const Selection& scope) {
    if (src.empty() || scope.isEmpty() || scope.width() != src.width ||
        scope.height() != src.height)
        return {};
    const Roi roi = scopeRoi(src, scope);
    if (roi.empty())
        return {};

    constexpr int kBins = 64;
    constexpr double kBrightestFraction = 0.50; // ... of what the user brushed, by luminance ...
    constexpr double kLeastRedFraction = 0.30;  // ... and the least-red 30% of THOSE

    // The luminance half comes first and it is what makes this a sclera tone rather than a lash:
    // red excess vanishes on anything dark, so ranking by redness alone elects the iris, the lashes
    // and the lid shadow -- measured on a real conjunctivitis photograph, that returned a mid grey
    // (luminance 0.51) for an eye whose sclera measures 0.87, and every downstream term is relative
    // to this tone (header note).
    const auto bin = [](float v, float lo, float hi) {
        const float t = (std::clamp(v, lo, hi) - lo) / (hi - lo);
        return static_cast<std::size_t>(std::min(kBins - 1, static_cast<int>(t * kBins)));
    };

    // Gathering at two coverage thresholds, solidly-inside first: a soft brush edge should not get
    // a vote in the reference when the stroke has a solid core, but a stroke that is ALL edge (a
    // very low opacity pass) still has to answer with something.
    const auto gather = [&](std::uint8_t minCoverage) -> ColorF {
        // Pass 1: how bright is the brighter half of the brushed pixels?
        std::array<std::uint32_t, kBins> lumHist{};
        std::size_t total = 0;
        const auto forEachPixel = [&](auto&& fn) {
            for (long y = roi.y0; y < roi.y1; ++y)
                for (long x = roi.x0; x < roi.x1; ++x) {
                    const auto ux = static_cast<std::uint32_t>(x);
                    const auto uy = static_cast<std::uint32_t>(y);
                    if (scope.at(ux, uy) < minCoverage)
                        continue;
                    const ColorF c = pixelAt(src, ux, uy);
                    if (c.a > 0.0f)
                        fn(c);
                }
        };
        forEachPixel([&](ColorF c) {
            ++lumHist[bin(detail::lum({c.r, c.g, c.b}), 0.0f, 1.0f)];
            ++total;
        });
        if (total < std::size_t{16})
            return {};
        // The histogram is cumulated from the BRIGHT end, so the cutoff is a floor, not a ceiling.
        const auto wantBright =
            std::max<std::size_t>(1, static_cast<std::size_t>(static_cast<double>(total) *
                                                              kBrightestFraction));
        std::size_t cum = 0;
        std::size_t lumCutoff = 0;
        for (std::size_t b = kBins; b-- > 0;) {
            cum += lumHist[b];
            if (cum >= wantBright) {
                lumCutoff = b;
                break;
            }
        }

        // Pass 2: among those, how red is the least-red 30%?
        std::array<std::uint32_t, kBins> redHist{};
        std::size_t bright = 0;
        const auto isBright = [&](ColorF c) {
            return bin(detail::lum({c.r, c.g, c.b}), 0.0f, 1.0f) >= lumCutoff;
        };
        forEachPixel([&](ColorF c) {
            if (!isBright(c))
                return;
            ++redHist[bin(redExcess(c), -1.0f, 1.0f)];
            ++bright;
        });
        if (bright == 0)
            return {};
        const auto wantRed =
            std::max<std::size_t>(1, static_cast<std::size_t>(static_cast<double>(bright) *
                                                              kLeastRedFraction));
        cum = 0;
        std::size_t redCutoff = static_cast<std::size_t>(kBins) - 1;
        for (std::size_t b = 0; b < static_cast<std::size_t>(kBins); ++b) {
            cum += redHist[b];
            if (cum >= wantRed) {
                redCutoff = b;
                break;
            }
        }

        // Pass 3: their mean.
        double sr = 0.0, sg = 0.0, sb = 0.0;
        std::size_t n = 0;
        forEachPixel([&](ColorF c) {
            if (!isBright(c) || bin(redExcess(c), -1.0f, 1.0f) > redCutoff)
                return;
            sr += c.r;
            sg += c.g;
            sb += c.b;
            ++n;
        });
        if (n == 0)
            return {};
        const auto d = static_cast<double>(n);
        return {static_cast<float>(sr / d), static_cast<float>(sg / d), static_cast<float>(sb / d),
                1.0f};
    };

    const ColorF solid = gather(kAntsCoverageThreshold);
    if (solid.a > 0.0f)
        return solid;
    return gather(1);
}

// ---- The one entry point -----------------------------------------------------------------------

RetouchPatch retouchEye(const Image& src, const Selection& scope, const RedEyeParams& params) {
    if (src.empty() || scope.isEmpty() || scope.width() != src.width ||
        scope.height() != src.height)
        return {};
    const Roi roi = scopeRoi(src, scope);
    if (roi.empty())
        return {};

    const std::uint32_t rw = roi.w();
    const std::uint32_t rh = roi.h();
    const Image before = common::copyRegion(src, roi.x0, roi.y0, rw, rh);
    Image patch = before;

    const float floorFrac = clamp01(static_cast<float>(params.vascularityFloor));

    if (params.mode == RedEyeMode::Flash) {
        const float strength = clamp01(static_cast<float>(params.strength));
        if (strength <= 0.0f)
            return {};

        // The STRICT score over the ROI, and the support field it casts around itself. A glow
        // fades into the iris over a pixel or two and those transition pixels score near zero on
        // the strict pair, so a single ramp leaves them uncorrected: a red ring at exactly the
        // radius where the glow met the iris, which is the artifact this pass exists to remove.
        // Hysteresis (Canny 1986): the permissive ramp is admitted only where the strict one has
        // already fired nearby. Both ramps are fixed constants and the field never leaves the
        // scope, so nothing here detects anything (invariants 1-2).
        const auto n = static_cast<std::size_t>(rw) * static_cast<std::size_t>(rh);
        std::vector<float> support(n);
        for (std::uint32_t y = 0; y < rh; ++y)
            for (std::uint32_t x = 0; x < rw; ++x) {
                const auto sx = static_cast<std::uint32_t>(roi.x0 + static_cast<long>(x));
                const auto sy = static_cast<std::uint32_t>(roi.y0 + static_cast<long>(y));
                if (coverageAt(scope, sx, sy) <= 0.0f)
                    continue;
                const ColorF c = pixelAt(src, sx, sy);
                if (c.a > 0.0f)
                    support[static_cast<std::size_t>(y) * rw + x] = flashGlowScore(c);
            }
        const float reach = std::max(0.0f, static_cast<float>(params.rimReach));
        const bool hysteresis = reach > 0.0f;
        if (hysteresis)
            boxBlurPlane(support, static_cast<int>(rw), static_cast<int>(rh), reach);

        // The iris the glow sits in: the tone of the brushed pixels that are NOT glow. It is what
        // the fade at the pupil's edge should be corrected toward, and it is also what makes the
        // permissive half of the hysteresis safe -- "redder than the iris beside me" fires on a
        // glow's tail over any iris colour and is flat zero on a brown iris, which no absolute
        // threshold can manage (a brown iris and a glow's tail measure the same excess).
        // A pixel votes for the iris tone only if it is not glow AND is clear of the glow's
        // influence: the fade IS the thing being measured against this tone, so letting it vote
        // pulls the reference red and the rim survives at half strength. The support field, already
        // computed, is exactly "how near the glow am I".
        const float irisReach = reach * kIrisFieldReachScale;
        const auto supportAt = [&](std::uint32_t sx, std::uint32_t sy) {
            const long lx = static_cast<long>(sx) - roi.x0;
            const long ly = static_cast<long>(sy) - roi.y0;
            if (lx < 0 || ly < 0 || lx >= static_cast<long>(rw) || ly >= static_cast<long>(rh))
                return 0.0f; // outside the scope there is no glow, so nothing to stand clear of
            return support[static_cast<std::size_t>(ly) * rw + static_cast<std::size_t>(lx)];
        };
        const LocalTone iris = localToneField(
            src, scope, roi, ColorF{}, irisReach, kIrisEvidenceLo, kIrisEvidenceHi,
            static_cast<long>(std::ceil(irisReach)) + 2, /*inScopeOnly=*/false,
            [&](ColorF c, std::uint32_t sx, std::uint32_t sy) {
                return (1.0f - flashGlowScore(c)) *
                       (1.0f - smoothstep(kIrisVoteSupportLo, kIrisVoteSupportHi,
                                          supportAt(sx, sy)));
            });

        for (std::uint32_t y = 0; y < rh; ++y) {
            const auto sy = static_cast<std::uint32_t>(roi.y0 + static_cast<long>(y));
            for (std::uint32_t x = 0; x < rw; ++x) {
                const auto sx = static_cast<std::uint32_t>(roi.x0 + static_cast<long>(x));
                const float cov = coverageAt(scope, sx, sy);
                if (cov <= 0.0f)
                    continue;
                const ColorF c = pixelAt(src, sx, sy);
                if (c.a <= 0.0f)
                    continue; // nothing to correct where there is no pixel
                const auto i = static_cast<std::size_t>(y) * rw + x;
                const ColorF irisTone = iris.at(i);
                const float irisEvidence = iris.evidence[i];
                const float glow = flashGlowScore(c);
                float score = glow;
                if (hysteresis && score < 1.0f) {
                    const float excess = redExcess(c);
                    const float purity = excess / std::max(c.r, 1e-3f);
                    // Two permissive readings, whichever is more sure: an absolute one (for a glow
                    // so large the field sees no iris at all) and the relative one above.
                    const float weakAbs =
                        smoothstep(kFlashWeakExcessLo, kFlashWeakExcessHi, excess) *
                        smoothstep(kFlashWeakPurityLo, kFlashWeakPurityHi, purity);
                    const float weakRel =
                        smoothstep(kRimAboveIrisLo, kRimAboveIrisHi, excess - redExcess(irisTone)) *
                        irisEvidence;
                    score = std::max(score, std::max(weakAbs, weakRel) *
                                                smoothstep(kFlashSupportLo, kFlashSupportHi,
                                                           support[i]));
                }
                // The scope GATES here rather than scaling (kFlashScopeGate): a brush shoulder
                // multiplied into the correction is itself a ring-drawing machine.
                const float w = smoothstep(0.0f, kFlashScopeGate, cov) * score * strength;
                if (w <= 0.0f)
                    continue;
                writePixel(patch, x, y,
                           correctFlashGlow(c, w, glow, irisTone, irisEvidence, params));
            }
        }
    } else {
        const float amount = clamp01(static_cast<float>(params.amount));
        if (amount <= 0.0f)
            return {};
        const ColorF ref = scleraReference(src, scope);
        if (ref.a <= 0.0f)
            return {}; // nothing usable under the brush: no reference, no correction
        const float refLum = detail::lum({ref.r, ref.g, ref.b});

        // Everything below reads only pixels the user painted, so the ROI (the scope's own bounds)
        // is the whole work rect -- no padding, and nothing outside the stroke is ever consulted.
        const float radius = std::max(0.0f, static_cast<float>(params.veinRadius));
        const bool veins = params.suppressVeins && radius > 0.0f;
        const LocalTone white = localToneField(src, scope, roi, ref,
                                              radius * kWhiteFieldReachScale, kWhiteEvidenceLo,
                                              kWhiteEvidenceHi, /*pad=*/0, /*inScopeOnly=*/true,
                                              scleraVote(ref));

        ScleraGrade grade;
        grade.refLum = refLum;
        grade.satRef = detail::sat({ref.r, ref.g, ref.b});
        grade.licence = scleraWhiteness(ref);
        grade.floorFrac = floorFrac;

        for (std::uint32_t y = 0; y < rh; ++y) {
            const auto sy = static_cast<std::uint32_t>(roi.y0 + static_cast<long>(y));
            for (std::uint32_t x = 0; x < rw; ++x) {
                const auto sx = static_cast<std::uint32_t>(roi.x0 + static_cast<long>(x));
                const float cov = coverageAt(scope, sx, sy);
                if (cov <= 0.0f)
                    continue;
                ColorF c = pixelAt(src, sx, sy);
                if (c.a <= 0.0f)
                    continue;

                const auto i = static_cast<std::size_t>(y) * rw + x;
                const ColorF tone = white.at(i);
                const float toneLum = detail::lum({tone.r, tone.g, tone.b});

                // (b) Vein suppression FIRST -- it is the structural edit, and the harmonization
                //     below then grades whatever survives it. A vessel is replaced by the white it
                //     lies on, in proportion to how much redder than that white it is; on clean
                //     sclera and on an iris alike the local white IS the pixel, so both terms
                //     vanish and neither is touched. The luminance target is the BRIGHTER of the
                //     two, so the vessel's dark dip goes with its colour -- attenuating only the
                //     chroma, as this did until §9.8, is exactly what left the veins a dull grey
                //     while the eye still read as unhealthy.
                // A pull of exactly 0 (the floor at 1) skips the mix rather than running it with
                // t = 0: "keep everything" has to mean a byte-exact no-op, not a rounding wobble.
                const float pull = cov * amount * (1.0f - floorFrac);
                if (veins && pull > 0.0f) {
                    const float v =
                        smoothstep(kVeinExcessLo, kVeinExcessHi, redExcess(c) - redExcess(tone)) *
                        smoothstep(kVeinRedKeepLo, kVeinRedKeepHi, c.r / std::max(tone.r, 1e-4f)) *
                        white.evidence[i];
                    if (v > 0.0f) {
                        const detail::Rgb target =
                            detail::setLum({tone.r, tone.g, tone.b},
                                           std::max(detail::lum({c.r, c.g, c.b}), toneLum));
                        const float t = pull * v;
                        c = {mix(c.r, target.r, t), mix(c.g, target.g, t), mix(c.b, target.b, t),
                             c.a};
                    }
                }

                // (a) Harmonize what is left toward that same white. Skipped outright at a floor
                // of 1 for the same byte-exactness reason: setSat/setLum round-trip to the same
                // colour only up to rounding, and the floor promises the pixel is left alone.
                if (floorFrac < 1.0f) {
                    float w = cov * amount * scleraRednessScore(c, refLum);
                    if (params.protectCornerWarmth)
                        w *= 1.0f - kWarmProtectMax * smoothstep(kWarmProtectLo, kWarmProtectHi,
                                                                 c.g - c.b);
                    if (w > 0.0f) {
                        grade.tone = tone;
                        grade.toneLum = toneLum;
                        c = harmonizeSclera(c, w, grade);
                    }
                }
                writePixel(patch, x, y, c);
            }
        }
    }

    if (patch == before)
        return {}; // nothing red in reach: land no undo step at all
    return {std::move(patch), roi.x0, roi.y0};
}

} // namespace mosaic::core
