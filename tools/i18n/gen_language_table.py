#!/usr/bin/env python3
"""Regenerate src/common/languages.cpp from the language table.

The C++ side needs the display names (English name + endonym) so Settings -> General can
offer a language list; po/LINGUAS carries only the codes. Same source of truth, same rule as
gen_linguas.py: edit tools/i18n/languages.py, then run this.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import languages  # noqa: E402

HEADER = """\
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
"""

FOOTER = """\
};

}  // namespace

std::span<const LanguageInfo> knownLanguages() { return kLanguages; }

const LanguageInfo* findLanguage(std::string_view code) {
    const auto* it = std::lower_bound(std::begin(kLanguages), std::end(kLanguages), code,
                                      [](const LanguageInfo& l, std::string_view c) {
                                          return std::string_view(l.code) < c;
                                      });
    return it != std::end(kLanguages) && std::string_view(it->code) == code ? it : nullptr;
}

}  // namespace mosaic::common
"""


def main():
    rows = []
    for code in sorted(languages.LANGUAGES):
        english, endonym, _plural, rtl = languages.LANGUAGES[code]
        rows.append(f'    {{"{code}", "{english}", "{endonym}", {"true" if rtl else "false"}}},')
    out = Path(__file__).resolve().parents[2] / "src" / "common" / "languages.cpp"
    out.write_text(HEADER + "\n".join(rows) + "\n" + FOOTER, encoding="utf-8")
    print(f"{out}: {len(rows)} languages")


if __name__ == "__main__":
    main()
