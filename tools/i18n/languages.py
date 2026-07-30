"""The languages Mosaic ships catalogs for, with the metadata a .po header needs.

This is the single source of truth: po/LINGUAS is generated from it, the translator work
lists are generated from it, and assemble_po.py stamps each catalog's header from it. Adding
a language means adding one row here and regenerating (see docs/i18n.md).

The set matches Krita's, minus `tok` (Toki Pona): a 137-word constructed language cannot carry
terms like "Levels" or "Chromatic Aberration" without coining vocabulary that no future
contributor would agree with, so it is left to someone who actually speaks it.

Row: code -> (English name, endonym, Plural-Forms, RTL?)

`Plural-Forms` is unused today -- Mosaic has no ngettext() call sites, it sidesteps plurals with
"%zu layer(s)" -- but msgfmt wants a well-formed header and the formulas are needed the moment a
plural msgid appears. Values follow the gettext manual / KDE practice.
"""

# Common plural formulas, named so the table below stays readable.
ONE = "nplurals=1; plural=0;"
GERMANIC = "nplurals=2; plural=(n != 1);"
ROMANCE = "nplurals=2; plural=(n > 1);"
SLAVIC3 = ("nplurals=3; plural=(n%10==1 && n%100!=11 ? 0 : n%10>=2 && n%10<=4 "
           "&& (n%100<10 || n%100>=20) ? 1 : 2);")

LANGUAGES = {
    # code            English name        endonym                plural            rtl
    "af":            ("Afrikaans",        "Afrikaans",           GERMANIC,         False),
    "ar":            ("Arabic",           "العربية",              "nplurals=6; plural=(n==0 ? 0 : n==1 ? 1 : n==2 ? 2 : n%100>=3 && n%100<=10 ? 3 : n%100>=11 ? 4 : 5);", True),
    "be":            ("Belarusian",       "беларуская",          SLAVIC3,          False),
    "bg":            ("Bulgarian",        "български",           GERMANIC,         False),
    "br":            ("Breton",           "brezhoneg",           ROMANCE,          False),
    "bs":            ("Bosnian",          "bosanski",            SLAVIC3,          False),
    "ca":            ("Catalan",          "català",              GERMANIC,         False),
    "ca@valencia":   ("Valencian",        "valencià",            GERMANIC,         False),
    "cs":            ("Czech",            "čeština",             "nplurals=3; plural=(n==1) ? 0 : (n>=2 && n<=4) ? 1 : 2;", False),
    "cy":            ("Welsh",            "Cymraeg",             "nplurals=4; plural=(n==1) ? 0 : (n==2) ? 1 : (n != 8 && n != 11) ? 2 : 3;", False),
    "da":            ("Danish",           "dansk",               GERMANIC,         False),
    "de":            ("German",           "Deutsch",             GERMANIC,         False),
    "el":            ("Greek",            "Ελληνικά",            GERMANIC,         False),
    "en_GB":         ("British English",  "British English",     GERMANIC,         False),
    "eo":            ("Esperanto",        "Esperanto",           GERMANIC,         False),
    "es":            ("Spanish",          "español",             GERMANIC,         False),
    "et":            ("Estonian",         "eesti",               GERMANIC,         False),
    "eu":            ("Basque",           "euskara",             GERMANIC,         False),
    "fa":            ("Persian",          "فارسی",                ROMANCE,          True),
    "fi":            ("Finnish",          "suomi",               GERMANIC,         False),
    "fr":            ("French",           "français",            ROMANCE,          False),
    "fy":            ("Western Frisian",  "Frysk",               GERMANIC,         False),
    "ga":            ("Irish",            "Gaeilge",             "nplurals=5; plural=(n==1 ? 0 : n==2 ? 1 : n<7 ? 2 : n<11 ? 3 : 4);", False),
    "gl":            ("Galician",         "galego",              GERMANIC,         False),
    "he":            ("Hebrew",           "עברית",                GERMANIC,         True),
    "hi":            ("Hindi",            "हिन्दी",                  GERMANIC,         False),
    "hne":           ("Chhattisgarhi",    "छत्तीसगढ़ी",              GERMANIC,         False),
    "hr":            ("Croatian",         "hrvatski",            SLAVIC3,          False),
    "hu":            ("Hungarian",        "magyar",              GERMANIC,         False),
    "ia":            ("Interlingua",      "interlingua",         GERMANIC,         False),
    "id":            ("Indonesian",       "Indonesia",           ONE,              False),
    "is":            ("Icelandic",        "íslenska",            "nplurals=2; plural=(n%10!=1 || n%100==11);", False),
    "it":            ("Italian",          "italiano",            GERMANIC,         False),
    "ja":            ("Japanese",         "日本語",                ONE,              False),
    "ka":            ("Georgian",         "ქართული",              GERMANIC,         False),
    "kk":            ("Kazakh",           "қазақ тілі",          GERMANIC,         False),
    "km":            ("Khmer",            "ភាសាខ្មែរ",               ONE,              False),
    "ko":            ("Korean",           "한국어",                ONE,              False),
    "lt":            ("Lithuanian",       "lietuvių",            "nplurals=3; plural=(n%10==1 && n%100!=11 ? 0 : n%10>=2 && (n%100<10 || n%100>=20) ? 1 : 2);", False),
    "lv":            ("Latvian",          "latviešu",            "nplurals=3; plural=(n%10==1 && n%100!=11 ? 0 : n != 0 ? 1 : 2);", False),
    "mai":           ("Maithili",         "मैथिली",                 GERMANIC,         False),
    "mk":            ("Macedonian",       "македонски",          "nplurals=2; plural=(n%10==1 && n%100!=11) ? 0 : 1;", False),
    "mr":            ("Marathi",          "मराठी",                GERMANIC,         False),
    "ms":            ("Malay",            "Bahasa Melayu",       ONE,              False),
    "nb":            ("Norwegian Bokmål", "norsk bokmål",        GERMANIC,         False),
    "nds":           ("Low German",       "Plattdüütsch",        GERMANIC,         False),
    "ne":            ("Nepali",           "नेपाली",                GERMANIC,         False),
    "nl":            ("Dutch",            "Nederlands",          GERMANIC,         False),
    "nn":            ("Norwegian Nynorsk", "norsk nynorsk",      GERMANIC,         False),
    "oc":            ("Occitan",          "occitan",             ROMANCE,          False),
    "pa":            ("Punjabi",          "ਪੰਜਾਬੀ",                GERMANIC,         False),
    "pl":            ("Polish",           "polski",              "nplurals=3; plural=(n==1 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);", False),
    "pt":            ("Portuguese",       "português",           GERMANIC,         False),
    "pt_BR":         ("Brazilian Portuguese", "português do Brasil", ROMANCE,      False),
    "ro":            ("Romanian",         "română",              "nplurals=3; plural=(n==1 ? 0 : (n==0 || (n%100 > 0 && n%100 < 20)) ? 1 : 2);", False),
    "ru":            ("Russian",          "русский",             SLAVIC3,          False),
    "se":            ("Northern Sami",    "davvisámegiella",     "nplurals=3; plural=(n==1 ? 0 : n==2 ? 1 : 2);", False),
    "sk":            ("Slovak",           "slovenčina",          "nplurals=3; plural=(n==1) ? 0 : (n>=2 && n<=4) ? 1 : 2;", False),
    "sl":            ("Slovenian",        "slovenščina",         "nplurals=4; plural=(n%100==1 ? 0 : n%100==2 ? 1 : n%100==3 || n%100==4 ? 2 : 3);", False),
    "sq":            ("Albanian",         "shqip",               GERMANIC,         False),
    "sv":            ("Swedish",          "svenska",             GERMANIC,         False),
    "ta":            ("Tamil",            "தமிழ்",                 GERMANIC,         False),
    "tg":            ("Tajik",            "тоҷикӣ",              ROMANCE,          False),
    "th":            ("Thai",             "ไทย",                  ONE,              False),
    "tr":            ("Turkish",          "Türkçe",              ROMANCE,          False),
    "ug":            ("Uyghur",           "ئۇيغۇرچە",              GERMANIC,         True),
    "uk":            ("Ukrainian",        "українська",          SLAVIC3,          False),
    "uz":            ("Uzbek",            "oʻzbekcha",           GERMANIC,         False),
    "uz@cyrillic":   ("Uzbek (Cyrillic)", "ўзбекча",             GERMANIC,         False),
    "vi":            ("Vietnamese",       "Tiếng Việt",          ONE,              False),
    "wa":            ("Walloon",          "walon",               ROMANCE,          False),
    "xh":            ("Xhosa",            "isiXhosa",            GERMANIC,         False),
    "zh_CN":         ("Chinese (Simplified)",  "简体中文",         ONE,              False),
    "zh_TW":         ("Chinese (Traditional)", "繁體中文",         ONE,              False),
}

# Catalogs in a stable, reproducible order (LINGUAS, work-list splits, build rules).
CODES = sorted(LANGUAGES)


def name(code):
    return LANGUAGES[code][0]


def endonym(code):
    return LANGUAGES[code][1]


def plural_forms(code):
    return LANGUAGES[code][2]


def is_rtl(code):
    return LANGUAGES[code][3]
