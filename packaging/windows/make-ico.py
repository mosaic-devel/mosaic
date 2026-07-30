#!/usr/bin/env python3
"""Build a multi-resolution Windows .ico from one of Mosaic's SVG icons (PLAN.md S57/S59).

Windows reads a DIFFERENT size out of the same .ico depending on where the file appears -- 16 px in
the title bar and the Explorer details view, 32 px on the desktop and in the taskbar, 48 px in
Explorer's "medium icons", 256 px in "extra large icons" and the Alt-Tab switcher -- and it does
NOT rescale for you: a single-size .ico is drawn nearest-neighbour at every other size, which is
exactly the crunchy look that marks a program as sloppily packaged. So every size is rendered from
the SVG independently, at its own pixel grid, rather than downsampled from one big raster.

The 256 px entry is stored PNG-compressed (Vista+ "compressed icon"), which Pillow does on its own
for entries above 255 px. A 256x256 BMP entry would add ~256 KiB of uncompressed pixels to the
executable for no gain.

Usage: make-ico.py <icon.svg> <out.ico> [--sizes 16,24,32,48,64,128,256]

Requires rsvg-convert (librsvg) and Pillow. Both are checked up front with an actionable message --
a traceback out of an import is not a build error a reader can act on.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

# 24 and 64 are in the list because Windows asks for them at fractional DPI scalings (150% of 16 is
# 24, 200% of 32 is 64); leaving them out is what makes an icon look soft on a HiDPI laptop only.
DEFAULT_SIZES = (16, 24, 32, 48, 64, 128, 256)


def die(msg):
    print(f"make-ico.py: {msg}", file=sys.stderr)
    raise SystemExit(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("svg")
    ap.add_argument("out")
    ap.add_argument("--sizes", default=",".join(str(s) for s in DEFAULT_SIZES),
                    help="comma-separated square sizes to embed")
    args = ap.parse_args()

    try:
        from PIL import Image
    except ImportError:
        die("Pillow is required. Install it (Arch: python-pillow, Debian: python3-pil) or point\n"
            "            MOSAIC_WIN_PYTHON at a virtualenv that has it. See "
            "packaging/windows/README.md.")
    if shutil.which("rsvg-convert") is None:
        die("rsvg-convert is required to rasterize the SVG. Install librsvg (Arch: librsvg,\n"
            "            Debian: librsvg2-bin). See packaging/windows/README.md.")
    if not os.path.isfile(args.svg):
        die(f"no such icon source: {args.svg}")

    sizes = [int(s) for s in args.sizes.split(",") if s.strip()]
    if not sizes:
        die("--sizes is empty")

    tmp = tempfile.mkdtemp(prefix="mosaic-ico-")
    try:
        frames = []
        for px in sorted(sizes):
            png = os.path.join(tmp, f"{px}.png")
            # -w/-h both given so a non-square viewBox cannot produce a non-square icon entry;
            # Windows rejects .ico entries that are not square.
            subprocess.run(["rsvg-convert", "-w", str(px), "-h", str(px), args.svg, "-o", png],
                           check=True)
            frames.append(Image.open(png).convert("RGBA"))

        # Pillow writes the whole set from ONE image plus `sizes`, resampling internally -- which is
        # the thing this script exists to avoid. Writing the largest frame and passing the full size
        # list would throw away the per-size renders, so the frames are appended explicitly and the
        # first one only sets the header.
        frames[-1].save(args.out, format="ICO",
                        sizes=[(f.width, f.height) for f in frames],
                        append_images=frames[:-1])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print(f"wrote {args.out} ({', '.join(f'{s}x{s}' for s in sorted(sizes))})")


if __name__ == "__main__":
    sys.exit(main())
