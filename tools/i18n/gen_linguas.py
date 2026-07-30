#!/usr/bin/env python3
"""Regenerate po/LINGUAS from the language table. Run after adding a language."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import languages  # noqa: E402

HEADER = """\
# The languages Mosaic ships catalogs for, one per line, in the order gettext tools expect.
#
# GENERATED from tools/i18n/languages.py -- edit that table, not this file, then run:
#     tools/i18n/gen_linguas.py
# The set matches Krita's, minus `tok` (see languages.py for why).
"""


def main():
    out = Path(__file__).resolve().parents[2] / "po" / "LINGUAS"
    out.write_text(HEADER + "\n".join(languages.CODES) + "\n", encoding="utf-8")
    print(f"{out}: {len(languages.CODES)} languages")


if __name__ == "__main__":
    main()
