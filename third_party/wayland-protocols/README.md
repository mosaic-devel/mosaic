# Vendored Wayland protocol descriptions

Two staging protocols from [wayland-protocols][wp], copied verbatim from the 1.49 release:

| file | upstream path | what Mosaic uses it for |
| --- | --- | --- |
| `xdg-toplevel-icon-v1.xml` | `staging/xdg-toplevel-icon/` | `src/platform/wayland_toplevel_icon.cpp` — hand the compositor the window icon as pixels |
| `xdg-dialog-v1.xml` | `staging/xdg-dialog/` | `src/platform/wayland_dialog.cpp` — tell the compositor a toplevel is a modal dialog |

`wayland-scanner` turns each into client stubs at build time; see `src/platform/CMakeLists.txt`.
Both are MIT, and each XML carries its own copyright block — reproduced in `LICENSE` alongside.

## Why these are vendored when the other three protocols are not

`src/platform/CMakeLists.txt` takes `tablet-unstable-v2`, `cursor-shape-v1` and
`xdg-foreign-unstable-v2` from the **system's** `wayland-protocols`. These two are different for two
reasons, and the first is not a preference:

1. **The build host is too old.** The AppImage is built on Ubuntu 24.04 — deliberately, because an
   AppImage's glibc floor is whatever its build host has (`packaging/linux/make-appimage.sh`
   explains this at length). 24.04 ships wayland-protocols **1.34**, and `xdg-toplevel-icon-v1`
   landed in **1.36**. Sourcing it from the system would silently drop the window icon from the one
   artifact whose whole problem is that it has no window icon.

2. **Pinning the wire format.** Vendoring fixes the protocol *version* the stubs are generated
   against, so every platform speaks the same one no matter what the build host carries.

## Updating

Copy the newer XML over the old one and rebuild; the generated stubs follow automatically. Check
the `<interface ... version="N">` attributes if you do — `wl_registry_bind` in each backend asks for
version 1 explicitly, and a client must never bind a version higher than it was written against.

[wp]: https://gitlab.freedesktop.org/wayland/wayland-protocols
