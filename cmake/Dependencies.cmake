# System-first dependency discovery.
#
# Mosaic prefers dependencies installed via the system package manager. This module PROBES for
# them and prints a report at the end of configuration. It is INFORMATIONAL ONLY: nothing here
# fails a build. The authoritative probes are the per-module ones in src/*/CMakeLists.txt, which
# is what "required" below means -- a library marked required is one whose absence will stop the
# configure a moment later, in the module that actually needs it.
#
# ⚠ KEEP THIS LIST HONEST. It drifted badly once: it went on reporting OpenEXR, LibRaw and libzip
# as "[absent]" long after nothing in src/ referenced them (OpenEXR survives only in comments;
# the other two not at all), and it probed FLTK in module mode while src/ui and src/platform use
# CONFIG mode, so it announced "[absent] FLTK" on builds that had just found FLTK. A summary that
# contradicts the build it summarises is worse than no summary -- it sends people hunting for
# dependencies that are not missing, or not wanted.

function(_mosaic_record found name tier)
    if(found)
        set_property(GLOBAL APPEND PROPERTY MOSAIC_DEPS_FOUND "${name}")
    elseif(tier STREQUAL "REQUIRED")
        set_property(GLOBAL APPEND PROPERTY MOSAIC_DEPS_MISSING_REQ "${name}")
    else()
        set_property(GLOBAL APPEND PROPERTY MOSAIC_DEPS_MISSING_OPT "${name}")
    endif()
endfunction()

# ---- find_package-based probes ----------------------------------------------
find_package(Vulkan QUIET)
_mosaic_record("${Vulkan_FOUND}" "Vulkan (headers + loader)" REQUIRED)

# CONFIG: FLTK 1.4 ships FLTKConfig.cmake, and src/ui + src/platform both ask for it that way.
find_package(FLTK QUIET CONFIG)
_mosaic_record("${FLTK_FOUND}" "FLTK 1.4 (GUI toolkit)" REQUIRED)

find_package(spdlog QUIET CONFIG)
_mosaic_record("${spdlog_FOUND}" "spdlog (logging)" REQUIRED)

# glibc has gettext built in, so this is free on Linux and a real dependency elsewhere.
find_package(Intl QUIET)
_mosaic_record("${Intl_FOUND}" "gettext / libintl (i18n)" OPTIONAL)

# ---- pkg-config-based probes ------------------------------------------------
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    # module|label|tier -- mirroring src/core, src/io and src/platform.
    set(_mosaic_probes
        "lcms2|lcms2 (colour management)|REQUIRED"
        "freetype2|FreeType|REQUIRED"
        "harfbuzz|HarfBuzz|REQUIRED"
        "libpng|libpng|REQUIRED"
        "libturbojpeg|libjpeg-turbo|REQUIRED"
        "zlib|zlib|REQUIRED"
        "liblz4|lz4|REQUIRED"
        "libzstd|zstd|REQUIRED"
        "fontconfig|Fontconfig|REQUIRED"
        "libjxl|libjxl (JPEG XL)|OPTIONAL"
        "libwebp|libwebp|OPTIONAL"
        "libavif|libavif|OPTIONAL"
        "libtiff-4|libtiff|OPTIONAL")

    # Linux-only integration. macOS and Windows answer these with native APIs instead
    # (NSSpellChecker / ISpellChecker, CoreFoundation hyphenation, the XDG portal's counterparts),
    # so probing them off Linux would report absences that are correct and meaningless.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND _mosaic_probes
            "enchant-2|enchant-2 (spell-check)|REQUIRED"
            "libsystemd|libsystemd (XDG portal / sd-bus)|REQUIRED"
            "xi|libXi (XInput2 tablet)|REQUIRED"
            "xrandr|libXrandr (refresh rate)|REQUIRED"
            "wayland-client|wayland-client|REQUIRED"
            "wayland-protocols|wayland-protocols|REQUIRED"
            "libcanberra|libcanberra (alert sounds)|OPTIONAL")
    endif()

    foreach(_entry IN LISTS _mosaic_probes)
        string(REPLACE "|" ";" _parts "${_entry}")
        list(GET _parts 0 _mod)
        list(GET _parts 1 _label)
        list(GET _parts 2 _tier)
        string(MAKE_C_IDENTIFIER "MOSAIC_PC_${_mod}" _var)
        pkg_check_modules(${_var} QUIET "${_mod}")
        _mosaic_record("${${_var}_FOUND}" "${_label}" "${_tier}")
    endforeach()
else()
    _mosaic_record(FALSE "pkg-config (needed to locate most libraries)" REQUIRED)
endif()

# libhyphen ships no pkg-config file, so it is located by header + library like src/core does.
# macOS uses CoreFoundation's hyphenator instead and needs neither.
if(NOT APPLE)
    find_library(MOSAIC_PROBE_HYPHEN NAMES hyphen)
    find_path(MOSAIC_PROBE_HYPHEN_INC NAMES hyphen.h)
    if(MOSAIC_PROBE_HYPHEN AND MOSAIC_PROBE_HYPHEN_INC)
        _mosaic_record(TRUE "libhyphen (hyphenation)" REQUIRED)
    else()
        _mosaic_record(FALSE "libhyphen (hyphenation)" REQUIRED)
    endif()
endif()

# giflib ships no .pc file either (src/io locates it the same way).
find_path(MOSAIC_PROBE_GIF_INC NAMES gif_lib.h)
find_library(MOSAIC_PROBE_GIF NAMES gif)
if(MOSAIC_PROBE_GIF AND MOSAIC_PROBE_GIF_INC)
    _mosaic_record(TRUE "giflib (GIF)" OPTIONAL)
else()
    _mosaic_record(FALSE "giflib (GIF)" OPTIONAL)
endif()

# A SPIR-V compiler is a BUILD tool, not a library, and its absence is fatal in EmbedShaders --
# which is a confusing place to discover it, so say so here first.
find_program(MOSAIC_PROBE_GLSL NAMES glslc glslangValidator)
_mosaic_record("${MOSAIC_PROBE_GLSL}" "glslc / glslangValidator (SPIR-V)" REQUIRED)

# ---- end-of-configure summary ----------------------------------------------
function(mosaic_print_summary)
    get_property(_found      GLOBAL PROPERTY MOSAIC_DEPS_FOUND)
    get_property(_miss_req   GLOBAL PROPERTY MOSAIC_DEPS_MISSING_REQ)
    get_property(_miss_opt   GLOBAL PROPERTY MOSAIC_DEPS_MISSING_OPT)
    message(STATUS "")
    message(STATUS "==================== Mosaic configuration ====================")
    message(STATUS "  Version     : ${PROJECT_VERSION}")
    message(STATUS "  Build type  : ${CMAKE_BUILD_TYPE}")
    message(STATUS "  Compiler    : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
    message(STATUS "  System      : ${CMAKE_SYSTEM_NAME}")
    message(STATUS "  Tests       : ${MOSAIC_BUILD_TESTS}")
    message(STATUS "  Werror      : ON (mandatory, all presets)")
    if(MOSAIC_SANITIZE)
        message(STATUS "  Sanitizers  : ${MOSAIC_SANITIZE}")
    endif()
    message(STATUS "  ---- Dependencies ----")
    foreach(_d IN LISTS _found)
        message(STATUS "    [found]    ${_d}")
    endforeach()
    foreach(_d IN LISTS _miss_opt)
        message(STATUS "    [optional] ${_d} -- absent; the feature it backs is disabled")
    endforeach()
    foreach(_d IN LISTS _miss_req)
        message(STATUS "    [MISSING]  ${_d} -- REQUIRED; configure will fail below")
    endforeach()
    if(NOT _miss_req)
        message(STATUS "  All required dependencies present.")
    endif()
    message(STATUS "  Install commands per platform: see README.md.")
    message(STATUS "==============================================================")
    message(STATUS "")
endfunction()
