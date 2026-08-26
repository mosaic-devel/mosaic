#pragma once

class Fl_Window; // forward-declared: this header stays free of FLTK/Wayland includes.

struct xdg_toplevel;

namespace mosaic::platform {

// The `xdg_toplevel` of a shown, top-level FLTK window -- the object every xdg-shell EXTENSION
// protocol addresses a window by. Null when the session is not Wayland, when `win` is not shown or
// is not a toplevel (sub-windows, menus and tooltips have none), or when FLTK cannot answer.
//
// ⚠ FLTK 1.4.5 DOES NOT EXPOSE THIS. Its public FL/wayland.H stops at display / xid / surface /
// compositor / buffer_scale / gc / glcontext, and master still does today -- so a stock FLTK gives
// a client no way to reach xdg-toplevel-icon-v1 or xdg-dialog-v1 at all. The toplevel lives in
// FLTK's PRIVATE `struct wld_window`, and for a decorated window it is owned by libdecor on top of
// that. `packaging/linux/patches/fltk-1.4.5-wayland-toplevel-accessor.patch` adds the one free
// function that exposes what Fl_Wayland_Window_Driver::xdg_toplevel() already computes internally;
// the Linux release build applies it (.github/workflows/release.yml), and it is written to be
// upstreamable so the patch can be dropped when FLTK takes it.
//
// A build against an UNPATCHED FLTK -- every distro package today, including the one CI uses --
// still compiles and links this file: the accessor is referenced through a WEAK symbol, which
// resolves to null when nothing defines it. So the two protocols below simply report themselves
// unavailable there rather than failing the build, and everything except the final request is
// still compiled (and -Werror'd) by CI. See docs/wayland.md §4.
//
// The alternative -- hand-declaring FLTK's private `struct wld_window` to read the frame out of it
// -- was rejected: that layout is version- and build-config-dependent, against a libfltk the distro
// built, and reading a union member through a wrong layout is a wild pointer.
[[nodiscard]] xdg_toplevel* waylandToplevel(Fl_Window* win);

// Whether this binary is linked against an FLTK that carries the accessor at all. False means every
// xdg-shell extension below is inert no matter what the compositor supports; it is the difference
// between "your compositor lacks it" and "your build lacks it", which is worth logging distinctly.
[[nodiscard]] bool waylandToplevelAccessorPresent() noexcept;

} // namespace mosaic::platform
