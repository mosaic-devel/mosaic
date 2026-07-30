# Vendored, header-only, permissively-licensed dependencies.
# Each library keeps its upstream LICENSE alongside it; see docs/third-party-licenses.md.

# doctest (MIT) -- unit-testing framework. Header at third_party/doctest/doctest.h,
# included as <doctest/doctest.h>.
add_library(doctest INTERFACE)
add_library(doctest::doctest ALIAS doctest)
target_include_directories(doctest INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")

# nanosvg (zlib) -- header-only SVG parser + rasterizer. Headers at
# third_party/nanosvg/, included as <nanosvg.h> / <nanosvgrast.h>. The implementation
# macros are defined in exactly one TU (src/common/image_svg.cpp). Used to rasterize the
# app icon (and later tool icons) from the single SVG source. The rasterizer needs libm.
add_library(nanosvg INTERFACE)
add_library(nanosvg::nanosvg ALIAS nanosvg)
target_include_directories(nanosvg INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/nanosvg")
if(UNIX)
    target_link_libraries(nanosvg INTERFACE m)
endif()

# nlohmann/json (MIT) -- single-header JSON library, pinned at v3.11.3. Header at
# third_party/nlohmann/json.hpp, included as <nlohmann/json.hpp>. The serialization format
# for the settings store (S5) and, later, the .mosaic manifest / .ora / .kpp readers.
add_library(nlohmann_json INTERFACE)
add_library(nlohmann::json ALIAS nlohmann_json)
target_include_directories(nlohmann_json INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")

# earcut.hpp (ISC) -- Mapbox's header-only ear-clipping polygon triangulator (holes supported).
# Header at third_party/earcut/earcut.hpp, included as <earcut.hpp>. Triangulates the glyph
# contours into the extruded-text mesh's front/back caps (Type 3D, S30-c; docs/type-tool.md §10.2).
add_library(earcut INTERFACE)
add_library(earcut::earcut ALIAS earcut)
target_include_directories(earcut INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/earcut")

# Vulkan Memory Allocator (MIT), pinned at v3.3.0. Header at third_party/vma/vk_mem_alloc.h,
# included as <vk_mem_alloc.h>. Header-only API; the implementation is compiled in exactly one
# TU (src/render/vma_impl.cpp). Backs all device memory in the GPU compositor (S7-b) onward.
add_library(vma INTERFACE)
add_library(vma::vma ALIAS vma)
target_include_directories(vma INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/vma")

# xxHash (BSD-2), pinned at v0.8.3 -- single-header in XXH_INLINE_ALL mode (the official
# no-link usage). Header at third_party/xxhash/xxhash.h, included as <xxhash.h>; the one
# consumer TU (src/io/mosaic/chunk.cpp) defines XXH_INLINE_ALL before including. Backs the
# .mosaic fast checksum tier (xxh3-64, spec 2.2). Vendored rather than a system dep so niche
# distros without libxxhash can still build a Save that works.
add_library(xxhash INTERFACE)
add_library(xxhash::xxhash ALIAS xxhash)
target_include_directories(xxhash INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/xxhash")

# BLAKE3 (CC0-1.0 OR Apache-2.0; taken under CC0), pinned at 1.8.2 -- the upstream C reference
# implementation, PORTABLE lanes only (the SIMD .c/.S files are deliberately not vendored:
# .mosaic uses BLAKE3 for ROOT-chunk checksums, a few KB per save, where portable throughput is
# already orders of magnitude past need). If H4 content-addressing (Build 2) ever measures
# hashing as hot, drop the upstream SIMD sources in beside these and delete the NO_* defines.
# Included as <blake3.h>. Vendored for the same niche-distro reason as xxHash.
enable_language(C) # blake3 is C; the rest of the tree is C++
add_library(blake3 STATIC
    blake3/blake3.c
    blake3/blake3_dispatch.c
    blake3/blake3_portable.c)
add_library(blake3::blake3 ALIAS blake3)
target_include_directories(blake3 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/blake3")
target_compile_definitions(blake3 PRIVATE
    BLAKE3_NO_SSE2 BLAKE3_NO_SSE41 BLAKE3_NO_AVX2 BLAKE3_NO_AVX512 BLAKE3_USE_NEON=0)

# pugixml (MIT), pinned at v1.16 (c8033ce9d039e7f9d134877c363397b3cfe20816). Included as
# <pugixml.hpp>. The XML reader under the brush-preset formats (docs/brushes.md §7): the sensor
# <params> fragments inside a .kpp's dynamics params, and later the preset document itself, its
# nested brush_definition sub-documents, and a .bundle's META-INF/manifest.xml.
#
# A hand-rolled scanner is a false economy here and the note says so: attribute order is not fixed
# in shipped presets (a name=-first regex mis-parses 64 of 82), and the input is a third-party file
# a user chose to open, so entity refs, CDATA and hostile nesting are all reachable.
#
# Vendored rather than a system dep for the same reason as xxHash/BLAKE3 -- brush import must not
# hinge on a distro shipping libpugixml. XPath is compiled out: nothing here queries, and it is
# the only part of pugixml that throws.
add_library(pugixml STATIC pugixml/pugixml.cpp)
add_library(pugixml::pugixml ALIAS pugixml)
target_include_directories(pugixml PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/pugixml")
target_compile_definitions(pugixml PUBLIC PUGIXML_NO_XPATH)

# Hosek-Wilkie analytic sky-dome model (BSD-3), upstream v1.4a -- the Texture Generator's sky
# radiance (docs/texture-generator.md §4.1; the photorealism front-runner among analytic daylight
# models, and decisively BSD). Vendored TRIMMED to the RGB lane (the BLAKE3 portable-lanes
# precedent): the ~580 KB spectral/XYZ datasets and the alien-world / spectral-solar functions
# serve nothing Mosaic calls. Included as <ArHosekSkyModel.h>; evaluated per composite render by
# src/core/texture/sky_render.cpp. Vendored (not a system dep) because no distro packages it.
add_library(hosekwilkie STATIC hosekwilkie/ArHosekSkyModel.c)
add_library(hosekwilkie::hosekwilkie ALIAS hosekwilkie)
target_include_directories(hosekwilkie PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/hosekwilkie")
if(UNIX)
    target_link_libraries(hosekwilkie PRIVATE m)
endif()
