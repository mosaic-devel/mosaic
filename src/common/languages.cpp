// The languages Mosaic ships catalogs for, with the names the Settings language picker shows.
//
// GENERATED from tools/i18n/languages.py -- edit that table, not this file, then run:
//     tools/i18n/gen_language_table.py
// The twin of po/LINGUAS (gen_linguas.py), which carries the same set without the display names.
#include "common/languages.hpp"

#include <algorithm>

namespace mosaic::common {
namespace {

// Sorted by `code` so findLanguage can binary-search; the picker sorts by name itself.
constexpr LanguageInfo kLanguages[] = {
    {"af", "Afrikaans", "Afrikaans", false},
    {"ar", "Arabic", "العربية", true},
    {"be", "Belarusian", "беларуская", false},
    {"bg", "Bulgarian", "български", false},
    {"br", "Breton", "brezhoneg", false},
    {"bs", "Bosnian", "bosanski", false},
    {"ca", "Catalan", "català", false},
    {"ca@valencia", "Valencian", "valencià", false},
    {"cs", "Czech", "čeština", false},
    {"cy", "Welsh", "Cymraeg", false},
    {"da", "Danish", "dansk", false},
    {"de", "German", "Deutsch", false},
    {"el", "Greek", "Ελληνικά", false},
    {"en_GB", "British English", "British English", false},
    {"eo", "Esperanto", "Esperanto", false},
    {"es", "Spanish", "español", false},
    {"et", "Estonian", "eesti", false},
    {"eu", "Basque", "euskara", false},
    {"fa", "Persian", "فارسی", true},
    {"fi", "Finnish", "suomi", false},
    {"fr", "French", "français", false},
    {"fy", "Western Frisian", "Frysk", false},
    {"ga", "Irish", "Gaeilge", false},
    {"gl", "Galician", "galego", false},
    {"he", "Hebrew", "עברית", true},
    {"hi", "Hindi", "हिन्दी", false},
    {"hne", "Chhattisgarhi", "छत्तीसगढ़ी", false},
    {"hr", "Croatian", "hrvatski", false},
    {"hu", "Hungarian", "magyar", false},
    {"ia", "Interlingua", "interlingua", false},
    {"id", "Indonesian", "Indonesia", false},
    {"is", "Icelandic", "íslenska", false},
    {"it", "Italian", "italiano", false},
    {"ja", "Japanese", "日本語", false},
    {"ka", "Georgian", "ქართული", false},
    {"kk", "Kazakh", "қазақ тілі", false},
    {"km", "Khmer", "ភាសាខ្មែរ", false},
    {"ko", "Korean", "한국어", false},
    {"lt", "Lithuanian", "lietuvių", false},
    {"lv", "Latvian", "latviešu", false},
    {"mai", "Maithili", "मैथिली", false},
    {"mk", "Macedonian", "македонски", false},
    {"mr", "Marathi", "मराठी", false},
    {"ms", "Malay", "Bahasa Melayu", false},
    {"nb", "Norwegian Bokmål", "norsk bokmål", false},
    {"nds", "Low German", "Plattdüütsch", false},
    {"ne", "Nepali", "नेपाली", false},
    {"nl", "Dutch", "Nederlands", false},
    {"nn", "Norwegian Nynorsk", "norsk nynorsk", false},
    {"oc", "Occitan", "occitan", false},
    {"pa", "Punjabi", "ਪੰਜਾਬੀ", false},
    {"pl", "Polish", "polski", false},
    {"pt", "Portuguese", "português", false},
    {"pt_BR", "Brazilian Portuguese", "português do Brasil", false},
    {"ro", "Romanian", "română", false},
    {"ru", "Russian", "русский", false},
    {"se", "Northern Sami", "davvisámegiella", false},
    {"sk", "Slovak", "slovenčina", false},
    {"sl", "Slovenian", "slovenščina", false},
    {"sq", "Albanian", "shqip", false},
    {"sv", "Swedish", "svenska", false},
    {"ta", "Tamil", "தமிழ்", false},
    {"tg", "Tajik", "тоҷикӣ", false},
    {"th", "Thai", "ไทย", false},
    {"tr", "Turkish", "Türkçe", false},
    {"ug", "Uyghur", "ئۇيغۇرچە", true},
    {"uk", "Ukrainian", "українська", false},
    {"uz", "Uzbek", "oʻzbekcha", false},
    {"uz@cyrillic", "Uzbek (Cyrillic)", "ўзбекча", false},
    {"vi", "Vietnamese", "Tiếng Việt", false},
    {"wa", "Walloon", "walon", false},
    {"xh", "Xhosa", "isiXhosa", false},
    {"zh_CN", "Chinese (Simplified)", "简体中文", false},
    {"zh_TW", "Chinese (Traditional)", "繁體中文", false},
};

} // namespace

std::span<const LanguageInfo> knownLanguages() {
    return kLanguages;
}

const LanguageInfo* findLanguage(std::string_view code) {
    const auto* it = std::lower_bound(
        std::begin(kLanguages), std::end(kLanguages), code,
        [](const LanguageInfo& l, std::string_view c) { return std::string_view(l.code) < c; });
    return it != std::end(kLanguages) && std::string_view(it->code) == code ? it : nullptr;
}

} // namespace mosaic::common
