# apple_cursor (vendored cursor art)

This directory vendors **only the `svg/` source folder** and the `LICENSE` from the
**apple_cursor** project — a macOS-style cursor theme by Kaiz Khatri (ful1e5).

- Upstream: https://github.com/ful1e5/apple_cursor
- License: **GNU GPL v3.0** (see `LICENSE`) — compatible with Mosaic, which is also GPLv3.

## What Mosaic uses it for

Mosaic's canvas pan gesture needs an open-hand ("grab") and a closed-fist ("grabbing")
pointer. FLTK 1.4's `Fl_Cursor` enum exposes no such pair (only the *pointing*
`FL_CURSOR_HAND`), so rather than depend on a platform cursor library (libXcursor is X11-only),
we rasterize these SVGs ourselves via `common::rasterizeSvg` (nanosvg) and hand the bitmap to
FLTK — identical on X11, Wayland, Windows and macOS.

| Mosaic role            | File           |
| ---------------------- | -------------- |
| open hand ("grab")     | `svg/hand1.svg` |
| closed fist ("grabbing") | `svg/move.svg`  |

Both are baked into the binary at build time (`cmake/EmbedAssets.cmake`, invoked from
`src/ui/CMakeLists.txt`) as `mosaic::assets::cursor_grab_svg` / `cursor_grabbing_svg`, and read
in `src/ui/cursors.cpp` (`ui::panCursor`). The hotspots there are apple_cursor's own X11
values from upstream `configs/x.build.toml` (`hand1` → 134,81; `move` → 139,86, on a 256 canvas).

Note: nanosvg ignores the SVGs' drop-shadow `<filter>`, which is the desired result for a cursor.

## Updating

Re-copy the upstream `svg/` folder and `LICENSE` here. The whole `svg/` set is vendored (not just
the two files in use) so other cursors can be adopted later without another upstream trip.
