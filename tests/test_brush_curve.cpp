#include <doctest/doctest.h>

#include "core/brush/curve.hpp"

#include <clocale>
#include <cmath>
#include <string>

using mosaic::core::brush::Curve;
using mosaic::core::brush::CurvePoint;
using mosaic::core::brush::evalLut;
using mosaic::core::brush::kIdentityCurve;

namespace {
constexpr double kEps = 1e-9;

// Switches LC_ALL to the first locale in `names` the host actually has, and puts it back on scope
// exit. `comma` reports whether the locale we landed on writes a comma for the decimal point --
// there is nothing to test if the host ships no such locale, and CI images often ship only C.
class ScopedLocale {
public:
    explicit ScopedLocale(std::initializer_list<const char*> names) {
        const char* current = std::setlocale(LC_ALL, nullptr);
        m_saved = current != nullptr ? current : "C";
        for (const char* n : names) {
            if (std::setlocale(LC_ALL, n) != nullptr) {
                m_applied = true;
                break;
            }
        }
    }
    ~ScopedLocale() { std::setlocale(LC_ALL, m_saved.c_str()); }

    ScopedLocale(const ScopedLocale&) = delete;
    ScopedLocale& operator=(const ScopedLocale&) = delete;

    [[nodiscard]] bool comma() const {
        const std::lconv* lc = std::localeconv();
        return m_applied && lc != nullptr && lc->decimal_point != nullptr &&
               std::string(lc->decimal_point) == ",";
    }

private:
    std::string m_saved;
    bool m_applied = false;
};

// Locales that write "0,5" rather than "0.5". Any one of them exercises the whole bug.
constexpr auto kCommaLocales = {"pl_PL.utf8",     "pl_PL.UTF-8", "de_DE.utf8", "de_DE.UTF-8",
                                "fr_FR.UTF-8",    "fr_FR.utf8",  "es_ES.UTF-8", "ru_RU.UTF-8",
                                "nl_NL.UTF-8",    "it_IT.UTF-8"};
} // namespace

TEST_CASE("brush curve: identity") {
    const Curve c;
    CHECK(c.isIdentity());
    CHECK(c.toString() == std::string(kIdentityCurve));

    // A two-point spline is exactly linear, so the identity is exact at every x, not merely close.
    for (int i = 0; i <= 20; ++i) {
        const double x = i / 20.0;
        CHECK(c.eval(x) == doctest::Approx(x).epsilon(kEps));
    }
}

TEST_CASE("brush curve: round-trips the interchange format byte-exactly") {
    // The %g/6-significant-digit formatting is load-bearing: an imported preset that we re-save must
    // produce the same bytes, or diffing a preset library becomes noise.
    for (const char* s : {"0,0;1,1;", "0,1;0.5,0;1,1;", "0,0.753769;1,1;",
                          "0,1;0.5,0,is_corner;1,1;", "0,0;0.25,0.1;0.75,0.9;1,1;"}) {
        CAPTURE(s);
        CHECK(Curve::fromString(s).toString() == std::string(s));
    }
}

TEST_CASE("brush curve: parser tolerates the legacy and sloppy forms") {
    // No trailing semicolon.
    CHECK(Curve::fromString("0,0;1,1").isIdentity());
    // Whitespace around tokens.
    CHECK(Curve::fromString(" 0 , 0 ; 1 , 1 ; ").isIdentity());
    // Unknown trailing flags are ignored, not rejected (the format reserves room for more).
    const Curve extra = Curve::fromString("0,0,something_new;1,1;");
    CHECK(extra.points().size() == 2);
    CHECK_FALSE(extra.points()[0].corner);
    // Garbage degrades to the identity rather than throwing: a corrupt curve in one option must not
    // stop the whole preset from loading.
    CHECK(Curve::fromString("").isIdentity());
    CHECK(Curve::fromString(";;;").isIdentity());
    CHECK(Curve::fromString("nonsense").isIdentity());
    CHECK(Curve::fromString("0,0;bad,1;1,1;").points().size() == 2);
}

TEST_CASE("brush curve: points are sorted and duplicate x dropped") {
    const Curve c(std::vector<CurvePoint>{{1.0, 1.0, false}, {0.5, 0.25, false}, {0.0, 0.0, false}});
    REQUIRE(c.points().size() == 3);
    CHECK(c.points()[0].x == 0.0);
    CHECK(c.points()[1].x == 0.5);
    CHECK(c.points()[2].x == 1.0);

    // A zero-width interval has no spline; the first point at each x wins.
    const Curve dup(
        std::vector<CurvePoint>{{0.0, 0.0, false}, {0.5, 0.2, false}, {0.5, 0.9, false}, {1.0, 1.0, false}});
    REQUIRE(dup.points().size() == 3);
    CHECK(dup.points()[1].y == doctest::Approx(0.2));
}

TEST_CASE("brush curve: a corner severs the spline into straight segments") {
    // This is THE discriminating property between the two point kinds, and the reason the corner
    // flag has to survive import. Each side of a corner that sits between two endpoints is a
    // 2-point segment => exactly linear.
    const Curve v = Curve::fromString("0,1;0.5,0,is_corner;1,1;");
    REQUIRE(v.points().size() == 3);
    CHECK(v.points()[1].corner);

    CHECK(v.eval(0.00) == doctest::Approx(1.0).epsilon(kEps));
    CHECK(v.eval(0.25) == doctest::Approx(0.5).epsilon(kEps)); // exactly halfway down the left leg
    CHECK(v.eval(0.50) == doctest::Approx(0.0).epsilon(kEps));
    CHECK(v.eval(0.75) == doctest::Approx(0.5).epsilon(kEps));
    CHECK(v.eval(1.00) == doctest::Approx(1.0).epsilon(kEps));

    // The same points WITHOUT the corner are a smooth natural spline, which dips below the chord on
    // the way down -- so the two must disagree at 0.25. (If they ever agree, the corner flag is
    // being dropped somewhere.)
    const Curve u = Curve::fromString("0,1;0.5,0;1,1;");
    CHECK(u.eval(0.25) != doctest::Approx(0.5).epsilon(1e-6));
    CHECK(u.eval(0.25) < 0.5);
}

TEST_CASE("brush curve: natural spline is smooth, symmetric and interpolating") {
    const Curve u = Curve::fromString("0,1;0.5,0;1,1;");

    // Interpolates its control points.
    CHECK(u.eval(0.0) == doctest::Approx(1.0).epsilon(kEps));
    CHECK(u.eval(0.5) == doctest::Approx(0.0).epsilon(kEps));
    CHECK(u.eval(1.0) == doctest::Approx(1.0).epsilon(kEps));

    // Symmetric about x = 0.5, because the control points are.
    for (int i = 1; i < 10; ++i) {
        const double x = i / 20.0;
        CAPTURE(x);
        CHECK(u.eval(x) == doctest::Approx(u.eval(1.0 - x)).epsilon(1e-12));
    }

    // C1: no kink at the interior knot. Compare one-sided slopes across x = 0.5.
    constexpr double h = 1e-6;
    const double left = (u.eval(0.5) - u.eval(0.5 - h)) / h;
    const double right = (u.eval(0.5 + h) - u.eval(0.5)) / h;
    CHECK(left == doctest::Approx(right).epsilon(1e-4));
}

TEST_CASE("brush curve: domain is clamped flat and range is clamped to [0,1]") {
    const Curve c = Curve::fromString("0.2,0.3;0.8,0.7;");
    // Outside the control points' x-domain the curve is held flat, never extrapolated -- a cubic
    // run past its last knot diverges fast, and a sensor can legitimately hand us x = 0 or 1.
    CHECK(c.eval(-5.0) == doctest::Approx(0.3));
    CHECK(c.eval(0.0) == doctest::Approx(0.3));
    CHECK(c.eval(1.0) == doctest::Approx(0.7));
    CHECK(c.eval(5.0) == doctest::Approx(0.7));

    // A natural spline overshoots between widely separated knots; option strengths outside [0,1]
    // are meaningless, so eval() clamps.
    const Curve over = Curve::fromString("0,0;0.5,1;1,0;");
    for (int i = 0; i <= 100; ++i) {
        const double y = over.eval(i / 100.0);
        CHECK(y >= 0.0);
        CHECK(y <= 1.0);
    }
}

TEST_CASE("brush curve: degenerate point counts behave") {
    const Curve one(std::vector<CurvePoint>{{0.4, 0.6, false}});
    CHECK(one.eval(0.0) == doctest::Approx(0.6));
    CHECK(one.eval(1.0) == doctest::Approx(0.6));
    CHECK_FALSE(one.isIdentity());

    // An empty list is the identity, not an empty curve: every option must have a usable response.
    CHECK(Curve(std::vector<CurvePoint>{}).isIdentity());
}

TEST_CASE("brush curve: LUT matches direct evaluation") {
    const Curve u = Curve::fromString("0,1;0.25,0.2;0.6,0.9;1,0;");
    const std::vector<float> lut = u.toLut(256);
    REQUIRE(lut.size() == 256);

    // Sample points land exactly on LUT entries.
    for (std::size_t i = 0; i < lut.size(); i += 17) {
        const double x = static_cast<double>(i) / 255.0;
        CHECK(lut[i] == doctest::Approx(static_cast<float>(u.eval(x))).epsilon(1e-6));
    }
    // Interpolated lookups track the spline closely between them.
    for (int i = 0; i <= 100; ++i) {
        const double x = i / 100.0;
        CAPTURE(x);
        CHECK(evalLut(lut, x) == doctest::Approx(static_cast<float>(u.eval(x))).epsilon(2e-3));
    }

    // Out-of-range lookups saturate rather than read out of bounds.
    CHECK(evalLut(lut, -1.0) == doctest::Approx(lut.front()));
    CHECK(evalLut(lut, 2.0) == doctest::Approx(lut.back()));

    CHECK(Curve().toLut(1).size() == 2); // size < 2 is widened, never empty
}

TEST_CASE("brush curve: a real shipped preset curve") {
    // Taken verbatim from a shipped pixel-brush preset's size sensor (a two-sensor list; this is the
    // pressure child's curve). It must parse, round-trip, and stay monotone.
    const Curve c = Curve::fromString("0,0.753769;1,1;");
    CHECK(c.toString() == "0,0.753769;1,1;");
    CHECK(c.eval(0.0) == doctest::Approx(0.753769));
    CHECK(c.eval(1.0) == doctest::Approx(1.0));
    double prev = -1.0;
    for (int i = 0; i <= 50; ++i) {
        const double y = c.eval(i / 50.0);
        CHECK(y >= prev);
        prev = y;
    }
}

TEST_CASE("brush curve: parsing and formatting ignore LC_NUMERIC") {
    // Mosaic adopts the user's locale at startup (std::setlocale(LC_ALL, ""), common/i18n.cpp), and
    // strtod / snprintf("%g") both honour LC_NUMERIC. The preset format does not: it always writes
    // '.'. Under a comma-decimal locale the unguarded code SILENTLY DROPS control points -- "0.5"
    // stops strtod at the '.', fails the full-consumption check, and takes the whole point with it
    // -- and writes ',' back out, where ',' is the format's own x/y separator.
    //
    // A bare test binary never calls setlocale, so this is the only place the bug is reachable.
    const ScopedLocale loc(kCommaLocales);
    if (!loc.comma()) {
        MESSAGE("no comma-decimal locale installed; skipping the LC_NUMERIC checks");
        return;
    }
    REQUIRE(std::string(std::localeconv()->decimal_point) == ",");

    // Read: every control point survives, with its fractional part intact.
    const Curve c = Curve::fromString("0,0.753769;1,1;");
    REQUIRE(c.points().size() == 2);
    CHECK(c.points()[0].y == doctest::Approx(0.753769));
    CHECK(c.eval(0.0) == doctest::Approx(0.753769));

    // A corner knot at a fractional x is the case that loses an entire point, not just precision.
    const Curve corner = Curve::fromString("0,1;0.5,0,is_corner;1,1;");
    REQUIRE(corner.points().size() == 3);
    CHECK(corner.points()[1].x == doctest::Approx(0.5));
    CHECK(corner.points()[1].corner);

    // Write: '.' comes back, never the locale's ','. A comma here would re-parse as a fourth token.
    CHECK(c.toString() == "0,0.753769;1,1;");
    CHECK(corner.toString() == "0,1;0.5,0,is_corner;1,1;");
    CHECK(Curve().toString() == std::string(kIdentityCurve));

    // And the round-trip closes, which it cannot do if either direction leaks the locale.
    for (const char* s : {"0,0;1,1;", "0,1;0.5,0,is_corner;1,1;", "0,0.753769;1,1;",
                          "0,0;0.25,0.1;0.75,0.9;1,1;"}) {
        CAPTURE(s);
        CHECK(Curve::fromString(s).toString() == std::string(s));
    }

    // A token that uses the LOCALE's separator is malformed in every locale, not valid in some.
    // Otherwise "22,5" would mean 22.5 in Warsaw and nothing in London.
    const Curve localeSeparator = Curve::fromString("0,0;1,1,5;");
    CHECK(localeSeparator.points().size() == 2); // ",5" is an unknown flag token, not a decimal
}
