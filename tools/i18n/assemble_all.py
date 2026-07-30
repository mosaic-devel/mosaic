#!/usr/bin/env python3
"""Assemble every po/<lang>/mosaic.po from a directory of `<lang>.tsv` drafts, then verify.

    tools/i18n/assemble_all.py po/mosaic.pot --drafts <dir> --po-dir po

Reports per-language coverage and every rejected translation, then runs `msgfmt --check` over the
result so the catalogs are known-good before they are committed rather than at build time.
"""

import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import assemble_po  # noqa: E402
import languages  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pot", type=Path)
    ap.add_argument("--drafts", type=Path, required=True)
    ap.add_argument("--po-dir", type=Path, required=True)
    ap.add_argument("--date", default="2026-07-24 12:00+0000")
    args = ap.parse_args()

    total_filled = 0
    rejected_total = 0
    missing = []
    rows = []

    for lang in languages.CODES:
        tsv = args.drafts / f"{lang}.tsv"
        if not tsv.exists():
            missing.append(lang)
            continue
        translations = assemble_po.read_tsv(tsv)
        text, stats, problems = assemble_po.build(args.pot, lang, translations, args.date)
        out = args.po_dir / lang / "mosaic.po"
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text, encoding="utf-8")

        total_filled += stats["filled"]
        rejected_total += len(problems)
        rows.append((lang, stats, problems))

    print(f"{'lang':<14}{'filled':>7}{'rejected':>10}{'==english':>11}")
    for lang, stats, problems in rows:
        flag = "  <-- check" if stats["identical"] > 40 else ""
        print(f"{lang:<14}{stats['filled']:>7}{len(problems):>10}{stats['identical']:>11}{flag}")
        for p in problems:
            print(f"    ! {p}")

    print(f"\n{len(rows)} catalogs, {total_filled} strings, {rejected_total} rejected")
    if missing:
        print(f"MISSING drafts ({len(missing)}): {' '.join(missing)}")

    # Independent second opinion: msgfmt's own format/header checks.
    bad = []
    for lang, _stats, _problems in rows:
        po = args.po_dir / lang / "mosaic.po"
        rc = subprocess.run(["msgfmt", "--check", "-o", "/dev/null", str(po)],
                            capture_output=True, text=True)
        if rc.returncode != 0:
            bad.append((lang, rc.stderr.strip()))
    if bad:
        print("\nmsgfmt --check FAILED:")
        for lang, err in bad:
            print(f"  {lang}: {err}")
        return 1
    print("msgfmt --check: all catalogs OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
