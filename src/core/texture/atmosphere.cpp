// Physical single-scattering atmosphere (S55 night overhaul, phase 4). Technique lineage in
// atmosphere.hpp. The integrator is the textbook single-scattering
// radiative-transfer integral over a spherical-shell atmosphere; reimplemented from Nishita 1993 /
// Preetham 1999 / Bruneton 2008 EQUATIONS (physics), never from their engine source.

#include "core/texture/atmosphere.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace mosaic::core::texture {

namespace {

constexpr double kPi = std::numbers::pi;

// --- Physical constants (metres; sea-level scattering coefficients per metre). These are the
// standard atmospheric-scattering values used across the literature (Bruneton 2008/2017, Preetham
// 1999): Earth radius, a 60 km atmosphere shell, the Rayleigh coefficients from a lambda^-4 fit at
// ~680/550/440 nm, a grey Mie coefficient, and the Rayleigh/Mie scale heights. Laws of nature --
// the numeric values are measured physical properties of air, not authored art. ------------------
constexpr double kEarthRadius = 6360.0e3;       // R_e
constexpr double kAtmosphereRadius = 6420.0e3;  // R_a (60 km shell)
constexpr double kHRayleigh = 8000.0;           // Rayleigh density scale height
constexpr double kHMie = 1200.0;                // Mie density scale height
constexpr double kBetaR[3] = {5.802e-6, 13.558e-6, 33.100e-6};  // Rayleigh scattering, RGB
constexpr double kBetaMie = 3.996e-6;           // base Mie scattering (grey), pre-turbidity
// Solar intensity folded into the output (a fixed magic constant, as every real-time atmosphere
// uses); the renderer applies its own display exposure on top to sit in the tonemap's range.
constexpr double kSunIntensity = 22.0;
// A faint deep-night sky floor added in the renderer would double-count; the integrator itself
// returns pure single scattering, which correctly falls to ~0 once the sun is well below the
// horizon (Earth's shadow has risen past the whole visible atmosphere). The renderer supplies the
// airglow/starlight floor.

// Camera a few metres above sea level (avoids the exact-surface singularity; invisible otherwise).
constexpr double kCameraRadius = kEarthRadius + 2.0;

// Sentinel optical depth for a sun ray that strikes the Earth before reaching space: the sample is
// in shadow, so its transmittance-to-sun is 0. Large enough that exp(-beta*od) underflows to 0.
constexpr float kBlockedOd = 1.0e12f;

double clampd(double v, double lo, double hi) noexcept { return std::clamp(v, lo, hi); }

// Distance along a ray from a point at radius r (on the +z axis, P = (0,0,r)) whose direction has
// zenith-cosine mu, to the sphere of radius `sphere`. Returns the far (exit) root, or -1 if the
// ray misses the sphere. Uses the reduced quadratic t^2 + 2 r mu t + (r^2 - sphere^2) = 0.
double distanceToSphere(double r, double mu, double sphere) noexcept {
    const double disc = sphere * sphere - r * r * (1.0 - mu * mu);
    if (disc < 0.0) return -1.0;
    return -r * mu + std::sqrt(disc);
}

// True if a ray from radius r with zenith-cosine mu strikes the Earth ahead of it (t > 0).
bool hitsGround(double r, double mu) noexcept {
    const double disc = kEarthRadius * kEarthRadius - r * r * (1.0 - mu * mu);
    if (disc < 0.0) return false;
    const double tNear = -r * mu - std::sqrt(disc);  // near intersection
    return tNear > 1.0;                              // ahead of the sample (metres)
}

// Optical depth (Rayleigh, Mie) integrated from a point at radius r, zenith-cosine mu, to the top
// of the atmosphere. The point is on the +z axis; density depends only on altitude, so this is the
// full spherical-shell light path for the tabulation. Ground-occluded rays return the sentinel.
void opticalDepthToSpace(double r, double mu, float& odR, float& odM) noexcept {
    if (hitsGround(r, mu)) {
        odR = kBlockedOd;
        odM = kBlockedOd;
        return;
    }
    const double tTop = distanceToSphere(r, mu, kAtmosphereRadius);
    if (tTop <= 0.0) {
        odR = 0.0f;
        odM = 0.0f;
        return;
    }
    constexpr int kSteps = 32;
    const double dt = tTop / kSteps;
    const double rmu = r * mu;
    double sumR = 0.0, sumM = 0.0;
    for (int k = 0; k < kSteps; ++k) {
        const double t = (k + 0.5) * dt;
        const double rr = std::sqrt(r * r + 2.0 * rmu * t + t * t);
        const double h = std::max(0.0, rr - kEarthRadius);
        sumR += std::exp(-h / kHRayleigh) * dt;
        sumM += std::exp(-h / kHMie) * dt;
    }
    odR = static_cast<float>(sumR);
    odM = static_cast<float>(sumM);
}

// Cornette-Shanks Mie phase (a smooth Henyey-Greenstein variant), and the Rayleigh phase.
double rayleighPhase(double cosT) noexcept { return (3.0 / (16.0 * kPi)) * (1.0 + cosT * cosT); }

double miePhase(double cosT, double g) noexcept {
    const double g2 = g * g;
    const double num = (1.0 - g2) * (1.0 + cosT * cosT);
    const double den = (2.0 + g2) * std::pow(1.0 + g2 - 2.0 * g * cosT, 1.5);
    return (3.0 / (8.0 * kPi)) * num / std::max(1e-6, den);
}

// Bakes Atmosphere::msTable (defined below sampleOd, which it marches through).
void bakeMultipleScattering(Atmosphere& a);

}  // namespace

Atmosphere cookAtmosphere(SkyVec3 sunDir, double turbidity) noexcept {
    Atmosphere a;
    a.sunDir = skyNormalize(sunDir);
    // Turbidity scales the Mie load: a crystalline sky (turbidity 1) keeps the base coefficient, a
    // murky one (10) piles on aerosol. Mie extinction carries ~10% absorption over its scattering.
    const double haze = clampd(0.35 + 0.65 * (turbidity - 1.0), 0.35, 6.0);
    a.betaMieScatter = kBetaMie * haze;
    a.betaMieExtinct = a.betaMieScatter * 1.11;
    a.mieG = 0.76;
    a.viewSteps = 32;

    a.odRayleigh.assign(static_cast<std::size_t>(Atmosphere::kRSteps) * Atmosphere::kMuSteps, 0.0f);
    a.odMie.assign(a.odRayleigh.size(), 0.0f);
    for (int ir = 0; ir < Atmosphere::kRSteps; ++ir) {
        const double fr = Atmosphere::kRSteps > 1
                              ? static_cast<double>(ir) / (Atmosphere::kRSteps - 1)
                              : 0.0;
        const double r = kEarthRadius + fr * (kAtmosphereRadius - kEarthRadius);
        for (int imu = 0; imu < Atmosphere::kMuSteps; ++imu) {
            const double fmu = static_cast<double>(imu) / (Atmosphere::kMuSteps - 1);
            const double mu = -1.0 + 2.0 * fmu;
            float odR = 0.0f, odM = 0.0f;
            opticalDepthToSpace(r, mu, odR, odM);
            const std::size_t idx =
                static_cast<std::size_t>(ir) * Atmosphere::kMuSteps + imu;
            a.odRayleigh[idx] = odR;
            a.odMie[idx] = odM;
        }
    }
    bakeMultipleScattering(a);
    return a;
}

namespace {

// Bilinear sample of a cooked optical-depth table at (radius r, zenith-cosine mu). If either of the
// four corners is the ground-occluded sentinel the whole tap is treated as blocked (the sun is
// below that sample's local horizon) -- taking the max keeps the shadow edge crisp instead of
// bleeding a fractional sun through the terminator.
void sampleOd(const Atmosphere& a, double r, double mu, double& odR, double& odM) noexcept {
    const double fr = clampd((r - kEarthRadius) / (kAtmosphereRadius - kEarthRadius), 0.0, 1.0) *
                      (Atmosphere::kRSteps - 1);
    const double fmu = clampd((mu + 1.0) * 0.5, 0.0, 1.0) * (Atmosphere::kMuSteps - 1);
    int ir0 = static_cast<int>(std::floor(fr));
    int imu0 = static_cast<int>(std::floor(fmu));
    ir0 = std::clamp(ir0, 0, Atmosphere::kRSteps - 2 >= 0 ? Atmosphere::kRSteps - 2 : 0);
    imu0 = std::clamp(imu0, 0, Atmosphere::kMuSteps - 2);
    const int ir1 = std::min(ir0 + 1, Atmosphere::kRSteps - 1);
    const int imu1 = std::min(imu0 + 1, Atmosphere::kMuSteps - 1);
    const double tr = fr - ir0;
    const double tmu = fmu - imu0;
    const auto at = [&](int iri, int imui, float& rr, float& mm) {
        const std::size_t idx = static_cast<std::size_t>(iri) * Atmosphere::kMuSteps + imui;
        rr = a.odRayleigh[idx];
        mm = a.odMie[idx];
    };
    float r00, m00, r10, m10, r01, m01, r11, m11;
    at(ir0, imu0, r00, m00);
    at(ir0, imu1, r01, m01);
    at(ir1, imu0, r10, m10);
    at(ir1, imu1, r11, m11);
    // Any blocked corner -> blocked (Earth shadow); the max propagates the sentinel.
    if (r00 >= kBlockedOd || r01 >= kBlockedOd || r10 >= kBlockedOd || r11 >= kBlockedOd) {
        odR = kBlockedOd;
        odM = kBlockedOd;
        return;
    }
    const double bR = (r00 * (1.0 - tmu) + r01 * tmu) * (1.0 - tr) +
                      (r10 * (1.0 - tmu) + r11 * tmu) * tr;
    const double bM = (m00 * (1.0 - tmu) + m01 * tmu) * (1.0 - tr) +
                      (m10 * (1.0 - tmu) + m11 * tmu) * tr;
    odR = bR;
    odM = bM;
}

// ---- Isotropic multiple scattering (Hillaire 2020's Psi_ms; physics only, no code taken) ------
// For each (radius r, sun zenith-cosine muS) cell: march a small fan of directions out of the
// point, collecting (a) L2 -- the mean single-scattered radiance arriving from the rest of the
// atmosphere (the real Rayleigh/Mie phases against the sun, per channel, transmittance both to
// the sample and onward to the sun) -- and (b) f_ms, the mean fraction of that light scattered
// AGAIN before escaping. The full multiple-scattering sum is then the geometric series
// Psi = L2 * (1 + f + f^2 + ...) = L2 / (1 - f), Hillaire's energy-conserving closure (his
// second simplifying assumption: higher orders are isotropic, so each order transfers the same
// fraction). Ground bounce is omitted (a dark-earth twilight assumption; it only brightens).

constexpr int kMsDirs = 32;   // Fibonacci-sphere directions per cell
constexpr int kMsSteps = 16;  // march samples per direction

// Truncation compensation: the isotropic estimate stops at second-order sources (plus the
// geometric-series closure), omits the ground bounce, and models no ozone -- all of which real
// twilights collect. A fixed gain restores the missing energy without touching the TABLE's
// shape (the anti-solar lift and the blue shift stay pure scattering geometry) -- the same
// calibrated-constant approach as the cloud marcher's kSunGain. 5.0 puts the civil-twilight
// zenith's total/single-scatter ratio at ~8, inside the 5-10 the twilight literature measures
// (Hulburt 1953 / Rozenberg's secondary-scattering dominance). Bounded: it scales a convergent
// series, so radiance stays finite and monotone with the sun's descent.
constexpr double kMsGain = 5.0;

void bakeMultipleScattering(Atmosphere& a) {
    a.msTable.assign(static_cast<std::size_t>(Atmosphere::kMsRSteps) * Atmosphere::kMsMuSteps * 3,
                     0.0f);
    // Fibonacci sphere: a deterministic, evenly-spread direction fan.
    std::array<std::array<double, 3>, kMsDirs> dirs;
    for (int i = 0; i < kMsDirs; ++i) {
        const double z = 1.0 - 2.0 * (i + 0.5) / kMsDirs;
        const double rho = std::sqrt(std::max(0.0, 1.0 - z * z));
        const double phi = i * 2.399963229728653;  // the golden angle
        dirs[static_cast<std::size_t>(i)] = {rho * std::cos(phi), rho * std::sin(phi), z};
    }

    for (int ir = 0; ir < Atmosphere::kMsRSteps; ++ir) {
        const double fr = static_cast<double>(ir) / (Atmosphere::kMsRSteps - 1);
        const double r = kEarthRadius + 2.0 + fr * (kAtmosphereRadius - kEarthRadius - 2.0);
        for (int imu = 0; imu < Atmosphere::kMsMuSteps; ++imu) {
            const double muS = -1.0 + 2.0 * static_cast<double>(imu) /
                                         (Atmosphere::kMsMuSteps - 1);
            // The sun in the point's local frame (P on the +z axis): zenith-cosine muS.
            const double sx = std::sqrt(std::max(0.0, 1.0 - muS * muS));
            std::array<double, 3> L2{0.0, 0.0, 0.0};
            std::array<double, 3> fms{0.0, 0.0, 0.0};

            for (const auto& w : dirs) {
                const double muW = w[2];
                double tMax = distanceToSphere(r, muW, kAtmosphereRadius);
                if (tMax <= 0.0) continue;
                const double discG = kEarthRadius * kEarthRadius - r * r * (1.0 - muW * muW);
                if (discG >= 0.0) {
                    const double tG = -r * muW - std::sqrt(discG);
                    if (tG > 1.0) tMax = std::min(tMax, tG);
                }
                const double cosT = w[0] * sx + w[2] * muS;  // scattering angle vs the sun
                const double pR = rayleighPhase(cosT);
                const double pM = miePhase(cosT, a.mieG);
                const double dt = tMax / kMsSteps;
                double odR = 0.0, odM = 0.0;  // running point->sample optical depth
                for (int k = 0; k < kMsSteps; ++k) {
                    const double t = (k + 0.5) * dt;
                    const double px = w[0] * t, py = w[1] * t, pz = r + w[2] * t;
                    const double rr = std::sqrt(px * px + py * py + pz * pz);
                    const double h = std::max(0.0, rr - kEarthRadius);
                    const double dR = std::exp(-h / kHRayleigh);
                    const double dM = std::exp(-h / kHMie);
                    odR += dR * dt;
                    odM += dM * dt;
                    const double muSun = (px * sx + pz * muS) / rr;
                    double odsR = 0.0, odsM = 0.0;
                    sampleOd(a, rr, muSun, odsR, odsM);
                    const bool sunlit = odsR < kBlockedOd;
                    for (int c = 0; c < 3; ++c) {
                        const double tauView = kBetaR[c] * odR + a.betaMieExtinct * odM;
                        const double tView = std::exp(-tauView);
                        fms[static_cast<std::size_t>(c)] +=
                            tView * (kBetaR[c] * dR + a.betaMieScatter * dM) * dt;
                        if (sunlit) {
                            const double tauSun =
                                kBetaR[c] * odsR + a.betaMieExtinct * odsM;
                            L2[static_cast<std::size_t>(c)] +=
                                tView *
                                (kBetaR[c] * dR * pR + a.betaMieScatter * dM * pM) *
                                std::exp(-tauSun) * dt;
                        }
                    }
                }
            }
            const std::size_t idx =
                (static_cast<std::size_t>(ir) * Atmosphere::kMsMuSteps + imu) * 3;
            for (int c = 0; c < 3; ++c) {
                const double l2 = L2[static_cast<std::size_t>(c)] / kMsDirs;
                const double f = std::min(0.95, fms[static_cast<std::size_t>(c)] / kMsDirs);
                a.msTable[idx + static_cast<std::size_t>(c)] =
                    static_cast<float>(kMsGain * l2 / (1.0 - f));
            }
        }
    }
}

// Bilinear sample of the multiple-scattering table at (radius r, sun zenith-cosine muS). Hot --
// called per view-march sample -- so the four corner rows are resolved once and read directly.
void sampleMs(const Atmosphere& a, double r, double muS, double psi[3]) noexcept {
    const double fr = clampd((r - kEarthRadius) / (kAtmosphereRadius - kEarthRadius), 0.0, 1.0) *
                      (Atmosphere::kMsRSteps - 1);
    const double fmu = clampd((muS + 1.0) * 0.5, 0.0, 1.0) * (Atmosphere::kMsMuSteps - 1);
    int ir0 = static_cast<int>(fr);
    int imu0 = static_cast<int>(fmu);
    ir0 = std::min(ir0, Atmosphere::kMsRSteps - 2);
    imu0 = std::min(imu0, Atmosphere::kMsMuSteps - 2);
    const double tr = fr - ir0;
    const double tmu = fmu - imu0;
    const double w00 = (1.0 - tr) * (1.0 - tmu), w01 = (1.0 - tr) * tmu;
    const double w10 = tr * (1.0 - tmu), w11 = tr * tmu;
    const float* p0 =
        a.msTable.data() + (static_cast<std::size_t>(ir0) * Atmosphere::kMsMuSteps + imu0) * 3;
    const float* p1 = p0 + static_cast<std::size_t>(Atmosphere::kMsMuSteps) * 3;
    for (int c = 0; c < 3; ++c)
        psi[c] = w00 * p0[c] + w01 * p0[c + 3] + w10 * p1[c] + w11 * p1[c + 3];
}

}  // namespace

Rgb Atmosphere::radiance(SkyVec3 viewDir) const noexcept {
    if (odRayleigh.empty()) return {0.0, 0.0, 0.0};  // not cooked (defensive)
    const SkyVec3 V = skyNormalize(viewDir);
    const double r0 = kCameraRadius;
    const double mu0 = V.z;  // zenith-cosine at the camera (local zenith = +z)

    // The segment of the view ray inside the atmosphere: from the camera to the top of the shell,
    // clipped to the ground if a downward ray strikes the Earth first.
    double tMax = distanceToSphere(r0, mu0, kAtmosphereRadius);
    if (tMax <= 0.0) return {0.0, 0.0, 0.0};
    {
        const double discG = kEarthRadius * kEarthRadius - r0 * r0 * (1.0 - mu0 * mu0);
        if (discG >= 0.0) {
            const double tG = -r0 * mu0 - std::sqrt(discG);
            if (tG > 1.0) tMax = std::min(tMax, tG);
        }
    }
    const int N = viewSteps;
    const double dt = tMax / N;
    const double rmu0 = r0 * mu0;

    const double cosT = skyDot(V, sunDir);  // scattering angle cosine (view vs. sun)
    const double phaseR = rayleighPhase(cosT);
    const double phaseM = miePhase(cosT, mieG);

    // Per-channel in-scatter accumulators (transmittance is wavelength-dependent, so Rayleigh and
    // Mie must be summed per channel, not once as a grey integral).
    std::array<double, 3> sumR{0.0, 0.0, 0.0};
    std::array<double, 3> sumM{0.0, 0.0, 0.0};
    std::array<double, 3> sumMs{0.0, 0.0, 0.0};  // isotropic multiple scattering (Psi_ms)
    double viewOdR = 0.0, viewOdM = 0.0;  // running camera->sample optical depth (grey per species)

    for (int k = 0; k < N; ++k) {
        const double t = (k + 0.5) * dt;
        const double rr = std::sqrt(r0 * r0 + 2.0 * rmu0 * t + t * t);
        const double h = std::max(0.0, rr - kEarthRadius);
        const double dR = std::exp(-h / kHRayleigh);
        const double dM = std::exp(-h / kHMie);
        viewOdR += dR * dt;
        viewOdM += dM * dt;

        // Sun transmittance from this sample: the sample's own zenith-cosine toward the sun.
        const double sx = V.x * t, sy = V.y * t, sz = r0 + V.z * t;
        const double muSun = (sx * sunDir.x + sy * sunDir.y + sz * sunDir.z) / rr;
        double odsR = 0.0, odsM = 0.0;
        sampleOd(*this, rr, muSun, odsR, odsM);
        const bool sunlit = odsR < kBlockedOd;  // shadowed samples get NO direct sunlight...

        // ...but they still receive the multiple-scattering ambient (that is the mechanism that
        // keeps a twilight sky alive after Earth's shadow has swallowed the direct term). The
        // Psi_ms table is isotropic, so it adds without a phase, attenuated to the camera only.
        // A whole-shell-dark table (deep night) short-circuits to the plain single-scatter cost.
        double psi[3];
        sampleMs(*this, rr, muSun, psi);
        const bool anyMs = psi[0] + psi[1] + psi[2] > 0.0;
        if (!sunlit && !anyMs) continue;
        for (int c = 0; c < 3; ++c) {
            const double tauView = kBetaR[c] * viewOdR + betaMieExtinct * viewOdM;
            const double tView = std::exp(-tauView);
            if (anyMs) sumMs[c] += psi[c] * (kBetaR[c] * dR + betaMieScatter * dM) * tView * dt;
            if (sunlit) {
                const double trans =
                    tView * std::exp(-(kBetaR[c] * odsR + betaMieExtinct * odsM));
                sumR[c] += dR * trans * dt;
                sumM[c] += dM * trans * dt;
            }
        }
    }

    Rgb out;
    out.r = kSunIntensity *
            (kBetaR[0] * phaseR * sumR[0] + betaMieScatter * phaseM * sumM[0] + sumMs[0]);
    out.g = kSunIntensity *
            (kBetaR[1] * phaseR * sumR[1] + betaMieScatter * phaseM * sumM[1] + sumMs[1]);
    out.b = kSunIntensity *
            (kBetaR[2] * phaseR * sumR[2] + betaMieScatter * phaseM * sumM[2] + sumMs[2]);
    // Finite, non-negative by construction (products of non-negative exponentials); clamp defends
    // against any denormal underflow noise.
    out.r = std::max(0.0, out.r);
    out.g = std::max(0.0, out.g);
    out.b = std::max(0.0, out.b);
    return out;
}

}  // namespace mosaic::core::texture
