#!/usr/bin/env python3
"""Build po/<lang>/mosaic.po from the template plus a translator's `id<TAB>translation` file.

Translators (human or otherwise) never hand-write .po syntax. They fill in a numbered TSV; this
script owns the framing -- header, Plural-Forms, source locations, c-format flags, line wrapping --
so a catalog cannot be malformed by a stray quote or a dropped continuation line.

It is also the safety gate. A translation is **rejected** (left empty, so gettext falls back to the
English msgid) when it would break something at runtime:

  * a printf specifier set that differs from the English -- a format/vararg mismatch is a crash,
    not a typo;
  * a different number of unescaped '/' in an FLTK menu path -- that silently reparents menu items;
  * an escape sequence .po cannot represent.

Rejections are reported, never silently swallowed. Everything not in the TSV stays `msgstr ""`,
which is what a partial catalog looks like and is perfectly valid.

    tools/i18n/assemble_po.py po/mosaic.pot --lang de --tsv drafts/de.tsv --out po/de/mosaic.po
"""

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import languages  # noqa: E402
import potfile  # noqa: E402
from core_worklist import FORMAT_SPEC, menu_segments, is_menu_path  # noqa: E402

PROJECT_VERSION = "0.2.17"
BUGS_URL = "https://github.com/mosaic-devel/mosaic/issues"

# The escapes msgfmt accepts in a .po string. Anything else is a malformed translation.
VALID_ESCAPE = re.compile(r'\\[ntr"\\abfv]')
ANY_BACKSLASH = re.compile(r"\\.")


def normalise(text):
    """Repair the one mistake worth repairing -- a bare `"` -- and reject the rest.

    Returns (fixed_text, error_or_None). A bare double quote is unambiguous: it can only have been
    meant literally, so escaping it is safe. An unknown backslash escape is *not* unambiguous
    (`\\d` could be a typo or intended literally), so it is refused rather than guessed at.
    """
    out, i = [], 0
    while i < len(text):
        ch = text[i]
        if ch == "\\":
            if i + 1 >= len(text):
                return None, "trailing backslash"
            pair = text[i:i + 2]
            if pair == "\\/":      # escaped slash inside an FLTK menu label -- keep verbatim
                out.append(pair)
            elif VALID_ESCAPE.fullmatch(pair):
                out.append(pair)
            else:
                return None, f"unsupported escape {pair!r}"
            i += 2
            continue
        if ch == '"':
            out.append('\\"')      # bare quote -> escaped quote
        elif ch in "\n\r\t":
            out.append({"\n": "\\n", "\r": "\\r", "\t": "\\t"}[ch])
        else:
            out.append(ch)
        i += 1
    return "".join(out), None


# Decompose one specifier so its ARGUMENT TYPE can be compared. Flags, width and precision do not
# change what is popped off the vararg list; the length modifier and conversion do.
SPEC_PARTS = re.compile(
    r"%(?:(?P<pos>\d+)\$)?[-+ #0']*(?:\d+|\*)?(?:\.(?:\d+|\*))?"
    r"(?P<len>hh|h|ll|l|L|q|j|z|Z|t)?(?P<conv>[diouxXeEfFgGaAcspn])")


def spec_type(spec):
    """The vararg type a specifier consumes: "%.1f" -> "f", "%zu" -> "zu", "%u" -> "u"."""
    m = SPEC_PARTS.fullmatch(spec)
    if m is None:
        return spec
    return (m.group("len") or "") + m.group("conv")


def _specifier_matches(text):
    return [m for m in FORMAT_SPEC.finditer(text) if m.group(0) != "%%"]


def fix_format(english, translated):
    """Make the translation's format specifiers positionally safe. Returns (fixed, error).

    Reordering `%zu ... %s` into `%s ... %zu` is the single most common thing a translator does to
    a format string, because word order differs between languages. It is also the most dangerous:
    the specifiers still *look* right, but printf pops a `const char*` where the caller pushed a
    `size_t`. My earlier multiset check waved that through; msgfmt did not, which is how it was
    caught.

    gettext's answer is positional specifiers (`%1$zu`, `%2$s`), so rather than reject a perfectly
    good translation for having natural word order, rewrite it into that form: match each
    translated specifier to the English specifier of the same type, lowest unused index first.
    Ambiguity between two same-type specifiers is harmless -- they are interchangeable by
    definition. Anything that cannot be mapped is still rejected.
    """
    eng = [m.group(0) for m in _specifier_matches(english)]
    hits = _specifier_matches(translated)
    got = [m.group(0) for m in hits]

    if not eng and not got:
        return translated, None

    # A translation that is already positional is the translator's explicit intent; validate the
    # indices and types, then leave it exactly as written.
    if any(SPEC_PARTS.fullmatch(s) and SPEC_PARTS.fullmatch(s).group("pos") for s in got):
        for spec in got:
            m = SPEC_PARTS.fullmatch(spec)
            if m is None or not m.group("pos"):
                return None, f"mixes positional and non-positional specifiers: {got}"
            idx = int(m.group("pos"))
            if not 1 <= idx <= len(eng):
                return None, f"positional argument {idx} out of range (english has {len(eng)})"
            if spec_type(spec) != spec_type(eng[idx - 1]):
                return None, (f"argument {idx} is {spec_type(spec)}, "
                              f"english has {spec_type(eng[idx - 1])}")
        return translated, None

    if sorted(map(spec_type, eng)) != sorted(map(spec_type, got)):
        return None, f"format specifiers differ: {eng} vs {got}"

    if [spec_type(s) for s in eng] == [spec_type(s) for s in got]:
        return translated, None  # same order, so plain specifiers are already correct

    out, last, used = [], 0, set()
    for m in hits:
        want = spec_type(m.group(0))
        idx = next((i for i, e in enumerate(eng) if spec_type(e) == want and i not in used), None)
        if idx is None:
            return None, f"cannot map {m.group(0)} onto {eng}"
        used.add(idx)
        out.append(translated[last:m.start()])
        out.append(f"%{idx + 1}${m.group(0)[1:]}")
        last = m.end()
    out.append(translated[last:])
    return "".join(out), None


def check(english, translated):
    """Non-format safety checks. Returns an error string, or None when the translation is fine."""
    if is_menu_path(english):
        want, got = len(menu_segments(english)), len(menu_segments(translated))
        if want != got:
            return f"menu path has {got} segments, expected {want}"
    return None


def read_tsv(path):
    """`id<TAB>translation` per line; blanks and `#` comments ignored. Later wins on duplicates."""
    out = {}
    if not path.exists():
        return out
    with open(path, encoding="utf-8") as fh:
        for lineno, raw in enumerate(fh, 1):
            line = raw.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            if "\t" not in line:
                print(f"  {path.name}:{lineno}: no tab separator, skipped", file=sys.stderr)
                continue
            key, value = line.split("\t", 1)
            key = key.strip()
            if not key.isdigit():
                print(f"  {path.name}:{lineno}: id {key!r} is not a number, skipped",
                      file=sys.stderr)
                continue
            if value.strip():
                out[int(key)] = value  # NOT stripped -- see match_edge_space()
    return out


def match_edge_space(english, translated):
    """Give the translation exactly the English's leading/trailing spaces.

    Several msgids are sentence fragments glued to a value at runtime -- "Version ", "by ",
    "Add " -- where the trailing space is load-bearing. Translators drop it constantly, and TSV
    round-trips eat it. Rather than trust either side, restate the English's edge whitespace on
    the translation: it removes stray padding *and* restores what was meant to be there.
    """
    if not english.strip():
        return translated
    lead = english[:len(english) - len(english.lstrip(" "))]
    trail = english[len(english.rstrip(" ")):]
    return lead + translated.strip() + trail


def build(pot, lang, translations, revision_date):
    entries = potfile.parse(pot)
    eng_name, _endonym, plural, rtl = languages.LANGUAGES[lang]

    lines = [
        f"# Mosaic -- {eng_name} translation.",
        "# Copyright (C) 2026 The Mosaic contributors",
        "# This file is distributed under the same license as the Mosaic package.",
        "#",
        "# Machine-assisted first draft (S54). It is a starting point, not a finished",
        "# translation: corrections from native speakers are welcome and expected. Entries",
        '# left as msgstr "" fall back to the English text, which is intended.',
        "#",
        'msgid ""',
        'msgstr ""',
        f'"Project-Id-Version: Mosaic {PROJECT_VERSION}\\n"',
        f'"Report-Msgid-Bugs-To: {BUGS_URL}\\n"',
        f'"POT-Creation-Date: {potfile.header_date(pot)}\\n"',
        f'"PO-Revision-Date: {revision_date}\\n"',
        '"Last-Translator: Mosaic i18n bootstrap <noreply@localhost>\\n"',
        f'"Language-Team: {eng_name}\\n"',
        f'"Language: {lang}\\n"',
        '"MIME-Version: 1.0\\n"',
        '"Content-Type: text/plain; charset=UTF-8\\n"',
        '"Content-Transfer-Encoding: 8bit\\n"',
        f'"Plural-Forms: {plural}\\n"',
    ]
    if rtl:
        # Not consumed by gettext; a marker for anyone auditing bidi coverage later.
        lines.append('"X-Text-Direction: rtl\\n"')
    lines.append("")

    stats = {"filled": 0, "empty": 0, "rejected": 0, "identical": 0}
    problems = []
    for e in entries:
        msgstr = ""
        raw = translations.get(e.index)
        if raw:
            fixed, err = normalise(match_edge_space(e.msgid, raw))
            # Format rules apply only where xgettext proved the string reaches printf. Applying
            # them to every string misfires on plain labels -- "50% Gray" scans as the conversion
            # "% G" -- and there is no runtime hazard to guard against when nothing formats it.
            if err is None and "c-format" in e.flags:
                fixed, err = fix_format(e.msgid, fixed)
            if err is None:
                err = check(e.msgid, fixed)
            if err:
                problems.append(f"#{e.index} {e.msgid[:60]!r}: {err}")
                stats["rejected"] += 1
            else:
                msgstr = fixed
                stats["filled"] += 1
                if fixed == e.msgid:
                    stats["identical"] += 1
        if not msgstr:
            stats["empty"] += 1

        for loc_line in _wrap_locations(e.locations):
            lines.append(loc_line)
        if e.flags:
            lines.append("#, " + ", ".join(e.flags))
        lines.append(potfile.wrap_po_string("msgid", e.msgid))
        lines.append(potfile.wrap_po_string("msgstr", msgstr))
        lines.append("")

    return "\n".join(lines), stats, problems


def _wrap_locations(locations, width=76):
    """`#:` lines, wrapped the way xgettext wraps them so diffs stay small."""
    out, cur = [], "#:"
    for loc in locations:
        if len(cur) + 1 + len(loc) > width and cur != "#:":
            out.append(cur)
            cur = "#:"
        cur += " " + loc
    if cur != "#:":
        out.append(cur)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pot", type=Path)
    ap.add_argument("--lang", required=True)
    ap.add_argument("--tsv", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--date", default="2026-07-24 12:00+0000")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    if args.lang not in languages.LANGUAGES:
        sys.exit(f"unknown language {args.lang!r} (not in tools/i18n/languages.py)")

    translations = read_tsv(args.tsv)
    text, stats, problems = build(args.pot, args.lang, translations, args.date)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(text, encoding="utf-8")

    if not args.quiet:
        print(f"{args.lang:<14} filled {stats['filled']:>4}  empty {stats['empty']:>4}"
              f"  rejected {stats['rejected']:>3}  same-as-english {stats['identical']:>3}")
        for p in problems:
            print(f"    ! {p}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
