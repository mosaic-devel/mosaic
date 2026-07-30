# Cross-compile Mosaic for Windows from Linux using MinGW-w64 (PLAN.md S57). No Visual Studio,
# ever -- the Windows build is a Linux cross-build like the macOS one.
#
# Select the target CPU with -DMOSAIC_WIN_ARCH=x86_64|aarch64 (default x86_64). The two arches use
# DIFFERENT toolchains, and that is deliberate rather than accidental:
#
#   x86_64   the system mingw-w64 GCC (`x86_64-w64-mingw32-gcc`, Arch: mingw-w64-gcc). Same
#            compiler family and version as the Linux build, so this -Werror-clean codebase stays
#            clean with no new diagnostics to suppress.
#   aarch64  llvm-mingw (`/opt/llvm-mingw`, Arch: llvm-mingw). The GNU mingw-w64 toolchain has no
#            aarch64 target at all -- LLVM is the only way to build Windows-on-ARM from Linux.
#            Clang raises several -Wall diagnostics GCC does not; MosaicHelpers silences exactly
#            the same four the macOS build needed, and nothing more.
#
# Usage (see packaging/windows/README.md and docs/build-windows.md):
#   cmake --preset windows-x86_64      # or windows-arm64
#
# Third-party libraries come from packaging/windows/build-deps.sh, which installs a per-arch
# prefix named by the MOSAIC_WIN_PREFIX environment variable. Kept out of this file so nothing
# machine-specific is committed.

set(CMAKE_SYSTEM_NAME Windows)

# ---- target CPU -------------------------------------------------------------
set(MOSAIC_WIN_ARCH "x86_64" CACHE STRING "Windows target CPU: x86_64 or aarch64")
set_property(CACHE MOSAIC_WIN_ARCH PROPERTY STRINGS x86_64 aarch64)

if(MOSAIC_WIN_ARCH STREQUAL "x86_64")
    set(CMAKE_SYSTEM_PROCESSOR AMD64)
    set(_triple "x86_64-w64-mingw32")
    set(_llvm OFF)
elseif(MOSAIC_WIN_ARCH STREQUAL "aarch64")
    set(CMAKE_SYSTEM_PROCESSOR ARM64)
    set(_triple "aarch64-w64-mingw32")
    set(_llvm ON)
else()
    message(FATAL_ERROR "MOSAIC_WIN_ARCH must be x86_64 or aarch64 (got '${MOSAIC_WIN_ARCH}')")
endif()

# ---- locate the toolchain ----------------------------------------------------
# MOSAIC_LLVM_MINGW (env) overrides the llvm-mingw root; /opt/llvm-mingw is the Arch package's
# location. The GNU toolchain is expected on PATH under its triple prefix, as every distro ships it.
if(_llvm)
    set(_llvmroot "$ENV{MOSAIC_LLVM_MINGW}")
    if(NOT _llvmroot)
        set(_llvmroot "/opt/llvm-mingw")
    endif()
    if(NOT EXISTS "${_llvmroot}/bin/${_triple}-clang")
        message(FATAL_ERROR
            "Windows-on-ARM needs llvm-mingw: no '${_llvmroot}/bin/${_triple}-clang'.\n"
            "Install it (Arch: llvm-mingw) or set MOSAIC_LLVM_MINGW to its root.\n"
            "See docs/build-windows.md.")
    endif()
    set(_bin "${_llvmroot}/bin/${_triple}-")
    set(CMAKE_C_COMPILER   "${_bin}clang")
    set(CMAKE_CXX_COMPILER "${_bin}clang++")
    set(CMAKE_RC_COMPILER  "${_bin}windres")
    set(CMAKE_AR           "${_llvmroot}/bin/llvm-ar")
    set(CMAKE_RANLIB       "${_llvmroot}/bin/llvm-ranlib")
    set(CMAKE_STRIP        "${_llvmroot}/bin/llvm-strip")
    set(CMAKE_DLLTOOL      "${_llvmroot}/bin/llvm-dlltool")
    set(_sysroot "${_llvmroot}/${_triple}")
else()
    find_program(_gcc "${_triple}-gcc")
    if(NOT _gcc)
        message(FATAL_ERROR
            "Windows cross-compilation needs the MinGW-w64 GCC toolchain: '${_triple}-gcc' is not "
            "on PATH.\nArch: pacman -S mingw-w64-gcc   Debian: apt install g++-mingw-w64-x86-64\n"
            "See docs/build-windows.md.")
    endif()
    set(CMAKE_C_COMPILER   "${_triple}-gcc")
    set(CMAKE_CXX_COMPILER "${_triple}-g++")
    set(CMAKE_RC_COMPILER  "${_triple}-windres")
    set(CMAKE_AR           "${_triple}-ar")
    set(CMAKE_RANLIB       "${_triple}-ranlib")
    set(CMAKE_STRIP        "${_triple}-strip")
    set(CMAKE_DLLTOOL      "${_triple}-dlltool")
    set(_sysroot "/usr/${_triple}")
endif()

set(MOSAIC_WIN_TRIPLE "${_triple}" CACHE INTERNAL "MinGW-w64 target triplet")

# ---- find-root: the MinGW sysroot + the cross-built dependency prefix ---------
set(CMAKE_FIND_ROOT_PATH "${_sysroot}")
if(DEFINED ENV{MOSAIC_WIN_PREFIX})
    list(PREPEND CMAKE_FIND_ROOT_PATH "$ENV{MOSAIC_WIN_PREFIX}")
    list(PREPEND CMAKE_PREFIX_PATH "$ENV{MOSAIC_WIN_PREFIX}")
endif()
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)   # host tools (glslc, xgettext) stay on the host
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# The cross-built deps install at real absolute paths, so PKG_CONFIG_SYSROOT_DIR must stay UNSET --
# setting it would wrongly prepend a sysroot to the deps' own -I/-L paths (the same trap the macOS
# toolchain documents).
if(DEFINED ENV{MOSAIC_WIN_PREFIX})
    set(ENV{PKG_CONFIG_LIBDIR} "$ENV{MOSAIC_WIN_PREFIX}/lib/pkgconfig")
    unset(ENV{PKG_CONFIG_SYSROOT_DIR})
endif()

# ---- Windows target settings -------------------------------------------------
# ⚠ SCOPE RULE, earned the hard way: only things that are TRUE OF THE TARGET belong here. This file
# is also the toolchain for packaging/windows/build-deps.sh, so anything set here is imposed on every
# third-party library too. UNICODE/_UNICODE and NOMINMAX are opinions about the API surface, not
# properties of the target -- they live in the root CMakeLists under `if(WIN32)`, which the
# separately-configured dependency builds never see. (Putting UNICODE here made every generic Win32
# macro in the Vulkan-Loader resolve to its -W variant while the loader passes char*, and it failed
# to compile.) See the long note in CMakeLists.txt.
#
# Windows 10 1809 (0x0A00) floor: the Pointer Input Stack (Windows Ink), ISpellChecker,
# UISettings/immersive dark mode and the per-monitor-v2 DPI awareness the manifest asks for are all
# at or below it, and it is the oldest release Microsoft still services. WINVER and _WIN32_WINNT must
# agree, and EVERY library in the payload must agree with them -- a dependency compiled against an
# older header set can pick a different struct layout for the same API, which is exactly why this
# pair is target truth and belongs in the toolchain.
#
# _USE_MATH_DEFINES is here because it is purely ADDITIVE (it only defines names, never removes or
# redirects any), so imposing it on a dependency cannot break one. It is also not optional: the
# project sets CMAKE_CXX_EXTENSIONS OFF, so it compiles as strict `-std=c++20`, which defines
# __STRICT_ANSI__ -- and mingw-w64's <math.h> gates the whole M_* family behind
# `!defined(__STRICT_ANSI__) || defined(_USE_MATH_DEFINES)`. Without it every M_PI in the tree
# (core/vector, ui/widgets, ui/vulkan_canvas, ...) is an undeclared identifier. Verified with a
# two-line probe compiled both ways.
#
# These ride *_FLAGS_INIT rather than add_compile_definitions(): a toolchain file is re-read for
# every try_compile, and only the _INIT variables reach those probe builds.
set(_win_defs "-DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 -D_USE_MATH_DEFINES")
set(CMAKE_C_FLAGS_INIT   "${_win_defs}")
set(CMAKE_CXX_FLAGS_INIT "${_win_defs}")

# ---- one C runtime, payload-wide -------------------------------------------------
# A DLL payload only works if every image agrees on ONE C runtime: a FILE*, an errno or a locale
# crossing a CRT boundary is undefined behaviour, and Mosaic hands a FILE* straight to libpng
# (io/png.cpp -> png_init_io) among others. Nothing here has to be set to achieve that, and that is
# worth writing down because the opposite looks true at a glance:
#
#   `x86_64-w64-mingw32-gcc -dumpspecs` shows `%{!mcrtdll=*:-lmsvcrt}`, which reads as "this
#   toolchain defaults to the legacy msvcrt while the cross-built DLLs use the UCRT". It is a FALSE
#   ALARM. mingw-w64's CRT is configured `--with-default-msvcrt=ucrt` on this host, so `libmsvcrt.a`
#   is itself an alias for the UCRT import library, and llvm-mingw is UCRT-based for the same reason.
#
# Confirmed the only way that actually settles it -- `objdump -p` on the built artefacts: mosaic.exe
# and every dependency DLL import `api-ms-win-crt-*`. Re-check that way (never from the specs) if a
# dependency is ever replaced with a prebuilt binary or the host toolchain changes. Adding
# `-mcrtdll=ucrt` would be a no-op today and is deliberately not set, so there is one less knob whose
# meaning depends on how the distro built its CRT.
#
# Mosaic's own modules link statically into mosaic.exe; the THIRD-PARTY stack ships as DLLs beside
# it (user decision, S57). CMAKE_CXX_STANDARD_LIBRARIES is deliberately LEFT ALONE -- on MinGW it
# carries the Win32 import libraries (kernel32/user32/gdi32/ole32/...) that every Windows program
# needs, and the previous skeleton's `-static` in that slot is exactly what the DLL layout replaces.
# The three MinGW runtime DLLs are copied next to the exe by packaging/windows/make-package.sh
# rather than linked in statically, so every DLL in the payload agrees on one libstdc++ and one
# pthread implementation.
