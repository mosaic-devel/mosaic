# Cross-compile Mosaic for macOS from Linux using osxcross + MoltenVK (PLAN.md S58).
#
# This file is machine-independent: it locates the toolchain through the OSXCROSS_ROOT
# environment variable (the osxcross `target/` directory) and never hard-codes a host path.
# Select the target CPU with -DMOSAIC_OSX_ARCH=arm64|x86_64 (default arm64); the universal
# build lipos the two single-arch trees together (see packaging/macos/).
#
# Usage (see packaging/macos/README.md and docs/build-macos.md):
#   export OSXCROSS_ROOT=/path/to/osxcross/target
#   cmake --preset macos-arm64      # or macos-x86_64
#
# Deployment target is macOS 11.0 (Big Sur) -- the first Apple-Silicon release, so one floor covers
# both Intel and Apple-Silicon Macs. The SDK version only bounds the newest APIs available at
# compile time; it does not raise this floor.
# ⚠ packaging/macos/build-deps.sh must agree (MOSAIC_MACOS_MIN): a dependency built at a different
# floor than the app that links it warns on every object, and claims a compatibility it does not
# have.

if(NOT DEFINED ENV{OSXCROSS_ROOT})
    message(FATAL_ERROR
        "macOS cross-compilation needs OSXCROSS_ROOT set to the osxcross 'target' directory.\n"
        "See docs/build-macos.md (osxcross setup) and packaging/macos/README.md.")
endif()

set(_osxcross "$ENV{OSXCROSS_ROOT}")
if(NOT EXISTS "${_osxcross}/bin")
    message(FATAL_ERROR "OSXCROSS_ROOT='${_osxcross}' has no bin/ -- is it the osxcross target dir?")
endif()

# The osxcross clang wrappers locate their companion ld64/as on PATH. Put the toolchain bin dir
# there so the configure-time compiler checks link (otherwise the wrapper falls back to the host
# GNU ld: "unrecognised emulation mode"). NOTE: `cmake --build` also needs this on PATH at build
# time -- use packaging/macos/build-app.sh, which exports it, or add it yourself.
set(ENV{PATH} "${_osxcross}/bin:$ENV{PATH}")

# ---- target CPU -------------------------------------------------------------
set(MOSAIC_OSX_ARCH "arm64" CACHE STRING "macOS target CPU: arm64 or x86_64")
set_property(CACHE MOSAIC_OSX_ARCH PROPERTY STRINGS arm64 x86_64)
if(MOSAIC_OSX_ARCH STREQUAL "arm64")
    set(_clangpfx "oa64")
    set(_machine "arm64")
elseif(MOSAIC_OSX_ARCH STREQUAL "x86_64")
    set(_clangpfx "o64")
    set(_machine "x86_64")
else()
    message(FATAL_ERROR "MOSAIC_OSX_ARCH must be arm64 or x86_64 (got '${MOSAIC_OSX_ARCH}')")
endif()

set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR "${_machine}")

# ---- discover the SDK + the darwinNN tool triple (no version hard-coded) -----
file(GLOB _sdks "${_osxcross}/SDK/MacOSX*.sdk")
if(NOT _sdks)
    message(FATAL_ERROR "no MacOSX*.sdk under '${_osxcross}/SDK' -- osxcross SDK not installed.")
endif()
list(GET _sdks 0 CMAKE_OSX_SYSROOT)

# The cctools/ld64 tools are named "<arch>-apple-darwinNN-<tool>"; find the darwinNN in use.
file(GLOB _cc "${_osxcross}/bin/x86_64-apple-darwin*-clang")
list(GET _cc 0 _cc0)
string(REGEX REPLACE ".*/x86_64-(apple-darwin[0-9.]+)-clang$" "\\1" _darwin "${_cc0}")
set(_triple "${_machine}-${_darwin}")

set(CMAKE_C_COMPILER   "${_osxcross}/bin/${_clangpfx}-clang")
set(CMAKE_CXX_COMPILER "${_osxcross}/bin/${_clangpfx}-clang++")
set(CMAKE_AR                "${_osxcross}/bin/${_triple}-ar")
set(CMAKE_RANLIB            "${_osxcross}/bin/${_triple}-ranlib")
set(CMAKE_STRIP             "${_osxcross}/bin/${_triple}-strip")
set(CMAKE_INSTALL_NAME_TOOL "${_osxcross}/bin/${_triple}-install_name_tool")
set(CMAKE_LIBTOOL           "${_osxcross}/bin/${_triple}-libtool")

set(CMAKE_OSX_ARCHITECTURES "${_machine}" CACHE STRING "")
# macOS 11.0 (Big Sur). This was 13.3 from S58 until S59, because libc++ gates floating-point
# std::to_chars/std::from_chars behind 13.3 and Mosaic uses charconv throughout -- but that reason
# had already stopped applying: src/common/charconv_compat.hpp supplies the ENTIRE fallback
# (strtod / snprintf, both locale-corrected) behind _LIBCPP_VERSION, so Mosaic never calls libc++'s
# floating-point charconv at all. The comment outlived the problem it described, and cost every Mac
# older than Ventura for nothing.
#
# 11.0 is the first Apple-Silicon release, which makes it the lowest floor worth having: below it
# there is no arm64 to serve. The Quick Look PREVIEW extension still needs 12.0 (QLPreviewProvider),
# which is why its .appex plist says so -- src/thumbnailer/quicklook_macos.mm already carries the
# API_AVAILABLE(macos(12.0)) annotation that makes referencing it from a lower-targeted binary
# legal, so no second build is needed: on macOS 11 that one extension simply does not load.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")

# ---- find-root: SDK sysroot + optional cross-built dependency prefix ----------
# MOSAIC_MAC_PREFIX (env) is where packaging/macos/build-deps.sh installs the per-arch
# third-party libraries. Kept out of this file so nothing machine-specific is committed.
set(CMAKE_FIND_ROOT_PATH "${CMAKE_OSX_SYSROOT}")
if(DEFINED ENV{MOSAIC_MAC_PREFIX})
    list(PREPEND CMAKE_FIND_ROOT_PATH "$ENV{MOSAIC_MAC_PREFIX}")
    list(PREPEND CMAKE_PREFIX_PATH "$ENV{MOSAIC_MAC_PREFIX}")
endif()
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)   # host tools (glslc, xgettext) stay on the host
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# pkg-config reads plain-text .pc files, so the host binary is fine for cross use as long as it is
# pointed at the target prefix. The cross-built deps install at real absolute paths (not relative to
# the SDK sysroot), so PKG_CONFIG_SYSROOT_DIR must stay UNSET -- setting it would wrongly prepend the
# sysroot to the deps' own -I/-L paths.
if(DEFINED ENV{MOSAIC_MAC_PREFIX})
    set(ENV{PKG_CONFIG_LIBDIR} "$ENV{MOSAIC_MAC_PREFIX}/lib/pkgconfig")
    unset(ENV{PKG_CONFIG_SYSROOT_DIR})
endif()
