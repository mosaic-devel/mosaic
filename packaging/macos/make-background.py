#!/usr/bin/env python3
"""Render the Mosaic .dmg background (PLAN.md S59): the app icon, tilted and drawn at a low
opacity, bleeding off the left edge -- echoing the About-box watermark -- on the app's DARK chrome
ground, with a drag arrow pointing from the app icon toward the Applications folder.

Rendered at 2x for Retina. The dark gradient is DITHERED (triangular noise) so it does not band on
the narrow value range, and the arrow is SUPERSAMPLED for anti-aliasing. rsvg-convert + Pillow + numpy.

Usage: make-background.py <app_icon.svg> <out.png> [--width W] [--height H]
"""
import argparse
import os
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image, ImageDraw

WIN_W, WIN_H = 660, 400          # logical DMG window size (points); must match make-dmg.sh
SCALE = 2                        # Retina
OPACITY = 0.10                   # watermark strength on the dark ground
ANGLE = -18                      # degrees, matching the About-box watermark tilt
ICON_POINTS = 560                # watermark size before rotation, in points
# Dark chrome, from the app's dark theme (ui/theme.cpp: windowBg {30,33,46} / panelBg {37,41,56}).
TOP = (37, 41, 56)
BOT = (24, 27, 38)
ARROW = (150, 165, 214)          # soft blue-grey, reads on the dark ground
PLATE = (232, 234, 240)          # light card under each icon so Finder's black labels stay legible

# Icon centres Finder places (must match the Iloc args in make-dmg.sh: app@180, apps@480, y=205).
APP_CX, APPS_CX, ICON_CY = 180, 480, 205
DPI = 144                        # 2x: makes Finder size the 1320x800 image to the 660x400 window


def dithered_gradient(w, h):
    """A vertical TOP->BOT gradient with triangular-PDF dither, so the narrow dark range doesn't
    band. Returned as an RGBA uint8 image."""
    t = np.linspace(0.0, 1.0, h)[:, None, None]           # (h,1,1)
    top = np.array(TOP, dtype=np.float32)
    bot = np.array(BOT, dtype=np.float32)
    grad = top * (1.0 - t) + bot * t                       # (h,1,3)
    grad = np.repeat(grad, w, axis=1)                      # (h,w,3)
    rng = np.random.default_rng(0)                         # fixed seed -> deterministic output
    dither = rng.random((h, w, 3), dtype=np.float32) - rng.random((h, w, 3), dtype=np.float32)
    grad = np.clip(grad + dither * 1.5, 0, 255).astype(np.uint8)
    a = np.full((h, w, 1), 255, dtype=np.uint8)
    return Image.fromarray(np.concatenate([grad, a], axis=2), "RGBA")


def overlay_layer(w, h):
    """A supersampled, anti-aliased overlay: a light card under each icon (so the black Finder
    labels read on the dark ground) plus the drag arrow. Transparent elsewhere."""
    ss = 4
    lw, lh = w * ss, h * ss
    layer = Image.new("RGBA", (lw, lh), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    s = SCALE * ss

    # Light cards behind each icon+label. The label sits below the icon, so the card is centred a
    # little below the icon centre and is tall enough to cover both.
    card_w, card_h, radius = 150 * s, 182 * s, 18 * s
    card_cy = (ICON_CY + 11) * s
    for cx in (APP_CX, APPS_CX):
        x = cx * s
        d.rounded_rectangle([x - card_w // 2, card_cy - card_h // 2,
                             x + card_w // 2, card_cy + card_h // 2],
                            radius=radius, fill=PLATE + (232,))

    # Drag arrow between the two cards.
    y = ICON_CY * s
    x1 = (APP_CX + 88) * s          # just right of the app card
    x2 = (APPS_CX - 88) * s         # tip, just left of the Applications card
    shaft_h, head_w, head_h = 10 * s, 26 * s, 32 * s
    d.rounded_rectangle([x1, y - shaft_h // 2, x2 - head_w + shaft_h, y + shaft_h // 2],
                        radius=shaft_h // 2, fill=ARROW + (215,))
    d.polygon([(x2, y), (x2 - head_w, y - head_h // 2), (x2 - head_w, y + head_h // 2)],
              fill=ARROW + (240,))
    return layer.resize((w, h), Image.LANCZOS)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("svg")
    ap.add_argument("out")
    ap.add_argument("--width", type=int, default=WIN_W)
    ap.add_argument("--height", type=int, default=WIN_H)
    args = ap.parse_args()

    w, h = args.width * SCALE, args.height * SCALE
    bg = dithered_gradient(w, h)

    # Rasterize the icon large, ghost it, tilt it, bleed it off the left edge. On the dark ground the
    # icon's own light tiles read as a faint bright watermark (the dark icon body blends in).
    with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as tf:
        icon_png = tf.name
    try:
        px = int(ICON_POINTS * SCALE)
        subprocess.run(["rsvg-convert", "-w", str(px), "-h", str(px), args.svg, "-o", icon_png],
                       check=True)
        icon = Image.open(icon_png).convert("RGBA")
        alpha = icon.split()[3].point(lambda v: int(v * OPACITY))
        icon.putalpha(alpha)
        icon = icon.rotate(ANGLE, expand=True, resample=Image.BICUBIC)
        cx, cy = int(0.05 * w), int(0.52 * h)
        bg.alpha_composite(icon, (cx - icon.width // 2, cy - icon.height // 2))
    finally:
        os.unlink(icon_png)

    bg.alpha_composite(overlay_layer(w, h))

    rgb = bg.convert("RGB")
    if args.out.lower().endswith((".tif", ".tiff")):
        # 144 dpi tags this as a 2x image so Finder draws it at the logical window size.
        rgb.save(args.out, compression="tiff_lzw", dpi=(DPI, DPI))
    else:
        rgb.save(args.out, dpi=(DPI, DPI))
    print(f"wrote {args.out} ({w}x{h}px; logical {args.width}x{args.height})")


if __name__ == "__main__":
    sys.exit(main())
