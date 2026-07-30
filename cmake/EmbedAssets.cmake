# Embed binary assets (e.g. SVGs) into the build as C++ headers.
#
#   mosaic_embed_asset(<target> NAMESPACE mosaic::assets SYMBOL app_icon_svg
#                      FILE "${CMAKE_SOURCE_DIR}/assets/app_icon.svg")
#
# Generates build/<preset>/generated/assets/<symbol>.hpp exposing
# `<NAMESPACE>::<symbol>` (an `unsigned char[]`) and `..._size`, adds the generated dir to
# the target's PUBLIC include path (so code does `#include <assets/<symbol>.hpp>`), and
# wires the generation step into the target's build.

function(mosaic_embed_asset target)
    cmake_parse_arguments(ARG "" "NAMESPACE;SYMBOL;FILE" "" ${ARGN})
    if(NOT ARG_NAMESPACE OR NOT ARG_SYMBOL OR NOT ARG_FILE)
        message(FATAL_ERROR "mosaic_embed_asset: NAMESPACE, SYMBOL and FILE are required")
    endif()

    set(_gendir "${CMAKE_BINARY_DIR}/generated/assets")
    file(MAKE_DIRECTORY "${_gendir}")
    set(_hpp "${_gendir}/${ARG_SYMBOL}.hpp")

    # ALWAYS-RUN, not an mtime-tracked add_custom_command: assets arrive as copies that can
    # PRESERVE their source mtimes (cp -p, archive extraction, a file manager) -- older than the
    # generated header, so a dependency-driven rule silently keeps embedding the OLD bytes (this
    # shipped a stale icon set once). EmbedBytes goes through copy_if_different, so the per-build
    # cost is one cheap script run per asset and downstream recompiles only happen on real change.
    add_custom_target(${target}_${ARG_SYMBOL}_asset
        COMMAND "${CMAKE_COMMAND}" "-DBYTES_IN=${ARG_FILE}" "-DHPP=${_hpp}"
                "-DSYM=${ARG_SYMBOL}" "-DNS=${ARG_NAMESPACE}"
                -P "${CMAKE_SOURCE_DIR}/cmake/EmbedBytes.cmake"
        BYPRODUCTS "${_hpp}"
        COMMENT "embed asset ${ARG_FILE} -> ${ARG_SYMBOL}.hpp"
        VERBATIM)
    add_dependencies(${target} ${target}_${ARG_SYMBOL}_asset)
    target_include_directories(${target} PUBLIC "${CMAKE_BINARY_DIR}/generated")
endfunction()
