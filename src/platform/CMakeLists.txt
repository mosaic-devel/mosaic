# platform -- OS integration. From S3 it needs FLTK (native window handles for the Vulkan
# canvas). On Linux FLTK's hybrid build brings in the X11 + Wayland accessors used by
# native_window.cpp; on macOS (S58) the Cocoa build feeds a CAMetalLayer via native_window_macos.mm.
find_package(FLTK REQUIRED CONFIG)

# OS-specific window/tablet sources. Linux speaks X11 + Wayland (+ their tablet backends); macOS
# speaks Cocoa and has no XInput2/zwp_tablet path (tablet pressure is a Mac-side follow-up, S58).
if(APPLE)
    set(_platform_os_sources
        native_window_macos.mm
        file_dialog_macos.cpp
        system_theme_macos.mm
        system_theme_watch_macos.mm
        display_refresh_macos.mm
        session_end_stub.cpp    # no applicationShouldTerminate: hook yet -- see the stub's own note
        wayland_integration_stub.cpp)  # no Wayland here; FLTK's own icon() drives the dock icon
elseif(WIN32)
    # Windows (S57): FLTK is its native Win32/GDI backend, so there are no X11/Wayland accessors to
    # compile. The Vulkan surface is fed an HINSTANCE + HWND by native_window_win32.cpp; the picker
    # is the shell's own Common Item Dialog (IFileDialog), not Fl_Native_File_Chooser, whose Windows
    # driver still calls the legacy GetOpenFileName; the appearance comes from the registry keys the
    # shell itself acts on. The XI2/zwp_tablet pair is replaced by tablet_win32.cpp, which speaks
    # BOTH Windows Ink (the WM_POINTER stack) and WinTab -- and needs no wintab32 import library
    # because Wintab32.dll is resolved with LoadLibraryW at runtime: it is a tablet-DRIVER component,
    # absent on a machine with no tablet, so linking it would stop Mosaic from starting there.
    # session_end_win32.cpp is the only file in this directory that SUBCLASSES an FLTK window --
    # WM_QUERYENDSESSION is sent rather than posted, so Fl::add_system_handler cannot reach it.
    set(_platform_os_sources
        native_window_win32.cpp
        display_refresh_win32.cpp
        file_dialog_win32.cpp
        session_end_win32.cpp
        system_theme_win32.cpp
        system_theme_watch_win32.cpp
        tablet_win32.cpp
        wayland_integration_stub.cpp)  # no Wayland here; FLTK's own icon() drives the window icon
else()
    set(_platform_os_sources
        native_window.cpp
        wayland_subsurface.cpp
        wayland_foreign.cpp
        wayland_toplevel.cpp        # the weak-symbol xdg_toplevel accessor the two below need
        wayland_toplevel_icon.cpp   # xdg-toplevel-icon-v1: a window icon Wayland otherwise has no
        wayland_dialog.cpp          # xdg-dialog-v1: "this is a modal dialog" (S59-a)
        display_refresh.cpp
        file_dialog.cpp
        session_end_stub.cpp   # Wayland has no session-end protocol; X11's is XSMP (stub's note)
        system_theme.cpp
        system_theme_watch.cpp
        tablet_x11.cpp
        tablet_wayland.cpp)
endif()

mosaic_add_module(platform
    SOURCES
        platform.cpp
        font_db.cpp
        system_sound.cpp   # every platform: NSBeep is plain C, so macOS needs no .mm sibling here
        tablet.cpp
        ${_platform_os_sources}
    DEPS
        mosaic::common
        mosaic::core   # font_db.cpp implements core::text::FontProvider (S29-a)
        fltk::fltk)

# font_db.cpp resolves system fonts through fontconfig (FcFontMatch / FcFontList / charset coverage)
# on every platform -- cross-built for macOS at S58. PRIVATE: the FcConfig/FcPattern types stay
# inside font_db.cpp, never in the header.
find_package(PkgConfig REQUIRED)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
target_link_libraries(mosaic_platform PRIVATE PkgConfig::FONTCONFIG)

if(APPLE)
    # Static fontconfig needs its XML parser (expat) named explicitly -- on Linux the shared
    # fontconfig pulls it in transitively; a static one does not.
    pkg_check_modules(EXPAT REQUIRED IMPORTED_TARGET expat)
    target_link_libraries(mosaic_platform PRIVATE PkgConfig::EXPAT)

    # native_window_macos.mm attaches a CAMetalLayer (QuartzCore/Metal) to the FLTK window's NSView
    # (Cocoa); file_dialog.cpp routes to Fl_Native_File_Chooser (the native Cocoa panels);
    # system_theme_macos.mm reads NSAppearance + NSColor.controlAccentColor (AppKit). FLTK's config
    # already links the umbrella frameworks, but name the ones we call directly so a static link
    # resolves cleanly.
    target_link_libraries(mosaic_platform PRIVATE
        "-framework Cocoa" "-framework AppKit" "-framework QuartzCore" "-framework Metal"
        # display_refresh_macos.mm reads CGDisplayCopyDisplayMode /
        # CGDisplayModeGetRefreshRate; NSScreen comes from the AppKit above.
        "-framework CoreGraphics")
elseif(WIN32)
    # ---- Windows OS integration -----------------------------------------------------------------
    # native_window_win32.cpp: GetModuleHandleW (kernel32) + GetClientRect (user32).
    # system_theme{,_watch}_win32.cpp: RegGetValueW / RegOpenKeyExW / RegNotifyChangeKeyValue
    #   (advapi32) + CreateEventW / WaitForSingleObject (kernel32).
    # system_sound.cpp: MessageBeep (user32).
    # file_dialog_win32.cpp: CoInitializeEx / CoCreateInstance / CoTaskMemFree (ole32),
    #   SHCreateItemFromParsingName (shell32), and the CLSID_File{Open,Save}Dialog /
    #   IID_IFileDialog / IID_IShellItem constants from `uuid` -- MinGW has no MSVC __uuidof, so the
    #   GUIDs come out of an import library rather than the compiler.
    # session_end_win32.cpp: SetWindowLongPtrW / CallWindowProcW / GetAncestor / IsWindow (user32).
    #   Its ShutdownBlockReason{Create,Destroy} are NOT imported -- they are GetProcAddress'd out of
    #   the already-mapped user32, for the reason the file writes out at length (a missing import
    #   kills the process before main(), and this session already shipped that crash once).
    #
    # CMake's MinGW default CMAKE_CXX_STANDARD_LIBRARIES happens to carry all of these already; they
    # are named anyway for the same reason the Apple branch names its frameworks -- a module should
    # declare what it actually calls, so a later edit to that global variable cannot quietly break
    # the link. (The toolchain file records exactly one such edit going wrong.) NO dwmapi: the accent
    # colour is read from HKCU\...\DWM\ColorizationColor, not via DwmGetColorizationColor.
    target_link_libraries(mosaic_platform PRIVATE advapi32 user32 kernel32 ole32 shell32 uuid)
else()
    # ---- Linux OS integration ------------------------------------------------------------------
    # file_dialog.cpp calls the XDG Desktop Portal FileChooser over sd-bus (no GTK/GLib -- libsystemd
    # is present on every systemd box). PRIVATE: the sd_bus types stay inside file_dialog.cpp.
    pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
    target_link_libraries(mosaic_platform PRIVATE PkgConfig::SYSTEMD)

    # tablet_x11.cpp speaks XInput2 (XIQueryDevice / XISelectEvents / XIQueryVersion). libX11 comes in
    # through FLTK's hybrid build already; libXi does not, so link it here. NOT quiet/optional: the
    # XI2 backend is the primary tablet path (docs/tablet.md §3) and a Linux build without it would be
    # a silently pressure-less Mosaic.
    pkg_check_modules(XI REQUIRED IMPORTED_TARGET xi)
    target_link_libraries(mosaic_platform PRIVATE PkgConfig::XI)

    # display_refresh.cpp asks RandR which CRTC a window sits on and what mode that CRTC is running
    # (XRRGetScreenResourcesCurrent / XRRGetCrtcInfo / XRRGetOutputInfo). libX11 comes in through
    # FLTK's hybrid build; libXrandr does not, exactly like libXi above. REQUIRED rather than
    # optional: without it an X11 session has no way to learn the refresh rate, and the frame pacer
    # would fall back to its conservative floor on every Xorg desktop.
    pkg_check_modules(XRANDR REQUIRED IMPORTED_TARGET xrandr)
    target_link_libraries(mosaic_platform PRIVATE PkgConfig::XRANDR)

    # system_sound.cpp plays the desktop's own alert sounds through libcanberra (LGPL-2.1+), which
    # resolves freedesktop event names against the user's XDG sound theme and honours their
    # event-sounds switch. QUIET/optional on purpose -- unlike XI2 and zwp_tablet_v2 above, a build
    # without it loses a courtesy, not a capability: playSystemSound() simply goes silent. FLTK's
    # fl_beep is NOT a fallback (XBell on X11 reaches nothing; the Wayland driver prints a BEL byte
    # to stderr).
    pkg_check_modules(CANBERRA QUIET IMPORTED_TARGET libcanberra)
    if(CANBERRA_FOUND)
        target_link_libraries(mosaic_platform PRIVATE PkgConfig::CANBERRA)
        target_compile_definitions(mosaic_platform PRIVATE MOSAIC_HAVE_CANBERRA)
    else()
        message(STATUS "libcanberra not found: dialog alert sounds are disabled on this build.")
    endif()

    # wayland_subsurface.cpp calls libwayland-client directly (registry bind + wl_subcompositor +
    # wl_subsurface requests that FLTK does not expose). Unlike render -- which only needs the WSI
    # *headers* because the surface entry points come from the Vulkan loader -- we must link the
    # library. Probed via pkg-config; on the hybrid Linux FLTK build it is always present.
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(WAYLAND_CLIENT QUIET wayland-client)
        if(WAYLAND_CLIENT_FOUND)
            target_include_directories(mosaic_platform PRIVATE ${WAYLAND_CLIENT_INCLUDE_DIRS})
            target_link_directories(mosaic_platform PRIVATE ${WAYLAND_CLIENT_LIBRARY_DIRS})
            target_link_libraries(mosaic_platform PRIVATE ${WAYLAND_CLIENT_LIBRARIES})
        endif()
    endif()

    # tablet_wayland.cpp speaks zwp_tablet_v2 (docs/tablet.md §4), whose client stubs wayland-scanner
    # generates from the wayland-protocols XML. We compile the private-code .c into the module and give
    # tablet_wayland.cpp the generated client header on a PRIVATE include path -- the header stays out
    # of tablet_wayland.hpp, so no consumer of the module needs this generated dir. REQUIRED, not
    # optional: on native Wayland this is the only path to pressure, exactly as the XI2 backend is on
    # XWayland (a build without it would be a silently pressure-less Mosaic on KWin/GNOME).
    enable_language(C) # wayland-scanner emits C; the project is otherwise C++-only
    pkg_get_variable(WL_PROTO_DIR wayland-protocols pkgdatadir)
    pkg_get_variable(WL_SCANNER wayland-scanner wayland_scanner)
    if(NOT WL_PROTO_DIR OR NOT WL_SCANNER)
        message(FATAL_ERROR "wayland-protocols + wayland-scanner are required for the zwp_tablet_v2 "
            "backend (tablet_wayland.cpp). Install them, or the Linux build has no native-Wayland "
            "tablet path.")
    endif()
    set(_wl_gen "${CMAKE_CURRENT_BINARY_DIR}/wayland-gen")
    file(MAKE_DIRECTORY "${_wl_gen}")

    # The protocols we generate stubs for:
    #   tablet-unstable-v2  the pen itself -- REQUIRED (see above).
    #   cursor-shape-v1     the pen's CURSOR. A client that binds the tablet manager owns its tool's
    #                       cursor, and one that never sets it gets whatever the compositor defaults to
    #                       (KWin: a crosshair, which is what Mosaic used to show over every pixel of
    #                       its UI on native Wayland). FLTK cannot help: its cursor calls go through
    #                       wl_pointer.set_cursor, and there IS no wl_pointer for a tablet tool. This
    #                       protocol names a cursor by SHAPE instead of demanding we build, theme and
    #                       feed a cursor surface ourselves. Optional -- if the compositor lacks it we
    #                       fall back to hiding the tool cursor, which is right over the canvas anyway.
    #   xdg-foreign-unstable-v2  export our top-level surface so the XDG portal's file dialog can be
    #                       parented + made modal to it on native Wayland (wayland_foreign.cpp). Without
    #                       it the KDE/GNOME portal picker floats as its own un-parented taskbar window.
    set(_wl_protos
        "${WL_PROTO_DIR}/unstable/tablet/tablet-unstable-v2.xml"
        "${WL_PROTO_DIR}/staging/cursor-shape/cursor-shape-v1.xml"
        "${WL_PROTO_DIR}/unstable/xdg-foreign/xdg-foreign-unstable-v2.xml")

    # Two more, taken from the VENDORED copies in third_party/wayland-protocols rather than from the
    # system's wayland-protocols, and deliberately so:
    #
    #   xdg-toplevel-icon-v1  a window icon supplied as PIXELS. Wayland has no _NET_WM_ICON, so
    #                         without this a client cannot give the compositor an icon at all --
    #                         the icon can only come from an INSTALLED .desktop, which is why an
    #                         AppImage shows a generic placeholder (wayland_toplevel_icon.cpp).
    #   xdg-dialog-v1         "this toplevel is a modal dialog", so the compositor can dim the
    #                         parent and keep it from raising above (wayland_dialog.cpp).
    #
    # ⚠ WHY VENDORED. The AppImage is built on Ubuntu 24.04 -- chosen deliberately, because an
    # AppImage's glibc floor is its build host's (see packaging/linux/make-appimage.sh) -- and 24.04
    # ships wayland-protocols 1.34, while xdg-toplevel-icon-v1 landed in 1.36. Sourcing these from
    # the system would silently drop the icon on exactly the artifact that needs it most. Vendoring
    # also pins the protocol VERSION the stubs are generated against, so every platform speaks the
    # same wire format regardless of what the build host happens to carry.
    list(APPEND _wl_protos
        "${CMAKE_SOURCE_DIR}/third_party/wayland-protocols/xdg-toplevel-icon-v1.xml"
        "${CMAKE_SOURCE_DIR}/third_party/wayland-protocols/xdg-dialog-v1.xml")

    foreach(_proto IN LISTS _wl_protos)
        if(NOT EXISTS "${_proto}")
            message(FATAL_ERROR "missing wayland protocol XML: ${_proto}")
        endif()
        get_filename_component(_stem "${_proto}" NAME_WE)
        add_custom_command(
            OUTPUT "${_wl_gen}/${_stem}-client.h" "${_wl_gen}/${_stem}.c"
            COMMAND "${WL_SCANNER}" client-header "${_proto}" "${_wl_gen}/${_stem}-client.h"
            COMMAND "${WL_SCANNER}" private-code "${_proto}" "${_wl_gen}/${_stem}.c"
            DEPENDS "${_proto}" VERBATIM)
        target_sources(mosaic_platform PRIVATE "${_wl_gen}/${_stem}.c" "${_wl_gen}/${_stem}-client.h")
        # scanner-generated C is not held to the project's warning bar.
        set_source_files_properties("${_wl_gen}/${_stem}.c" PROPERTIES COMPILE_OPTIONS "-w")
    endforeach()
    target_include_directories(mosaic_platform PRIVATE "${_wl_gen}")
endif()
