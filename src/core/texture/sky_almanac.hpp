#pragma once

#include <cstddef>

#include "core/texture/solar.hpp"  // UtcTime, SunPosition

// The night-sky "master clock" engine (S55 night overhaul, phase 5). Turns a civil-UTC instant +
// an observer location into the whole sky's coherent state -- the Sun's and Moon's real positions,
// the Moon's real phase, the daylight/twilight/night band, and the nearest catalogued city -- in
// one call, and can stamp that state straight onto SkyParams so the Sun, Moon and (already
// clock-driven) star field all agree. Pure over the existing solar.hpp + lunar.hpp + city_catalog
// solvers: no FLTK, no clocks, no locale, unit-testable headlessly. The dialog binds its
// date/time/place controls to this; the info panel reads it; the first-set-wins moon-phase latch
// lives in the dialog and calls applyMasterClock only while it is in ephemeris mode.
namespace mosaic::core::texture {

struct SkyParams;  // texture_params.hpp (only referenced by applyMasterClock, below)

// The eight conventional lunar phase names, for the info-panel readout.
enum class MoonPhaseName {
    New,
    WaxingCrescent,
    FirstQuarter,
    WaxingGibbous,
    Full,
    WaningGibbous,
    LastQuarter,
    WaningCrescent,
};
// Display text ("New Moon", "Waxing Crescent", ...). Always a valid non-null string.
[[nodiscard]] const char* moonPhaseNameText(MoonPhaseName p) noexcept;

// --- Rise / set / transit (S55 night overhaul, phase-5 additive) ---------------------------------
// One rise-, set-, or transit-event time for a celestial body over one UTC day, as fractional UTC
// hours in [0, 24). Rise/set events can be ABSENT at high latitude: `valid` is then false and, when
// the body is genuinely up or down the WHOLE day, one polar flag says which -- `alwaysUp` (never
// crosses the event altitude downward: circumpolar / midnight sun / never sets) or `alwaysDown`
// (never crosses it upward: polar night / never rises). A rise/set that is simply absent from THIS
// UTC window (its partner event happened but this one fell on the neighbouring UTC day) is `valid`
// false with NEITHER polar flag set. Transit (upper culmination) is always valid. Convert to a local
// clock reading with utcHourToLocal(hourUtc, utcOffsetHours).
struct RiseSetEvent {
    bool valid = false;       // the event occurred during the UTC day
    double hourUtc = 0.0;     // fractional UTC hours [0, 24) when valid
    bool alwaysUp = false;    // polar: body stayed above the event altitude the whole day
    bool alwaysDown = false;  // polar: body stayed below the event altitude the whole day
};

// Rise, set and transit for one body over one UTC day at one place. rise = the body's ascending
// crossing of the event altitude; set = its descending crossing; transit = upper culmination
// (meridian crossing, the day's highest altitude). Times are fractional UTC hours; altitudes are
// GEOMETRIC (airless), the convention the standard event altitudes (-0.833 deg etc.) are defined in.
struct RiseSetTimes {
    RiseSetEvent rise{};
    RiseSetEvent set{};
    RiseSetEvent transit{};           // always valid; hourUtc = the moment of upper culmination
    double transitAltitudeDeg = 0.0;  // geometric altitude at transit (the day's maximum)
};

// The three standard twilight bands, named by the Sun's geometric depression below the horizon.
enum class TwilightKind {
    Civil,         // Sun at  -6 deg  (limit of comfortable outdoor light without artificial aid)
    Nautical,      // Sun at -12 deg  (sea horizon still faintly discernible)
    Astronomical,  // Sun at -18 deg  (onset/end of full astronomical darkness)
};

// The sky's state at (t, latitude +N, longitude +E). Everything the master clock + info panel need.
struct SkyAlmanac {
    double sunAzimuthDeg = 0.0;        // compass, 0 N .. 90 E (solar.hpp convention)
    double sunElevationDeg = 0.0;      // apparent; negative = below the horizon
    double moonAzimuthDeg = 0.0;       // compass, topocentric (parallax + refraction)
    double moonElevationDeg = 0.0;     // apparent
    double moonIlluminatedFraction = 0.0;  // 0 new .. 1 full
    double moonAgeDays = 0.0;          // synodic age (0 at new .. ~29.53)
    MoonPhaseName moonPhaseName = MoonPhaseName::New;
    bool moonWaxing = true;            // waxing (age < half a synodic month) vs waning

    // Daylight classification from the Sun's apparent elevation (the standard thresholds). Exactly
    // one of these is true.
    bool isDaylight = false;           // sun elevation >= -0.833 deg (disc's top at the horizon)
    bool isCivilTwilight = false;      // -6    .. -0.833
    bool isNauticalTwilight = false;   // -12   .. -6
    bool isAstroTwilight = false;      // -18   .. -12
    bool isNight = false;              // sun elevation < -18 (true darkness)
    const char* skyStateText = "";     // "Daylight" / "Civil twilight" / ... / "Night"

    std::size_t nearestCityIndex = 0;  // index into cityCatalog()
    double nearestCityDistanceKm = 0.0;

    // -- Rise/set/transit over the UTC DAY that contains this instant (phase-5 additive; fractional
    //    UTC hours, polar-aware). The info panel reads these directly. --
    RiseSetTimes sunDayTimes{};              // sunrise / sunset / solar noon (event alt -0.833 deg)
    RiseSetTimes moonDayTimes{};             // moonrise / moonset / lunar transit
    RiseSetTimes civilTwilightTimes{};       // civil dawn (rise) / dusk (set) at  -6 deg
    RiseSetTimes nauticalTwilightTimes{};    // nautical dawn / dusk at -12 deg
    RiseSetTimes astronomicalTwilightTimes{};// astronomical dawn / dusk at -18 deg
};

// Compute the full almanac at (t, latitude +N, longitude +E). Pure.
[[nodiscard]] SkyAlmanac computeSkyAlmanac(const UtcTime& t, double latitudeDeg,
                                           double longitudeDeg);

// The master clock: stamp the Sun's and Moon's real positions + the Moon's real phase (mode 2:
// ephemeris) for (t, latitude, longitude) onto `sky`, and record the observer clock/place on it,
// so the Sun, Moon and star field are all coherent. Enables the Moon. Leaves camera, cloud,
// exposure and every artistic field untouched -- only the celestial bodies move. The dialog calls
// this when its moon-source latch is in "from date & place" mode.
void applyMasterClock(SkyParams& sky, const UtcTime& t, double latitudeDeg, double longitudeDeg);

// --- Rise / set / transit solvers (phase-5 additive) --------------------------------------------
// All take a `date` (only its year/month/day are used; the hour is ignored) and search that whole
// UTC day. Times come back as fractional UTC hours in [0, 24); see RiseSetTimes / RiseSetEvent for
// the polar (never-rises / never-sets) semantics. Pure -- no clocks, no locale.

// Sun rise / set / transit. Rise & set use the standard solar event altitude h0 = -0.8333 deg (mean
// horizontal refraction 34' + the Sun's semidiameter 16', Meeus ch. 15) against the Sun's geometric
// altitude; transit is solar noon.
[[nodiscard]] RiseSetTimes sunRiseSetTimes(const UtcTime& date, double latitudeDeg,
                                           double longitudeDeg);

// Sun events for an ARBITRARY target geometric altitude (deg) -- the general engine behind
// sunRiseSetTimes and the twilights. rise = ascending crossing of `targetAltDeg`, set = descending.
[[nodiscard]] RiseSetTimes sunEventsAtAltitude(const UtcTime& date, double latitudeDeg,
                                               double longitudeDeg, double targetAltDeg);

// Twilight dawn (rise = the morning crossing, sky getting lighter) and dusk (set = the evening
// crossing, sky getting darker) for a band. transit is the same solar upper culmination as
// sunRiseSetTimes.
[[nodiscard]] RiseSetTimes sunTwilightTimes(const UtcTime& date, double latitudeDeg,
                                            double longitudeDeg, TwilightKind kind);

// Moon rise / set / transit. The Moon moves ~0.5 deg/hr and is near enough that parallax matters, so
// the event altitude is the Meeus ch. 15 lunar value h0 = 0.7275*pi - 0.5667 deg (pi = the Moon's
// horizontal parallax at each trial instant) tested against the Moon's real GEOCENTRIC altitude,
// re-evaluated as it moves. transit is lunar upper culmination.
[[nodiscard]] RiseSetTimes moonRiseSetTimes(const UtcTime& date, double latitudeDeg,
                                            double longitudeDeg);

// Convert a fractional UTC hour to a local clock reading for a UTC offset (hours; e.g. -5 EST,
// +5.5 IST, +1 BST), wrapped into [0, 24). Pure formatting helper for the dialog.
[[nodiscard]] double utcHourToLocal(double hourUtc, double utcOffsetHours) noexcept;

}  // namespace mosaic::core::texture
