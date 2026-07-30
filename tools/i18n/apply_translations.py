#!/usr/bin/env python3
"""Fill in specific msgids across many catalogs, keyed by the ENGLISH TEXT rather than by position.

Use this when a handful of new strings appear and the catalogs already exist -- the normal case
after a UI change. The bulk path (core_worklist.py + assemble_po.py) numbers strings by their
position in the template, which is fine for a from-scratch pass but is NOT replayable afterwards:
adding four strings to one source file shifted 650 of the 1020 positions, so replaying an old
numbered draft against a new template would silently write 650 wrong translations per language.
Keying on the msgid cannot drift.

Workflow for new strings: regenerate the template (`--target pot`), `msgmerge` it into every
catalog (which matches by msgid and leaves existing work alone), then feed the new strings here.

Input TSV, one line per (language, string):

    de<TAB>Open an image<TAB>Bild öffnen

Applies the same safety gate as assemble_po.py -- escape repair, edge-whitespace matching, and
format/menu checks -- and refuses anything it cannot make safe.

    tools/i18n/apply_translations.py po --tsv new.tsv
"""

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import languages  # noqa: E402
import potfile  # noqa: E402
from assemble_po import check, fix_format, match_edge_space, normalise  # noqa: E402

BLOCK_MSGID = re.compile(r'^msgid ((?:"(?:[^"\\]|\\.)*"\s*)+)', re.M)
BLOCK_MSGSTR = re.compile(r'^msgstr ((?:"(?:[^"\\]|\\.)*"\s*)+)', re.M)


def unquote(match):
    return "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', match.group(1)))


def apply_to_catalog(po_path, wanted):
    """wanted: {escaped msgid -> escaped msgstr}. Returns (applied, problems)."""
    text = po_path.read_text(encoding="utf-8")
    blocks = text.split("\n\n")
    applied, problems = 0, []

    for i, block in enumerate(blocks):
        mi = BLOCK_MSGID.search(block)
        ms = BLOCK_MSGSTR.search(block)
        if not mi or not ms:
            continue
        msgid = unquote(mi)
        if msgid not in wanted:
            continue
        raw = wanted[msgid]

        fixed, err = normalise(match_edge_space(msgid, raw))
        if err is None and "c-format" in block.split("msgid")[0]:
            fixed, err = fix_format(msgid, fixed)
        if err is None:
            err = check(msgid, fixed)
        if err:
            problems.append(f"{msgid[:40]!r}: {err}")
            continue

        blocks[i] = block[:ms.start()] + potfile.wrap_po_string("msgstr", fixed) + \
            block[ms.end():].rstrip("\n")
        applied += 1

    if applied:
        po_path.write_text("\n\n".join(blocks), encoding="utf-8")
    return applied, problems


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("po_dir", type=Path)
    ap.add_argument("--tsv", type=Path, required=True)
    args = ap.parse_args()

    by_lang = {}
    for lineno, raw in enumerate(args.tsv.read_text(encoding="utf-8").splitlines(), 1):
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        parts = raw.split("\t")
        if len(parts) < 3:
            print(f"  {args.tsv.name}:{lineno}: need lang<TAB>msgid<TAB>msgstr", file=sys.stderr)
            continue
        lang, msgid, msgstr = parts[0].strip(), parts[1], "\t".join(parts[2:])
        if lang not in languages.LANGUAGES:
            print(f"  {args.tsv.name}:{lineno}: unknown language {lang!r}", file=sys.stderr)
            continue
        if msgstr.strip():
            by_lang.setdefault(lang, {})[msgid] = msgstr

    total, failures = 0, 0
    for lang in languages.CODES:
        if lang not in by_lang:
            continue
        po = args.po_dir / lang / "mosaic.po"
        if not po.exists():
            print(f"  {lang}: no catalog, skipped", file=sys.stderr)
            continue
        applied, problems = apply_to_catalog(po, by_lang[lang])
        total += applied
        missing = len(by_lang[lang]) - applied - len(problems)
        note = f"  ({missing} msgid(s) not found in catalog)" if missing else ""
        print(f"{lang:<14} applied {applied}{note}")
        for p in problems:
            failures += 1
            print(f"    ! {p}")

    print(f"\n{total} translations applied across {len(by_lang)} languages")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
