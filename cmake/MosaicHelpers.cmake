# Shared helper functions for the Mosaic build.

# Apply the project's standard warning set to a target. Warnings are errors, always, in
# every preset: first-party code builds clean or not at all (decision 2026-07-16; there is
# no opt-out toggle). Vendored code under third_party/ is exempt via SYSTEM includes.
function(mosaic_apply_warnings target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # -Wno-missing-field-initializers: we use C++20 designated initializers (heavily for
        # Vulkan structs) and intentionally leave trailing members zero-initialized; GCC
        # otherwise warns on every such struct (Clang does not).
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wno-missing-field-initializers -Werror)
        # Clang enables several diagnostics under -Wall that this GCC-developed, -Werror-clean
        # codebase does not trip on GCC. They are cosmetic, not bug-catching (unused documentation
        # constants; brace-elision on nested aggregate init; unused lambda captures) -- disabling
        # them matches the Linux warning set rather than lowering the bar, and first-party code
        # still builds fully warning-clean on GCC.
        #
        # Keyed on the COMPILER, not on APPLE (which is what S58 wrote when osxcross was the only
        # clang in the build): S57's Windows-on-ARM target is llvm-mingw, i.e. the same clang
        # diagnostics arriving on a completely different platform. Two cross-builds needing one
        # identical exemption list is the evidence that this belongs to clang.
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${target} PRIVATE
                -Wno-unused-const-variable -Wno-missing-braces -Wno-unused-lambda-capture
                -Wno-unused-private-field)
        endif()

        # GCC 12/13 report false -Wstringop-overflow / -Warray-bounds / -Wrestrict inside
        # <bits/stl_algobase.h> for perfectly ordinary container calls, once the standard library
        # has been inlined far enough that the optimizer loses track of the region's size. The one
        # that surfaced here is ByteWriter::text() in src/formats/formats.hpp -- a
        # vector<uint8_t>::insert of a two-character string_view, reported as "writing 1 byte into
        # a region of size 0". The code is correct; the analysis is not, and upstream fixed this
        # family in GCC 14.
        #
        # Scoped to GCC BELOW 14 so the diagnostics stay fully armed on the compilers the project
        # is developed and CI'd against (the Arch container's GCC is far newer). Without this an
        # LTS-distro build -- which is exactly what the release AppImages need, since an AppImage's
        # glibc floor is its build host's -- cannot get past -Werror.
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
            target_compile_options(${target} PRIVATE
                -Wno-stringop-overflow -Wno-array-bounds -Wno-restrict)
        endif()
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    endif()
endfunction()

# Apply sanitizers (from MOSAIC_SANITIZE) to a target, for both compile and link.
function(mosaic_apply_sanitizers target)
    if(MOSAIC_SANITIZE AND (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
        string(REPLACE "," ";" _sans "${MOSAIC_SANITIZE}")
        foreach(_s IN LISTS _sans)
            target_compile_options(${target} PRIVATE -fsanitize=${_s} -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=${_s})
        endforeach()
    endif()
endfunction()

# Optimize a pixel-loop module even in Debug builds. The CPU compositor runs a full-document
# float composite per frame during drags/slider gestures; at -O0 that is hundreds of
# milliseconds (the "everything is laggy in dev builds" root cause, S15.x). The UI and tooling
# stay at -O0 and fully debuggable; stepping into these modules just sees optimized frames.
# Sanitizer builds keep working (GCC/Clang support -O2 with ASan/UBSan).
function(mosaic_optimize_pixel_loops target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE $<$<CONFIG:Debug>:-O2>)
    endif()
endfunction()

# Declare a Mosaic static-library module that lives under src/<name>/.
#   mosaic_add_module(core SOURCES core.cpp DEPS mosaic::common)
# Creates target `mosaic_<name>` plus alias `mosaic::<name>`, exposes `src/` as a public
# include root, requires C++20, and applies the standard warning/sanitizer settings.
function(mosaic_add_module name)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPS" ${ARGN})
    add_library(mosaic_${name} STATIC ${ARG_SOURCES})
    add_library(mosaic::${name} ALIAS mosaic_${name})
    target_include_directories(mosaic_${name} PUBLIC "${CMAKE_SOURCE_DIR}/src")
    target_compile_features(mosaic_${name} PUBLIC cxx_std_20)
    if(ARG_DEPS)
        target_link_libraries(mosaic_${name} PUBLIC ${ARG_DEPS})
    endif()
    mosaic_apply_warnings(mosaic_${name})
    mosaic_apply_sanitizers(mosaic_${name})
endfunction()
