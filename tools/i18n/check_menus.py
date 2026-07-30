#!/usr/bin/env python3
"""Verify menu-path translation is all-or-nothing per subtree.

FLTK builds its menu tree by splitting each item's path on '/', so sibling items are joined by
their *shared parent text*. Translate `&File/&New...` but leave `&File/&Open...` alone and the
menu bar grows TWO top-level menus -- a translated one and an English one -- each holding half the
items. Same for a parent whose two children disagree on its spelling.

That failure is invisible to msgfmt (both entries are individually valid) and invisible to a
format-specifier check. It only shows up when someone opens the menu. So check it directly:

  for every English parent path, every child must agree on that parent's translation, and a
  parent must not be left half-translated.

    tools/i18n/check_menus.py po --pot po/mosaic.pot
"""

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import languages  # noqa: E402
import potfile  # noqa: E402
from core_worklist import is_menu_path, menu_segments  # noqa: E402


def parents(segments):
    """Every proper prefix of a path, as tuples: ('&File',), ('&File', '&Open &Recent') ..."""
    return [tuple(segments[:i]) for i in range(1, len(segments))]


def check_language(po_path, menu_msgids):
    """Return a list of human-readable problems for one catalog."""
    entries = potfile.parse(po_path)
    translated = {}
    for e in entries:
        if e.msgid not in menu_msgids:
            continue
        # Re-read the msgstr for this entry from the file; potfile.parse keeps only msgids, so
        # pull the pair straight out of the text for the ones we care about.
        translated[e.msgid] = None

    text = po_path.read_text(encoding="utf-8")
    for block in text.split("\n\n"):
        mi = re.search(r'^msgid ((?:"(?:[^"\\]|\\.)*"\s*)+)', block, re.M)
        ms = re.search(r'^msgstr ((?:"(?:[^"\\]|\\.)*"\s*)+)', block, re.M)
        if not mi or not ms:
            continue
        def join(m):
            return "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1)))
        msgid, msgstr = join(mi), join(ms)
        if msgid in translated:
            translated[msgid] = msgstr or None

    # english parent tuple -> {translated parent text -> [msgids]}
    seen = {}
    for msgid in menu_msgids:
        eng = menu_segments(msgid)
        got = translated.get(msgid)
        loc = menu_segments(got) if got else None
        for depth, parent in enumerate(parents(eng), start=1):
            if loc is not None and len(loc) == len(eng):
                rendered = tuple(loc[:depth])
            else:
                rendered = parent  # untranslated: FLTK shows the English parent
            seen.setdefault(parent, {}).setdefault(rendered, []).append(msgid)

    problems = []
    for parent, variants in sorted(seen.items()):
        if len(variants) > 1:
            shown = " | ".join("/".join(v) for v in sorted(variants))
            problems.append(f'menu "{"/".join(parent)}" splits into: {shown}')
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("po_dir", type=Path)
    ap.add_argument("--pot", type=Path, required=True)
    args = ap.parse_args()

    menu_msgids = [e.msgid for e in potfile.parse(args.pot) if is_menu_path(e.msgid)]
    print(f"{len(menu_msgids)} menu paths in the template")

    failed = 0
    for lang in languages.CODES:
        po = args.po_dir / lang / "mosaic.po"
        if not po.exists():
            continue
        problems = check_language(po, menu_msgids)
        if problems:
            failed += 1
            print(f"\n{lang}:")
            for p in problems:
                print(f"  ! {p}")
    if failed:
        print(f"\n{failed} language(s) would render a split menu bar")
        return 1
    print("every catalog keeps its menu tree intact")
    return 0


if __name__ == "__main__":
    sys.exit(main())
