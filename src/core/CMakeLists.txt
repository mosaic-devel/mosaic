# lcms2 backs core/color_management (S12-b). PRIVATE: the lcms2 types never appear in headers.
find_package(PkgConfig REQUIRED)
pkg_check_modules(LCMS2 REQUIRED IMPORTED_TARGET lcms2)

# FreeType (outlines + colour glyphs) and HarfBuzz (shaping) back core/text (S29-a). PRIVATE: both
# are confined to text/shaping.cpp + text/text_render.cpp behind a PImpl, never in a public header.
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)

# Hyphenation. On Linux AND Windows, libhyphen (Liang's algorithm) backs core/text/hyphenator.cpp
# (Type hyphenation, deferred §1); it ships no pkg-config file, so locate the lib + header directly,
# PRIVATE behind a PImpl like the above. macOS (S58) uses the system hyphenator (CoreFoundation's
# CFStringGetHyphenationLocationBeforeIndex) via text/hyphenator_macos.mm INSTEAD, so libhyphen is not
# cross-built there and no /usr/share/hyphen dictionaries are bundled -- the OS provides the data.
# Windows has no hyphenation API at all, so it keeps libhyphen (cross-built by
# packaging/windows/build-deps.sh) and ships the hyph_*.dic files inside the application payload --
# see MOSAIC_HYPHEN_DIR below and the _WIN32 branch of hyphenator.cpp for how they are found.
# Taken under libhyphen's LGPL-2.1/MPL arms for GPLv3 compatibility (docs/third-party-licenses.md).
if(APPLE)
    set(MOSAIC_HYPHEN_SOURCE text/hyphenator_macos.mm)
else()
    find_library(HYPHEN_LIB NAMES hyphen REQUIRED)
    find_path(HYPHEN_INCLUDE_DIR NAMES hyphen.h REQUIRED)
    set(MOSAIC_HYPHEN_SOURCE text/hyphenator.cpp)
endif()

# enchant-2 backs core/text/spell_checker.cpp (Type spell-checking, deferred §2). It unifies the
# hunspell/nuspell/aspell providers + finds the system dictionaries, and (unlike libhyphen) ships a
# pkg-config file. PRIVATE behind a PImpl; LGPL-2.1+ (GPLv3-compatible, docs/third-party-licenses.md).
# macOS (S58) uses the system spellchecker (NSSpellChecker) via spell_checker_macos.mm and Windows
# (S57) the OS Spell Checking API (ISpellChecker, Windows 8+) via spell_checker_win32.cpp INSTEAD, so
# enchant + glib are cross-built for NEITHER -- both OSes provide the dictionaries, including the
# extra languages the user installs through their own system settings.
if(APPLE)
    set(MOSAIC_SPELL_SOURCE text/spell_checker_macos.mm)
elseif(WIN32)
    set(MOSAIC_SPELL_SOURCE text/spell_checker_win32.cpp)
else()
    pkg_check_modules(ENCHANT REQUIRED IMPORTED_TARGET enchant-2)
    set(MOSAIC_SPELL_SOURCE text/spell_checker.cpp)
endif()

mosaic_add_module(core
    SOURCES
        core.cpp
        layer.cpp
        tile_grid.cpp
        adjustments.cpp
        document.cpp
        command.cpp
        commands.cpp
        color_management.cpp
        color_sample.cpp
        selection.cpp
        stroke_confinement.cpp
        layer_grow.cpp
        edge_grow.cpp
        red_eye.cpp
        clone_stamp.cpp
        fill.cpp
        clipboard.cpp
        text/text_model.cpp
        text/text_edit.cpp
        text/language.cpp
        text/tokenize.cpp
        ${MOSAIC_HYPHEN_SOURCE}
        ${MOSAIC_SPELL_SOURCE}
        text/spell_scan.cpp
        text/spell_worker.cpp
        text/shaping.cpp
        text/text_render.cpp
        text/text_layer_render.cpp
        text/extrude_mesh.cpp
        text/extrude_overlay.cpp
        text/extrude_render.cpp
        texture/noise.cpp
        texture/solar.cpp
        texture/lunar.cpp
        texture/star_catalog.cpp
        texture/city_catalog.cpp
        texture/moon_texture.cpp
        texture/moon_elevation.cpp
        texture/cloud_volume.cpp
        texture/atmosphere.cpp
        texture/sky_render.cpp
        texture/sky_almanac.cpp
        texture/sky_estimate.cpp
        texture/sky_estimate_worker.cpp
        texture/sky_estimate_commit.cpp
        texture/paper_render.cpp
        texture/grass_render.cpp
        texture/material_render.cpp
        texture/texture_render.cpp
        texture/texture_layer_render.cpp
        texture/render_worker.cpp
        vector/flatten.cpp
        vector/corner.cpp
        vector/boolean.cpp
        vector/to_path.cpp
        vector/hit.cpp
        vector/pattern.cpp
        vector/raster.cpp
        vector/stroke.cpp
        brush/brush_engine.cpp
        brush/curve.cpp
        brush/sensors.cpp
        brush/stroke_path.cpp
        brush/stroke_preview.cpp
        brush/stroke_smoother.cpp
        brush/stroke_state.cpp
        brush/tablet_policy.cpp
        brush/curve_option.cpp
        brush/mask_generator.cpp
        brush/dab_mask.cpp
        brush/mask_stroke.cpp
        brush/dab.cpp
        brush/bitmap_tip.cpp
        brush/dab_cache.cpp
        brush/brush_tip.cpp
        brush/tip_outline.cpp
        brush/stroke_painter.cpp
        brush/sketch_painter.cpp
        brush/hairy_painter.cpp
        brush/curve_painter.cpp
        brush/particle_painter.cpp
        brush/experiment_painter.cpp
        brush/hatching.cpp
        brush/texture.cpp
        inpaint/inpaint_engine.cpp
        inpaint/outpaint.cpp
        inpaint/backends/he_sun/offset_statistics.cpp
        inpaint/backends/he_sun/working_region.cpp
        inpaint/backends/he_sun/graph_cut.cpp
        inpaint/backends/he_sun/graph_completion.cpp
        inpaint/backends/he_sun/offset_stats_backend.cpp
        inpaint/backends/pde/pde_backend.cpp
        inpaint/backends/resynth/resynth_backend.cpp
        inpaint/backends/script/script_backend.cpp
        retarget/credits.cpp
        retarget/importance_map.cpp
        retarget/smart_crop.cpp
        retarget/keep_regions.cpp
        retarget/recompose.cpp
        retarget/inpaint_fill.cpp
    DEPS
        mosaic::common)

# pugixml backs core/brush/sensors.cpp (the <params id=...> dynamics fragment) and, at Arc B, the
# .kpp preset document. PRIVATE: the pugi types never appear in a header, like lcms2 above.
target_link_libraries(mosaic_core PRIVATE PkgConfig::LCMS2 PkgConfig::FREETYPE PkgConfig::HARFBUZZ
    earcut::earcut pugixml::pugixml hosekwilkie::hosekwilkie)
if(APPLE)
    # spell_checker_macos.mm -> NSSpellChecker (AppKit); hyphenator_macos.mm -> CoreFoundation.
    target_link_libraries(mosaic_core PRIVATE "-framework AppKit" "-framework Foundation"
        "-framework CoreFoundation")
elseif(WIN32)
    # libhyphen (hyphenator.cpp) stays; the spell backend is the OS one, which needs COM's allocator
    # and CoCreateInstance from ole32. MinGW's default standard-library list already carries ole32,
    # but naming it keeps the dependency honest rather than inherited. No `uuid`: the GUIDs
    # spell_checker_win32.cpp needs are absent from MinGW's libuuid.a and are spelled out in the
    # source instead (see the comment there).
    target_link_libraries(mosaic_core PRIVATE ${HYPHEN_LIB} ole32)
    target_include_directories(mosaic_core PRIVATE ${HYPHEN_INCLUDE_DIR})
else()
    # libhyphen (hyphenator.cpp) + enchant (spell_checker.cpp) are the Linux text backends.
    target_link_libraries(mosaic_core PRIVATE ${HYPHEN_LIB} PkgConfig::ENCHANT)
    target_include_directories(mosaic_core PRIVATE ${HYPHEN_INCLUDE_DIR})
endif()

# The system hyphenation-dictionary dir (hyph_*.dic), read at runtime by the libhyphen Hyphenator.
# Overridable per platform / for a bundled fallback (deferred §1); defaults in hyphenator.cpp to
# /usr/share/hyphen. macOS hyphenates via CoreFoundation (no .dic files), so it is excluded.
#
# On Windows there is no system dictionary directory to point at -- the .dic files ship inside the
# application payload -- so hyphenator.cpp resolves installedDataDir()/"hyphen" FIRST and this stays
# only as the (never-matching, harmless) tail of its search order.
if(NOT APPLE)
    target_compile_definitions(mosaic_core PRIVATE MOSAIC_HYPHEN_DIR="/usr/share/hyphen")
endif()

mosaic_optimize_pixel_loops(mosaic_core)  # selection rasterizers/clipboard walks run per gesture frame

# The vendored default CMYK press profile (FOGRA39-based; HEIDELBERG licence -- see
# third_party/icc-profiles/README.md) is loaded from disk at runtime: embedding a 1.8 MB binary
# through EmbedAssets.cmake is pathologically slow (it is a CMake-script byte converter sized for
# small SVGs), and a real file is what S12-c (user profiles) and S59 (installed data + licence
# strippability) want anyway. Dev builds read straight from the source tree; S59 replaces this
# with an installed-data-dir lookup.
target_compile_definitions(mosaic_core PRIVATE
    MOSAIC_DEFAULT_CMYK_PROFILE="${CMAKE_SOURCE_DIR}/third_party/icc-profiles/ISOcoated_v2_300_eci.icc")
