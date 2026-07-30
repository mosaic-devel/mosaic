#include "common/i18n.hpp"

#include <cctype>
#include <clocale>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#if defined(__APPLE__) || defined(_WIN32)
#  include <system_error>

// installedDataDir(): the .app's Contents/Resources, or the Windows payload beside mosaic.exe.
#  include "common/settings.hpp"
#endif

#if MOSAIC_HAVE_GETTEXT
#  include <libintl.h>
#endif

namespace mosaic::common::i18n {

namespace {
// The $MOSAIC_LANG value that took effect, kept for activeLanguageOverride(). Empty when the
// override was absent (or this build has no gettext), i.e. the system locale decides.
std::string g_languageOverride;
}  // namespace

#if MOSAIC_HAVE_GETTEXT
namespace {
// putenv/unsetenv, spelled portably. mingw has no POSIX setenv; _putenv_s with an empty value
// is its documented way to remove a variable.
//
// ⚠ On Windows this reaches libintl only because mosaic.exe and libintl-8.dll share one C runtime.
// libintl reads the environment with getenv(), i.e. through its own CRT's copy; each CRT snapshots
// the process environment block when it loads and never re-reads it, so a _putenv_s into one CRT's
// copy would be invisible to another one and $MOSAIC_LANG would silently do nothing. Adding a
// SetEnvironmentVariableW next to this would NOT rescue such a split -- that updates the Win32
// block, which neither CRT re-reads -- so there is deliberately no such call here.
//
// VERIFIED, not assumed: every DLL in the payload and mosaic.exe itself import `api-ms-win-crt-*`,
// i.e. the UCRT. ⚠ Do not re-derive this from `gcc -dumpspecs`, which shows `-lmsvcrt` and looks
// alarming: mingw-w64's CRT is configured `--with-default-msvcrt=ucrt` here, so `libmsvcrt.a` is
// itself an alias for the UCRT import library. The honest check is `objdump -p` on the built
// artefacts. One CRT is a payload-wide invariant far bigger than this function -- a FILE*, an errno
// or a locale crossing a CRT boundary is undefined behaviour -- so if a dependency is ever swapped
// for a prebuilt binary, check it the same way. See docs/build-windows.md.
void setEnvVar(const char* key, const char* value) {
#  if defined(_WIN32)
    ::_putenv_s(key, value != nullptr ? value : "");
#  else
    if (value != nullptr) {
        ::setenv(key, value, 1);
    } else {
        ::unsetenv(key);
    }
#  endif
}

// Resolve a catalog directory: the explicit arg, else $MOSAIC_LOCALEDIR (run-from-build-tree
// override), else the app's own bundled copy on macOS/Windows, else the compiled-in install
// default. Empty when none is available.
//
// A std::filesystem::path rather than a std::string, because on Windows the directory has to reach
// gettext as UTF-16 and a narrowing conversion in the middle is exactly what must not happen there
// (see bindDomain below). On POSIX the switch is a no-op: `path` IS a byte string, so .c_str()
// hands bindtextdomain the very same bytes the std::string did.
std::filesystem::path resolveLocaleDir(const char* localeDir) {
    std::filesystem::path dir =
        localeDir != nullptr ? std::filesystem::path(localeDir) : std::filesystem::path{};
    if (const char* env = std::getenv("MOSAIC_LOCALEDIR"); env && *env) {
        dir = env;
    }
#  if defined(__APPLE__) || defined(_WIN32)
    if (dir.empty()) {
        // The app ships its catalogs in <resources>/locale, next to brushes/ and presets/: that is
        // Contents/Resources/locale inside a .app, and data/locale beside mosaic.exe in the Windows
        // payload. installedDataDir() derives it from the executable's location so the install
        // stays relocatable -- whereas the compiled-in MOSAIC_LOCALEDIR points into the
        // *cross-build* prefix, a path that cannot exist on the user's machine. Without this the
        // app is English-only however many catalogs shipped. (S54; make-dmg.sh step 2c, S57 for
        // Windows.)
        std::error_code ec;
        const std::filesystem::path res = installedDataDir() / "locale";
        if (std::filesystem::is_directory(res, ec)) {
            dir = res;
        }
    }
#  endif
#  ifdef MOSAIC_LOCALEDIR
    if (dir.empty()) {
        dir = MOSAIC_LOCALEDIR;
    }
#  endif
    return dir;
}

// Bind a domain's catalog dir + UTF-8 codeset (no-op without a directory). Shared by init() and
// initDomain(); neither changes the active default domain here -- init() does that separately.
void bindDomain(const char* domain, const char* localeDir) {
    const auto dir = resolveLocaleDir(localeDir);
    if (dir.empty()) {
        return;
    }
#  if defined(_WIN32)
    // wbindtextdomain, NOT bindtextdomain. libintl opens the .mo through the narrow CRT, so the
    // narrow entry point can only name a directory the ACTIVE CODE PAGE can spell -- and a portable
    // zip unpacked into, say, "C:\Users\Ömer\Downloads\mosaic" is precisely the case it cannot. The
    // failure is silent (no catalog found, everything stays English), which is the worst kind. The
    // wide variant takes the path's native UTF-16 form with nothing in between; it has existed
    // since gettext 0.22 and we cross-build 0.22.5 (verified exported from libintl-8.dll).
    wbindtextdomain(domain, dir.c_str());
#  else
    bindtextdomain(domain, dir.c_str());
#  endif
    bind_textdomain_codeset(domain, "UTF-8");
}

// Redirect message lookup to $MOSAIC_LANG, if set. Called AFTER setlocale(LC_ALL, "") so the
// process still adopts the user's real locale for everything that is not a UI string.
//
// Why an app-specific variable rather than "just set LANGUAGE"? Because switching the UI language
// otherwise means finding a locale the machine has actually generated -- `LANG=ja_JP.UTF-8` does
// nothing on a box that never ran locale-gen for it -- which makes "try the app in Japanese" a
// sysadmin task. MOSAIC_LANG always works, because a gettext catalog lookup needs no system locale.
//
// $LANGUAGE is the right knob: highest-priority selector, and it takes a colon-separated fallback
// list, which is exactly what the @modifier catalogs want ("ca@valencia:ca").
//
// The catch is that gettext deliberately IGNORES $LANGUAGE when the message category is "C" -- it
// will not emit non-ASCII into a locale that cannot represent it. So a user with LC_ALL=C, or with
// no locale set at all (bare login shell, CI runner, systemd unit), would get silence and no clue
// why. Defeating that guard takes BOTH of the following, because the two gettext implementations
// we ship against decide "is the category C?" differently:
//
//  * glibc asks `setlocale(category, NULL)` -- the locale actually APPLIED to the process. No
//    amount of environment editing moves it; only another setlocale() call does.
//  * GNU libintl (macOS/Windows) reads the ENVIRONMENT instead -- $LC_ALL, then $LC_MESSAGES,
//    then $LANG. $LC_ALL outranks $LC_MESSAGES there, hence the unset.
//
// Hence: fix the environment for libintl, then re-apply LC_MESSAGES for glibc.
//
// ⚠ On Windows, LC_MESSAGES is not a real category at all -- the MSVCRT has never had one.
// <libintl.h> defines the name (as 1729) and libintl_setlocale keeps the value in a static shadow
// of its own, which is what setlocale(LC_MESSAGES, ...) reads and writes here. That is why this
// whole block still COMPILES and still means something there, and also why the candidate loop below
// can never fire on Windows: the shadow is seeded from libintl's own locale detection, which
// derives a real name from the thread locale and never yields "C". The environment fix above is
// doing all the work, exactly as it does on macOS -- same libintl, same rules. Nothing here may be
// re-expressed as a Windows locale name ("de-DE"): these strings are gettext's POSIX-shaped catalog
// selectors, not CRT locale names, and libintl is the only thing that reads them.
//
// Two things that look like fixes but are not:
//
//  * C.UTF-8 is NOT an escape hatch. glibc excludes it from $LANGUAGE honouring exactly as it
//    excludes "C" (verified on glibc 2.43), because C.UTF-8 is by definition the locale-neutral
//    locale. Only a real, generated locale gets the category off the guard.
//  * Re-applying LC_MESSAGES unconditionally is a regression, not a no-op. When the user already
//    has a good locale the guard was never going to bite, and pointing the category at some
//    fallback would DOWNGRADE a working setup. So intervene only when the category is genuinely
//    neutered, and never accept a candidate that does not clear the guard.
//
// Only start-up state is touched, before the first lookup, so gettext has nothing cached yet and
// no _nl_msg_cat_cntr bump is needed.

// Does this LC_MESSAGES value make gettext ignore $LANGUAGE?
bool categoryIsNeutered(const char* applied) {
    return applied == nullptr || std::strcmp(applied, "C") == 0 ||
           std::strcmp(applied, "POSIX") == 0 || std::strcmp(applied, "C.UTF-8") == 0 ||
           std::strcmp(applied, "C.utf8") == 0;
}

void applyLanguageOverride() {
    // Cleared first so a second init() cannot report a language the current environment no longer
    // asks for. (Only the tests call init() twice, but stale global state is stale global state.)
    g_languageOverride.clear();
    const char* want = std::getenv("MOSAIC_LANG");
    if (want == nullptr || *want == '\0') {
        return;
    }
    g_languageOverride = want;
    setEnvVar("LANGUAGE", want);
    setEnvVar("LC_ALL", nullptr);

    // The first list entry is the only one that names a single locale; the fallbacks after it are
    // $LANGUAGE's business, not setlocale's.
    const std::string first = g_languageOverride.substr(0, g_languageOverride.find(':'));
    setEnvVar("LC_MESSAGES", first.c_str());

    if (!categoryIsNeutered(std::setlocale(LC_MESSAGES, nullptr))) {
        return;  // the user has a real locale; $LANGUAGE is already being honoured
    }

    // Candidates, best first. Only the first few try to MATCH the requested language, so that
    // LC_MESSAGES keeps an honest meaning; the tail exists purely to get the category off the
    // guard, after which $LANGUAGE -- not this locale -- picks the catalog.
    const std::string base = first.substr(0, first.find('@'));  // "ca@valencia" -> "ca"
    const std::string utf8 = base + ".UTF-8";
    std::string guess;
    if (base.size() == 2) {
        // "de" -> "de_DE.UTF-8". A heuristic, and a wrong one for e.g. cs (cs_CZ) or ja (ja_JP) --
        // but it costs one failed setlocale and it hits for most of the large languages.
        guess = base + "_" + static_cast<char>(std::toupper(base[0]))
                + static_cast<char>(std::toupper(base[1])) + ".UTF-8";
    }
    for (const std::string& candidate : {first, utf8, guess, std::string("en_US.UTF-8")}) {
        if (candidate.empty()) {
            continue;
        }
        // A candidate only counts if it applies AND clears the guard -- setlocale succeeds for
        // "C.UTF-8", which would leave us exactly as stuck as before.
        if (const char* applied = std::setlocale(LC_MESSAGES, candidate.c_str());
            applied != nullptr && !categoryIsNeutered(applied)) {
            return;
        }
    }
    // No generated non-C message locale on this machine, so glibc will keep ignoring $LANGUAGE and
    // the UI stays English. Documented in docs/i18n.md; the fix is a real locale (any one will do).
    std::setlocale(LC_MESSAGES, "");  // leave the category as the environment described it
}
}  // namespace
#endif

void init([[maybe_unused]] const char* domain, [[maybe_unused]] const char* localeDir) {
#if MOSAIC_HAVE_GETTEXT
    // Adopt the user's locale (LC_MESSAGES selects the catalog, LC_CTYPE the codeset).
    //
    // ⚠ This sets LC_NUMERIC too, so `strtod` and `printf("%g")` change meaning for the whole
    // process from here on. Anything that reads or writes an INTERCHANGE format must not use them
    // naively: nlohmann/json carries its own locale-independent conversion, and the brush preset
    // formats go through core/brush/parse_util.hpp, which translates the decimal separator. Numbers
    // typed into, or displayed by, a widget are a different case and should follow the user's
    // locale.
    //
    // ⚠ Windows does not weaken that guard, but it does EXERCISE it far more often. There, gettext
    // redirects setlocale to libintl_setlocale (see <libintl.h>), which for LC_ALL,"" hands the
    // real categories to the CRT one at a time and keeps LC_MESSAGES -- a category the MSVCRT does
    // not have -- in a shadow of its own. So LC_NUMERIC still lands in the CRT, and the CRT's
    // default for
    // "" is the user's REGIONAL FORMAT: "German_Germany.1252" and its comma decimal point is what a
    // freshly installed German PC gives every process, with no environment variable in sight. Both
    // guards read std::localeconv(), which is a plain CRT call and reports that truthfully, so the
    // Windows path cannot slip past them -- but it is the platform where they earn their keep.
    std::setlocale(LC_ALL, "");
    applyLanguageOverride();  // $MOSAIC_LANG re-points MESSAGES only; the rest stays system
    bindDomain(domain, localeDir);
    textdomain(domain);  // make `domain` the default for _()/gettext
#endif
}

const char* activeLanguageOverride() { return g_languageOverride.c_str(); }

void initDomain([[maybe_unused]] const char* domain, [[maybe_unused]] const char* localeDir) {
#if MOSAIC_HAVE_GETTEXT
    // Bind the satellite catalog only; the default domain set by init() is left untouched so _()
    // still resolves against "mosaic". This domain is reached explicitly via dtr().
    if (domain != nullptr && *domain != '\0') {
        bindDomain(domain, localeDir);
    }
#endif
}

const char* tr(const char* msgid) {
#if MOSAIC_HAVE_GETTEXT
    return msgid != nullptr ? ::gettext(msgid) : msgid;
#else
    return msgid;
#endif
}

const char* dtr([[maybe_unused]] const char* domain, const char* msgid) {
#if MOSAIC_HAVE_GETTEXT
    if (msgid == nullptr || domain == nullptr)
        return msgid;
    const char* out = ::dgettext(domain, msgid);
    return out != nullptr ? out : msgid; // dgettext never should, but never hand back null
#else
    return msgid;
#endif
}

}  // namespace mosaic::common::i18n
