#!/usr/bin/env python3
"""Emit the translator work list: the core UI strings, numbered, with per-string constraints.

The **core** is every extracted string except those used *only* by the two specialist dialogs --
the Texture Generator and the Settings long-form help. A string that also appears in a core file
(the ubiquitous "Cancel", say) stays in the core. That rule is deliberately mechanical so a future
contributor can widen coverage by editing one set below rather than re-curating 1000 strings.

Everything outside the core still ships in each catalog as `msgstr ""`, which gettext resolves to
the English msgid -- a partial catalog is a first-class thing, not a broken one.

    tools/i18n/core_worklist.py po/mosaic.pot -o <dir>/STRINGS.tsv

Output is TSV: id, flags, context, english (escaped exactly as the .po needs it).
"""

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import potfile  # noqa: E402

# Strings reachable ONLY from these files are out of scope for the first translation wave.
DEFERRED = {
    "src/ui/texture_generator_dialog.cpp",
    "src/ui/settings_dialog.cpp",
}

# printf-style specifiers. Every one in the English must reappear in the translation, or the
# format string and its varargs disagree at runtime -- which is a crash, not a cosmetic bug.
FORMAT_SPEC = re.compile(
    r"%(?:\d+\$)?[-+ #0']*(?:\d+|\*)?(?:\.(?:\d+|\*))?"
    r"(?:hh|h|ll|l|L|q|j|z|Z|t)?[diouxXeEfFgGaAcspn%]")


def specifiers(text):
    return [m for m in FORMAT_SPEC.findall(text) if m != "%%"]


def is_menu_path(text):
    """A FLTK menu path: '/'-separated, with '\\/' meaning a literal slash inside a label."""
    return "&" in text and re.search(r"(?<!\\)/", text) is not None


def menu_segments(text):
    """Split on unescaped '/' -- the separator count a translation must preserve."""
    return re.split(r"(?<!\\)/", text)


def classify(entry):
    """Compact, machine-checkable constraints for one string."""
    text = entry.msgid
    flags = []
    if is_menu_path(text):
        flags.append(f"menu:{len(menu_segments(text))}")
    elif "&" in text:
        flags.append("accel")
    # Only strings xgettext marked c-format actually reach a printf-family call. Trusting the
    # regex alone misfires on plain labels: "50% Gray" scans as the conversion "% G" (space flag,
    # G conversion), and demanding a translation preserve that nonsense token made several
    # translators drop the string rather than mangle their word for grey. xgettext does the real
    # analysis -- it knows the call site -- so defer to it. Only 48 of 1021 entries qualify.
    if "c-format" in entry.flags:
        specs = specifiers(text)
        if specs:
            flags.append("fmt:" + " ".join(specs))
    if "\\n" in text:
        flags.append("multiline")
    return ",".join(flags) or "-"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pot", type=Path)
    ap.add_argument("-o", "--output", type=Path, required=True)
    ap.add_argument("--all", action="store_true",
                    help="ignore the core rule and list every extracted string (wave 2)")
    args = ap.parse_args()

    entries = potfile.parse(args.pot)
    rows = []
    for e in entries:
        if not args.all and e.files and all(f in DEFERRED for f in e.files):
            continue
        # Context: the widget the string belongs to, which is what a translator actually needs to
        # pick between e.g. "Open" the verb and "Open" the adjective.
        ctx = Path(e.files[0]).stem if e.files else "?"
        rows.append((e.index, classify(e), ctx, e.msgid))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with open(args.output, "w", encoding="utf-8") as fh:
        fh.write("# id\tflags\tcontext\tenglish\n")
        for idx, flags, ctx, text in rows:
            fh.write(f"{idx}\t{flags}\t{ctx}\t{text}\n")

    chars = sum(len(r[3]) for r in rows)
    print(f"{len(rows)} strings ({chars} chars) of {len(entries)} extracted -> {args.output}")


if __name__ == "__main__":
    main()
