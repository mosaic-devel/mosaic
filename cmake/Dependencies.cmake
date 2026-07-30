# System-first dependency discovery.
#
# Mosaic prefers dependencies installed via the system package manager. This module
# PROBES for the libraries Mosaic will use as features land, records what was found and
# what is missing, and (via mosaic_print_summary) prints a clear report at the end of
# configuration. In the current early phase NONE of these are required to build the
# skeleton -- they are reported so the user can install them ahead of time. See PLAN.md
# section 6 for exact per-platform package names.

function(_mosaic_record found name)
    if(found)
        set_property(GLOBAL APPEND PROPERTY MOSAIC_DEPS_FOUND "${name}")
    else()
        set_property(GLOBAL APPEND PROPERTY MOSAIC_DEPS_MISSING "${name}")
    endif()
endfunction()

# ---- find_package-based probes (quiet / optional) ---------------------------
find_package(Vulkan QUIET)
_mosaic_record("${Vulkan_FOUND}" "Vulkan (headers + loader)")

find_package(FLTK QUIET)
_mosaic_record("${FLTK_FOUND}" "FLTK (GUI toolkit)")

find_package(Intl QUIET)
_mosaic_record("${Intl_FOUND}" "gettext / libintl (i18n)")

# ---- pkg-config-based probes (quiet / optional) -----------------------------
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    foreach(_entry IN ITEMS
            "lcms2|lcms2 (color management)"
            "libpng|libpng"
            "libturbojpeg|libjpeg-turbo"
            "libtiff-4|libtiff"
            "libwebp|libwebp"
            "OpenEXR|OpenEXR (HDR)"
            "libraw|LibRaw (RAW)"
            "freetype2|FreeType"
            "harfbuzz|HarfBuzz"
            "fontconfig|Fontconfig"
            "libzip|libzip"
            "spdlog|spdlog (logging)")
        string(REPLACE "|" ";" _parts "${_entry}")
        list(GET _parts 0 _mod)
        list(GET _parts 1 _label)
        string(MAKE_C_IDENTIFIER "MOSAIC_PC_${_mod}" _var)
        pkg_check_modules(${_var} QUIET "${_mod}")
        _mosaic_record("${${_var}_FOUND}" "${_label}")
    endforeach()
else()
    set_property(GLOBAL APPEND PROPERTY MOSAIC_DEPS_MISSING
        "pkg-config (needed to locate most libraries)")
endif()

# ---- end-of-configure summary ----------------------------------------------
function(mosaic_print_summary)
    get_property(_found   GLOBAL PROPERTY MOSAIC_DEPS_FOUND)
    get_property(_missing GLOBAL PROPERTY MOSAIC_DEPS_MISSING)
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
    message(STATUS "  ---- Dependencies (probed; most become required as features land) ----")
    foreach(_d IN LISTS _found)
        message(STATUS "    [found]   ${_d}")
    endforeach()
    foreach(_d IN LISTS _missing)
        message(STATUS "    [absent]  ${_d}")
    endforeach()
    message(STATUS "  Required now: Vulkan (render), FLTK (ui/platform), spdlog (logging).")
    message(STATUS "  gettext/libintl is used for i18n when present (English-only without it).")
    message(STATUS "  Install commands per platform: see PLAN.md section 6.")
    message(STATUS "==============================================================")
    message(STATUS "")
endfunction()
