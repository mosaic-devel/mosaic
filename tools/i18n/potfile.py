"""Minimal .pot/.po reader.

Deliberately not a general gettext parser: it understands exactly the shape `xgettext` emits for
po/mosaic.pot -- `#.`/`#:`/`#,` comment lines, then `msgid`, then `msgstr`, with the usual C string
continuation. That is all the S54 pipeline needs, and it keeps the tooling dependency-free.

Strings are kept in their **escaped** .po form throughout (`\\n`, `\\"`, `\\\\/` stay as the two
characters they are in the file). Nothing in the pipeline ever unescapes and re-escapes them, so
there is no round-trip that could silently corrupt a menu path or a format specifier.
"""

import re

_CONT = re.compile(r'"((?:[^"\\]|\\.)*)"')


class Entry:
    """One catalog entry: the escaped msgid plus the comment lines that introduce it."""

    __slots__ = ("msgid", "locations", "flags", "extracted", "index")

    def __init__(self, msgid, locations, flags, extracted, index):
        self.msgid = msgid          # escaped, exactly as written in the file
        self.locations = locations  # ["src/ui/app_window.cpp:123", ...]
        self.flags = flags          # ["c-format", ...]
        self.extracted = extracted  # "#." translator comments
        self.index = index          # stable 1-based id used by the work lists

    @property
    def files(self):
        return [loc.rsplit(":", 1)[0] for loc in self.locations]


def _join(lines):
    """Concatenate the quoted parts of a msgid/msgstr into one escaped string."""
    return "".join("".join(_CONT.findall(line)) for line in lines)


def parse(path):
    """Read a .pot/.po and return its entries, skipping the header (empty msgid)."""
    entries = []
    locations, flags, extracted = [], [], []
    msgid_lines, state = [], None

    def flush():
        nonlocal locations, flags, extracted, msgid_lines, state
        if state is not None:
            msgid = _join(msgid_lines)
            if msgid:  # the header entry has an empty msgid; it is regenerated, not copied
                entries.append(Entry(msgid, locations, flags, extracted, len(entries) + 1))
        locations, flags, extracted, msgid_lines, state = [], [], [], [], None

    with open(path, encoding="utf-8") as fh:
        for raw in fh:
            line = raw.rstrip("\n")
            if line.startswith("#:"):
                if state is not None:
                    flush()
                locations += line[2:].split()
            elif line.startswith("#,"):
                flags += [f.strip() for f in line[2:].split(",") if f.strip()]
            elif line.startswith("#."):
                extracted.append(line[2:].strip())
            elif line.startswith("msgid "):
                state = "msgid"
                msgid_lines = [line[len("msgid "):]]
            elif line.startswith("msgstr"):
                state = "msgstr"
            elif line.startswith('"') and state == "msgid":
                msgid_lines.append(line)
            elif not line.strip():
                flush()
    flush()
    return entries


def header_date(path):
    """The POT-Creation-Date from a template, so generated catalogs can cite it."""
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            if line.startswith('"POT-Creation-Date:'):
                return line.split(":", 1)[1].rstrip('\\n"\n ').strip()
    return ""


def wrap_po_string(keyword, escaped):
    """Render `keyword "..."` the way msgcat would: one line, or a leading "" plus split lines.

    Splitting only ever happens at an escaped newline, which is where gettext tools break too, so
    the output stays byte-stable when a catalog is later run through msgmerge.
    """
    if len(escaped) + len(keyword) + 3 <= 78 and "\\n" not in escaped:
        return f'{keyword} "{escaped}"'
    parts = [p + "\\n" for p in escaped.split("\\n")]
    if parts[-1] == "\\n":
        parts.pop()
    else:
        parts[-1] = parts[-1][:-2]
    parts = [p for p in parts if p]
    if len(parts) <= 1:
        return f'{keyword} "{escaped}"'
    out = [f'{keyword} ""'] + [f'"{p}"' for p in parts]
    return "\n".join(out)
