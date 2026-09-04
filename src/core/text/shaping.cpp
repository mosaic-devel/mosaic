// Text shaping, layout, and outline extraction. Technique lineage (see docs/type-tool.md §5):
//   - Shaping: HarfBuzz (MIT). Outlines / rasterization input: FreeType (GPLv2+ arm).
//   - Coverage (CPU): the S25 analytic scanline vector rasterizer (FreeType smooth / AGG --
//     classic, public). This file produces only the Contours; the fill happens in text_render.cpp.
//
// FreeType + HarfBuzz are confined to this translation unit (PImpl), so core's public headers stay
// free of them -- the same discipline color_management uses for lcms2.
#include "core/text/shaping.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_OUTLINE_H
#include FT_TRUETYPE_TABLES_H // TT_OS2: the strikeout metrics FT_FaceRec omits

#include <hb-ft.h>
#include <hb.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "core/text/hyphenator.hpp"
#include "core/text/language.hpp"
#include "core/text/tokenize.hpp"
#include "core/vector/flatten.hpp"

namespace mosaic::core::text {
namespace {

constexpr double kF26Dot6 = 1.0 / 64.0;

// Decode one UTF-8 codepoint starting at byte `i` in `s`; advances `i` past it. Malformed bytes
// decode as U+FFFD and advance one byte (defensive -- the model stores arbitrary utf8).
char32_t decodeUtf8(const std::string& s, std::size_t& i) {
    const auto n = s.size();
    const unsigned char c = static_cast<unsigned char>(s[i]);
    auto cont = [&](std::size_t k) {
        return k < n && (static_cast<unsigned char>(s[k]) & 0xC0) == 0x80;
    };
    if (c < 0x80) { ++i; return c; }
    if ((c & 0xE0) == 0xC0 && cont(i + 1)) {
        char32_t cp = (char32_t(c & 0x1F) << 6) | (s[i + 1] & 0x3F);
        i += 2; return cp;
    }
    if ((c & 0xF0) == 0xE0 && cont(i + 1) && cont(i + 2)) {
        char32_t cp = (char32_t(c & 0x0F) << 12) | (char32_t(s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
        i += 3; return cp;
    }
    if ((c & 0xF8) == 0xF0 && cont(i + 1) && cont(i + 2) && cont(i + 3)) {
        char32_t cp = (char32_t(c & 0x07) << 18) | (char32_t(s[i + 1] & 0x3F) << 12) |
                      (char32_t(s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
        i += 4; return cp;
    }
    ++i; return 0xFFFD;
}

// One glyph before line-breaking/alignment: advances & metrics in LAYER units, not yet positioned.
// Axis-abstract (see LayoutBasis): `inlineAdvance`/`inlineOffset` run along the flow within a line
// (horizontal: +x; vertical: down the column), `crossOffset`/`ascent`/`descent` run perpendicular
// to it (horizontal: y about the baseline; vertical: x about the column centreline). shapeSegment
// fills these from HarfBuzz's x/y advances+offsets per the writing mode.
struct RawGlyph {
    std::uint32_t glyphId = 0;
    std::size_t runIndex = 0;
    std::size_t cluster = 0;
    FontFace face;
    float sizePx = 0.0f;
    float inlineAdvance = 0.0f;              // pen advance along the inline axis
    float inlineOffset = 0.0f;               // glyph origin offset along the inline axis
    float crossOffset = 0.0f;                // glyph origin offset along the cross (block) axis
    float ascent = 0.0f, descent = 0.0f;     // cross-axis extents either side of the baseline
    float baselineShift = 0.0f;
    bool colorGlyph = false;
    bool whitespace = false;
    bool rotated = false;                    // vertical `mixed` sideways glyph: rotate outline 90 CW (B3)
};

struct LineRange {
    std::size_t begin, end;
    std::size_t paragraph;
    // An automatic hyphen to draw at this line's right edge (Area hyphenation): a synthetic '-'
    // glyph in the run's face, emitted by the position pass after the line's real glyphs. nullopt on
    // a normally-wrapped or final line. See the wrap loop and the position pass below.
    std::optional<RawGlyph> hyphen;
};

// Advance width of [b,e), excluding trailing whitespace (so alignment ignores a trailing space).
float contentWidth(const std::vector<RawGlyph>& g, std::size_t b, std::size_t e) {
    std::size_t last = e;
    while (last > b && g[last - 1].whitespace) --last;
    float w = 0.0f;
    for (std::size_t i = b; i < last; ++i) w += g[i].inlineAdvance;
    return w;
}

float maxEm(const std::vector<RawGlyph>& g, std::size_t b, std::size_t e, float fallback) {
    float m = 0.0f;
    for (std::size_t i = b; i < e; ++i) m = std::max(m, g[i].sizePx);
    return m > 0.0f ? m : fallback;
}

// The layout basis for a writing mode: the position pass computes glyph pens and line boxes in an
// axis-abstract frame -- glyphs advance along the INLINE axis, lines stack along the BLOCK axis --
// and hands the coordinates here to project into layer-local (x,y). This is the single switch that
// selects horizontal vs vertical flow: the line-breaking and alignment logic is axis-agnostic (it
// works purely in inline advances), so the vertical modes are just these branches plus the vertical
// HarfBuzz shaping. HorizontalTB is the identity projection -- inline = +x, block = +y -- so its pens
// and boxes are byte-for-byte the pre-vertical horizontal result. Vertical swaps the axes: inline =
// +y (down the column), block = +x (VerticalLR, columns rightward) or blockAnchor - x (VerticalRL,
// columns leftward from `blockAnchor`).
struct LayoutBasis {
    WritingMode mode = WritingMode::HorizontalTB;
    float blockAnchor = 0.0f;  // VerticalRL only: layer x that block-coord 0 maps to (the right edge)

    // A point at inline-axis coordinate `inl` and block-axis coordinate `blk`, in layer-local space.
    Vec2 place(float inl, float blk) const {
        switch (mode) {
            case WritingMode::VerticalRL: return {blockAnchor - blk, inl};  // columns right -> left
            case WritingMode::VerticalLR: return {blk, inl};                // columns left -> right
            case WritingMode::HorizontalTB:
            default:
                return {inl, blk};  // inline -> x, block -> y
        }
    }

    // The pen for a glyph whose line baseline is at block coord `base`, shifted off it by the glyph's
    // cross offset `cross` (its y/x HB offset) plus style `shift` (baseline shift), both toward the
    // ascent side. The cross shift is applied in LAYER space AFTER the RL column flip -- a glyph's own
    // centring / baseline-shift must never mirror with the column order, or its outline (always drawn
    // +x rightward) lands on the wrong side of the column (the bug that clipped all vertical-rl ink).
    // `cross`/`shift` are kept as separate subtractions so horizontal is bit-identical to the prior code.
    Vec2 glyphPen(float inl, float base, float cross, float shift) const {
        switch (mode) {
            case WritingMode::VerticalRL: return {blockAnchor - base - cross - shift, inl};
            case WritingMode::VerticalLR: return {base - cross - shift, inl};
            case WritingMode::HorizontalTB:
            default:
                return {inl, base - cross - shift};
        }
    }

    // The layer-local rect of a line whose content spans inline [inl, inl+inlLen], baseline at block
    // coord `base`, reaching `asc` toward the ascent side and `desc` toward the descent side.
    common::Rect lineRect(float inl, float inlLen, float base, float asc, float desc) const {
        switch (mode) {
            case WritingMode::HorizontalTB:
                // Kept as the exact prior horizontal expression so bounds stay bit-identical.
                return {inl, base - asc, std::max(inlLen, 0.0f), asc + desc};
            default: {
                // Vertical: build the box from its projected corners (inline span x cross span).
                const Vec2 p0 = place(inl, base - asc);
                const Vec2 p1 = place(inl + std::max(inlLen, 0.0f), base + desc);
                return common::Rect::fromCorners(p0, p1);
            }
        }
    }
};

}  // namespace

// ---------------------------------------------------------------------------------------------
// Impl -- the FreeType/HarfBuzz face cache + the shaping machinery
// ---------------------------------------------------------------------------------------------
struct TextShaper::Impl {
    FT_Library lib = nullptr;
    // Cached open faces (keyed path+index). One FT_Face serves EVERY variation-coordinate set of
    // its file: open() re-applies the requested coords whenever they differ from what the face
    // currently carries (cheap for the common same-coords case), so interleaved runs at different
    // weights stay correct without a face per coordinate set.
    struct FaceEntry {
        FT_Face face = nullptr;
        std::vector<FT_Fixed> baseCoords;      // design coords at open time (named instance / defaults)
        std::map<std::string, float> applied;  // the FontFace.variations currently set on the face
    };
    std::map<std::pair<std::string, int>, FaceEntry> faces;
    std::map<std::pair<std::string, int>, std::vector<VariableAxis>> axisCache;  // fvar is face-static
    Hyphenator hyphenator;                 // Area hyphenation (deferred §1); dictionaries lazy-loaded
    std::string defaultLanguage = detectSystemLanguage();  // fallback when a paragraph sets no language

    Impl() { FT_Init_FreeType(&lib); }
    ~Impl() {
        for (auto& [k, e] : faces) FT_Done_Face(e.face);
        if (lib) FT_Done_FreeType(lib);
    }

    static std::string axisTag(FT_ULong t) {
        const char s[4] = {static_cast<char>((t >> 24) & 0xFF), static_cast<char>((t >> 16) & 0xFF),
                           static_cast<char>((t >> 8) & 0xFF), static_cast<char>(t & 0xFF)};
        return std::string(s, 4);
    }

    // Apply `want` as design coordinates on a variable face: axes not named keep the coordinate the
    // face OPENED with (its named-instance or default position -- not the fvar default, so a face
    // resolved to an instance index renders that instance when nothing is overridden). No-op for
    // static faces and when the coords already match. hb_ft_font_create_referenced picks the
    // face's active coords up at creation, and every consumer re-opens right before use, so the
    // shared face flipping between coordinate sets is safe.
    void applyVariations(FaceEntry& e, const std::map<std::string, float>& want) {
        if (e.applied == want || !FT_HAS_MULTIPLE_MASTERS(e.face)) return;
        FT_MM_Var* mm = nullptr;
        if (FT_Get_MM_Var(e.face, &mm) != 0 || mm == nullptr) return;
        std::vector<FT_Fixed> coords(mm->num_axis);
        for (FT_UInt i = 0; i < mm->num_axis; ++i) {
            const FT_Var_Axis& ax = mm->axis[i];
            const auto v = want.find(axisTag(ax.tag));
            if (v != want.end()) {
                const double lo = ax.minimum / 65536.0, hi = ax.maximum / 65536.0;
                coords[i] = static_cast<FT_Fixed>(
                    std::lround(std::clamp(static_cast<double>(v->second), lo, hi) * 65536.0));
            } else {
                coords[i] = i < e.baseCoords.size() ? e.baseCoords[i] : ax.def;
            }
        }
        FT_Set_Var_Design_Coordinates(e.face, mm->num_axis, coords.data());
        FT_Done_MM_Var(lib, mm);
        e.applied = want;
    }

    FT_Face open(const FontFace& f) {
        auto key = std::make_pair(f.path, f.index);
        auto it = faces.find(key);
        if (it == faces.end()) {
            FT_Face face = nullptr;
            if (!lib || FT_New_Face(lib, f.path.c_str(), f.index, &face) != 0) return nullptr;
            FaceEntry entry;
            entry.face = face;
            if (FT_HAS_MULTIPLE_MASTERS(face)) {  // remember the opened position (instance/defaults)
                FT_MM_Var* mm = nullptr;
                if (FT_Get_MM_Var(face, &mm) == 0 && mm != nullptr) {
                    entry.baseCoords.resize(mm->num_axis);
                    if (FT_Get_Var_Design_Coordinates(face, mm->num_axis,
                                                      entry.baseCoords.data()) != 0)
                        for (FT_UInt i = 0; i < mm->num_axis; ++i)
                            entry.baseCoords[i] = mm->axis[i].def;
                    FT_Done_MM_Var(lib, mm);
                }
            }
            it = faces.emplace(std::move(key), std::move(entry)).first;
        }
        applyVariations(it->second, f.variations);
        return it->second.face;
    }

    // Set `face` to render at `sizePx` em. Scalable faces take the exact (fractional) size; bitmap-
    // only colour faces snap to their nearest strike and report a scale so the caller can map the
    // strike-space metrics/pixels back to `sizePx`. Returns {pxActuallySized, sizePx/pxSized}.
    struct Sized { float px; float scale; };
    Sized sizeFace(FT_Face face, float sizePx) {
        if (FT_IS_SCALABLE(face)) {
            FT_Set_Char_Size(face, 0, static_cast<FT_F26Dot6>(std::lround(sizePx * 64.0)), 72, 72);
            return {sizePx, 1.0f};
        }
        // Bitmap-strike face (e.g. emoji): pick the strike nearest the requested size.
        int best = 0;
        float bestPx = 0.0f;
        for (int i = 0; i < face->num_fixed_sizes; ++i) {
            const float px = static_cast<float>(face->available_sizes[i].y_ppem) / 64.0f;
            if (i == 0 || std::abs(px - sizePx) < std::abs(bestPx - sizePx)) {
                best = i;
                bestPx = px;
            }
        }
        if (face->num_fixed_sizes > 0) {
            FT_Select_Size(face, best);
            const float px = bestPx > 0.0f ? bestPx : sizePx;
            return {px, sizePx / px};
        }
        return {sizePx, 1.0f};
    }

    // Ascent+descent (layer units) for `style`'s resolved face at its em size -- the height an empty
    // block's caret should use so it matches the first glyph that will be typed (whose caret spans
    // the real font metrics, not the raw em). Falls back to the em when no face resolves (headless).
    float emptyCaretHeight(const CharStyle& style, const FontProvider& fonts) {
        if (const auto face = fonts.resolve(style.font)) {
            if (FT_Face ft = open(*face)) {
                const auto sized = sizeFace(ft, style.sizePx);
                const float ascent =
                    static_cast<float>(ft->size->metrics.ascender * kF26Dot6) * sized.scale;
                const float descent =
                    static_cast<float>(-ft->size->metrics.descender * kF26Dot6) * sized.scale;
                if (ascent + descent > 0.0f) return ascent + descent;
            }
        }
        return style.sizePx;
    }

    // A synthetic hyphen '-' (U+002D) glyph in `face` at `sizePx`, for a hyphenated Area line's right
    // edge. runIndex ties it to the breaking run's style so the render/fill stage paints it in the
    // same colour. Zero advance (harmless no-op) if the face has no hyphen or fails to open.
    RawGlyph shapeHyphen(const FontFace& face, float sizePx, std::size_t runIndex) {
        RawGlyph g;
        g.runIndex = runIndex;
        g.face = face;
        g.sizePx = sizePx;
        FT_Face ft = open(face);
        if (!ft) return g;
        const auto sized = sizeFace(ft, sizePx);
        const FT_UInt gid = FT_Get_Char_Index(ft, '-');
        g.glyphId = gid;
        if (gid != 0 && FT_Load_Glyph(ft, gid, FT_LOAD_NO_HINTING) == 0)
            g.inlineAdvance = static_cast<float>(ft->glyph->advance.x * kF26Dot6) * sized.scale;
        g.ascent = static_cast<float>(ft->size->metrics.ascender * kF26Dot6) * sized.scale;
        g.descent = static_cast<float>(-ft->size->metrics.descender * kF26Dot6) * sized.scale;
        return g;
    }

    // --- Optical kerning (R4, docs/type-tool.md §13) --------------------------------------------
    // Pair spacing computed from the glyph SHAPES rather than the font's kern pairs: each glyph's
    // left/right ink profile is sampled per scanline at a fixed probe size, and every adjacent
    // pair's mid-band mean gap is pulled toward the face's OWN stem gap (the 'nn' control pair) --
    // "uniformity of the optical white" -- the long-published URW/Karow lineage, with Neville's
    // ink-profile sampling. All profile math is in probe px; the applied delta converts to em.
    static constexpr int kOpticalProbePx = 64;       // profile raster em size
    static constexpr float kOpticalMaxAdjEm = 0.12f; // per-pair adjustment clamp (em)
    static constexpr int kOpticalMinRows = 3;        // fewer overlapping ink rows -> pair unusable

    struct GlyphProfile {
        // rows[r] = {leftmost, rightmost} inked x (probe px, glyph-origin-relative) on the
        // scanline at y = top - r (y-up from the baseline). {INT_MAX, INT_MIN} = no ink there.
        std::vector<std::pair<int, int>> rows;
        int top = 0;          // y of rows[0] above the baseline (probe px)
        float advance = 0.0f; // horizontal advance at probe size
        bool ink = false;
    };
    std::map<std::tuple<std::string, int, std::uint32_t, std::string>, GlyphProfile> profiles;
    std::map<std::tuple<std::string, int, std::string>, float> refGaps;  // per-face stem gap

    static std::string variationsKey(const std::map<std::string, float>& v) {
        std::string k;
        for (const auto& [tag, val] : v) {
            k += tag;
            k += std::to_string(val);
            k += ';';
        }
        return k;
    }

    const GlyphProfile& glyphProfile(const FontFace& face, std::uint32_t gid) {
        auto key = std::make_tuple(face.path, face.index, gid, variationsKey(face.variations));
        if (const auto it = profiles.find(key); it != profiles.end()) return it->second;
        GlyphProfile p;
        FT_Face ft = open(face);
        if (ft != nullptr && FT_IS_SCALABLE(ft)) {
            sizeFace(ft, static_cast<float>(kOpticalProbePx));
            if (FT_Load_Glyph(ft, gid, FT_LOAD_NO_HINTING | FT_LOAD_RENDER) == 0) {
                const FT_GlyphSlot slot = ft->glyph;
                p.advance = static_cast<float>(slot->advance.x * kF26Dot6);
                const FT_Bitmap& bm = slot->bitmap;
                if (bm.pixel_mode == FT_PIXEL_MODE_GRAY && bm.width > 0 && bm.rows > 0) {
                    p.top = slot->bitmap_top;
                    p.rows.assign(bm.rows, {INT_MAX, INT_MIN});
                    for (unsigned y = 0; y < bm.rows; ++y) {
                        const unsigned char* row = bm.buffer + static_cast<std::size_t>(y) * bm.pitch;
                        for (unsigned x = 0; x < bm.width; ++x) {
                            if (row[x] < 32) continue;  // ignore faint AA fringe
                            const int gx = slot->bitmap_left + static_cast<int>(x);
                            p.rows[y].first = std::min(p.rows[y].first, gx);
                            p.rows[y].second = std::max(p.rows[y].second, gx);
                            p.ink = true;
                        }
                    }
                }
            }
        }
        return profiles.emplace(std::move(key), std::move(p)).first->second;
    }

    // Mean horizontal white between A's right and B's left profile (B's origin at A's advance),
    // over the middle 60% of the pair's vertical overlap -- the eye weighs the letter core, not
    // ascender/descender tips. NaN when the glyphs share too few inked scanlines to judge.
    static float pairGapPx(const GlyphProfile& a, const GlyphProfile& b) {
        if (!a.ink || !b.ink) return std::numeric_limits<float>::quiet_NaN();
        const int aBot = a.top - static_cast<int>(a.rows.size()) + 1;
        const int bBot = b.top - static_cast<int>(b.rows.size()) + 1;
        int yHi = std::min(a.top, b.top);
        int yLo = std::max(aBot, bBot);
        const int h = yHi - yLo + 1;
        if (h < kOpticalMinRows) return std::numeric_limits<float>::quiet_NaN();
        yHi -= h / 5;  // trim to the middle ~60%
        yLo += h / 5;
        double sum = 0.0;
        int count = 0;
        for (int y = yLo; y <= yHi; ++y) {
            const auto& ra = a.rows[static_cast<std::size_t>(a.top - y)];
            const auto& rb = b.rows[static_cast<std::size_t>(b.top - y)];
            if (ra.second == INT_MIN || rb.first == INT_MAX) continue;  // one side blank here
            sum += (a.advance + static_cast<float>(rb.first)) - static_cast<float>(ra.second);
            ++count;
        }
        if (count < kOpticalMinRows) return std::numeric_limits<float>::quiet_NaN();
        return static_cast<float>(sum / count);
    }

    // The face's own fitted stem gap -- the white of its 'nn' pair (falling back through 'o', 'H')
    // -- the target every pair's optical white is pulled toward. NaN = face can't be judged.
    float referenceGapPx(const FontFace& face, FT_Face ft) {
        auto key = std::make_tuple(face.path, face.index, variationsKey(face.variations));
        if (const auto it = refGaps.find(key); it != refGaps.end()) return it->second;
        float ref = std::numeric_limits<float>::quiet_NaN();
        for (const char32_t probe : {U'n', U'o', U'H'}) {
            const FT_UInt gid = FT_Get_Char_Index(ft, static_cast<FT_ULong>(probe));
            if (gid == 0) continue;
            const GlyphProfile& p = glyphProfile(face, gid);
            const float g = pairGapPx(p, p);
            if (!std::isnan(g)) {
                ref = g;
                break;
            }
        }
        refGaps.emplace(std::move(key), ref);
        return ref;
    }

    // Adjust the advances of out[begin..) in place: each adjacent pair's gap moves toward the
    // face's reference. Whitespace, colour glyphs and zero-advance marks are left alone.
    void applyOpticalKerning(const FontFace& face, const CharStyle& style,
                             std::vector<RawGlyph>& out, std::size_t begin) {
        if (out.size() < begin + 2) return;
        FT_Face ft = open(face);
        if (ft == nullptr || !FT_IS_SCALABLE(ft)) return;
        const float ref = referenceGapPx(face, ft);
        if (std::isnan(ref)) return;
        const float maxAdjPx = kOpticalMaxAdjEm * static_cast<float>(kOpticalProbePx);
        for (std::size_t i = begin; i + 1 < out.size(); ++i) {
            RawGlyph& a = out[i];
            const RawGlyph& b = out[i + 1];
            if (a.whitespace || b.whitespace || a.colorGlyph || b.colorGlyph) continue;
            const GlyphProfile& pa = glyphProfile(face, a.glyphId);
            const GlyphProfile& pb = glyphProfile(face, b.glyphId);
            if (pa.advance < 1.0f || pb.advance < 1.0f) continue;  // combining marks etc.
            const float gap = pairGapPx(pa, pb);
            if (std::isnan(gap)) continue;
            const float deltaPx = std::clamp(ref - gap, -maxAdjPx, maxAdjPx);
            a.inlineAdvance += deltaPx / static_cast<float>(kOpticalProbePx) * style.sizePx;
        }
    }

    // Shape [segBegin,segEnd) of `utf8` with `face`, appending RawGlyphs tagged with `runIndex`.
    // `mode` selects horizontal (LTR/RTL) or vertical (top-to-bottom) shaping. `rotated` is set for a
    // sideways run in a vertical block (text-orientation mixed): it is shaped HORIZONTALLY (LTR, so
    // kerning/ligatures apply) but its horizontal advance becomes the down-column advance and its
    // outline is later turned 90 CW about the pen -- so a Latin word reads sideways down the column (B3).
    void shapeSegment(const std::string& utf8, std::size_t segBegin, std::size_t segEnd,
                      const FontFace& face, const StyleRun& run, std::size_t runIndex,
                      Paragraph::Direction dir, WritingMode mode, bool rotated,
                      std::vector<RawGlyph>& out) {
        FT_Face ft = open(face);
        if (!ft) return;
        const auto sized = sizeFace(ft, run.style.sizePx);
        const float scale = sized.scale;
        const bool color = FT_HAS_COLOR(ft);
        // Upright vertical shaping (glyphs stacked top-to-bottom): a vertical block MINUS the sideways
        // (rotated) runs, which are shaped horizontally and rotated at draw time instead.
        const bool uprightVertical = mode != WritingMode::HorizontalTB && !rotated;
        const float ascent = static_cast<float>(ft->size->metrics.ascender * kF26Dot6) * scale;
        const float descent = static_cast<float>(-ft->size->metrics.descender * kF26Dot6) * scale;
        const float trackPx = run.style.tracking / 1000.0f * run.style.sizePx;  // 1/1000 em -> px

        hb_font_t* font = hb_ft_font_create_referenced(ft);
        hb_ft_font_set_load_flags(font, FT_LOAD_NO_HINTING);
        hb_buffer_t* buf = hb_buffer_create();
        hb_buffer_add_utf8(buf, utf8.c_str(), static_cast<int>(utf8.size()),
                           static_cast<unsigned>(segBegin), static_cast<int>(segEnd - segBegin));
        hb_buffer_guess_segment_properties(buf);
        if (uprightVertical) hb_buffer_set_direction(buf, HB_DIRECTION_TTB);  // stack down the column
        else if (rotated) hb_buffer_set_direction(buf, HB_DIRECTION_LTR);  // sideways: shape horizontal
        else if (dir == Paragraph::Direction::LTR) hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
        else if (dir == Paragraph::Direction::RTL) hb_buffer_set_direction(buf, HB_DIRECTION_RTL);

        std::vector<hb_feature_t> feats;
        feats.reserve(run.style.features.size() + 2);
        for (const std::string& sfeat : run.style.features) {
            hb_feature_t f;
            if (hb_feature_from_string(sfeat.c_str(), -1, &f)) feats.push_back(f);
        }
        if (uprightVertical) {  // substitute the vertical glyph forms (CJK punctuation etc.)
            hb_feature_t vf;
            if (hb_feature_from_string("vert", -1, &vf)) feats.push_back(vf);
        }
        // Optical/None kerning: switch the font's own pair kerning off (both GPOS 'kern' and the
        // legacy kern table ride the feature). Optical then adds its shape-derived deltas below.
        // Upright vertical (TTB) shaping ignores the mode -- horizontal pair logic has no meaning
        // down a CJK column (a future 'vkrn' lane could).
        if (!uprightVertical && run.style.kerning != Kerning::Metric) {
            hb_feature_t kf;
            if (hb_feature_from_string("-kern", -1, &kf)) feats.push_back(kf);
        }
        hb_shape(font, buf, feats.empty() ? nullptr : feats.data(),
                 static_cast<unsigned>(feats.size()));

        // Vertical CJK is set on a central baseline running down the column with the glyph centred on
        // it; use a symmetric half-em to each side (the em box) so columns sit ~1em wide. HarfBuzz has
        // already folded each glyph's vertical origin into its offsets, so drawing at pen+offset with
        // the normal (horizontal) outline origin -- as glyphContours does -- lands the glyph correctly.
        const float vHalf = 0.5f * run.style.sizePx;

        const std::size_t outStart = out.size();  // this segment's glyphs (optical kerning below)
        unsigned count = 0;
        const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &count);
        const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &count);
        for (unsigned i = 0; i < count; ++i) {
            RawGlyph g;
            g.glyphId = info[i].codepoint;  // already a glyph index after shaping
            g.runIndex = runIndex;
            g.cluster = info[i].cluster;
            g.face = face;
            g.sizePx = run.style.sizePx;
            if (uprightVertical) {
                // Inline axis = down the column: HarfBuzz's y_advance is negative (font y-up), so the
                // downward (layer y-down) advance is its negation. The x/y offsets swap axes vs
                // horizontal -- y positions the glyph along the column, x across it -- and both flip
                // sign for the y-up -> y-down / basis convention (see the pen formula in layout()).
                g.inlineAdvance = static_cast<float>(-pos[i].y_advance * kF26Dot6) * scale + trackPx;
                g.inlineOffset = static_cast<float>(-pos[i].y_offset * kF26Dot6) * scale;
                g.crossOffset = static_cast<float>(-pos[i].x_offset * kF26Dot6) * scale;
                g.ascent = vHalf;
                g.descent = vHalf;
            } else if (rotated) {
                // Sideways glyph in a vertical column: shaped horizontally, so its HORIZONTAL advance
                // (x_advance) is the down-column advance and x_offset shifts it along the column. The
                // outline is turned 90 CW about the pen (glyphContours), which maps the glyph's +x
                // (rightward) advance to +y (down) and its ascent side to +x. To keep a straight column
                // we centre the FACE em box on the column centreline: crossOffset = (asc-desc)/2 slides
                // the alphabetic baseline off-centre by exactly that, and the cross extent is a
                // symmetric half em box each side. (See the pen formula in layout().)
                g.inlineAdvance = static_cast<float>(pos[i].x_advance * kF26Dot6) * scale + trackPx;
                g.inlineOffset = static_cast<float>(pos[i].x_offset * kF26Dot6) * scale;
                g.crossOffset = 0.5f * (ascent - descent);
                g.ascent = 0.5f * (ascent + descent);
                g.descent = 0.5f * (ascent + descent);
                g.rotated = true;
            } else {
                g.inlineAdvance = static_cast<float>(pos[i].x_advance * kF26Dot6) * scale + trackPx;
                g.inlineOffset = static_cast<float>(pos[i].x_offset * kF26Dot6) * scale;
                g.crossOffset = static_cast<float>(pos[i].y_offset * kF26Dot6) * scale;
                g.ascent = ascent;
                g.descent = descent;
            }
            g.baselineShift = run.style.baselineShift;
            g.colorGlyph = color;
            g.whitespace = g.cluster < utf8.size() &&
                           (utf8[g.cluster] == ' ' || utf8[g.cluster] == '\t');
            out.push_back(g);
        }
        hb_buffer_destroy(buf);
        hb_font_destroy(font);
        // Optical kerning: shape-derived pair deltas over this segment's HORIZONTALLY-shaped glyphs
        // (plain horizontal text AND rotated vertical-mixed runs; consecutive glyphs are visually
        // adjacent in both hb output orders). Applied after tracking -- the deltas are additive.
        if (!uprightVertical && run.style.kerning == Kerning::Optical)
            applyOpticalKerning(face, run.style, out, outStart);
    }

    // Shape the [pBegin,pEnd) paragraph: walk its runs, split each into per-face coverage segments
    // (emoji/CJK in a Latin run fall back), shape each. Returns logically-ordered glyphs.
    std::vector<RawGlyph> shapeParagraph(const TextBlock& block, const FontProvider& fonts,
                                         std::size_t pBegin, std::size_t pEnd,
                                         Paragraph::Direction dir) {
        const WritingMode mode = block.writingMode;
        // In a vertical block set `mixed`, sideways codepoints (Latin/digits/punctuation) rotate 90;
        // ideographs/kana/hangul/emoji stay upright. Horizontal or `upright` blocks rotate nothing.
        const bool mixed = mode != WritingMode::HorizontalTB &&
                           block.orientation == TextOrientation::Mixed;
        std::vector<RawGlyph> raw;
        for (std::size_t ri = 0; ri < block.runs.size(); ++ri) {
            const StyleRun& run = block.runs[ri];
            const std::size_t b = std::max(run.begin, pBegin);
            const std::size_t e = std::min(run.end, pEnd);
            if (b >= e) continue;

            const auto primary = fonts.resolve(run.style.font);
            if (!primary) continue;  // no fonts at all
            FT_Face primaryFt = open(*primary);

            std::size_t segStart = b;
            FontFace segFace = *primary;
            bool segRotated = false;
            bool have = false;
            std::size_t i = b;
            while (i < e) {
                const std::size_t cpStart = i;
                const char32_t cp = decodeUtf8(block.utf8, i);
                FontFace chosen = *primary;
                if (cp != U'\n') {
                    const bool covered = primaryFt && FT_Get_Char_Index(primaryFt, cp) != 0;
                    if (!covered) {
                        if (auto fb = fonts.fallbackFor(cp, run.style.font)) chosen = *fb;
                    }
                }
                const bool rot = mixed && cp != U'\n' && !isVerticalUpright(cp);
                if (!have) {
                    segFace = chosen;
                    segRotated = rot;
                    segStart = cpStart;
                    have = true;
                } else if (!(chosen == segFace) || rot != segRotated) {
                    // A face OR orientation change ends the segment (they shape differently).
                    shapeSegment(block.utf8, segStart, cpStart, segFace, run, ri, dir, mode,
                                 segRotated, raw);
                    segFace = chosen;
                    segRotated = rot;
                    segStart = cpStart;
                }
            }
            if (have)
                shapeSegment(block.utf8, segStart, e, segFace, run, ri, dir, mode, segRotated, raw);
        }
        return raw;
    }
};

TextShaper::TextShaper() : m_impl(std::make_unique<Impl>()) {}
TextShaper::~TextShaper() = default;
TextShaper::TextShaper(TextShaper&&) noexcept = default;
TextShaper& TextShaper::operator=(TextShaper&&) noexcept = default;

void TextShaper::setDefaultLanguage(std::string language) {
    m_impl->defaultLanguage = std::move(language);
}

namespace {

// Bow a laid-out block's baseline into a circular ARCH (S30, docs/type-tool.md §9). The flat layout is
// produced first (wrapping / alignment / per-run styling unchanged); this pass then carries each glyph
// RIGIDLY to its position ALONG the arc and turns it by the local tangent -- "placement by position +
// tangent". Each glyph's centre is placed at its own advance-distance along a circular arc of total
// length W (the text's advance) and signed sweep `bend`·kMaxSweep, so spacing stays even on the curve
// (letters no longer bunch where the arc flattens) and the word contracts horizontally as it bends --
// the standard text-on-an-arc look. `bend` in [-1,1]: positive arches up (∩), negative down (∪).
// Recomputes the block bounds/height from the warped glyph boxes so the pixel cache / Move gizmo frame
// the arch; the flat per-line fields are left alone -- caret/hit geometry rides `pen`+`baselineAngle`
// (which this sets) and bentArc.pointAt, not the line boxes.
void applyBend(ShapedBlock& sb, const TextBlock& block) {
    constexpr double kMaxSweep = kBendMaxSweep;  // total arc sweep at |bend|==1 (~137 degrees)
    const float bend = block.bend;
    if (sb.glyphs.empty() || sb.lines.empty() || std::abs(bend) < 1e-4f) return;

    // Flat content span (= the text's total advance) and the reference baseline (the first line's).
    double minX = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    for (const ShapedGlyph& g : sb.glyphs) {
        minX = std::min(minX, static_cast<double>(g.pen.x));
        maxX = std::max(maxX, static_cast<double>(g.pen.x) + g.advance);
    }
    const double W = maxX - minX;
    if (W <= 0.0) return;

    // ⚠ AREA: the FRAME drives the arc, not the text (user 2026-07-14: "only the bottom line of the
    // box conforms to the bend... the Area text conforms based on the text size instead of the Area
    // box"). The reference arc is the frame's TOP EDGE -- x0 = 0, y0 = 0, arc length = the box
    // width -- so the whole box warps as ONE annular sector: every line rides a parallel arc at its
    // own depth below the frame top, two different type sizes in the same box bend IDENTICALLY, and
    // the frame chrome (top edge, bowed bottom, radial sides) samples this same arc family. POINT
    // text keeps the text-driven arc -- its own advance span, anchored at the first baseline --
    // byte-identical to before (the bend goldens pin it). The gate mirrors the wrap's own Area rule
    // (frame == Area && inline > 0; inline = areaSize.x here because applyBend is horizontal-only
    // at its call site).
    const bool area = block.frame == TextFrame::Area && block.areaSize.x > 0.0;
    const double arcX0 = area ? 0.0 : minX;
    const double arcW = area ? block.areaSize.x : W;
    const double refBaseline = area ? 0.0 : static_cast<double>(sb.lines.front().baselineY);
    // A circular arc of arc-length arcW and signed sweep theta (theta>0 = arch up). Placing each glyph
    // at distance s ALONG this arc keeps its spacing even on the curve (rather than bunching where the
    // arc flattens); the arc contracts horizontally as it bends, the natural text-on-an-arc look.
    const double theta = static_cast<double>(bend) * kMaxSweep;
    sb.bentArc = {static_cast<float>(arcX0), static_cast<float>(refBaseline),
                  static_cast<float>(arcW), static_cast<float>(theta), true};

    double bx0 = std::numeric_limits<double>::max(), by0 = bx0;
    double bx1 = std::numeric_limits<double>::lowest(), by1 = bx1;
    for (ShapedGlyph& g : sb.glyphs) {
        // Distance along the arc of this glyph's CENTRE = its flat centre's advance from the arc's
        // origin (the line start for Point, the frame's left edge for Area).
        const double s = static_cast<double>(g.pen.x) + 0.5 * g.advance - arcX0;
        double angle = 0.0;
        const Vec2 pos = sb.bentArc.pointAt(s, angle);
        const double c = std::cos(angle), sn = std::sin(angle);
        // Multi-line: shift the glyph perpendicular to the arch by its flat distance below the reference
        // baseline (lower lines ride a parallel arch). N = tangent rotated +90 in y-down space = (-sn,c).
        const double dPerp = static_cast<double>(g.pen.y) - refBaseline;
        const double half = 0.5 * g.advance;
        // Left origin so the glyph's centre lands on `pos`: back off half an advance along the tangent,
        // then step dPerp along the normal.
        g.pen = {static_cast<float>(pos.x - c * half - sn * dPerp),
                 static_cast<float>(pos.y - sn * half + c * dPerp)};
        g.baselineAngle = static_cast<float>(angle);

        // Union the glyph's rotated metric box (local x in [0,advance], y in [-ascent,descent]).
        const double asc = sb.lines[g.line].ascent, desc = sb.lines[g.line].descent;
        for (const double lx : {0.0, static_cast<double>(g.advance)}) {
            for (const double ly : {-asc, desc}) {
                const double wx = g.pen.x + lx * c - ly * sn;
                const double wy = g.pen.y + lx * sn + ly * c;
                bx0 = std::min(bx0, wx);
                by0 = std::min(by0, wy);
                bx1 = std::max(bx1, wx);
                by1 = std::max(by1, wy);
            }
        }
    }
    if (bx1 >= bx0) {
        sb.bounds = common::Rect::fromCorners({static_cast<float>(bx0), static_cast<float>(by0)},
                                              {static_cast<float>(bx1), static_cast<float>(by1)});
        sb.height = static_cast<float>(by1);
    }
}

// Fit-to-path (S30, §9): carry the finished flat layout onto the block's baked path, glyph by
// glyph, exactly as applyBend carries it onto the arc -- centre at its advance-distance along the
// curve, turned to the local tangent, lower lines offset along the local normal. The mapping from
// flat x to arc-distance (shared with the editing chrome via ShapedBlock::pathRide/pathArcDistance)
// starts at bracket s0 and runs toward s1; `flip` reverses direction and mirrors across the path.
void applyPath(ShapedBlock& sb, const TextBlock& block) {
    const PathFit& fit = *block.pathFit;
    if (sb.glyphs.empty() || sb.lines.empty() || fit.baked.empty()) return;

    double minX = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    for (const ShapedGlyph& g : sb.glyphs) {
        minX = std::min(minX, static_cast<double>(g.pen.x));
        maxX = std::max(maxX, static_cast<double>(g.pen.x) + g.advance);
    }
    const double refBaseline = sb.lines.front().baselineY;
    sb.pathRide = {true, static_cast<float>(minX), static_cast<float>(std::max(0.0, maxX - minX))};

    double bx0 = std::numeric_limits<double>::max(), by0 = bx0;
    double bx1 = std::numeric_limits<double>::lowest(), by1 = bx1;
    for (ShapedGlyph& g : sb.glyphs) {
        const double flatCx = static_cast<double>(g.pen.x) + 0.5 * g.advance;
        double angle = 0.0;
        const Vec2 pos = samplePathBaseline(fit, pathArcDistance(fit, flatCx, minX), angle);
        const double c = std::cos(angle), sn = std::sin(angle);
        const double dPerp = static_cast<double>(g.pen.y) - refBaseline;
        const double half = 0.5 * g.advance;
        g.pen = {static_cast<float>(pos.x - c * half - sn * dPerp),
                 static_cast<float>(pos.y - sn * half + c * dPerp)};
        g.baselineAngle = static_cast<float>(angle);

        const double asc = sb.lines[g.line].ascent, desc = sb.lines[g.line].descent;
        for (const double lx : {0.0, static_cast<double>(g.advance)}) {
            for (const double ly : {-asc, desc}) {
                const double wx = g.pen.x + lx * c - ly * sn;
                const double wy = g.pen.y + lx * sn + ly * c;
                bx0 = std::min(bx0, wx);
                by0 = std::min(by0, wy);
                bx1 = std::max(bx1, wx);
                by1 = std::max(by1, wy);
            }
        }
    }
    if (bx1 >= bx0) {
        sb.bounds = common::Rect::fromCorners({static_cast<float>(bx0), static_cast<float>(by0)},
                                              {static_cast<float>(bx1), static_cast<float>(by1)});
        sb.height = static_cast<float>(by1);
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// fit-to-path samplers (§9) -- shared by applyPath above and the editing chrome (see shaping.hpp)
// ---------------------------------------------------------------------------------------------
double pathArcDistance(const PathFit& fit, double flatX, double originX) {
    const double d = flatX - originX;
    return fit.flip ? fit.s1 - d : fit.s0 + d;
}

Vec2 samplePathBaseline(const PathFit& fit, double arcDistance, double& angle) {
    constexpr double kPi = 3.14159265358979323846;
    const double total = vec::contourLength(fit.baked);
    Vec2 pos{0, 0};
    Vec2 tan{1, 0};
    // A closed single-contour path (circle/star/shape rim) wraps: the text slides around it
    // forever. Open paths extend STRAIGHT past their ends instead, so overflowing text stays
    // visible and editable (never piled up on the clamped end point).
    const bool wraps = fit.baked.size() == 1 && fit.baked.front().closed && total > 1e-9;
    if (wraps) {
        double m = std::fmod(arcDistance, total);
        if (m < 0.0) m += total;
        const vec::PathSample ps = vec::samplePathAt(fit.baked, m);
        pos = ps.pos;
        tan = ps.tangent;
    } else if (arcDistance < 0.0) {
        const vec::PathSample ps = vec::samplePathAt(fit.baked, 0.0);
        pos = ps.pos + ps.tangent * arcDistance;
        tan = ps.tangent;
    } else if (arcDistance > total) {
        const vec::PathSample ps = vec::samplePathAt(fit.baked, total);
        pos = ps.pos + ps.tangent * (arcDistance - total);
        tan = ps.tangent;
    } else {
        const vec::PathSample ps = vec::samplePathAt(fit.baked, arcDistance);
        pos = ps.pos;
        tan = ps.tangent;
    }
    angle = std::atan2(tan.y, tan.x);
    if (fit.flip) angle += kPi;  // the mirrored side reads along the reversed direction
    return pos;
}

// ---------------------------------------------------------------------------------------------
// layout
// ---------------------------------------------------------------------------------------------
ShapedBlock TextShaper::layout(const TextBlock& block, const FontProvider& fonts) {
    ShapedBlock out;
    out.writingMode = block.writingMode;  // downstream geometry reads the mode off the shaped block
    if (block.runs.empty()) {
        out.bounds = common::Rect{0, 0, 0, 0};  // empty text: just a caret line (drawn in S29-b)
        // The caret's height comes from the pending style's real face metrics, so an empty block's
        // caret already shows the size the typed text will be (no jump on the first glyph).
        out.emptyCaretHeight = m_impl->emptyCaretHeight(block.emptyStyle, fonts);
        return out;
    }
    const bool vertical = block.writingMode != WritingMode::HorizontalTB;
    // The wrapping budget runs along the INLINE axis: the box WIDTH for horizontal text, the box
    // HEIGHT for vertical (columns fill top-to-bottom, then wrap to the next column).
    const double boxInline = vertical ? block.areaSize.y : block.areaSize.x;
    const bool area = block.frame == TextFrame::Area && boxInline > 0.0;

    struct ParaGlyphs {
        std::vector<RawGlyph> glyphs;
        std::vector<LineRange> lines;
        std::size_t paragraph;
    };
    std::vector<ParaGlyphs> paras;

    std::size_t pStart = 0;
    std::size_t paraIndex = 0;
    const std::string& s = block.utf8;
    while (pStart <= s.size()) {
        const std::size_t nl = s.find('\n', pStart);
        const std::size_t pEnd = (nl == std::string::npos) ? s.size() : nl;
        const Paragraph& para =
            paraIndex < block.paragraphs.size() ? block.paragraphs[paraIndex] : Paragraph{};
        ParaGlyphs pg;
        pg.paragraph = paraIndex;
        pg.glyphs = m_impl->shapeParagraph(block, fonts, pStart, pEnd, para.direction);

        if (!area) {
            pg.lines.push_back({0, pg.glyphs.size(), paraIndex});  // Point: one line per paragraph
        } else {
            const float avail = static_cast<float>(boxInline) - 2.0f * kAreaInset;
            const float rightBudget = avail - static_cast<float>(para.indentRight);  // max inline pen

            // Effective hyphenation language: the paragraph's own, else the shaper default (locale/
            // document). Hyphenation only helps where text wraps, so it lives in this Area branch.
            const std::string hyLang =
                para.language.empty() ? m_impl->defaultLanguage : para.language;
            const bool doHyphenate = para.hyphenate && !hyLang.empty() && !vertical;

            std::size_t lineBegin = 0;
            std::size_t lastBreak = 0;  // glyph index just after the last whitespace
            float lineIndent = static_cast<float>(para.indentFirst + para.indentLeft);  // line start x
            float x = lineIndent;
            const auto advanceOf = [&](std::size_t from, std::size_t to) {
                float w = 0.0f;
                for (std::size_t k = from; k < to; ++k) w += pg.glyphs[k].inlineAdvance;
                return w;
            };

            for (std::size_t gi = 0; gi < pg.glyphs.size(); ++gi) {
                const RawGlyph& g = pg.glyphs[gi];
                if (x + g.inlineAdvance <= rightBudget) {  // fits: keep going
                    x += g.inlineAdvance;
                    if (g.whitespace) lastBreak = gi + 1;
                    continue;
                }

                // Overflow. Prefer hyphenating the current word (fills the line, tames justify
                // rivers); else wrap at the last whitespace; else an emergency character break.
                bool broke = false;
                if (doHyphenate && !g.whitespace) {
                    const std::size_t wordStart = lastBreak;
                    std::size_t wordEnd = gi;
                    while (wordEnd < pg.glyphs.size() && !pg.glyphs[wordEnd].whitespace) ++wordEnd;
                    const std::size_t byteBegin = pg.glyphs[wordStart].cluster;
                    const std::size_t byteEnd =
                        (wordEnd < pg.glyphs.size()) ? pg.glyphs[wordEnd].cluster : pEnd;
                    if (wordStart >= lineBegin && wordStart < wordEnd && byteEnd > byteBegin) {
                        const std::string word = s.substr(byteBegin, byteEnd - byteBegin);
                        const auto pts = m_impl->hyphenator.hyphenationPoints(word, hyLang);
                        if (!pts.empty()) {
                            const RawGlyph hy = m_impl->shapeHyphen(pg.glyphs[wordStart].face,
                                                                    pg.glyphs[wordStart].sizePx,
                                                                    pg.glyphs[wordStart].runIndex);
                            std::size_t bestK = lineBegin;  // the last break glyph that fits
                            for (std::size_t p : pts) {
                                const std::size_t absBreak = byteBegin + p;
                                std::size_t k = wordStart;
                                while (k < wordEnd && pg.glyphs[k].cluster < absBreak) ++k;
                                if (k >= wordEnd || pg.glyphs[k].cluster != absBreak) continue;
                                if (k <= lineBegin) continue;  // first part must be non-empty
                                if (lineIndent + advanceOf(lineBegin, k) + hy.inlineAdvance <= rightBudget)
                                    bestK = k;
                            }
                            if (bestK > lineBegin) {
                                LineRange lr{lineBegin, bestK, paraIndex};
                                lr.hyphen = hy;
                                pg.lines.push_back(lr);
                                lineBegin = bestK;
                                lastBreak = bestK;
                                lineIndent = static_cast<float>(para.indentLeft);
                                x = lineIndent + advanceOf(bestK, gi + 1);
                                broke = true;
                            }
                        }
                    }
                }

                if (!broke && lastBreak > lineBegin) {
                    // Word wrap: break at the last whitespace.
                    pg.lines.push_back({lineBegin, lastBreak, paraIndex});
                    lineBegin = lastBreak;
                    lineIndent = static_cast<float>(para.indentLeft);
                    x = lineIndent + advanceOf(lineBegin, gi + 1);
                    broke = true;
                } else if (!broke && gi > lineBegin && !g.whitespace) {
                    // No break fits and the word is itself wider than the box: emergency character
                    // break before this glyph (overflow-wrap: break-word), so it wraps in the box.
                    pg.lines.push_back({lineBegin, gi, paraIndex});
                    lineBegin = gi;
                    lastBreak = gi;  // a fresh line start is also a valid break boundary
                    lineIndent = static_cast<float>(para.indentLeft);
                    x = lineIndent + g.inlineAdvance;
                    broke = true;
                }
                if (!broke) x += g.inlineAdvance;
                if (g.whitespace) lastBreak = gi + 1;
            }
            pg.lines.push_back({lineBegin, pg.glyphs.size(), paraIndex});  // final line
        }
        paras.push_back(std::move(pg));

        ++paraIndex;
        if (nl == std::string::npos) break;
        pStart = nl + 1;
    }

    // Inline-axis extent for alignment (Area: the box's inline side minus its two insets; Point: the
    // widest line). "Width" here is the inline measure -- the column length for vertical text.
    const float insetX = area ? kAreaInset : 0.0f;  // §#4: keep glyphs/caret off the Area box frame
    const float insetY = area ? kAreaInset : 0.0f;
    float layoutWidth = area ? static_cast<float>(boxInline) - 2.0f * kAreaInset : 0.0f;
    if (!area) {
        for (const auto& pg : paras)
            for (const auto& ln : pg.lines)
                layoutWidth = std::max(layoutWidth, contentWidth(pg.glyphs, ln.begin, ln.end));
    }

    // Position pass: baselines (leading + paragraph spacing) and per-line alignment. Coordinates are
    // computed in the inline/block frame and projected to layer-local through `basis` (identity for
    // horizontal; axis-swapped for vertical). Vertical-rl fills columns from the box's RIGHT edge
    // leftward, so its block origin is anchored there for Area; a Point block has no box, so it grows
    // leftward into negative x from the layer origin (its bounds capture the extent either way).
    const float blockAnchor = (block.writingMode == WritingMode::VerticalRL && area)
                                  ? static_cast<float>(block.areaSize.x)
                                  : 0.0f;
    const LayoutBasis basis{block.writingMode, blockAnchor};
    float prevBaseline = 0.0f;
    bool first = true;
    std::size_t prevPara = static_cast<std::size_t>(-1);
    common::Rect bounds{};
    bool boundsSet = false;

    for (const auto& pg : paras) {
        const Paragraph& para = pg.paragraph < block.paragraphs.size()
                                    ? block.paragraphs[pg.paragraph]
                                    : Paragraph{};
        for (std::size_t li = 0; li < pg.lines.size(); ++li) {
            const LineRange& ln = pg.lines[li];
            const bool emptyLine = ln.begin == ln.end;
            const float em = maxEm(pg.glyphs, ln.begin, ln.end, 24.0f);
            float ascent = 0.0f, descent = 0.0f;
            for (std::size_t i = ln.begin; i < ln.end; ++i) {
                ascent = std::max(ascent, pg.glyphs[i].ascent);
                descent = std::max(descent, pg.glyphs[i].descent);
            }
            if (ln.hyphen) {  // a trailing auto-hyphen contributes its own face metrics
                ascent = std::max(ascent, ln.hyphen->ascent);
                descent = std::max(descent, ln.hyphen->descent);
            }
            if (emptyLine) {  // no glyphs to measure -- estimate the line's cross extent from the em
                if (vertical) {  // centred column baseline: half the em to each side
                    ascent = 0.5f * em;
                    descent = 0.5f * em;
                } else {  // a typical 80/20 ascent/descent split
                    ascent = 0.8f * em;
                    descent = 0.2f * em;
                }
            }
            const float lineHeight = para.leadingAbsolute ? para.leading : para.leading * em;

            float baselineY;
            if (first) {
                baselineY = insetY + static_cast<float>(para.spaceBefore) + ascent;
                first = false;
            } else {
                baselineY = prevBaseline + lineHeight;
                if (li == 0)  // first line of a new paragraph: add the inter-paragraph spacing
                    baselineY += static_cast<float>(para.spaceBefore) +
                                 (prevPara < block.paragraphs.size()
                                      ? static_cast<float>(block.paragraphs[prevPara].spaceAfter)
                                      : 0.0f);
            }
            prevBaseline = baselineY;
            prevPara = pg.paragraph;

            // The trailing auto-hyphen's advance counts toward the line width, so alignment and
            // justify leave room for it and the line box spans it.
            const float hyphenAdv = ln.hyphen ? ln.hyphen->inlineAdvance : 0.0f;
            const float width = contentWidth(pg.glyphs, ln.begin, ln.end) + hyphenAdv;
            const float availContent =
                layoutWidth - static_cast<float>(para.indentLeft + para.indentRight);
            float lineX = static_cast<float>(para.indentLeft);
            if (li == 0) lineX += static_cast<float>(para.indentFirst);
            switch (para.align) {
                case Paragraph::Align::Left: break;
                case Paragraph::Align::Center:
                    lineX = static_cast<float>(para.indentLeft) + (availContent - width) * 0.5f;
                    break;
                case Paragraph::Align::Right:
                    lineX = layoutWidth - static_cast<float>(para.indentRight) - width;
                    break;
                case Paragraph::Align::Justify: break;  // per-glyph below (Area, non-last line)
            }
            lineX += insetX;  // shift the whole line clear of the Area box's left edge (§#4)

            float justifyPerSpace = 0.0f;
            // Area justifies every WRAPPED line but a paragraph's final line (which sets ragged). Point
            // has exactly one line per paragraph, so that rule would never justify anything -- instead
            // justify every line but the very LAST of the block, stretching each to the block's natural
            // width (layoutWidth == the widest line; the widest line itself doesn't move).
            const bool lastOfPara = (li + 1 >= pg.lines.size());
            const bool lastOfBlock = lastOfPara && (&pg == &paras.back());
            const bool justifyLine = area ? !lastOfPara : !lastOfBlock;
            if (para.align == Paragraph::Align::Justify && justifyLine) {
                std::size_t spaces = 0;
                std::size_t lastInk = ln.end;
                while (lastInk > ln.begin && pg.glyphs[lastInk - 1].whitespace) --lastInk;
                for (std::size_t i = ln.begin; i < lastInk; ++i)
                    if (pg.glyphs[i].whitespace) ++spaces;
                if (spaces > 0) justifyPerSpace = (availContent - width) / static_cast<float>(spaces);
            }

            // A justified (stretched) line fills the available content width: its glyphs reach the
            // box's right inset, so its box must span that full width -- using the un-stretched
            // contentWidth here clips the justified text on the right AND makes the block's bounds
            // shrink/jump as you type (the spaces stretch differently each edit). user report.
            const float laidWidth = justifyPerSpace > 0.0f ? availContent : width;

            ShapedLine sl;
            sl.begin = out.glyphs.size();
            sl.paragraph = pg.paragraph;
            sl.baselineY = baselineY;
            sl.ascent = ascent;
            sl.descent = descent;
            sl.x = lineX;
            sl.width = laidWidth;

            float penX = lineX;
            for (std::size_t i = ln.begin; i < ln.end; ++i) {
                const RawGlyph& g = pg.glyphs[i];
                ShapedGlyph sg;
                sg.glyphId = g.glyphId;
                sg.runIndex = g.runIndex;
                sg.cluster = g.cluster;
                sg.line = out.lines.size();
                sg.face = g.face;
                sg.sizePx = g.sizePx;
                sg.colorGlyph = g.colorGlyph;
                sg.whitespace = g.whitespace;
                sg.rotated = g.rotated;
                sg.pen = basis.glyphPen(penX + g.inlineOffset, baselineY, g.crossOffset, g.baselineShift);
                sg.advance = g.inlineAdvance + (g.whitespace ? justifyPerSpace : 0.0f);
                out.glyphs.push_back(std::move(sg));
                penX += g.inlineAdvance;
                if (g.whitespace) penX += justifyPerSpace;
            }
            if (ln.hyphen) {  // draw the auto-hyphen at the line's right edge, in the run's face
                const RawGlyph& g = *ln.hyphen;
                ShapedGlyph sg;
                sg.glyphId = g.glyphId;
                sg.runIndex = g.runIndex;
                // Cluster == the line's byte end (the tail's first byte), so caret/hit-testing treat
                // the hyphen as a zero-stop trailing mark rather than a real caret position.
                sg.cluster = ln.end < pg.glyphs.size() ? pg.glyphs[ln.end].cluster : block.utf8.size();
                sg.line = out.lines.size();
                sg.face = g.face;
                sg.sizePx = g.sizePx;
                sg.pen = basis.place(penX, baselineY);
                sg.advance = g.inlineAdvance;
                out.glyphs.push_back(std::move(sg));
                penX += g.inlineAdvance;
            }
            sl.end = out.glyphs.size();
            out.lines.push_back(sl);

            const common::Rect lineBox = basis.lineRect(lineX, laidWidth, baselineY, ascent, descent);
            bounds = boundsSet ? bounds.united(lineBox) : lineBox;
            boundsSet = true;
        }
    }

    out.width = layoutWidth;
    out.height = boundsSet ? static_cast<float>(bounds.bottom()) : 0.0f;
    out.bounds = boundsSet ? bounds : common::Rect{0, 0, 0, 0};

    // S30 (§9): carry the finished flat layout onto a curved baseline if the block requests one.
    // Horizontal text only (the vertical modes reserve the axis). An extruded block warps too: the
    // warped glyph outlines flow straight into the extrude mesher, so the solid follows the curved
    // baseline (§9's deferred composition, landed 2026-07-07). Fit-to-path takes precedence over
    // bend; both rewrite glyph pens/angles and the block bounds in place, and a flat block takes
    // neither branch, so flat text stays byte-identical to before.
    if (block.writingMode == WritingMode::HorizontalTB) {
        if (block.pathFit && !block.pathFit->baked.empty())
            applyPath(out, block);
        else if (block.bend != 0.0f)
            applyBend(out, block);
    }
    return out;
}

common::Rect bentSectorBounds(const ShapedBlock::BentArc& arc, double depth) {
    if (std::abs(arc.theta) < 1e-4)
        return {static_cast<double>(arc.x0), static_cast<double>(arc.baseY),
                static_cast<double>(arc.W), depth};
    // Sample the two bounding arcs (the reference at d = 0 and the deep edge at d = depth). The
    // sector's radial sides are chords between corresponding endpoints, so the arc samples --
    // which include those endpoints -- already cover every extreme. 32 samples puts the sampled
    // apex within a fraction of a px of the true one at the max sweep (~137 deg).
    constexpr int kSamples = 32;
    double x0 = std::numeric_limits<double>::max(), y0 = x0;
    double x1 = std::numeric_limits<double>::lowest(), y1 = x1;
    for (const double d : {0.0, depth}) {
        for (int i = 0; i <= kSamples; ++i) {
            const double s = arc.W * (static_cast<double>(i) / kSamples);
            double ang = 0.0;
            const Vec2 p = arc.pointAt(s, ang);
            const double px = p.x - std::sin(ang) * d;
            const double py = p.y + std::cos(ang) * d;
            x0 = std::min(x0, px);
            y0 = std::min(y0, py);
            x1 = std::max(x1, px);
            y1 = std::max(y1, py);
        }
    }
    return common::Rect::fromCorners({x0, y0}, {x1, y1});
}

// ---------------------------------------------------------------------------------------------
// Outline extraction -- the Contours seam (§5.1)
// ---------------------------------------------------------------------------------------------
namespace {

struct OutlineCtx {
    Vec2 origin;
    bool rotated = false;  // turn the outline 90 CW about the pen (vertical `mixed` Latin, B3)
    double ca = 1.0, sa = 0.0;  // baselineAngle rotation about the pen (S30 bend/on-path); identity at 0
    std::vector<vec::Node> cur;
    vec::Path path;

    Vec2 L(const FT_Vector* v) const {  // font px (y-up, baseline) -> layer-local (y-down)
        // Upright: the layer offset from the pen is (x, -y). Rotated 90 CW (screen y-down) maps that
        // offset (dx,dy) -> (-dy,dx) = (y, x): the glyph's +x advance turns to +y down the column and
        // its top faces +x, so the row-set glyph reads sideways down the column.
        if (rotated) return {origin.x + v->y * kF26Dot6, origin.y + v->x * kF26Dot6};
        if (sa == 0.0 && ca == 1.0)  // flat: the exact pre-bend expression (keeps goldens byte-identical)
            return {origin.x + v->x * kF26Dot6, origin.y - v->y * kF26Dot6};
        // Bent/on-path: rotate the pen-local offset by baselineAngle so the glyph rides the tangent.
        const double lx = v->x * kF26Dot6, ly = -(v->y * kF26Dot6);
        return {origin.x + static_cast<float>(lx * ca - ly * sa),
                origin.y + static_cast<float>(lx * sa + ly * ca)};
    }
    void finish() {
        if (cur.size() >= 2) {
            // FreeType closes a contour back to its start point; merge that duplicate end node so
            // the closing segment is carried by first.inHandle (handles, not an extra anchor).
            const Vec2 d = cur.front().anchor - cur.back().anchor;
            if (std::abs(d.x) < 1e-6 && std::abs(d.y) < 1e-6) {
                cur.front().inHandle = cur.back().inHandle;
                cur.pop_back();
            }
            vec::SubPath sp;
            sp.nodes = std::move(cur);
            sp.closed = true;
            path.subpaths.push_back(std::move(sp));
        }
        cur.clear();
    }
};

int outMoveTo(const FT_Vector* to, void* user) {
    auto* c = static_cast<OutlineCtx*>(user);
    c->finish();
    const Vec2 p = c->L(to);
    c->cur.push_back(vec::Node{p, p, p});
    return 0;
}
int outLineTo(const FT_Vector* to, void* user) {
    auto* c = static_cast<OutlineCtx*>(user);
    if (c->cur.empty()) return 0;
    const Vec2 p = c->L(to);
    c->cur.back().outHandle = c->cur.back().anchor;  // straight out of the previous anchor
    c->cur.push_back(vec::Node{p, p, p});
    return 0;
}
int outConicTo(const FT_Vector* control, const FT_Vector* to, void* user) {
    auto* c = static_cast<OutlineCtx*>(user);
    if (c->cur.empty()) return 0;
    const Vec2 cp = c->L(control);
    const Vec2 a = c->cur.back().anchor;
    const Vec2 b = c->L(to);
    c->cur.back().outHandle = a + (cp - a) * (2.0 / 3.0);  // quadratic -> cubic control elevation
    c->cur.push_back(vec::Node{b, b + (cp - b) * (2.0 / 3.0), b});
    return 0;
}
int outCubicTo(const FT_Vector* c1, const FT_Vector* c2, const FT_Vector* to, void* user) {
    auto* c = static_cast<OutlineCtx*>(user);
    if (c->cur.empty()) return 0;
    c->cur.back().outHandle = c->L(c1);
    const Vec2 b = c->L(to);
    c->cur.push_back(vec::Node{b, c->L(c2), b});
    return 0;
}

}  // namespace

vec::Path TextShaper::glyphPath(const ShapedGlyph& g) {
    if (g.whitespace || g.colorGlyph) return {};
    FT_Face ft = m_impl->open(g.face);
    if (!ft) return {};
    m_impl->sizeFace(ft, g.sizePx);
    if (FT_Load_Glyph(ft, g.glyphId, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0) return {};
    if (ft->glyph->format != FT_GLYPH_FORMAT_OUTLINE) return {};

    OutlineCtx ctx;
    ctx.origin = g.pen;
    ctx.rotated = g.rotated;
    ctx.ca = std::cos(g.baselineAngle);  // bend/on-path: rotate the outline about the pen (§9)
    ctx.sa = std::sin(g.baselineAngle);
    FT_Outline_Funcs funcs{};
    funcs.move_to = outMoveTo;
    funcs.line_to = outLineTo;
    funcs.conic_to = outConicTo;
    funcs.cubic_to = outCubicTo;
    funcs.shift = 0;
    funcs.delta = 0;
    if (FT_Outline_Decompose(&ft->glyph->outline, &funcs, &ctx) != 0) return {};
    ctx.finish();
    if (ctx.path.subpaths.empty()) return {};
    ctx.path.fillRule = vec::FillRule::NonZero;  // glyph outlines use non-zero winding
    return ctx.path;
}

vec::Contours TextShaper::glyphContours(const ShapedGlyph& g, double tolerancePx) {
    const vec::Path path = glyphPath(g);
    if (path.subpaths.empty()) return {};
    return vec::flatten(path, tolerancePx);
}

DecorationMetrics TextShaper::decorationMetrics(const FontFace& face, float sizePx) {
    DecorationMetrics dm;
    dm.underlineThickness = std::max(1.0f, sizePx / 18.0f);  // sane defaults if the face lacks them
    dm.underlineOffset = sizePx / 8.0f;
    dm.strikeoutThickness = dm.underlineThickness;
    dm.strikeoutOffset = sizePx * 0.26f;
    FT_Face ft = m_impl->open(face);
    if (!ft) return dm;
    const auto sized = m_impl->sizeFace(ft, sizePx);
    if (ft->units_per_EM > 0 && FT_IS_SCALABLE(ft)) {
        const float toPx = sizePx / static_cast<float>(ft->units_per_EM) * sized.scale;
        if (ft->underline_thickness != 0) dm.underlineThickness = ft->underline_thickness * toPx;
        // underline_position is below the baseline (negative in font space) -> positive-down
        // offset.
        //
        // ⚠ ONLY when the face states one. A `post` table with underlinePosition == 0 is not a face
        // asking for an underline ON the baseline -- it is a face that left the field unfilled, and
        // plenty do. Taking it literally drew the bar straight through the glyph bottoms, where it
        // reads as "underline does nothing on this font" (user 2026-08-28). The default above (one
        // eighth of the em below the baseline) is the honest answer for a face that does not say.
        if (ft->underline_position != 0)
            dm.underlineOffset = -ft->underline_position * toPx;
        // Strikeout comes from OS/2 (yStrikeoutPosition / yStrikeoutSize), which FT_FaceRec does
        // not surface the way it surfaces the `post` underline pair -- so it has to be read off the
        // table directly. Before this it was pure guesswork (0.26 em above the baseline), which
        // lands too low on a large-x-height face and clips the glyph's waist on a small one; a face
        // that states its own position is the only one that knows. Same rule as underline: a zero
        // is an unfilled field, not a strike through the baseline.
        if (const auto* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(ft, FT_SFNT_OS2));
            os2 != nullptr && os2->version != 0xFFFFu) {
            if (os2->yStrikeoutSize != 0)
                dm.strikeoutThickness = os2->yStrikeoutSize * toPx;
            if (os2->yStrikeoutPosition != 0)
                dm.strikeoutOffset = os2->yStrikeoutPosition * toPx;
        }
    }
    return dm;
}

std::vector<VariableAxis> TextShaper::variableAxes(const FontFace& face) {
    auto key = std::make_pair(face.path, face.index);
    if (const auto it = m_impl->axisCache.find(key); it != m_impl->axisCache.end())
        return it->second;
    std::vector<VariableAxis> out;
    if (FT_Face ft = m_impl->open(face); ft != nullptr && FT_HAS_MULTIPLE_MASTERS(ft)) {
        FT_MM_Var* mm = nullptr;
        if (FT_Get_MM_Var(ft, &mm) == 0 && mm != nullptr) {
            for (FT_UInt i = 0; i < mm->num_axis; ++i) {
                FT_UInt flags = 0;
                // Hidden axes are the font's internal interpolation knobs, not user controls.
                if (FT_Get_Var_Axis_Flags(mm, i, &flags) == 0 &&
                    (flags & FT_VAR_AXIS_FLAG_HIDDEN) != 0)
                    continue;
                const FT_Var_Axis& ax = mm->axis[i];
                VariableAxis va;
                va.tag = Impl::axisTag(ax.tag);
                va.name = ax.name != nullptr && ax.name[0] != '\0' ? ax.name : va.tag;
                va.min = static_cast<float>(ax.minimum / 65536.0);
                va.def = static_cast<float>(ax.def / 65536.0);
                va.max = static_cast<float>(ax.maximum / 65536.0);
                if (va.max > va.min)  // a degenerate range can't be a slider
                    out.push_back(std::move(va));
            }
            FT_Done_MM_Var(m_impl->lib, mm);
        }
    }
    m_impl->axisCache.emplace(std::move(key), out);
    return out;
}

// ---------------------------------------------------------------------------------------------
// Colour glyphs (COLR/CPAL layers + embedded bitmaps) -- §4.2
// ---------------------------------------------------------------------------------------------
std::optional<TextShaper::ColorGlyphTile> TextShaper::colorGlyphTile(const ShapedGlyph& g) {
    FT_Face ft = m_impl->open(g.face);
    if (!ft || !FT_HAS_COLOR(ft)) return std::nullopt;
    const auto sized = m_impl->sizeFace(ft, g.sizePx);
    if (FT_Load_Glyph(ft, g.glyphId, FT_LOAD_COLOR | FT_LOAD_RENDER) != 0) return std::nullopt;
    FT_GlyphSlot slot = ft->glyph;
    if (slot->bitmap.pixel_mode != FT_PIXEL_MODE_BGRA || slot->bitmap.width == 0 ||
        slot->bitmap.rows == 0)
        return std::nullopt;

    const std::uint32_t w = slot->bitmap.width;
    const std::uint32_t h = slot->bitmap.rows;
    ColorGlyphTile tile;
    tile.rgba = common::Image(w, h);
    // FreeType BGRA bitmaps are premultiplied; convert to the project's straight-alpha RGBA.
    for (std::uint32_t y = 0; y < h; ++y) {
        const unsigned char* row =
            slot->bitmap.buffer + static_cast<std::ptrdiff_t>(y) * slot->bitmap.pitch;
        for (std::uint32_t x = 0; x < w; ++x) {
            const unsigned char bb = row[x * 4 + 0];
            const unsigned char gg = row[x * 4 + 1];
            const unsigned char rr = row[x * 4 + 2];
            const unsigned char aa = row[x * 4 + 3];
            auto un = [&](unsigned char ch) -> std::uint8_t {
                if (aa == 0) return 0;
                const int v = (ch * 255 + aa / 2) / aa;  // un-premultiply
                return static_cast<std::uint8_t>(std::min(255, v));
            };
            const std::size_t p = (static_cast<std::size_t>(y) * w + x) * 4;
            tile.rgba.rgba[p + 0] = un(rr);
            tile.rgba.rgba[p + 1] = un(gg);
            tile.rgba.rgba[p + 2] = un(bb);
            tile.rgba.rgba[p + 3] = aa;
        }
    }
    // The strike renders at `sized.px` px/em; we want it at g.sizePx, so each tile pixel covers
    // (g.sizePx / sized.px) layer units. bitmap_left/top are in strike px from the pen origin.
    const float scale = sized.scale;  // == g.sizePx / sized.px
    tile.pixelScale = sized.px > 0.0f ? sized.px / g.sizePx : 1.0f;  // tile px per layer unit
    tile.origin = {g.pen.x + slot->bitmap_left * scale, g.pen.y - slot->bitmap_top * scale};
    return tile;
}

}  // namespace mosaic::core::text
