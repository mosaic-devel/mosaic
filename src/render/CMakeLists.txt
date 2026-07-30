# render -- Vulkan backend. From S2 onward the Vulkan loader + headers are required.
find_package(Vulkan REQUIRED)

mosaic_add_module(render
    SOURCES
        render.cpp
        gpu_caps.cpp
        gpu_timer.cpp
        gpu_budget.cpp
        tile_residency.cpp
        tile_compositor.cpp
        composite_readback.cpp
        vulkan_context.cpp
        compute_fill.cpp
        region_fill.cpp
        effect_primitives.cpp
        blur.cpp
        blur_gpu.cpp
        stylize.cpp
        stylize_kernels.cpp
        layer_effects_render.cpp
        layer_effects_gpu.cpp
        histogram_gpu.cpp
        resample.cpp
        warp.cpp
        compositor.cpp
        document_ops.cpp
        gpu_compositor.cpp
        extrude_gpu.cpp
        texture_gpu.cpp
        vma_impl.cpp
        window_renderer.cpp
    DEPS
        mosaic::common
        mosaic::core
        vma::vma
        Vulkan::Vulkan)

# The VMA implementation TU is third-party code; build it without our warning set (it does not
# pass -Wall -Wextra -Wpedantic cleanly, and it is not ours to lint).
set_source_files_properties(vma_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")

# Debug-only diagnostics compile behind MOSAIC_DEBUG (the $<CONFIG:Debug> generator-expression
# pattern used by src/ui + MosaicHelpers.cmake). The canvas FPS readout's descriptor-set channel
# (binding 12) lives under this guard in window_renderer.cpp and MUST stay in lockstep with the
# shader's own MOSAIC_DEBUG guard (glslc is passed -DMOSAIC_DEBUG for Debug in EmbedShaders.cmake).
target_compile_definitions(mosaic_render PRIVATE $<$<CONFIG:Debug>:MOSAIC_DEBUG>)

# Compile GLSL -> SPIR-V and embed it (generates <shaders/*.spv.hpp>, adds the include dir,
# and makes mosaic_render depend on the generation step).
mosaic_embed_shaders(mosaic_render SHADERS fill.comp composite_blend.comp composite_tile.comp
                     adjust_tile.comp
                     tile_resolve.comp
                     canvas_present.comp
                     canvas_drag_composite.comp canvas_idle.comp extrude_raster.comp
                     texture_sky.comp texture_paper.comp blur_separable.comp blur_convert.comp
                     blur_lens.comp blur_dof.comp
                     le_plane.comp le_shade.comp histogram_bins.comp)

# The DESCRIPTOR-INDEXED tile kernels (S60-a item 10). The SAME sources as composite_tile.comp and
# adjust_tile.comp, each compiled a second time with -DMOSAIC_TILE_INDEXED into its own symbol.
#
# Two things this must NOT do, and both are the reason it is spelled out here rather than added to
# the list above:
#   * it must not raise MOSAIC_SHADER_TARGET_ENV. That variable is Mosaic's Vulkan FLOOR: every
#     shader in the list emits SPIR-V 1.0 because a 1.0 driver will load nothing newer. These blobs
#     need GL_EXT_nonuniform_qualifier and therefore their own, newer env -- and `TileCompositor`
#     asks `GpuCaps::fitsSpirvVersion` before it so much as creates a module, so a device that
#     cannot load them never sees them.
#   * it must not be a second copy of a shader. One source, two binaries: the resampling, blend and
#     adjustment math is written once, so the pairs cannot drift apart between parity runs.
#
# The two kernels are built by ONE loop for the same reason `initIndexedPipeline` gates them
# together: the indexed tier is all-or-nothing, and a build that produced one blob and not the
# other would make the active dispatch shape depend on what a document happens to contain.
mosaic_find_shader_compiler(_tile_compiler)
set(_tile_gendir "${CMAKE_BINARY_DIR}/generated/shaders")
# vulkan1.1 (SPIR-V 1.3), not vulkan1.2: descriptor indexing reaches a 1.1 device through
# VK_EXT_descriptor_indexing, and targeting 1.2 would emit SPIR-V 1.5 and lock those devices out of
# a tier they genuinely have.
set(MOSAIC_TILE_INDEXED_TARGET_ENV "vulkan1.1")
set(_tile_indexed_hpps "")
foreach(_tile_name composite_tile adjust_tile)
    set(_tile_src "${CMAKE_SOURCE_DIR}/shaders/${_tile_name}.comp")
    set(_tile_spv "${_tile_gendir}/${_tile_name}_indexed.comp.spv")
    set(_tile_hpp "${_tile_gendir}/${_tile_name}_indexed.comp.spv.hpp")
    if(_tile_compiler_KIND STREQUAL "glslc")
        add_custom_command(OUTPUT "${_tile_spv}"
            COMMAND "${_tile_compiler}" -O "--target-env=${MOSAIC_TILE_INDEXED_TARGET_ENV}"
                    -DMOSAIC_TILE_INDEXED=1 "${_tile_src}" -o "${_tile_spv}"
            DEPENDS "${_tile_src}"
            COMMENT "glslc ${_tile_name}.comp (descriptor-indexed) -> SPIR-V"
            VERBATIM)
    else()
        add_custom_command(OUTPUT "${_tile_spv}"
            COMMAND "${_tile_compiler}" --target-env "${MOSAIC_TILE_INDEXED_TARGET_ENV}"
                    -DMOSAIC_TILE_INDEXED=1 -o "${_tile_spv}" "${_tile_src}"
            DEPENDS "${_tile_src}"
            COMMENT "glslangValidator ${_tile_name}.comp (descriptor-indexed) -> SPIR-V"
            VERBATIM)
    endif()
    add_custom_command(OUTPUT "${_tile_hpp}"
        COMMAND "${CMAKE_COMMAND}" "-DSPV=${_tile_spv}" "-DHPP=${_tile_hpp}"
                "-DSYM=${_tile_name}_indexed_comp"
                -P "${CMAKE_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
        DEPENDS "${_tile_spv}" "${CMAKE_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
        COMMENT "embed ${_tile_name}_indexed.comp.spv -> .spv.hpp"
        VERBATIM)
    list(APPEND _tile_indexed_hpps "${_tile_hpp}")
endforeach()
add_custom_target(mosaic_render_shaders_indexed DEPENDS ${_tile_indexed_hpps})
add_dependencies(mosaic_render mosaic_render_shaders_indexed)

# texture_gpu.cpp cooks the Hosek-Wilkie state with the vendored reference (core links it
# PRIVATE, so the include path does not propagate here).
target_link_libraries(mosaic_render PRIVATE hosekwilkie::hosekwilkie)

# window_renderer.cpp pulls in the Vulkan WSI platform headers (vulkan_xlib.h /
# vulkan_wayland.h), which need the X11 and wayland-client development headers. The surface
# entry points themselves come from the Vulkan loader; we only need the include paths.
#
# Skipped entirely for Windows (S57): the WSI header there is vulkan_win32.h over <windows.h>, which
# the MinGW sysroot and the Vulkan headers already provide, so there is nothing to probe -- and
# probing anyway would have CMake looking for X11 and wayland-client inside a Windows dependency
# prefix. NOT extended to APPLE: this block has been reached (and found nothing) on every macOS
# cross-build since S58, and leaving it alone keeps that build bit-identical.
if(NOT WIN32)
    find_package(X11)
    if(X11_FOUND)
        target_include_directories(mosaic_render PRIVATE ${X11_INCLUDE_DIR})
    endif()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(WAYLAND_CLIENT QUIET wayland-client)
        if(WAYLAND_CLIENT_FOUND)
            target_include_directories(mosaic_render PRIVATE ${WAYLAND_CLIENT_INCLUDE_DIRS})
        endif()
    endif()
endif()

mosaic_optimize_pixel_loops(mosaic_render)  # the CPU compositor runs per frame during gestures
