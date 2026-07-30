// Must precede EVERY include: glibc only exposes nl_langinfo_l / _NL_MEASUREMENT (and newlocale,
// POSIX-2008) when _GNU_SOURCE is set before <features.h> is first pulled in. Harmless elsewhere.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "common/settings.hpp"

#include "common/fs_path.hpp" // utf8FromPath: an error message must not throw on a non-ASCII name

#include <cstdlib>
#include <exception>
#include <fstream>
#include <system_error>
#ifdef __APPLE__
#include <cstdint>
#include <mach-o/dyld.h> // _NSGetExecutablePath, to locate the .app bundle's Resources (S59)
#endif
#ifdef _WIN32
#include <vector>
// shlobj.h brings windows.h, the FOLDERID_* known-folder ids and SHGetKnownFolderPath (S57).
#include <shlobj.h>
#endif

#include <nlohmann/json.hpp>

#ifdef __GLIBC__
#include <langinfo.h> // _NL_MEASUREMENT (the locale's measurement category)
#include <locale.h>   // newlocale / nl_langinfo_l
#endif

namespace mosaic::common {
namespace {
using json = nlohmann::json;

#ifdef _WIN32
// A known folder as a native path, or {} when the shell cannot answer.
//
// Preferred over reading %APPDATA%/%LOCALAPPDATA% for two reasons that both bite real users: the
// variables are simply absent from a stripped environment (a service token, a scheduled task, a
// shell launched with a cleared block), and std::getenv hands back the path in the ACTIVE CODE
// PAGE, so a profile name holding a character the code page cannot spell arrives as '?' and every
// save afterwards fails on a path that does not exist. The known-folder API is UTF-16 end to end.
//
// SHGetKnownFolderPath needs no CoInitialize for the per-user filesystem folders (it is documented
// to work uninitialized); only the CoTaskMemFree of its result touches COM's allocator, which is
// always available in-process.
std::filesystem::path knownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    // KF_FLAG_DEFAULT, deliberately NOT KF_FLAG_CREATE: the callers create their own directory tree
    // when they first write, and merely ASKING where settings live must not make a folder appear.
    const HRESULT hr = SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw);
    std::filesystem::path out;
    if (SUCCEEDED(hr) && raw != nullptr) {
        out = std::filesystem::path(raw);  // already UTF-16: path's native ctor, nothing transcoded
    }
    if (raw != nullptr) {
        CoTaskMemFree(raw);  // ours to free even on failure, per the API contract
    }
    return out;
}

// Per-user AppData, roaming or local, with the environment variable as a last-ditch fallback for
// the (pathological) case where the shell API fails but the variable is still set.
std::filesystem::path appDataDir(bool roaming) {
    if (const std::filesystem::path base =
            knownFolder(roaming ? FOLDERID_RoamingAppData : FOLDERID_LocalAppData);
        !base.empty()) {
        return base / "mosaic";
    }
    if (const char* env = std::getenv(roaming ? "APPDATA" : "LOCALAPPDATA");
        env != nullptr && *env != '\0') {
        return std::filesystem::path(env) / "mosaic";
    }
    return {};
}

// The running executable's full path, or {} when it cannot be read.
//
// ⚠ The classic bug here is trusting MAX_PATH: GetModuleFileNameW TRUNCATES rather than failing,
// and on a modern CRT it still returns the buffer size it filled, so a deep install directory
// yields a plausible-looking WRONG path instead of an error. ERROR_INSUFFICIENT_BUFFER is the only
// reliable signal, and it must be read after clearing the last error (the function leaves it
// untouched on success). Long paths are entirely ordinary on Windows now that the 260-char limit is
// opt-out.
std::filesystem::path executablePath() {
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        SetLastError(ERROR_SUCCESS);
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) {
            return {};  // the only genuine failure mode for the current process
        }
        if (static_cast<std::size_t>(n) < buf.size() &&
            GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            return std::filesystem::path(std::wstring(buf.data(), n));
        }
        if (buf.size() >= 65536) {
            return {};  // longer than any real install path; stop doubling rather than spin
        }
        buf.resize(buf.size() * 2);
    }
}
#endif
}  // namespace

std::filesystem::path configDir() {
#if defined(_WIN32)
    // ROAMING AppData (%APPDATA%\mosaic). Windows has no XDG config/data/state split, so the
    // decision is only roaming-vs-local, and settings.json is the textbook roaming case: a
    // preference is something you want to find already set on the next machine you sign in to. What
    // must NOT roam is derived state -- see stateDir().
    return appDataDir(/*roaming=*/true);
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') {
        return std::filesystem::path(xdg) / "mosaic";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".config" / "mosaic";
    }
    return {};
#endif
}

std::filesystem::path defaultSettingsPath() {
    const std::filesystem::path dir = configDir();
    return dir.empty() ? std::filesystem::path{} : dir / "settings.json";
}

std::filesystem::path dataDir() {
#if defined(_WIN32)
    // ROAMING too, and the SAME folder as configDir(): the user's brush presets, icon packs and
    // recorded scripts are their authored work, so they roam for the same reason the settings do
    // (and it is where every Windows app the artist already owns keeps its presets). Giving them a
    // second sibling folder would only transplant a Unixism -- the XDG config/data split -- into a
    // platform that expects one per-app directory; settings.json coexisting with brushes/ inside it
    // is the native layout, not a collision.
    return appDataDir(/*roaming=*/true);
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && *xdg != '\0') {
        return std::filesystem::path(xdg) / "mosaic";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".local" / "share" / "mosaic";
    }
    return {};
#endif
}

std::filesystem::path stateDir() {
#if defined(_WIN32)
    // LOCAL AppData (%LOCALAPPDATA%\mosaic), and that is the RIGHT half of the split rather than an
    // accident: everything that lands here is derived and machine-specific -- the recent-file
    // preview thumbnails, the recovery journals -- and roaming it would sync a cache the other
    // machine cannot use, over a network, at sign-in. It is also the half Microsoft's own guidance
    // names for caches, and the half that a roaming-profile size quota does not count.
    return appDataDir(/*roaming=*/false);
#else
    if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg != nullptr && *xdg != '\0') {
        return std::filesystem::path(xdg) / "mosaic";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".local" / "state" / "mosaic";
    }
#endif
    return {};
}

std::filesystem::path installedDataDir() {
    if (const char* env = std::getenv("MOSAIC_DATA_DIR"); env != nullptr && *env != '\0') {
        return env; // the explicit override is taken verbatim, existing or not
    }
#ifdef __APPLE__
    // A .app bundle ships its data in Resources: <bundle>/Contents/MacOS/<exe> ->
    // <bundle>/Contents/Resources (brushes/, presets/, icc-profiles/). Derived from the executable
    // path so the bundle stays relocatable (S58/S59).
    {
        char buf[4096];
        std::uint32_t sz = sizeof(buf);
        std::error_code ec;
        if (_NSGetExecutablePath(buf, &sz) == 0) {
            const std::filesystem::path res =
                std::filesystem::path(buf).parent_path().parent_path() / "Resources";
            if (std::filesystem::is_directory(res, ec))
                return res;
        }
    }
#endif
#ifdef _WIN32
    // The Windows analogue of the .app-bundle lookup above, and it is not optional: a portable-zip
    // install has NO fixed prefix, and the MOSAIC_DATA_DIR baked in below names a directory on the
    // Linux CROSS-BUILD HOST which cannot exist on the user's PC. So the payload is found relative
    // to the running executable. Load-bearing for the brush set, document templates, ICC profiles,
    // the gettext catalogs (common/i18n.cpp) and the hyphenation dictionaries
    // (core/text/hyphenator.cpp).
    {
        std::error_code ec;
        const std::filesystem::path exe = executablePath();
        if (!exe.empty()) {
            const std::filesystem::path dir = exe.parent_path();
            // Two layouts, most likely first: the portable zip (mosaic.exe + the DLLs at the root,
            // read-only payload under data/, mirroring the source tree's own data/), then a
            // Unix-style prefix install with the exe in bin/ -- which is what a plain
            // `cmake --install` still produces and what an MSI may choose to ship.
            for (const std::filesystem::path& cand :
                 {dir / "data", dir.parent_path() / "share" / "mosaic"}) {
                if (std::filesystem::is_directory(cand, ec))
                    return cand;
            }
        }
    }
#endif
#ifdef MOSAIC_DATA_DIR
    {
        std::error_code ec;
        if (std::filesystem::is_directory(MOSAIC_DATA_DIR, ec))
            return MOSAIC_DATA_DIR;
    }
#endif
#ifdef MOSAIC_SOURCE_DATA_DIR
    {
        std::error_code ec;
        if (std::filesystem::is_directory(MOSAIC_SOURCE_DATA_DIR, ec))
            return MOSAIC_SOURCE_DATA_DIR;
    }
#endif
    return {};
}

Settings loadSettings(const std::filesystem::path& path, std::string* error, bool* existed) {
    Settings settings;

    std::error_code ec;
    if (path.empty() || !std::filesystem::exists(path, ec)) {
        if (existed != nullptr) *existed = false;
        return settings;
    }
    if (existed != nullptr) *existed = true;

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        // utf8FromPath, not path.string(): on Windows the latter transcodes to the active code
        // page, which can throw for a path the page cannot spell -- and an error REPORT must never
        // be the thing that throws. The identity on POSIX (common/fs_path.hpp).
        if (error != nullptr) *error = "cannot open settings file: " + utf8FromPath(path);
        return settings;
    }

    const json doc = json::parse(in, /*cb=*/nullptr, /*allow_exceptions=*/false,
                                 /*ignore_comments=*/true);
    if (doc.is_discarded() || !doc.is_object()) {
        if (error != nullptr) *error = "malformed settings JSON: " + utf8FromPath(path);
        return settings;
    }

    try {
        settings.theme = doc.value("theme", settings.theme);
        settings.logLevel = doc.value("logLevel", settings.logLevel);
        settings.language = doc.value("language", settings.language);
        settings.units = doc.value("units", settings.units);
        settings.pickerSurface = doc.value("pickerSurface", settings.pickerSurface);
        settings.dockWidth = doc.value("dockWidth", settings.dockWidth);
        settings.brushPresetHeight = doc.value("brushPresetHeight", settings.brushPresetHeight);
        settings.brushPreset = doc.value("brushPreset", settings.brushPreset);
        settings.eraserPreset = doc.value("eraserPreset", settings.eraserPreset);
        settings.brushPresetDisplay =
            doc.value("brushPresetDisplay", settings.brushPresetDisplay);
        settings.recentColors = doc.value("recentColors", settings.recentColors);
        settings.recentFiles = doc.value("recentFiles", settings.recentFiles);
        settings.recentSizes = doc.value("recentSizes", settings.recentSizes);
        settings.workingProfile = doc.value("workingProfile", settings.workingProfile);
        settings.cmykProfile = doc.value("cmykProfile", settings.cmykProfile);
        settings.cropSwitchToolAfterApply =
            doc.value("cropSwitchToolAfterApply", settings.cropSwitchToolAfterApply);
        settings.cropInitialFraming =
            doc.value("cropInitialFraming", settings.cropInitialFraming);
        settings.cropClearSelectionOnLeave =
            doc.value("cropClearSelectionOnLeave", settings.cropClearSelectionOnLeave);
        settings.multiSelectionEdits =
            doc.value("multiSelectionEdits", settings.multiSelectionEdits);
        settings.lassoSmooth = doc.value("lassoSmooth", settings.lassoSmooth);
        settings.selectBrushAddByDefault =
            doc.value("selectBrushAddByDefault", settings.selectBrushAddByDefault);
        settings.eraserSizeFollowsBrush =
            doc.value("eraserSizeFollowsBrush", settings.eraserSizeFollowsBrush);
        // Settings → Tablet (docs/tablet.md §7). Every one of these is well-defined for ANY value a
        // hand-edited file can hold: TabletPolicy clamps + swaps the pressure range, an unparseable
        // curve string yields the identity, and SpeedSmoother guards a non-positive window. So they
        // are read straight through with no validation here -- the ingest path is the validator.
        settings.tabletPressureCurve =
            doc.value("tabletPressureCurve", settings.tabletPressureCurve);
        settings.tabletPressureMin = doc.value("tabletPressureMin", settings.tabletPressureMin);
        settings.tabletPressureMax = doc.value("tabletPressureMax", settings.tabletPressureMax);
        settings.tabletTiltOffsetDegrees =
            doc.value("tabletTiltOffsetDegrees", settings.tabletTiltOffsetDegrees);
        settings.tabletSpeedMax = doc.value("tabletSpeedMax", settings.tabletSpeedMax);
        settings.tabletSpeedWindowMs =
            doc.value("tabletSpeedWindowMs", settings.tabletSpeedWindowMs);
        // Migration: this shipped for a few hours as a 0..1 STRENGTH before becoming a toggle, so a
        // settings file written in that window holds a number here, and get<bool>() on it throws.
        // Any nonzero strength meant "smoothing on".
        if (doc.contains("brushSmoothing")) {
            const auto& v = doc.at("brushSmoothing");
            if (v.is_boolean())
                settings.brushSmoothing = v.get<bool>();
            else if (v.is_number())
                settings.brushSmoothing = v.get<double>() > 0.0;
        }
        settings.eraserPresetFollowsBrush =
            doc.value("eraserPresetFollowsBrush", settings.eraserPresetFollowsBrush);
        settings.overlayLineStyle = doc.value("overlayLineStyle", settings.overlayLineStyle);
        settings.featherIndicator = doc.value("featherIndicator", settings.featherIndicator);
        settings.antsCirculate = doc.value("antsCirculate", settings.antsCirculate);
        settings.iconPack = doc.value("iconPack", settings.iconPack);
        settings.motivationalLines = doc.value("motivationalLines", settings.motivationalLines);
        settings.showUnsavedDuration =
            doc.value("showUnsavedDuration", settings.showUnsavedDuration);
        settings.unsavedIncludeSeconds =
            doc.value("unsavedIncludeSeconds", settings.unsavedIncludeSeconds);
        settings.spellCheck = doc.value("spellCheck", settings.spellCheck);
        settings.spellCheckAllCaps = doc.value("spellCheckAllCaps", settings.spellCheckAllCaps);
        settings.textLanguage = doc.value("textLanguage", settings.textLanguage);
        settings.emojiFont = doc.value("emojiFont", settings.emojiFont);
        settings.renderingMode = doc.value("renderingMode", settings.renderingMode);
        settings.showAllExportFormats =
            doc.value("showAllExportFormats", settings.showAllExportFormats);
        // Keybindings (S51-b): the sparse id -> chord-text override map. Read straight through --
        // ui::Keymap::setOverrides() is the validator (it drops unknown actions and unparseable
        // chords), exactly as the ingest path validates the tablet fields above.
        settings.keymap = doc.value("keymap", settings.keymap);
        settings.inpaintBackend = doc.value("inpaintBackend", settings.inpaintBackend);
        settings.inpaintPreset = doc.value("inpaintPreset", settings.inpaintPreset);
        settings.inpaintParams = doc.value("inpaintParams", settings.inpaintParams);
    } catch (const std::exception& e) {
        if (error != nullptr) *error = std::string("settings field has wrong type: ") + e.what();
        return Settings{};  // clean defaults rather than a half-populated struct
    }

    return settings;
}

bool saveSettings(const Settings& settings, const std::filesystem::path& path, std::string* error) {
    if (path.empty()) {
        if (error != nullptr) *error = "no settings path (HOME/XDG_CONFIG_HOME unset?)";
        return false;
    }

    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);  // ok if it already exists
    }

    json doc;
    doc["version"] = Settings::kSchemaVersion;
    doc["theme"] = settings.theme;
    doc["logLevel"] = settings.logLevel;
    doc["language"] = settings.language;
    doc["units"] = settings.units;
    doc["pickerSurface"] = settings.pickerSurface;
    doc["dockWidth"] = settings.dockWidth;
    doc["brushPresetHeight"] = settings.brushPresetHeight;
    doc["brushPreset"] = settings.brushPreset;
    doc["eraserPreset"] = settings.eraserPreset;
    doc["brushPresetDisplay"] = settings.brushPresetDisplay;
    doc["recentColors"] = settings.recentColors;
    doc["recentFiles"] = settings.recentFiles;
    doc["recentSizes"] = settings.recentSizes;
    doc["workingProfile"] = settings.workingProfile;
    doc["cmykProfile"] = settings.cmykProfile;
    doc["cropSwitchToolAfterApply"] = settings.cropSwitchToolAfterApply;
    doc["cropInitialFraming"] = settings.cropInitialFraming;
    doc["cropClearSelectionOnLeave"] = settings.cropClearSelectionOnLeave;
    doc["multiSelectionEdits"] = settings.multiSelectionEdits;
    doc["lassoSmooth"] = settings.lassoSmooth;
    doc["selectBrushAddByDefault"] = settings.selectBrushAddByDefault;
    doc["eraserSizeFollowsBrush"] = settings.eraserSizeFollowsBrush;
    doc["tabletPressureCurve"] = settings.tabletPressureCurve;
    doc["tabletPressureMin"] = settings.tabletPressureMin;
    doc["tabletPressureMax"] = settings.tabletPressureMax;
    doc["tabletTiltOffsetDegrees"] = settings.tabletTiltOffsetDegrees;
    doc["tabletSpeedMax"] = settings.tabletSpeedMax;
    doc["tabletSpeedWindowMs"] = settings.tabletSpeedWindowMs;
    doc["brushSmoothing"] = settings.brushSmoothing;
    doc["eraserPresetFollowsBrush"] = settings.eraserPresetFollowsBrush;
    doc["overlayLineStyle"] = settings.overlayLineStyle;
    doc["featherIndicator"] = settings.featherIndicator;
    doc["antsCirculate"] = settings.antsCirculate;
    doc["iconPack"] = settings.iconPack;
    doc["motivationalLines"] = settings.motivationalLines;
    doc["showUnsavedDuration"] = settings.showUnsavedDuration;
    doc["unsavedIncludeSeconds"] = settings.unsavedIncludeSeconds;
    doc["spellCheck"] = settings.spellCheck;
    doc["spellCheckAllCaps"] = settings.spellCheckAllCaps;
    doc["textLanguage"] = settings.textLanguage;
    doc["emojiFont"] = settings.emojiFont;
    doc["renderingMode"] = settings.renderingMode;
    doc["showAllExportFormats"] = settings.showAllExportFormats;
    doc["keymap"] = settings.keymap;
    doc["inpaintBackend"] = settings.inpaintBackend;
    doc["inpaintPreset"] = settings.inpaintPreset;
    doc["inpaintParams"] = settings.inpaintParams;

    // Write to a sibling temp file, then rename: a crash mid-write can never corrupt the
    // existing settings, and the rename replaces the destination in one step within the directory
    // (POSIX rename(2); on Windows libstdc++ spells fs::rename as MoveFileExW with
    // MOVEFILE_REPLACE_EXISTING, so the "the target already exists" trap does not apply here).
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error != nullptr) *error = "cannot write settings file: " + utf8FromPath(tmp);
            return false;
        }
        out << doc.dump(2) << '\n';
        if (!out) {
            if (error != nullptr) *error = "error writing settings file: " + utf8FromPath(tmp);
            return false;
        }
    }

    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        if (error != nullptr) *error = "cannot replace settings file: " + ec.message();
        std::filesystem::remove(tmp, ec);  // best-effort cleanup
        return false;
    }
    return true;
}

std::string detectMeasurementSystem() {
#ifdef __GLIBC__
    // The locale's measurement category: 1 = metric, 2 = US customary. Read from a locale built
    // from the environment (LC_MEASUREMENT/LC_ALL/LANG) so the answer does not depend on whether
    // the process has called setlocale() yet, and is independent of the UI language.
    if (const locale_t loc = newlocale(LC_ALL_MASK, "", static_cast<locale_t>(0));
        loc != static_cast<locale_t>(0)) {
        const char* m = nl_langinfo_l(_NL_MEASUREMENT_MEASUREMENT, loc);
        const char system = (m != nullptr) ? m[0] : '\1';
        freelocale(loc);
        return system == 2 ? "imperial" : "metric";
    }
#elif defined(_WIN32)
    // The same question the glibc branch asks, in the Win32 spelling: LOCALE_IMEASURE is "0" for
    // metric and "1" for US customary, read from the user's REGIONAL FORMAT (Settings ▸ Time &
    // Language ▸ Region) and not from their display language -- which is the whole point, since a
    // German-speaking user on a US regional format wants inches in the crop readout. Independent of
    // setlocale(), exactly like the nl_langinfo_l path.
    {
        wchar_t buf[8] = {};
        constexpr int kBuf = static_cast<int>(sizeof(buf) / sizeof(buf[0]));
        if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IMEASURE, buf, kBuf) > 0) {
            return buf[0] == L'1' ? "imperial" : "metric";
        }
    }
#endif
    return "metric";  // the global majority when the platform cannot report it
}

std::string resolveUnits(const std::string& pref) {
    if (pref == "metric" || pref == "imperial")
        return pref;
    return detectMeasurementSystem();  // "auto", "", or anything unrecognised follows the locale
}

}  // namespace mosaic::common
