# Compile GLSL shaders to SPIR-V at build time and embed them as C++ uint32_t arrays.
#
# Shaders live in shaders/<name>; for each, this generates
# build/<preset>/generated/shaders/<name>.spv.hpp exposing
# `mosaic::shaders::<sanitized_name>` (and `..._size`). The generated dir is added to the
# target's PUBLIC include path, so code includes e.g. <shaders/fill.comp.spv.hpp>.

function(mosaic_find_shader_compiler out_var)
    find_program(_glslc NAMES glslc HINTS ENV VULKAN_SDK PATH_SUFFIXES bin)
    if(_glslc)
        set(${out_var} "${_glslc}" PARENT_SCOPE)
        set(${out_var}_KIND "glslc" PARENT_SCOPE)
        return()
    endif()
    find_program(_glslang NAMES glslangValidator HINTS ENV VULKAN_SDK PATH_SUFFIXES bin)
    if(_glslang)
        set(${out_var} "${_glslang}" PARENT_SCOPE)
        set(${out_var}_KIND "glslangValidator" PARENT_SCOPE)
    endif()
endfunction()

# mosaic_embed_shaders(<target> SHADERS fill.comp other.comp ...)
function(mosaic_embed_shaders target)
    cmake_parse_arguments(ARG "" "" "SHADERS" ${ARGN})
    mosaic_find_shader_compiler(_compiler)
    if(NOT _compiler)
        message(FATAL_ERROR
            "No SPIR-V compiler found (glslc or glslangValidator). "
            "Install shaderc/glslang (see PLAN.md section 6).")
    endif()

    set(_gendir "${CMAKE_BINARY_DIR}/generated/shaders")
    file(MAKE_DIRECTORY "${_gendir}")
    set(_outputs "")

    # Debug builds define MOSAIC_DEBUG in the shader preprocessor too, so the debug-only overlay
    # pieces (e.g. the canvas FPS readout, binding 12 in canvas_present.comp) compile ONLY in Debug
    # -- keeping the SPIR-V's declared bindings in lockstep with the C++ descriptor set, which guards
    # the same code under $<CONFIG:Debug>:MOSAIC_DEBUG. Held as an (unquoted) list so it splices in as
    # a real argument in Debug and vanishes ENTIRELY otherwise -- an empty genex/string would survive
    # as a stray "" argument under VERBATIM, which glslc rejects as a second input file. These are
    # single-config (Ninja) presets, so CMAKE_BUILD_TYPE is the authoritative per-build config here.
    set(_shader_defs "")
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        list(APPEND _shader_defs "-DMOSAIC_DEBUG")
    endif()

    # SPIR-V target env == Mosaic's Vulkan FLOOR (S60-alpha, docs/s60-performance-plan.md 2.5).
    # This is not cosmetic: --target-env=vulkan1.2 emits SPIR-V 1.5, and a Vulkan 1.0 driver
    # accepts ONLY SPIR-V 1.0 -- so a 1.2-targeted binary cannot even load its own shaders on a
    # 1.0 device. Every shader here is plain `#version 450` with no `#extension` directive, no
    # subgroup ops and no 16/64-bit types, so they compile at vulkan1.0 unchanged.
    #
    # Tier-specific shader VARIANTS (subgroup histogram, fp16 blend, descriptor-indexed composite)
    # get their own target env when they arrive; they are selected at pipeline-creation time from
    # render::GpuCaps, never from a version test.
    set(MOSAIC_SHADER_TARGET_ENV "vulkan1.0" CACHE STRING
        "SPIR-V target environment for the baseline shader set (Mosaic's Vulkan floor)")

    foreach(_name IN LISTS ARG_SHADERS)
        set(_src "${CMAKE_SOURCE_DIR}/shaders/${_name}")
        set(_spv "${_gendir}/${_name}.spv")
        set(_hpp "${_gendir}/${_name}.spv.hpp")
        string(MAKE_C_IDENTIFIER "${_name}" _sym)

        # ${_shader_defs} (unquoted) splices in -DMOSAIC_DEBUG for Debug builds and expands to zero
        # arguments otherwise (see the note where it is defined).
        if(_compiler_KIND STREQUAL "glslc")
            add_custom_command(OUTPUT "${_spv}"
                COMMAND "${_compiler}" -O "--target-env=${MOSAIC_SHADER_TARGET_ENV}"
                        ${_shader_defs} "${_src}" -o "${_spv}"
                DEPENDS "${_src}"
                COMMENT "glslc ${_name} -> SPIR-V"
                VERBATIM)
        else()
            add_custom_command(OUTPUT "${_spv}"
                COMMAND "${_compiler}" --target-env "${MOSAIC_SHADER_TARGET_ENV}"
                        ${_shader_defs} -o "${_spv}" "${_src}"
                DEPENDS "${_src}"
                COMMENT "glslangValidator ${_name} -> SPIR-V"
                VERBATIM)
        endif()

        add_custom_command(OUTPUT "${_hpp}"
            COMMAND "${CMAKE_COMMAND}" "-DSPV=${_spv}" "-DHPP=${_hpp}" "-DSYM=${_sym}"
                    -P "${CMAKE_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
            DEPENDS "${_spv}" "${CMAKE_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
            COMMENT "embed ${_name}.spv -> ${_name}.spv.hpp"
            VERBATIM)
        list(APPEND _outputs "${_hpp}")
    endforeach()

    add_custom_target(${target}_shaders DEPENDS ${_outputs})
    add_dependencies(${target} ${target}_shaders)
    target_include_directories(${target} PUBLIC "${CMAKE_BINARY_DIR}/generated")
endfunction()
