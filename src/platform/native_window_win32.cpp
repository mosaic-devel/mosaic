// Windows implementation of platform/native_window.hpp (PLAN.md S57).
//
// This translation unit REPLACES native_window.cpp on Windows (see platform/CMakeLists.txt): FLTK
// is built here with FLTK_BACKEND_X11=OFF and FLTK_BACKEND_WAYLAND=OFF -- its native Win32/GDI
// backend -- so none of the accessors native_window.cpp reads even exist, and the Vulkan WSI
// surface is VK_KHR_win32_surface, which takes an HINSTANCE + HWND rather than a display/surface
// pair.
//
// Unlike macOS, where native_window_macos.mm has to CREATE the presentable object (a CAMetalLayer
// on a dedicated subview, because a Metal surface cannot be handed an NSWindow), the HWND FLTK
// already made IS presentable. So this file only reads handles back out of FLTK; it never touches
// the window.

#include "platform/native_window.hpp"

#include <FL/Fl_Window.H>
#include <FL/platform.H> // fl_win32_xid() -- and, on Windows, `typedef HWND Window`

// Named even though FL/platform.H has already dragged it in: GetModuleHandleW and GetClientRect
// below are OURS, not FLTK's, and an FLTK that stopped leaking the system header would otherwise
// break this file at a distance. The toolchain defines NOMINMAX globally
// (cmake/toolchains/mingw-w64.cmake), so <windows.h> cannot clobber std::min/std::max here.
#include <cstring>
#include <vector>
#include <windows.h>

namespace mosaic::platform {

void preferWaylandBackendIfUnset() {
    // No windowing-backend choice on Windows -- FLTK has exactly one here, and there is no
    // FLTK_BACKEND for it to honour. See the header for why this still exists as a symbol rather
    // than being #ifdef'd out at its one call site in main().
}

WindowSystem activeBackend() {
    return WindowSystem::Win32;
}

int windowBufferScale(Fl_Window* /*win*/) {
    // ⚠ HARD-CODED, the same deliberate pin X11 and macOS use (the reasoning is on
    // NativeSurfaceHandle::scale in native_window.cpp, and it applies here unchanged). Windows has
    // no per-window INTEGER buffer scale to read: its HiDPI is a FRACTIONAL per-monitor DPI, which
    // FLTK already folds into every logical coordinate through Fl::screen_scale(). Rounding that to
    // an integer here would feed the canvas overlay widths, the reticle and the RGBA cursor
    // rasterization a number none of them has ever been exercised with -- and at the single most
    // common Windows setting, 150%, both answers (1 and 2) are wrong. Windows HiDPI is its own
    // end-to-end pass, exactly as X11 HiDPI is.
    return 1;
}

void raiseNativeWindowToTop(Fl_Window* win) {
    // The counterpart to the HWND_BOTTOM sink in nativeSurfaceHandle(): that makes the canvas the
    // backdrop, and this is how anything that must sit ON the backdrop asserts it. Called on every
    // show() rather than once, because FLTK creates a sub-window's HWND lazily and reuses it after
    // hide(), so "already correct" and "never ordered at all" are indistinguishable from here --
    // and SetWindowPos on an already-top window is cheap and idempotent.
    if (win == nullptr || win->shown() == 0)
        return;
    HWND hwnd = fl_win32_xid(win);
    if (hwnd == nullptr)
        return;
    // NOT SWP_SHOWWINDOW: FLTK's show() owns visibility, and forcing it here would map a window
    // that FLTK is in the middle of hiding. NOACTIVATE keeps the keyboard focus where the menu
    // code put it (openFor() calls take_focus() straight after).
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void applyNativeWindowShape(Fl_Window* win, const unsigned char* rgba, int w, int h) {
    if (win == nullptr || win->shown() == 0)
        return;
    HWND hwnd = fl_win32_xid(win);
    if (hwnd == nullptr)
        return;
    if (rgba == nullptr || w <= 0 || h <= 0) {
        SetWindowRgn(hwnd, nullptr, TRUE); // no shape: back to the full rectangle
        return;
    }

    // Build the region as one ExtCreateRegion call over a RECT run per opaque span, rather than
    // CombineRgn per span: a few hundred CombineRgn calls per open is the kind of thing that shows
    // up as a stutter when a flyout is opened repeatedly, and the batched form is one GDI call.
    std::vector<RECT> rects;
    rects.reserve(static_cast<std::size_t>(h));
    for (int y = 0; y < h; ++y) {
        const unsigned char* row = rgba + static_cast<std::size_t>(y) * w * 4;
        int x = 0;
        while (x < w) {
            while (x < w &&
                   row[x * 4 + 3] < 128) // alpha is a clean 0/255 here; 128 is just a guard
                ++x;
            if (x >= w)
                break;
            const int runStart = x;
            while (x < w && row[x * 4 + 3] >= 128)
                ++x;
            rects.push_back(RECT{runStart, y, x, y + 1});
        }
    }
    if (rects.empty()) {
        SetWindowRgn(hwnd, nullptr, TRUE);
        return;
    }

    const std::size_t bytes = sizeof(RGNDATAHEADER) + rects.size() * sizeof(RECT);
    std::vector<unsigned char> blob(bytes);
    auto* data = reinterpret_cast<RGNDATA*>(blob.data());
    data->rdh.dwSize = sizeof(RGNDATAHEADER);
    data->rdh.iType = RDH_RECTANGLES;
    data->rdh.nCount = static_cast<DWORD>(rects.size());
    data->rdh.nRgnSize = static_cast<DWORD>(rects.size() * sizeof(RECT));
    data->rdh.rcBound = RECT{0, 0, w, h};
    std::memcpy(data->Buffer, rects.data(), rects.size() * sizeof(RECT));

    HRGN rgn = ExtCreateRegion(nullptr, static_cast<DWORD>(bytes), data);
    if (rgn == nullptr)
        return; // leave whatever region is in force rather than clearing to a rectangle
    // SetWindowRgn TAKES OWNERSHIP on success -- deleting rgn afterwards would be a double free.
    // On failure it does not, so the region has to be released on that path.
    if (SetWindowRgn(hwnd, rgn, TRUE) == 0)
        DeleteObject(rgn);
}

bool nativeSurfaceHandle(Fl_Window* win, NativeSurfaceHandle& out, std::string& error) {
    if (win == nullptr || win->shown() == 0) {
        error = "native handle requested for a window that is not shown";
        return false;
    }

    // FLTK creates the HWND inside Fl_Window::show(), so this is null exactly when the window has
    // not been shown -- which the test above already caught. It is still checked, because a window
    // can have been hidden again since, and every caller treats a false return as "ask later".
    HWND hwnd = fl_win32_xid(win);
    if (hwnd == nullptr) {
        error = "no HWND for window";
        return false;
    }

    // ⚠⚠ Make the window hierarchy CLIP, before anything is presented into it.
    //
    // FLTK's Windows driver sets neither WS_CLIPCHILDREN on a parent nor WS_CLIPSIBLINGS on a child
    // (verified in Fl_win32.cxx: the only style it adds for a sub-window is WS_CHILD). For a window
    // FLTK draws with GDI itself that is merely wasteful. For one whose child is presented into by
    // ANOTHER API it is a correctness bug, because GDI's default is that a parent's painting is NOT
    // clipped out of its children's rectangles:
    //
    //   * the parent paints its chrome over the whole client area, Vulkan presents again, the parent
    //     paints again -- the canvas and any chrome above it strobe against each other for as long as
    //     anything is repainting ("chrome over the canvas flashes furiously");
    //   * and while our frame loop is not running -- which is exactly the case for the duration of a
    //     modal IFileDialog, whose Show() pumps its own message loop -- whatever the parent painted
    //     last simply STAYS. That is the canvas going blank when the file picker opens and coming
    //     back when it closes.
    //
    // WS_CLIPSIBLINGS matters for the same reason between the canvas and the menu/popup sub-windows
    // (ui::MenuBar and ui::MenuPopup are sub-windows too, see docs).
    //
    // Done here rather than at window creation because this is the one function that is by definition
    // called when a window is about to be rendered into by Vulkan, and it is idempotent -- setting
    // bits that are already set costs one SetWindowLongPtrW and changes nothing. SWP_FRAMECHANGED
    // makes Windows recompute the clipping regions rather than waiting for the next size change.
    // ⚠ And clipping is only half of it: it decides WHO YIELDS, so it is worthless if the Z-ORDER is
    // wrong. A child window that is presented into is the BACKDROP of its parent's chrome, and it
    // must therefore sit at the BOTTOM of the sibling order -- Mosaic's menu bar, menu popups and
    // tool popovers are all sub-windows too (docs/ui), and FLTK z-orders siblings by creation, which
    // puts the canvas above popups built around the same time.
    //
    // Unclipped, that wrong order was survivable and hid itself: a popup painted over the canvas
    // anyway, the canvas presented over the popup, and the two strobed -- which is precisely the
    // "chrome over the canvas flashes furiously" report. Adding WS_CLIPSIBLINGS without this made
    // the clipping honest and the order visible instead: menus came out CUT OFF at the canvas edge.
    // Both symptoms are the same bug, and the pair of fixes has to land together.
    if (const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE); style != 0)
        SetWindowLongPtrW(hwnd, GWL_STYLE, style | WS_CLIPSIBLINGS);
    SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    for (HWND parent = GetAncestor(hwnd, GA_PARENT); parent != nullptr;
         parent = GetAncestor(parent, GA_PARENT)) {
        if (parent == GetDesktopWindow())
            break;
        const LONG_PTR style = GetWindowLongPtrW(parent, GWL_STYLE);
        if (style == 0 || (style & WS_CLIPCHILDREN) != 0)
            continue;
        SetWindowLongPtrW(parent, GWL_STYLE, style | WS_CLIPCHILDREN);
        SetWindowPos(parent, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }

    out.system = WindowSystem::Win32;
    // VkWin32SurfaceCreateInfoKHR wants the MODULE INSTANCE, not a display connection -- Windows
    // has no such object. GetModuleHandleW(nullptr) is the .exe's own HINSTANCE, and it is the same
    // value FLTK registers its window class with (fl_win32_display() is GetModuleHandle(NULL)), so
    // the surface and the window agree. Asking Win32 directly rather than FLTK also means this
    // answer is valid before FLTK has opened its display.
    out.display = GetModuleHandleW(nullptr);
    out.window = hwnd;
    out.scale = 1; // see windowBufferScale() above

    // The CLIENT RECT -- and this is where Windows deliberately diverges from the macOS backend,
    // which uses FLTK's own w()/h() for the "the swapchain must match the viewport" reason written
    // out there. Two facts make the client rect the right answer on this platform:
    //   * FLTK's w()/h() are LOGICAL units here. Fl_WinAPI_Window_Driver multiplies them by
    //     Fl::screen_scale() on the way to SetWindowPos, so on a 150%-DPI monitor the HWND is 1.5x
    //     the size FLTK reports, and `w() * scale` (scale pinned to 1, above) would understate the
    //     drawable by a third.
    //   * a Win32 VkSurfaceKHR always reports a CONCRETE currentExtent -- never the 0xFFFFFFFF
    //     "you choose" that Wayland gives -- so the swapchain will be the client rect whatever hint
    //     we return. Reporting anything else would only make the hint disagree with the surface.
    // SetWindowPos is synchronous, so the rect already describes the size FLTK has just set; there
    // is no Cocoa-style deferred frame reconciliation to race.
    RECT rc{};
    if (GetClientRect(hwnd, &rc) != 0) {
        out.pixelWidth = static_cast<int>(rc.right - rc.left);
        out.pixelHeight = static_cast<int>(rc.bottom - rc.top);
    }
    if (out.pixelWidth <= 0 || out.pixelHeight <= 0) {
        // An empty client area: the window is minimized, or we were asked between window creation
        // and the first WM_SIZE. FLTK's own size is the fallback, as it is on macOS.
        out.pixelWidth = win->w();
        out.pixelHeight = win->h();
    }
    return true;
}

} // namespace mosaic::platform
