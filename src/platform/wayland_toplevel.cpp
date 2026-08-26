#include "platform/wayland_toplevel.hpp"

#include "platform/native_window.hpp" // activeBackend() -- is this session actually Wayland?

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/platform.H> // fl_wl_xid(), and (on a patched FLTK) fl_wl_toplevel()

// The accessor added by packaging/linux/patches/fltk-1.4.5-wayland-toplevel-accessor.patch.
//
// DECLARED WEAK ON PURPOSE, and this one attribute is what keeps the whole feature buildable
// everywhere. A weak *undefined* symbol is not an error to the linker: it resolves to null when
// nothing defines it. So this translation unit -- and both protocol backends that call into it --
// compiles and links identically against a patched FLTK (where it binds to the real function) and
// against every stock distro FLTK (where it stays null and the feature reports itself absent).
// That is why there is no MOSAIC_HAVE_* build flag and no #ifdef anywhere in this feature: CI
// builds against Arch's unpatched `fltk` package and still compiles every line of it under -Werror.
//
// Re-declaring it here is harmless when FL/wayland.H already declares it (the patched case): the
// signatures agree and the added attribute only weakens the reference. Verified warning-clean under
// -Wall -Wextra -Werror on both GCC and Clang, patched and stock.
//
// ⚠ One ELF subtlety, since it is easy to get wrong: a weak undefined reference does NOT by itself
// pull a member out of a static archive. It resolves only because Fl_Wayland_Window_Driver.cxx.o --
// the object that defines the accessor -- is already linked in for the window driver itself, which
// any FLTK program that opens a window needs. Were that ever not so, this would silently answer
// null on a patched build rather than break, which is the safe direction to fail.
extern "C++" __attribute__((weak)) xdg_toplevel* fl_wl_toplevel(wld_window* xid);

namespace mosaic::platform {

bool waylandToplevelAccessorPresent() noexcept {
    return fl_wl_toplevel != nullptr;
}

xdg_toplevel* waylandToplevel(Fl_Window* win) {
    if (fl_wl_toplevel == nullptr || win == nullptr)
        return nullptr;
    // activeBackend() is only meaningful once a window has been shown, which is also the only point
    // an xdg_toplevel exists -- so the two conditions are checked in the order that makes the X11
    // case cost nothing.
    if (activeBackend() != WindowSystem::Wayland || win->shown() == 0)
        return nullptr;
    wld_window* xid = fl_wl_xid(win);
    if (xid == nullptr)
        return nullptr;
    // Null for a sub-window, menu window or tooltip: those have no xdg_toplevel of their own. The
    // caller treats that exactly like "unsupported", which is what it is for them.
    return fl_wl_toplevel(xid);
}

} // namespace mosaic::platform
