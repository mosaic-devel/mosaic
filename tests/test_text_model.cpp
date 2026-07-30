// Run-list & paragraph invariant tests for the text model (docs/type-tool.md §3.1). The covering/
// non-overlapping run invariant is the model's most error-prone bit, so it is exercised hard here
// -- no font machinery is involved, so these are deterministic on every machine.
#include <doctest/doctest.h>

#include "core/text/text_model.hpp"

using namespace mosaic::core::text;
using mosaic::common::ColorF;

namespace {

CharStyle styled(ColorF c, float size = 24.0f) {
    CharStyle s;
    s.setSolidFill(c);
    s.sizePx = size;
    return s;
}

constexpr ColorF kRed{1, 0, 0, 1};
constexpr ColorF kGreen{0, 1, 0, 1};
constexpr ColorF kBlue{0, 0, 1, 1};

}  // namespace

TEST_CASE("paragraphCount counts '\\n'-delimited paragraphs") {
    CHECK(paragraphCount("") == 1);            // the empty string is one (empty) paragraph
    CHECK(paragraphCount("hello") == 1);
    CHECK(paragraphCount("a\nb") == 2);
    CHECK(paragraphCount("a\nb\nc") == 3);
    CHECK(paragraphCount("trailing\n") == 2);  // the bit after the last '\n' is an empty paragraph
    CHECK(paragraphCount("\n") == 2);
}

TEST_CASE("makeBlock produces a valid single-run block") {
    TextBlock b = makeBlock("hello world", styled(kRed));
    CHECK(isValid(b));
    REQUIRE(b.runs.size() == 1);
    CHECK(b.runs[0].begin == 0);
    CHECK(b.runs[0].end == 11);
    CHECK(b.runs[0].style.solidFill() == kRed);
    CHECK(b.paragraphs.size() == 1);
}

TEST_CASE("a block defaults to horizontal writing mode; the enum extends operator==") {
    // Vertical writing-mode model (docs/type-vertical-writing-mode.md commit A): the fields are inert
    // (nothing shapes/renders them yet) but must default to the existing horizontal flow so every
    // current block is unchanged, and the defaulted operator== must see them (else a mode change would
    // not register as an edit).
    const TextBlock b = makeBlock("hello", styled(kRed));
    CHECK(b.writingMode == WritingMode::HorizontalTB);
    CHECK(b.orientation == TextOrientation::Mixed);

    TextBlock v = b;
    CHECK(v == b);  // an untouched copy is equal
    v.writingMode = WritingMode::VerticalRL;
    CHECK(v != b);  // a writing-mode change is a real difference (operator== extended)

    TextBlock o = b;
    o.orientation = TextOrientation::Upright;
    CHECK(o != b);  // and so is an orientation change
}

TEST_CASE("makeBlock on empty text has no runs but one paragraph") {
    TextBlock b = makeBlock("");
    CHECK(isValid(b));
    CHECK(b.runs.empty());
    CHECK(b.paragraphs.size() == 1);
    CHECK(b.empty());
}

TEST_CASE("makeBlock builds the parallel paragraph list") {
    TextBlock b = makeBlock("a\nb\nc");
    CHECK(isValid(b));
    CHECK(b.paragraphs.size() == 3);
    REQUIRE(b.runs.size() == 1);  // newlines do not split runs -- only style does
    CHECK(b.runs[0].end == 5);
}

TEST_CASE("isValid rejects gaps, overlaps, empty runs, paragraph mismatch") {
    TextBlock good = makeBlock("abcdef", styled(kRed));
    CHECK(isValid(good));

    TextBlock gap = good;
    gap.runs = {{0, 2, styled(kRed)}, {3, 6, styled(kBlue)}};  // hole at [2,3)
    CHECK_FALSE(isValid(gap));

    TextBlock overlap = good;
    overlap.runs = {{0, 4, styled(kRed)}, {2, 6, styled(kBlue)}};  // overlap [2,4)
    CHECK_FALSE(isValid(overlap));

    TextBlock empties = good;
    empties.runs = {{0, 3, styled(kRed)}, {3, 3, styled(kBlue)}, {3, 6, styled(kGreen)}};
    CHECK_FALSE(isValid(empties));  // zero-length run in the middle

    TextBlock shortEnd = good;
    shortEnd.runs = {{0, 4, styled(kRed)}};  // does not reach utf8.size()
    CHECK_FALSE(isValid(shortEnd));

    TextBlock badParas = good;
    badParas.paragraphs.clear();  // size 0 != paragraphCount 1
    CHECK_FALSE(isValid(badParas));
}

TEST_CASE("normalize repairs gaps and merges equal neighbours") {
    TextBlock b;
    b.utf8 = "abcdef";
    b.runs = {{0, 2, styled(kRed)}, {4, 6, styled(kRed)}};  // gap [2,4); same style on both sides
    normalize(b);
    CHECK(isValid(b));
    // The gap fills with the neighbouring (red) style and the three pieces merge into one run.
    REQUIRE(b.runs.size() == 1);
    CHECK(b.runs[0].begin == 0);
    CHECK(b.runs[0].end == 6);
    CHECK(b.runs[0].style.solidFill() == kRed);
}

TEST_CASE("normalize drops overlaps (earlier run wins) and is idempotent") {
    TextBlock b;
    b.utf8 = "abcdef";
    b.runs = {{0, 4, styled(kRed)}, {2, 6, styled(kBlue)}};  // overlap [2,4)
    normalize(b);
    CHECK(isValid(b));
    REQUIRE(b.runs.size() == 2);
    CHECK(b.runs[0].end == 4);                 // red keeps [0,4)
    CHECK(b.runs[1].begin == 4);               // blue resumes at 4
    CHECK(b.runs[1].style.solidFill() == kBlue);

    TextBlock again = b;
    normalize(again);
    CHECK(again == b);  // idempotent
}

TEST_CASE("normalize trims/pads the paragraph list to the '\\n' count") {
    TextBlock b = makeBlock("a\nb\nc");
    b.paragraphs.resize(1);   // under-count
    normalize(b);
    CHECK(b.paragraphs.size() == 3);

    b.paragraphs.resize(9);   // over-count
    normalize(b);
    CHECK(b.paragraphs.size() == 3);
}

TEST_CASE("styleAt returns the run covering a byte, clamping at end") {
    TextBlock b;
    b.utf8 = "abcdef";
    b.runs = {{0, 3, styled(kRed)}, {3, 6, styled(kBlue)}};
    b.paragraphs = {Paragraph{}};
    REQUIRE(isValid(b));
    CHECK(styleAt(b, 0).solidFill() == kRed);
    CHECK(styleAt(b, 2).solidFill() == kRed);
    CHECK(styleAt(b, 3).solidFill() == kBlue);
    CHECK(styleAt(b, 5).solidFill() == kBlue);
    CHECK(styleAt(b, 6).solidFill() == kBlue);    // end-of-text clamps into the last run
    CHECK(styleAt(b, 99).solidFill() == kBlue);
    CHECK(styleAt(makeBlock(""), 0) == CharStyle{});  // empty block -> default style
}

TEST_CASE("an empty block keeps its pending style; typing inherits it") {
    // fixlist #3: makeBlock("", style) used to drop `style` (no run for empty text), so the caret
    // showed the 24px default and the first typed glyph lost the chosen size. emptyStyle fixes both.
    CharStyle st;
    st.sizePx = 80.0f;
    st.setSolidFill(kBlue);
    TextBlock b = makeBlock("", st);
    REQUIRE(b.runs.empty());
    CHECK(b.emptyStyle.sizePx == doctest::Approx(80.0f));
    CHECK(styleAt(b, 0).sizePx == doctest::Approx(80.0f));   // empty -> the pending style
    CHECK(styleAt(b, 0).solidFill() == kBlue);
    // Typing into the empty block inherits the pending style (no explicit override needed).
    replaceText(b, 0, 0, "A");
    REQUIRE(isValid(b));
    CHECK(styleAt(b, 0).sizePx == doctest::Approx(80.0f));
    CHECK(styleAt(b, 0).solidFill() == kBlue);
}

TEST_CASE("scaleTextSizes scales every run + the pending style, clamped, invariant kept") {
    TextBlock b = makeBlock("abcdef", styled(kRed, 20.0f));  // emptyStyle seeded at 20px
    setStyleRange(b, 2, 4, styled(kBlue, 40.0f));            // a second run at 40px
    scaleTextSizes(b, 1.5f);
    CHECK(isValid(b));
    CHECK(styleAt(b, 0).sizePx == doctest::Approx(30.0f));   // 20 * 1.5
    CHECK(styleAt(b, 3).sizePx == doctest::Approx(60.0f));   // 40 * 1.5 (the inner run)
    CHECK(b.emptyStyle.sizePx == doctest::Approx(30.0f));    // the pending caret style scales too
    // A tiny factor floors at the minimum rather than collapsing the text away.
    scaleTextSizes(b, 0.0001f, /*minSizePx=*/8.0f);
    CHECK(styleAt(b, 0).sizePx == doctest::Approx(8.0f));
    scaleTextSizes(b, -2.0f);  // a non-positive factor is a no-op
    CHECK(styleAt(b, 0).sizePx == doctest::Approx(8.0f));
}

TEST_CASE("setStyleRange splits runs at boundaries") {
    TextBlock b = makeBlock("abcdef", styled(kRed));
    setStyleRange(b, 2, 4, styled(kBlue));
    CHECK(isValid(b));
    REQUIRE(b.runs.size() == 3);
    CHECK(b.runs[0].begin == 0);
    CHECK(b.runs[0].end == 2);
    CHECK(b.runs[0].style.solidFill() == kRed);
    CHECK(b.runs[1].begin == 2);
    CHECK(b.runs[1].end == 4);
    CHECK(b.runs[1].style.solidFill() == kBlue);
    CHECK(b.runs[2].begin == 4);
    CHECK(b.runs[2].style.solidFill() == kRed);
}

TEST_CASE("setStyleRange over the whole text collapses back to one run") {
    TextBlock b = makeBlock("abcdef", styled(kRed));
    setStyleRange(b, 2, 4, styled(kBlue));  // fragment into three
    setStyleRange(b, 0, 6, styled(kGreen)); // re-cover everything
    CHECK(isValid(b));
    REQUIRE(b.runs.size() == 1);
    CHECK(b.runs[0].style.solidFill() == kGreen);
}

TEST_CASE("setStyleRange merges when the new style matches a neighbour") {
    TextBlock b = makeBlock("abcdef", styled(kRed));
    setStyleRange(b, 2, 4, styled(kBlue));
    setStyleRange(b, 2, 4, styled(kRed));  // revert the middle -> should merge to one red run
    CHECK(isValid(b));
    REQUIRE(b.runs.size() == 1);
    CHECK(b.runs[0].end == 6);
}

TEST_CASE("setStyleRange clamps and no-ops on empty/inverted ranges") {
    TextBlock b = makeBlock("abcdef", styled(kRed));
    const TextBlock before = b;
    setStyleRange(b, 4, 4, styled(kBlue));   // empty
    CHECK(b == before);
    setStyleRange(b, 5, 2, styled(kBlue));   // inverted
    CHECK(b == before);
    setStyleRange(b, 4, 99, styled(kBlue));  // end clamps to size()
    CHECK(isValid(b));
    CHECK(b.runs.back().end == 6);
    CHECK(b.runs.back().style.solidFill() == kBlue);
}

TEST_CASE("mutateStyleRange changes one property, preserving the rest") {
    CharStyle base = styled(kRed, 24.0f);
    base.underline = true;
    TextBlock b = makeBlock("abcdef", base);
    mutateStyleRange(b, 0, 3, [](CharStyle& s) { s.sizePx = 48.0f; });
    CHECK(isValid(b));
    REQUIRE(b.runs.size() == 2);
    CHECK(b.runs[0].style.sizePx == doctest::Approx(48.0f));
    CHECK(b.runs[0].style.solidFill() == kRed);  // colour untouched
    CHECK(b.runs[0].style.underline);            // other props untouched
    CHECK(b.runs[1].style.sizePx == doctest::Approx(24.0f));
}

// --- S29-c: selection-wide style/paragraph queries (what the context bar / Type panel read) ------

TEST_CASE("commonStyle on a caret / empty block agrees on every field") {
    SUBCASE("empty block returns its emptyStyle, all-agree") {
        TextBlock b = makeBlock("", styled(kRed, 30.0f));
        CommonStyle cs = commonStyle(b, 0, 0);
        CHECK(cs.style.sizePx == doctest::Approx(30.0f));
        CHECK(cs.style.solidFill() == kRed);
        CHECK(cs.agree.sizePx);
        CHECK(cs.agree.paint);
        CHECK(cs.agree.family);
    }
    SUBCASE("caret in mixed text reads the single inherited run, all-agree") {
        TextBlock b = makeBlock("abcdef", styled(kRed, 24.0f));
        mutateStyleRange(b, 3, 6, [](CharStyle& s) { s.sizePx = 48.0f; });
        CommonStyle cs = commonStyle(b, 1, 1);  // caret inside the first (red, 24px) run
        CHECK(cs.style.sizePx == doctest::Approx(24.0f));
        CHECK(cs.agree.sizePx);
        CHECK(cs.agree.paint);
    }
}

TEST_CASE("commonStyle flags only the fields that differ across the selection") {
    TextBlock b = makeBlock("abcdef", styled(kRed, 24.0f));
    // Right half: bigger AND blue, but same family/italic as the left half.
    mutateStyleRange(b, 3, 6, [](CharStyle& s) {
        s.sizePx = 48.0f;
        s.setSolidFill(kBlue);
    });
    CommonStyle cs = commonStyle(b, 0, 6);  // the whole, two-run range
    CHECK_FALSE(cs.agree.sizePx);           // 24 vs 48 -> mixed
    CHECK_FALSE(cs.agree.paint);            // red vs blue -> mixed
    CHECK(cs.agree.family);                 // both "Sans" -> uniform
    CHECK(cs.agree.italic);                 // both upright -> uniform
    CHECK(cs.agree.underline);
    CHECK(cs.style.sizePx == doctest::Approx(24.0f));  // `style` carries the FIRST run's value
    CHECK(cs.style.solidFill() == kRed);
}

TEST_CASE("commonStyle on a sub-range fully inside one run agrees") {
    TextBlock b = makeBlock("abcdef", styled(kRed, 24.0f));
    mutateStyleRange(b, 3, 6, [](CharStyle& s) { s.sizePx = 48.0f; });
    CommonStyle cs = commonStyle(b, 0, 3);  // exactly the first run
    CHECK(cs.agree.sizePx);
    CHECK(cs.style.sizePx == doctest::Approx(24.0f));
}

TEST_CASE("paragraphIndexAt counts newlines before the offset") {
    const std::string s = "a\nbb\nccc";  // paras: [0]"a" [1]"bb" [2]"ccc"
    CHECK(paragraphIndexAt(s, 0) == 0);
    CHECK(paragraphIndexAt(s, 1) == 0);  // at the '\n' -> still paragraph 0
    CHECK(paragraphIndexAt(s, 2) == 1);  // just past it -> paragraph 1
    CHECK(paragraphIndexAt(s, 5) == 2);
    CHECK(paragraphIndexAt(s, 999) == 2);  // clamps to size()
}

TEST_CASE("commonParagraph / mutateParagraphRange span the touched paragraphs") {
    TextBlock b = makeBlock("a\nbb\nccc", styled(kRed));  // 3 paragraphs
    b.paragraphs[0].align = Paragraph::Align::Left;
    b.paragraphs[1].align = Paragraph::Align::Center;
    b.paragraphs[2].align = Paragraph::Align::Right;

    SUBCASE("a caret reads its single paragraph, all-agree") {
        CommonParagraph cp = commonParagraph(b, 3, 3);  // inside "bb"
        CHECK(cp.para.align == Paragraph::Align::Center);
        CHECK(cp.agree.align);
    }
    SUBCASE("a multi-paragraph selection flags align as mixed") {
        CommonParagraph cp = commonParagraph(b, 0, 5);  // paragraphs 0 and 1
        CHECK_FALSE(cp.agree.align);
    }
    SUBCASE("mutateParagraphRange sets every touched paragraph, and only those") {
        mutateParagraphRange(b, 0, 5, [](Paragraph& p) { p.align = Paragraph::Align::Justify; });
        CHECK(b.paragraphs[0].align == Paragraph::Align::Justify);
        CHECK(b.paragraphs[1].align == Paragraph::Align::Justify);
        CHECK(b.paragraphs[2].align == Paragraph::Align::Right);  // untouched
        CHECK(isValid(b));
    }
}

// --- OpenType feature toggles (R4 §3.4) --------------------------------------------------------
TEST_CASE("featureEnabled reads presence against the shaper default") {
    std::vector<std::string> feats;
    // Untouched: defaults hold.
    CHECK(featureEnabled(feats, "liga", true));
    CHECK_FALSE(featureEnabled(feats, "smcp", false));
    // Explicit enable / disable.
    feats = {"smcp"};
    CHECK(featureEnabled(feats, "smcp", false));
    feats = {"-liga"};
    CHECK_FALSE(featureEnabled(feats, "liga", true));
    // Later entries win (HarfBuzz list order).
    feats = {"-liga", "liga"};
    CHECK(featureEnabled(feats, "liga", true));
}

TEST_CASE("setFeatureEnabled records only deviations from the default and normalizes") {
    std::vector<std::string> feats;
    // Turning a default-ON feature on is a no-op (stays empty).
    setFeatureEnabled(feats, "liga", true, true);
    CHECK(feats.empty());
    // Turning it off records "-liga"; back on removes it again.
    setFeatureEnabled(feats, "liga", true, false);
    CHECK(feats == std::vector<std::string>{"-liga"});
    setFeatureEnabled(feats, "liga", true, true);
    CHECK(feats.empty());
    // Default-OFF: on records the tag, off removes it.
    setFeatureEnabled(feats, "smcp", false, true);
    CHECK(feats == std::vector<std::string>{"smcp"});
    setFeatureEnabled(feats, "smcp", false, false);
    CHECK(feats.empty());
    // Normalization drops stale duplicates of both forms.
    feats = {"-liga", "liga", "smcp"};
    setFeatureEnabled(feats, "liga", true, false);
    CHECK(feats == std::vector<std::string>{"smcp", "-liga"});
}

TEST_CASE("kerning mode defaults to Metric and narrows style agreement") {
    CharStyle st;
    CHECK(st.kerning == Kerning::Metric);

    TextBlock b = makeBlock("abcd", st);
    CharStyle optical = st;
    optical.kerning = Kerning::Optical;
    setStyleRange(b, 2, 4, optical);
    const CommonStyle cs = commonStyle(b, 0, 4);
    CHECK_FALSE(cs.agree.kerning);
    CHECK(commonStyle(b, 0, 2).agree.kerning);
}

// --- 3D extrusion model (S30-c, docs/type-tool.md §10.1) ----------------------------------------
TEST_CASE("extrude is nullopt by default and inert in block equality") {
    TextBlock b = makeBlock("hi");
    CHECK_FALSE(b.extrude.has_value());  // every existing block stays flat 2D

    TextBlock c = b;
    CHECK(b == c);
    c.extrude = Extrude{};
    CHECK_FALSE(b == c);  // enabling 3D is a real model change (undo-able through the block funnel)
    b.extrude = Extrude{};
    CHECK(b == c);
}

TEST_CASE("extrude defaults match the §10.1 spec (near-ortho, lit, one soft key light)") {
    const Extrude e;
    CHECK(e.depth == 20.0f);
    CHECK(e.orientation == Quat::identity());
    CHECK(e.perspective == 10.0f);  // near-orthographic default
    CHECK(e.lightingEnabled);
    REQUIRE(e.lights.size() == 1);  // the soft key light: 3D looks good the moment it's enabled
    CHECK(e.lights[0].intensity < 1.0f);
    CHECK(e.bevelFront.size == 0.0f);  // bevel off until asked for
    CHECK(e.runMaterials.empty());     // one material, one draw, until runs override (§10.4)

    // The override map + defaulted equality compose (a per-run material is a model change).
    Extrude gold = e;
    gold.runMaterials[0] = Material{{1.0f, 0.85f, 0.1f, 1.0f}, 1.0f, 0.25f};
    CHECK_FALSE(gold == e);
}
