#include "io/options_schema.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

// OptionsSchema (docs/export-system-plan.md §2.1, §6.5): the typed, data-only description the
// Export modal's backend-settings panel is RENDERED FROM. The tests below use a synthetic schema
// modelled on the hardest real ones in Appendix A -- libjxl (a bool that disables a real knob),
// libtiff (a per-codec quality that only applies to one compression choice) and libjpeg (an enum
// whose value the loss diff reads) -- because those are the shapes the vocabulary has to survive.
using namespace mosaic::io;

namespace {

// Deliberately awkward: an out-of-range default is NOT used (validateSchema would flag it), but
// every other trap is here -- a dependent knob, a doubly-dependent knob, a negated condition,
// a forward reference, a non-default group.
OptionsSchema hardSchema() {
    OptionsSchema s;
    s.groups.push_back(OptionGroup{"advanced", "Advanced", true});

    OptionDesc lossless;
    lossless.key = "lossless";
    lossless.label = "Lossless";
    lossless.type = OptionType::Bool;
    lossless.defaultValue = boolValue(false);
    s.options.push_back(lossless);

    // Forward-references "compression", declared after it: panel order and dependency order
    // are independent.
    OptionDesc distance;
    distance.key = "distance";
    distance.label = "Distance";
    distance.type = OptionType::Real;
    distance.defaultValue = realValue(1.0);
    distance.min = 0.0;
    distance.max = 25.0;
    distance.step = 0.1;
    distance.decimals = 1;
    distance.visibleWhen.push_back(OptionCondition{"lossless", {"false"}, false});
    distance.visibleWhen.push_back(OptionCondition{"compression", {"none"}, /*negate=*/true});
    s.options.push_back(distance);

    OptionDesc effort;
    effort.key = "effort";
    effort.label = "Effort";
    effort.type = OptionType::Int;
    effort.defaultValue = intValue(7);
    effort.min = 1.0;
    effort.max = 9.0;
    effort.step = 1.0;
    effort.group = "advanced";
    s.options.push_back(effort);

    OptionDesc compression;
    compression.key = "compression";
    compression.label = "Compression";
    compression.type = OptionType::Enum;
    compression.defaultValue = textValue("zstd");
    compression.choices = {{"none", "None", ""}, {"lzw", "LZW", ""}, {"zstd", "Zstandard", ""}};
    s.options.push_back(compression);

    OptionDesc zstdLevel;
    zstdLevel.key = "zstd_level";
    zstdLevel.label = "Zstandard level";
    zstdLevel.type = OptionType::Int;
    zstdLevel.defaultValue = intValue(9);
    zstdLevel.min = 1.0;
    zstdLevel.max = 22.0;
    zstdLevel.step = 1.0;
    zstdLevel.group = "advanced";
    zstdLevel.visibleWhen.push_back(OptionCondition{"compression", {"zstd"}, false});
    s.options.push_back(zstdLevel);

    OptionDesc comment;
    comment.key = "comment";
    comment.label = "Comment";
    comment.type = OptionType::Text;
    comment.defaultValue = textValue("");
    comment.group = "advanced";
    s.options.push_back(comment);

    return s;
}

} // namespace

TEST_CASE("a hand-written schema modelled on the real encoders validates") {
    const std::vector<std::string> problems = validateSchema(hardSchema());
    for (const std::string& p : problems)
        MESSAGE(p);
    CHECK(problems.empty());
}

TEST_CASE("defaults() answers every declared key, and only those") {
    const OptionsSchema s = hardSchema();
    const OptionValues v = s.defaults();
    CHECK(v.size() == 6);
    CHECK_FALSE(v.boolean("lossless", true));
    CHECK(v.number("distance", -1.0) == doctest::Approx(1.0));
    CHECK(v.integer("effort", -1) == 7);
    CHECK(v.text("compression", "?") == "zstd");
    CHECK(v.integer("zstd_level", -1) == 9);
    CHECK(v.text("comment", "?").empty());
    CHECK_FALSE(v.has("quality")); // never declared, never invented
}

TEST_CASE("coerce clamps, snaps, retypes and prunes") {
    const OptionsSchema s = hardSchema();

    SUBCASE("out-of-range numbers are clamped to the declared range") {
        OptionValues v;
        v.set("effort", intValue(99));
        v.set("distance", realValue(-4.0));
        s.coerce(v);
        CHECK(v.integer("effort") == 9);
        CHECK(v.number("distance") == doctest::Approx(0.0));

        v.set("effort", intValue(-3));
        v.set("distance", realValue(1000.0));
        s.coerce(v);
        CHECK(v.integer("effort") == 1);
        CHECK(v.number("distance") == doctest::Approx(25.0));
    }

    SUBCASE("a value of the wrong type falls back to the default rather than being reinterpreted") {
        OptionValues v;
        v.set("effort", textValue("nine"));   // a string where an int belongs
        v.set("lossless", textValue("yes"));  // a string where a bool belongs
        s.coerce(v);
        CHECK(v.integer("effort") == 7);
        CHECK_FALSE(v.boolean("lossless"));
    }

    SUBCASE("an enum id the schema does not offer snaps back to the default") {
        OptionValues v;
        v.set("compression", textValue("brotli"));
        s.coerce(v);
        CHECK(v.text("compression") == "zstd");

        v.set("compression", textValue("lzw")); // a real choice survives untouched
        s.coerce(v);
        CHECK(v.text("compression") == "lzw");
    }

    SUBCASE("keys the schema does not declare are dropped") {
        OptionValues v = s.defaults();
        v.set("subsampling", textValue("4:2:0")); // a preset aimed at another format
        v.set("quality", intValue(80));
        s.coerce(v);
        CHECK_FALSE(v.has("subsampling"));
        CHECK_FALSE(v.has("quality"));
        CHECK(v.size() == 6);
    }

    SUBCASE("missing keys are filled, so encode() can read without checking") {
        OptionValues v;
        v.set("effort", intValue(3));
        s.coerce(v);
        CHECK(v.size() == 6);
        CHECK(v.integer("effort") == 3);
        CHECK(v.text("compression") == "zstd");
    }

    SUBCASE("coerce is idempotent") {
        OptionValues v;
        v.set("effort", intValue(99));
        v.set("compression", textValue("brotli"));
        s.coerce(v);
        const OptionValues once = v;
        s.coerce(v);
        CHECK(v == once);
    }

    SUBCASE("an int is snapped onto its step grid") {
        OptionsSchema stepped;
        OptionDesc d;
        d.key = "palette";
        d.label = "Palette size";
        d.type = OptionType::Int;
        d.defaultValue = intValue(256);
        d.min = 2.0;
        d.max = 256.0;
        d.step = 2.0;
        stepped.options.push_back(d);

        OptionValues v;
        v.set("palette", intValue(17));
        stepped.coerce(v);
        CHECK(v.integer("palette") == 18); // 2 + 8*2, the nearest point on the grid
    }
}

TEST_CASE("dependent visibility is evaluated from the current values") {
    const OptionsSchema s = hardSchema();
    OptionValues v = s.defaults();

    // lossless=false, compression=zstd: the lossy knob and the zstd knob are both live.
    CHECK(s.visible("distance", v));
    CHECK(s.visible("zstd_level", v));
    CHECK(s.visible("effort", v));   // no conditions at all
    CHECK(s.visible("comment", v));

    SUBCASE("a bool condition") {
        v.set("lossless", boolValue(true));
        CHECK_FALSE(s.visible("distance", v)); // a distance means nothing when lossless
        CHECK(s.visible("zstd_level", v));     // unrelated, untouched
    }

    SUBCASE("an enum condition") {
        v.set("compression", textValue("lzw"));
        CHECK_FALSE(s.visible("zstd_level", v));
        CHECK(s.visible("distance", v)); // "not none" still holds
    }

    SUBCASE("a negated condition") {
        v.set("compression", textValue("none"));
        CHECK_FALSE(s.visible("distance", v));   // negate: hidden BECAUSE it matched
        CHECK_FALSE(s.visible("zstd_level", v));
    }

    SUBCASE("conditions AND together") {
        v.set("lossless", boolValue(true));
        v.set("compression", textValue("lzw"));
        CHECK_FALSE(s.visible("distance", v)); // one failing condition is enough
    }

    SUBCASE("hiding does not delete: the value is still there for encode() to read") {
        v.set("lossless", boolValue(true));
        s.coerce(v);
        CHECK_FALSE(s.visible("distance", v));
        CHECK(v.has("distance"));
        CHECK(v.number("distance") == doctest::Approx(1.0));
    }

    CHECK_FALSE(s.visible("nonexistent", v)); // an unknown key is never visible
}

TEST_CASE("validateSchema names each class of mistake") {
    const auto fails = [](const OptionsSchema& s, const char* fragment) {
        const std::vector<std::string> problems = validateSchema(s);
        REQUIRE_FALSE(problems.empty());
        bool found = false;
        for (const std::string& p : problems)
            if (p.find(fragment) != std::string::npos)
                found = true;
        if (!found)
            for (const std::string& p : problems)
                MESSAGE(p);
        CHECK(found);
    };

    SUBCASE("duplicate keys") {
        OptionsSchema s;
        OptionDesc a;
        a.key = "q";
        a.label = "Quality";
        a.type = OptionType::Bool;
        a.defaultValue = boolValue(false);
        s.options.push_back(a);
        s.options.push_back(a);
        fails(s, "duplicate key");
    }

    SUBCASE("an enum default that is not a choice") {
        OptionsSchema s;
        OptionDesc a;
        a.key = "c";
        a.label = "Compression";
        a.type = OptionType::Enum;
        a.choices = {{"lzw", "LZW", ""}};
        a.defaultValue = textValue("zstd");
        s.options.push_back(a);
        fails(s, "not one of the choice ids");
    }

    SUBCASE("a numeric default outside the range") {
        OptionsSchema s;
        OptionDesc a;
        a.key = "n";
        a.label = "N";
        a.type = OptionType::Int;
        a.defaultValue = intValue(50);
        a.min = 0.0;
        a.max = 9.0;
        s.options.push_back(a);
        fails(s, "outside [min, max]");
    }

    SUBCASE("a default of the wrong type") {
        OptionsSchema s;
        OptionDesc a;
        a.key = "n";
        a.label = "N";
        a.type = OptionType::Int;
        a.defaultValue = textValue("50");
        s.options.push_back(a);
        fails(s, "non-int default");
    }

    SUBCASE("a condition on an option that does not exist") {
        OptionsSchema s;
        OptionDesc a;
        a.key = "n";
        a.label = "N";
        a.type = OptionType::Bool;
        a.defaultValue = boolValue(false);
        a.visibleWhen.push_back(OptionCondition{"ghost", {"true"}, false});
        s.options.push_back(a);
        fails(s, "does not declare");
    }

    SUBCASE("a condition wanting an enum id that does not exist") {
        OptionsSchema s = hardSchema();
        for (OptionDesc& d : s.options)
            if (d.key == "zstd_level")
                d.visibleWhen = {OptionCondition{"compression", {"brotli"}, false}};
        fails(s, "enum id that does not exist");
    }

    SUBCASE("a condition on a Real source") {
        OptionsSchema s = hardSchema();
        for (OptionDesc& d : s.options)
            if (d.key == "effort")
                d.visibleWhen = {OptionCondition{"distance", {"1"}, false}};
        fails(s, "Real option");
    }

    SUBCASE("an option in an undeclared group") {
        OptionsSchema s;
        OptionDesc a;
        a.key = "n";
        a.label = "N";
        a.type = OptionType::Bool;
        a.defaultValue = boolValue(false);
        a.group = "expert";
        s.options.push_back(a);
        fails(s, "group the schema does not declare");
    }

    SUBCASE("an enum with no choices") {
        OptionsSchema s;
        OptionDesc a;
        a.key = "c";
        a.label = "C";
        a.type = OptionType::Enum;
        a.defaultValue = textValue("x");
        s.options.push_back(a);
        fails(s, "no choices");
    }

    SUBCASE("a self-referential condition") {
        OptionsSchema s;
        OptionDesc a;
        a.key = "n";
        a.label = "N";
        a.type = OptionType::Bool;
        a.defaultValue = boolValue(false);
        a.visibleWhen.push_back(OptionCondition{"n", {"true"}, false});
        s.options.push_back(a);
        fails(s, "on itself");
    }
}

TEST_CASE("values read across the numeric arms but never across a real type mismatch") {
    OptionValues v;
    v.set("i", intValue(7));
    v.set("d", realValue(2.5));
    v.set("b", boolValue(true));
    v.set("s", textValue("lzw"));

    CHECK(v.number("i", -1.0) == doctest::Approx(7.0)); // an int read as a double
    CHECK(v.integer("d", -1) == 3);                     // a double read as an int, rounded
    CHECK(v.boolean("b"));
    CHECK(v.text("s") == "lzw");

    // A genuine mismatch yields the fallback -- never a guess.
    CHECK(v.integer("s", -1) == -1);
    CHECK(v.text("i", "fallback") == "fallback");
    CHECK_FALSE(v.boolean("s", false));

    // A missing key yields the fallback too.
    CHECK(v.integer("absent", 42) == 42);
    CHECK(v.find("absent") == nullptr);
    CHECK(v.find("i") != nullptr);
}

TEST_CASE("the canonical string form is stable and locale-independent") {
    // These are what an OptionCondition compares against and what a preset would store.
    CHECK(optionValueToString(boolValue(true)) == "true");
    CHECK(optionValueToString(boolValue(false)) == "false");
    CHECK(optionValueToString(intValue(-9)) == "-9");
    CHECK(optionValueToString(realValue(2.5)) == "2.5"); // never "2,5"
    CHECK(optionValueToString(realValue(3.0)) == "3");
    CHECK(optionValueToString(textValue("4:2:0")) == "4:2:0");

    OptionValues v;
    v.set("b", boolValue(false));
    CHECK(v.asString("b") == "false");
    CHECK(v.asString("missing").empty());
}

TEST_CASE("OptionValues is ordered, comparable and erasable") {
    OptionValues a;
    a.set("z", intValue(1));
    a.set("a", intValue(2));
    OptionValues b;
    b.set("a", intValue(2));
    b.set("z", intValue(1));
    CHECK(a == b); // insertion order must not matter

    // Iteration is sorted, so any golden or preset serialization is deterministic.
    std::string keys;
    for (const auto& entry : a.all())
        keys += entry.first;
    CHECK(keys == "az");

    a.set("z", intValue(5));
    CHECK_FALSE(a == b);
    a.erase("z");
    CHECK(a.size() == 1);
    CHECK_FALSE(a.has("z"));
    a.clear();
    CHECK(a.empty());
}
