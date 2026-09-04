#pragma once

// Internationalization (GNU gettext). User-facing strings are wrapped in _() at the point of
// use (or marked with N_() where defined far from where they are shown) so xgettext can
// extract them into po/mosaic.pot; a translator drops in a .po, we compile it to a .mo under
// the locale dir, and the same binary speaks another language. English needs no catalog --
// the msgid *is* the English text. See docs/i18n.md.
//
// The gettext call lives behind tr() in the common module, so only common links libintl;
// every other module simply includes this header and uses _(). The macros use xgettext's
// default keywords (_ , N_), so extraction works regardless of how they expand.

#include <string>
#include <vector>

namespace mosaic::common::i18n {

// Set the message locale from the environment and bind the textdomain to its catalog dir.
// `domain` is the gettext domain (the .mo basename, default "mosaic"). `localeDir` is where
// <lang>/LC_MESSAGES/<domain>.mo live; nullptr/empty uses the MOSAIC_LOCALEDIR environment
// variable if set, else the compiled-in default. Best-effort and never throws; with no
// catalog present every string stays English.
//
// $MOSAIC_LANG overrides the UI language for this run alone (see applyLanguageOverride in the
// .cpp for why it is not simply $LANGUAGE):
//     MOSAIC_LANG=de mosaic          # German UI
//     MOSAIC_LANG=ca@valencia:ca     # Valencian, falling back to Catalan
//     MOSAIC_LANG=en mosaic          # force the untranslated English source strings
// It moves MESSAGES only -- number, date and collation formatting stay on the system locale,
// because the target locale may well not be generated on this machine.
//
// `preferred` is the SAVED language (Settings -> General; empty = follow the system locale). It
// takes the same values as $MOSAIC_LANG and travels the same code path -- but $MOSAIC_LANG wins,
// because a variable set for one run is a more specific instruction than a stored preference.
// It must be passed HERE rather than applied later: gettext resolves the catalog once, so a
// language chosen after the first _() would leave already-looked-up strings in the old one.
void init(const char* domain = "mosaic", const char* localeDir = nullptr,
          const char* preferred = nullptr);

// The language init() actually selected: the $MOSAIC_LANG value (or, failing that, init()'s
// `preferred` argument) when an override is in force, otherwise empty (meaning "whatever the
// system locale asked for"). Diagnostics only.
[[nodiscard]] const char* activeLanguageOverride();

// The languages a catalog is actually INSTALLED for, as gettext selectors ("de", "pt_BR",
// "ca@valencia"), sorted. Found by scanning the catalog directory (resolved exactly as init()
// does) for `<code>/LC_MESSAGES/<domain>.mo`. Empty when there is no catalog directory -- an
// uninstalled build with no $MOSAIC_LOCALEDIR, or a build without gettext -- which is the honest
// answer: a language picker must not offer a translation that cannot load. Pair it with
// common::knownLanguages() (common/languages.hpp) for the display names.
[[nodiscard]] std::vector<std::string> installedLanguages(const char* domain = "mosaic",
                                                          const char* localeDir = nullptr);

// Translate `msgid` via the active domain, or return it unchanged (English / no catalog /
// built without gettext / nullptr). The returned pointer is owned by gettext and stable.
[[nodiscard]] const char* tr(const char* msgid);

// Bind an ADDITIONAL gettext domain's catalog directory (+ UTF-8 codeset) without changing the
// active default domain that init() selected. Used for satellite catalogs that should not live in
// the main "mosaic" .mo -- currently the "motivate" domain (the Annoyances one-liners, kept out of
// po/mosaic.pot so 100 jokes never burden the main translation). `localeDir` resolves like init()'s
// (arg, else $MOSAIC_LOCALEDIR, else the compiled-in default). Best-effort; never throws.
void initDomain(const char* domain, const char* localeDir = nullptr);

// Translate `msgid` through a SPECIFIC domain (dgettext) rather than the active default one -- the
// read side of initDomain(). Returns `msgid` unchanged with no gettext / no catalog / a null msgid.
// The returned pointer is owned by gettext and stable.
[[nodiscard]] const char* dtr(const char* domain, const char* msgid);

}  // namespace mosaic::common::i18n

// _(): mark for extraction AND translate now. N_(): mark for extraction only (translate later
// with _()). Guarded so a host that already defines _ wins rather than causing a redefinition.
#ifndef _
#  define _(msgid) ::mosaic::common::i18n::tr(msgid)
#endif
#ifndef N_
#  define N_(msgid) (msgid)
#endif
