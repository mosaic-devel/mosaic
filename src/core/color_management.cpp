#include "core/color_management.hpp"

#include "common/settings.hpp" // installedDataDir() -- resolve the bundled default CMYK profile

#include <lcms2.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <system_error>

namespace mosaic::core {
namespace {

// Build the working-space RGB profile in memory from primaries + transfer curve. The CIE xy
// chromaticities below are the published ones for each standard; whites are D65 except where the
// standard says otherwise. lcms2 adapts everything to the D50 PCS internally (Bradford).
cmsHPROFILE buildRgbProfile(ColorSpace cs) {
    constexpr cmsCIExyY kD65{0.3127, 0.3290, 1.0};

    // sRGB-shaped parametric curve (lcms2 type 4): Y = (a*X + b)^g above d, c*X below.
    const auto srgbCurve = [] {
        const std::array<cmsFloat64Number, 5> p{2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92,
                                                0.04045};
        return cmsBuildParametricToneCurve(nullptr, 4, p.data());
    };
    // BT.709/2020-shaped curve (also type 4) -- the conventional ICC rendering of Rec.2020.
    const auto bt2020Curve = [] {
        const std::array<cmsFloat64Number, 5> p{1.0 / 0.45, 1.0 / 1.099, 0.099 / 1.099, 1.0 / 4.5,
                                                0.081};
        return cmsBuildParametricToneCurve(nullptr, 4, p.data());
    };

    cmsCIExyYTRIPLE primaries{};
    cmsToneCurve* curve = nullptr;
    switch (cs) {
    case ColorSpace::SRGB:
        return cmsCreate_sRGBProfile();
    case ColorSpace::LinearSRGB:
        primaries = {{0.6400, 0.3300, 1.0}, {0.3000, 0.6000, 1.0}, {0.1500, 0.0600, 1.0}};
        curve = cmsBuildGamma(nullptr, 1.0);
        break;
    case ColorSpace::DisplayP3:
        primaries = {{0.6800, 0.3200, 1.0}, {0.2650, 0.6900, 1.0}, {0.1500, 0.0600, 1.0}};
        curve = srgbCurve();
        break;
    case ColorSpace::AdobeRGB:
        primaries = {{0.6400, 0.3300, 1.0}, {0.2100, 0.7100, 1.0}, {0.1500, 0.0600, 1.0}};
        curve = cmsBuildGamma(nullptr, 563.0 / 256.0); // the spec's exact 2.19921875
        break;
    case ColorSpace::Rec2020:
        primaries = {{0.7080, 0.2920, 1.0}, {0.1700, 0.7970, 1.0}, {0.1310, 0.0460, 1.0}};
        curve = bt2020Curve();
        break;
    }
    cmsToneCurve* curves[3] = {curve, curve, curve};
    cmsHPROFILE profile = cmsCreateRGBProfile(&kD65, &primaries, curves);
    cmsFreeToneCurve(curve);
    return profile;
}

// Serialise an open profile as .icc bytes. Two passes, which is lcms2's own protocol: the first
// asks how many bytes are needed, the second fills them.
std::vector<std::uint8_t> saveProfileBytes(cmsHPROFILE profile) {
    if (profile == nullptr)
        return {};
    cmsUInt32Number needed = 0;
    if (cmsSaveProfileToMem(profile, nullptr, &needed) == 0 || needed == 0)
        return {};
    std::vector<std::uint8_t> bytes(needed);
    if (cmsSaveProfileToMem(profile, bytes.data(), &needed) == 0)
        return {};
    bytes.resize(needed);  // the second pass may report fewer bytes than the first reserved
    return bytes;
}

// The default CMYK press profile's path. Prefer the installed/bundled copy
// (installedDataDir()/icc-profiles/ -- the macOS .app's Resources, or an install prefix), which is
// the S59 direction; fall back to the compile-time path baked for dev builds. Empty if neither.
std::string defaultCmykPath() {
    const std::filesystem::path installed =
        common::installedDataDir() / "icc-profiles" / "ISOcoated_v2_300_eci.icc";
    std::error_code ec;
    if (!installed.empty() && std::filesystem::exists(installed, ec))
        return installed.string();
#ifdef MOSAIC_DEFAULT_CMYK_PROFILE
    return MOSAIC_DEFAULT_CMYK_PROFILE;
#else
    return {};
#endif
}

} // namespace

struct ColorEngine::Impl {
    cmsHPROFILE rgb = nullptr;  // kept open: every transform is (re)built against it
    cmsHPROFILE cmyk = nullptr; // kept open too: a working-space change rebuilds CMYK transforms
    std::string desc;           // file-loaded working profile's description ("" = built-in enum)
    cmsHTRANSFORM rgbToLab = nullptr;  // TYPE_RGB_8   -> TYPE_Lab_FLT
    cmsHTRANSFORM labToRgb = nullptr;  // TYPE_Lab_FLT -> TYPE_RGB_FLT, unbounded (gamut probe)
    cmsHTRANSFORM rgbToCmyk = nullptr; // TYPE_RGB_8   -> TYPE_CMYK_FLT (ink %)
    cmsHTRANSFORM cmykToRgb = nullptr; // TYPE_CMYK_FLT -> TYPE_RGB_8

    explicit Impl(ColorSpace cs) {
        rgb = buildRgbProfile(cs);
        buildLabTransforms();
    }

    void buildLabTransforms() {
        if (rgbToLab != nullptr)
            cmsDeleteTransform(rgbToLab);
        if (labToRgb != nullptr)
            cmsDeleteTransform(labToRgb);
        cmsHPROFILE lab = cmsCreateLab4Profile(nullptr); // D50, the PCS white
        rgbToLab = cmsCreateTransform(rgb, TYPE_RGB_8, lab, TYPE_Lab_FLT,
                                      INTENT_RELATIVE_COLORIMETRIC, 0);
        // Float-to-float keeps lcms2 in unbounded mode, so out-of-gamut Lab values come back as
        // RGB channels outside [0, 1] instead of silently clamping -- exactly the gamut probe.
        labToRgb = cmsCreateTransform(lab, TYPE_Lab_FLT, rgb, TYPE_RGB_FLT,
                                      INTENT_RELATIVE_COLORIMETRIC, 0);
        cmsCloseProfile(lab);
    }

    bool buildCmykTransforms() {
        if (rgbToCmyk != nullptr)
            cmsDeleteTransform(rgbToCmyk);
        if (cmykToRgb != nullptr)
            cmsDeleteTransform(cmykToRgb);
        rgbToCmyk = nullptr;
        cmykToRgb = nullptr;
        if (cmyk == nullptr)
            return false;
        // Relative colorimetric + black-point compensation: the conventional working pairing for
        // RGB <-> press-CMYK editing conversions (paper white maps to working white).
        rgbToCmyk = cmsCreateTransform(rgb, TYPE_RGB_8, cmyk, TYPE_CMYK_FLT,
                                       INTENT_RELATIVE_COLORIMETRIC,
                                       cmsFLAGS_BLACKPOINTCOMPENSATION);
        cmykToRgb = cmsCreateTransform(cmyk, TYPE_CMYK_FLT, rgb, TYPE_RGB_8,
                                       INTENT_RELATIVE_COLORIMETRIC,
                                       cmsFLAGS_BLACKPOINTCOMPENSATION);
        return rgbToCmyk != nullptr && cmykToRgb != nullptr;
    }

    bool loadCmyk(const void* data, std::size_t size) {
        return adoptCmykProfile(cmsOpenProfileFromMem(data, static_cast<cmsUInt32Number>(size)));
    }

    bool loadCmykFile(const char* path) {
        return adoptCmykProfile(cmsOpenProfileFromFile(path, "r"));
    }

    bool adoptCmykProfile(cmsHPROFILE prof) {
        if (prof == nullptr || cmsGetColorSpace(prof) != cmsSigCmykData) {
            if (prof != nullptr)
                cmsCloseProfile(prof);
            return false;
        }
        if (cmyk != nullptr)
            cmsCloseProfile(cmyk);
        cmyk = prof;
        if (!buildCmykTransforms()) {
            cmsCloseProfile(cmyk);
            cmyk = nullptr;
            return false;
        }
        return true;
    }

    bool adoptWorkingProfile(cmsHPROFILE prof) {
        if (prof == nullptr || cmsGetColorSpace(prof) != cmsSigRgbData) {
            if (prof != nullptr)
                cmsCloseProfile(prof);
            return false;
        }
        char buf[256] = {};
        cmsGetProfileInfoASCII(prof, cmsInfoDescription, "en", "US", buf, sizeof buf);
        desc = buf;
        if (rgb != nullptr)
            cmsCloseProfile(rgb);
        rgb = prof;
        buildLabTransforms();
        buildCmykTransforms(); // CMYK (if loaded) is anchored to the working profile too
        return true;
    }

    ~Impl() {
        if (rgbToLab != nullptr)
            cmsDeleteTransform(rgbToLab);
        if (labToRgb != nullptr)
            cmsDeleteTransform(labToRgb);
        if (rgbToCmyk != nullptr)
            cmsDeleteTransform(rgbToCmyk);
        if (cmykToRgb != nullptr)
            cmsDeleteTransform(cmykToRgb);
        if (rgb != nullptr)
            cmsCloseProfile(rgb);
        if (cmyk != nullptr)
            cmsCloseProfile(cmyk);
    }
};

ColorEngine::ColorEngine(ColorSpace working)
    : m_working(working), m_impl(std::make_unique<Impl>(working)) {
    // The vendored FOGRA39-based default press profile (third_party/icc-profiles/, HEIDELBERG
    // licence), loaded from disk so distros that object to its (redistributable but non-DFSG)
    // licence can strip the file -- hasCmyk() then stays false and CMYK UI hides. Resolved from the
    // installed/bundled data dir (S59), falling back to the compile-time dev path.
    if (const std::string cmyk = defaultCmykPath(); !cmyk.empty())
        m_impl->loadCmykFile(cmyk.c_str());
}

ColorEngine::~ColorEngine() = default;

bool ColorEngine::hasCmyk() const noexcept {
    return m_impl->rgbToCmyk != nullptr && m_impl->cmykToRgb != nullptr;
}

bool ColorEngine::loadCmykProfile(const void* data, std::size_t size) {
    return m_impl->loadCmyk(data, size);
}

bool ColorEngine::loadCmykProfileFile(const char* path) {
    return m_impl->loadCmykFile(path);
}

bool ColorEngine::loadDefaultCmykProfile() {
    const std::string cmyk = defaultCmykPath();
    return !cmyk.empty() && m_impl->loadCmykFile(cmyk.c_str());
}

bool ColorEngine::loadWorkingProfileFile(const char* path) {
    return m_impl->adoptWorkingProfile(cmsOpenProfileFromFile(path, "r"));
}

std::string ColorEngine::workingName() const {
    return m_impl->desc.empty() ? std::string(colorSpaceName(m_working)) : m_impl->desc;
}

Cmyk ColorEngine::toCmyk(common::Color8 rgb) const {
    const std::uint8_t in[3] = {rgb.r, rgb.g, rgb.b};
    float out[4] = {};
    cmsDoTransform(m_impl->rgbToCmyk, in, out, 1);
    return {out[0], out[1], out[2], out[3]};
}

common::Color8 ColorEngine::cmykToRgb(Cmyk c) const {
    const float in[4] = {std::clamp(c.c, 0.0F, 100.0F), std::clamp(c.m, 0.0F, 100.0F),
                         std::clamp(c.y, 0.0F, 100.0F), std::clamp(c.k, 0.0F, 100.0F)};
    std::uint8_t out[3] = {};
    cmsDoTransform(m_impl->cmykToRgb, in, out, 1);
    return {out[0], out[1], out[2], 255};
}

Lab ColorEngine::toLab(common::Color8 rgb) const {
    const std::uint8_t in[3] = {rgb.r, rgb.g, rgb.b};
    float out[3] = {};
    cmsDoTransform(m_impl->rgbToLab, in, out, 1);
    return {out[0], out[1], out[2]};
}

RgbF ColorEngine::toRgbUnclamped(Lab lab) const {
    const float in[3] = {lab.l, lab.a, lab.b};
    float out[3] = {};
    cmsDoTransform(m_impl->labToRgb, in, out, 1);
    return {out[0], out[1], out[2]};
}

common::Color8 ColorEngine::toRgbClamped(Lab lab) const {
    const RgbF f = toRgbUnclamped(lab);
    const auto to8 = [](float v) {
        return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0F, 1.0F) * 255.0F));
    };
    return {to8(f.r), to8(f.g), to8(f.b), 255};
}

bool ColorEngine::inGamut(const RgbF& c) noexcept {
    constexpr float kEps = 0.002F; // float/8-bit rounding slack
    const auto ok = [](float v) { return v >= -kEps && v <= 1.0F + kEps; };
    return ok(c.r) && ok(c.g) && ok(c.b);
}

std::vector<std::uint8_t> workingSpaceIccProfile(ColorSpace cs) {
    cmsHPROFILE profile = buildRgbProfile(cs);
    if (profile == nullptr)
        return {};
    // lcms2 names every profile it builds "RGB built-in" (and sRGB's "sRGB built-in"), which is
    // unhelpful in another application's profile list. Say which space this actually is.
    if (cmsMLU* mlu = cmsMLUalloc(nullptr, 1); mlu != nullptr) {
        cmsMLUsetASCII(mlu, "en", "US", std::string(colorSpaceName(cs)).c_str());
        cmsWriteTag(profile, cmsSigProfileDescriptionTag, mlu);
        cmsMLUfree(mlu);
    }
    std::vector<std::uint8_t> bytes = saveProfileBytes(profile);
    cmsCloseProfile(profile);
    return bytes;
}

std::vector<std::uint8_t> documentIccProfile(const Document& doc) {
    if (const std::string& path = doc.iccProfilePath(); !path.empty()) {
        cmsHPROFILE custom = cmsOpenProfileFromFile(path.c_str(), "r");
        if (custom != nullptr) {
            // The same gate loadWorkingProfileFile applies: an export must not staple a press or
            // grey profile onto an RGB image and call it that image's colour space.
            const bool isRgb = cmsGetColorSpace(custom) == cmsSigRgbData;
            std::vector<std::uint8_t> bytes = isRgb ? saveProfileBytes(custom)
                                                    : std::vector<std::uint8_t>{};
            cmsCloseProfile(custom);
            if (!bytes.empty())
                return bytes;
        }
        // Unopenable or not RGB: fall through to the working space's built-in profile.
    }
    if (doc.colorSpace() == ColorSpace::SRGB)
        return {};  // the universal default needs no tag; see the header
    return workingSpaceIccProfile(doc.colorSpace());
}

std::string iccProfileName(const std::string& path) {
    cmsHPROFILE prof = cmsOpenProfileFromFile(path.c_str(), "r");
    if (prof == nullptr)
        return {};
    char buf[256] = {};
    cmsGetProfileInfoASCII(prof, cmsInfoDescription, "en", "US", buf, sizeof buf);
    cmsCloseProfile(prof);
    return buf;
}

std::string cmykProfileName(const std::string& path) {
    return iccProfileName(path);
}

std::string defaultCmykProfileName() {
    const std::string cmyk = defaultCmykPath();
    return cmyk.empty() ? std::string{} : cmykProfileName(cmyk);
}

} // namespace mosaic::core
