#include "core/text/text_model.hpp"

#include <algorithm>

// Run-list invariant machinery for the text model (docs/type-tool.md §3.1). The whole point is
// that every consumer downstream (shaping, the caret, the panel, SVG export) can assume `runs`
// tiles [0, utf8.size()) with no gaps, overlaps, or empty runs -- so these helpers are the only
// code allowed to rebuild the list, and they always restore that invariant.
namespace mosaic::core::text {

std::size_t paragraphCount(const std::string& utf8) {
    // Paragraphs are '\n'-delimited; N newlines => N+1 paragraphs. "" is one (empty) paragraph.
    return static_cast<std::size_t>(std::count(utf8.begin(), utf8.end(), '\n')) + 1;
}

bool isValid(const TextBlock& block) {
    if (block.paragraphs.size() != paragraphCount(block.utf8)) return false;

    const std::size_t n = block.utf8.size();
    if (block.runs.empty()) return n == 0;  // empty text <=> empty run list

    if (block.runs.front().begin != 0) return false;
    if (block.runs.back().end != n) return false;
    std::size_t cursor = 0;
    for (const StyleRun& r : block.runs) {
        if (r.begin != cursor) return false;  // gap or overlap or out-of-order
        if (r.end <= r.begin) return false;    // empty / inverted run
        cursor = r.end;
    }
    return cursor == n;
}

void normalize(TextBlock& block) {
    const std::size_t n = block.utf8.size();

    // 1) Paragraphs parallel the '\n' splits: pad with a copy of the last (or a default), or trim.
    const std::size_t want = paragraphCount(block.utf8);
    if (block.paragraphs.size() < want) {
        const Paragraph pad = block.paragraphs.empty() ? Paragraph{} : block.paragraphs.back();
        block.paragraphs.resize(want, pad);
    } else if (block.paragraphs.size() > want) {
        block.paragraphs.resize(want);
    }

    // 2) Runs. Empty text => no runs.
    if (n == 0) {
        block.runs.clear();
        return;
    }

    // Drop runs that fell outside the text or collapsed, clamp to [0,n], sort by start.
    std::vector<StyleRun> in;
    in.reserve(block.runs.size());
    for (StyleRun r : block.runs) {
        r.begin = std::min(r.begin, n);
        r.end = std::min(r.end, n);
        if (r.end > r.begin) in.push_back(std::move(r));
    }
    std::stable_sort(in.begin(), in.end(),
                     [](const StyleRun& a, const StyleRun& b) { return a.begin < b.begin; });

    // Rebuild a tiling: walk left to right, filling gaps with the previous run's style (or a
    // default before the first), and dropping any overlap (the earlier run wins up to its end).
    std::vector<StyleRun> out;
    std::size_t cursor = 0;
    CharStyle prev{};  // style used to fill a leading/closing gap
    for (const StyleRun& r : in) {
        const std::size_t start = std::max(r.begin, cursor);
        if (start > cursor) {  // gap before this run -> fill it
            out.push_back(StyleRun{cursor, start, prev});
        }
        if (r.end > start) {
            out.push_back(StyleRun{start, r.end, r.style});
            cursor = r.end;
            prev = r.style;
        }
    }
    if (cursor < n) out.push_back(StyleRun{cursor, n, prev});  // trailing gap (or no runs at all)

    // 3) Merge adjacent runs with identical style (so edits don't fragment the list forever).
    std::vector<StyleRun> merged;
    merged.reserve(out.size());
    for (StyleRun& r : out) {
        if (!merged.empty() && merged.back().end == r.begin && merged.back().style == r.style) {
            merged.back().end = r.end;
        } else {
            merged.push_back(std::move(r));
        }
    }
    block.runs = std::move(merged);
}

CharStyle styleAt(const TextBlock& block, std::size_t pos) {
    if (block.runs.empty()) return block.emptyStyle;  // empty block: the pending caret/first-char style
    // Clamp end-of-text into the last run (the style a caret at EOL/insertion inherits).
    if (pos >= block.runs.back().end) return block.runs.back().style;
    for (const StyleRun& r : block.runs) {
        if (pos < r.end) return r.style;  // runs are sorted & contiguous, so first hit wins
    }
    return block.runs.back().style;
}

namespace {
// Shared core of setStyleRange/mutateStyleRange: tile a fresh run list, applying `apply` (which
// produces the new style for the covered span from the old style at that span) over [begin,end).
template <class Fn>
void editStyleRange(TextBlock& block, std::size_t begin, std::size_t end, Fn&& apply) {
    const std::size_t n = block.utf8.size();
    begin = std::min(begin, n);
    end = std::min(end, n);
    if (begin >= end) return;  // empty / inverted -> no-op
    normalize(block);          // start from a clean tiling so the split logic is simple

    std::vector<StyleRun> out;
    out.reserve(block.runs.size() + 2);
    for (const StyleRun& r : block.runs) {
        // Split r against [begin,end) into up to three pieces: before / inside / after.
        const std::size_t b = r.begin, e = r.end;
        const std::size_t lo = std::max(b, begin);
        const std::size_t hi = std::min(e, end);
        if (lo >= hi) {  // no overlap -> r passes through untouched
            out.push_back(r);
            continue;
        }
        if (b < lo) out.push_back(StyleRun{b, lo, r.style});       // unchanged head
        CharStyle edited = apply(r.style);                         // covered middle
        out.push_back(StyleRun{lo, hi, std::move(edited)});
        if (hi < e) out.push_back(StyleRun{hi, e, r.style});       // unchanged tail
    }
    block.runs = std::move(out);
    normalize(block);  // merge equal neighbours produced by the edit
}
}  // namespace

void setStyleRange(TextBlock& block, std::size_t begin, std::size_t end, const CharStyle& style) {
    editStyleRange(block, begin, end, [&](const CharStyle&) { return style; });
}

void mutateStyleRange(TextBlock& block, std::size_t begin, std::size_t end,
                      const std::function<void(CharStyle&)>& mutate) {
    editStyleRange(block, begin, end, [&](const CharStyle& old) {
        CharStyle s = old;
        mutate(s);
        return s;
    });
}

CommonStyle commonStyle(const TextBlock& block, std::size_t lo, std::size_t hi) {
    const std::size_t n = block.utf8.size();
    lo = std::min(lo, n);
    hi = std::min(hi, n);
    if (lo > hi) std::swap(lo, hi);

    CommonStyle result;
    if (block.runs.empty()) {       // empty block: the pending caret/first-char style, all-agree
        result.style = block.emptyStyle;
        return result;
    }
    if (lo == hi) {                 // caret: the single inherited style, all-agree
        result.style = styleAt(block, lo);
        return result;
    }
    bool first = true;
    for (const StyleRun& r : block.runs) {
        if (std::max(r.begin, lo) >= std::min(r.end, hi)) continue;  // run doesn't touch [lo,hi)
        if (first) {                 // seed `style` from the first touched run, then narrow `agree`
            result.style = r.style;
            first = false;
            continue;
        }
        const CharStyle& s = r.style;
        const CharStyle& base = result.style;
        StyleAgreement& ag = result.agree;
        ag.family &= (s.font.family == base.font.family);
        ag.weight &= (s.font.weight == base.font.weight);
        ag.italic &= (s.font.italic == base.font.italic);
        ag.widthAxis &= (s.font.widthAxis == base.font.widthAxis);
        ag.variations &= (s.font.variations == base.font.variations);
        ag.sizePx &= (s.sizePx == base.sizePx);
        ag.paint &= (s.paint == base.paint);
        ag.underline &= (s.underline == base.underline);
        ag.strikethrough &= (s.strikethrough == base.strikethrough);
        ag.tracking &= (s.tracking == base.tracking);
        ag.baselineShift &= (s.baselineShift == base.baselineShift);
        ag.features &= (s.features == base.features);
        ag.kerning &= (s.kerning == base.kerning);
    }
    return result;
}

std::size_t paragraphIndexAt(const std::string& utf8, std::size_t pos) {
    pos = std::min(pos, utf8.size());
    return static_cast<std::size_t>(
        std::count(utf8.begin(), utf8.begin() + static_cast<std::ptrdiff_t>(pos), '\n'));
}

namespace {
// The inclusive paragraph index range [pLo, pHi] a byte range [lo, hi) touches: the paragraphs of
// its first and last selected byte (a caret touches its one paragraph). Both clamped to the list.
std::pair<std::size_t, std::size_t> touchedParagraphs(const TextBlock& block, std::size_t lo,
                                                      std::size_t hi) {
    const std::size_t last = block.paragraphs.empty() ? 0 : block.paragraphs.size() - 1;
    const std::size_t pLo = std::min(paragraphIndexAt(block.utf8, lo), last);
    const std::size_t pHi =
        (hi > lo) ? std::min(paragraphIndexAt(block.utf8, hi - 1), last) : pLo;
    return {pLo, pHi};
}
}  // namespace

CommonParagraph commonParagraph(const TextBlock& block, std::size_t lo, std::size_t hi) {
    const std::size_t n = block.utf8.size();
    lo = std::min(lo, n);
    hi = std::min(hi, n);
    if (lo > hi) std::swap(lo, hi);

    CommonParagraph result;
    if (block.paragraphs.empty()) return result;  // defensive (always >= 1 after normalize)
    const auto [pLo, pHi] = touchedParagraphs(block, lo, hi);
    result.para = block.paragraphs[pLo];
    for (std::size_t i = pLo + 1; i <= pHi; ++i) {
        const Paragraph& p = block.paragraphs[i];
        const Paragraph& base = result.para;
        ParagraphAgreement& ag = result.agree;
        ag.align &= (p.align == base.align);
        ag.leading &= (p.leading == base.leading);
        ag.leadingAbsolute &= (p.leadingAbsolute == base.leadingAbsolute);
        ag.spaceBefore &= (p.spaceBefore == base.spaceBefore);
        ag.spaceAfter &= (p.spaceAfter == base.spaceAfter);
        ag.indentFirst &= (p.indentFirst == base.indentFirst);
        ag.indentLeft &= (p.indentLeft == base.indentLeft);
        ag.indentRight &= (p.indentRight == base.indentRight);
        ag.direction &= (p.direction == base.direction);
        ag.language &= (p.language == base.language);
        ag.hyphenate &= (p.hyphenate == base.hyphenate);
    }
    return result;
}

void mutateParagraphRange(TextBlock& block, std::size_t lo, std::size_t hi,
                          const std::function<void(Paragraph&)>& mutate) {
    const std::size_t n = block.utf8.size();
    lo = std::min(lo, n);
    hi = std::min(hi, n);
    if (lo > hi) std::swap(lo, hi);
    if (block.paragraphs.empty()) return;
    const auto [pLo, pHi] = touchedParagraphs(block, lo, hi);
    for (std::size_t i = pLo; i <= pHi; ++i) mutate(block.paragraphs[i]);
}

TextBlock makeBlock(std::string utf8, CharStyle style, TextFrame frame) {
    TextBlock block;
    block.utf8 = std::move(utf8);
    block.frame = frame;
    block.emptyStyle = style;  // the caret/first-char style while empty (copy; `style` may move below)
    if (!block.utf8.empty()) block.runs.push_back(StyleRun{0, block.utf8.size(), std::move(style)});
    normalize(block);  // builds the parallel paragraph list (and validates the single run)
    return block;
}

void scaleTextSizes(TextBlock& block, float factor, float minSizePx, float maxSizePx) {
    if (!(factor > 0.0f)) return;
    const auto scale = [&](CharStyle& s) {
        s.sizePx = std::clamp(s.sizePx * factor, minSizePx, maxSizePx);
        s.baselineShift *= factor;  // px-absolute, so it tracks the size (tracking is em-relative)
    };
    for (StyleRun& r : block.runs) scale(r.style);
    scale(block.emptyStyle);  // the pending caret/first-char style scales too
    // Sizes/shifts only -- run boundaries, text and paragraphs are unchanged, so the invariant holds
    // (no normalize needed; adjacent runs that were unequal stay unequal under a uniform scale).
}

std::size_t replaceText(TextBlock& block, std::size_t begin, std::size_t end,
                        std::string_view insert, std::optional<CharStyle> style) {
    const std::size_t n = block.utf8.size();
    begin = std::min(begin, n);
    end = std::min(end, n);
    if (begin > end) std::swap(begin, end);

    // The inserted span's style: an explicit override, else the style at the caret (typing inherits
    // the run to its left, which styleAt() returns at a boundary). Read before the runs are rebuilt.
    const CharStyle insStyle = style ? *style : styleAt(block, begin);

    // --- Paragraph splice (uses the OLD utf8 offsets, before the text is mutated). Paragraphs
    // [pBegin+1, pEnd] merge into pBegin; then `addPara` copies of paragraph pBegin are inserted
    // after it, so newlines in `insert` split it and a deleted newline merges its two paragraphs.
    const auto countNl = [](std::string_view s) {
        return static_cast<std::size_t>(std::count(s.begin(), s.end(), '\n'));
    };
    const std::size_t pBegin = countNl(std::string_view{block.utf8}.substr(0, begin));
    const std::size_t pEnd = countNl(std::string_view{block.utf8}.substr(0, end));
    const std::size_t addPara = countNl(insert);
    if (!block.paragraphs.empty()) {
        auto& ps = block.paragraphs;
        const Paragraph keep = ps[std::min(pBegin, ps.size() - 1)];
        const auto from = ps.begin() + static_cast<std::ptrdiff_t>(std::min(pBegin + 1, ps.size()));
        const auto to = ps.begin() + static_cast<std::ptrdiff_t>(std::min(pEnd + 1, ps.size()));
        if (from < to) ps.erase(from, to);
        if (addPara > 0)
            ps.insert(ps.begin() + static_cast<std::ptrdiff_t>(std::min(pBegin + 1, ps.size())),
                      addPara, keep);
    }

    // --- Rebuild runs as clipped segments: prefix [0,begin) ++ inserted ++ suffix [end,n) shifted
    // by the length delta. normalize() then fills any gap, merges equal neighbours, and rebuilds
    // paragraphs to the new count (the splice above set their styles; this only fixes the count).
    normalize(block);  // clean tiling to clip against
    const std::size_t insLen = insert.size();
    const std::ptrdiff_t delta =
        static_cast<std::ptrdiff_t>(begin + insLen) - static_cast<std::ptrdiff_t>(end);
    std::vector<StyleRun> out;
    out.reserve(block.runs.size() + 2);
    for (const StyleRun& r : block.runs) {  // prefix: r clipped to [0, begin)
        const std::size_t pb = std::min(r.begin, begin);
        const std::size_t pe = std::min(r.end, begin);
        if (pe > pb) out.push_back(StyleRun{pb, pe, r.style});
    }
    if (insLen > 0) out.push_back(StyleRun{begin, begin + insLen, insStyle});
    for (const StyleRun& r : block.runs) {  // suffix: r clipped to [end, n), shifted by delta
        const std::size_t sb = std::max(r.begin, end);
        const std::size_t se = std::max(r.end, end);
        if (se > sb)
            out.push_back(StyleRun{static_cast<std::size_t>(static_cast<std::ptrdiff_t>(sb) + delta),
                                   static_cast<std::size_t>(static_cast<std::ptrdiff_t>(se) + delta),
                                   r.style});
    }

    block.utf8.replace(begin, end - begin, insert.data(), insert.size());
    block.runs = std::move(out);
    normalize(block);
    return begin + insLen;
}

// ---------------------------------------------------------------------------------------------
// OpenType feature toggles (R4 §3.4)
// ---------------------------------------------------------------------------------------------
bool featureEnabled(const std::vector<std::string>& features, const std::string& tag,
                    bool defaultOn) {
    // Later entries win, mirroring how HarfBuzz applies the feature list in order.
    bool on = defaultOn;
    const std::string off = "-" + tag;
    for (const std::string& f : features) {
        if (f == tag)
            on = true;
        else if (f == off)
            on = false;
    }
    return on;
}

void setFeatureEnabled(std::vector<std::string>& features, const std::string& tag, bool defaultOn,
                       bool on) {
    const std::string off = "-" + tag;
    std::erase_if(features, [&](const std::string& f) { return f == tag || f == off; });
    if (on != defaultOn) features.push_back(on ? tag : off);
}

}  // namespace mosaic::core::text
