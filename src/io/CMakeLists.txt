# libpng + libjpeg-turbo back the S18-b PNG/JPEG codecs. PRIVATE: their types never appear in
# io's headers (loadImage hands back a common::Image), so dependents don't inherit them.
# io.cpp is the thin dispatch facade (sniff -> codec); png.cpp/jpeg.cpp own their libraries;
# mosaic/ is the native .mosaic format (docs/mosaic-native-format.md, S48 Build 1).
find_package(PkgConfig REQUIRED)
pkg_check_modules(PNG REQUIRED IMPORTED_TARGET libpng)
pkg_check_modules(TURBOJPEG REQUIRED IMPORTED_TARGET libturbojpeg)
# LZ4 (fast tier) + zstd (balanced/max) back the .mosaic compression profiles (spec 2.4).
# System deps by design (user call): both ubiquitous; zstd is not sanely vendorable.
pkg_check_modules(LZ4 REQUIRED IMPORTED_TARGET liblz4)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
# zlib backs the brush-format walkers directly (zTXt/iTXt inflate + chunk CRCs, brush/png_text.cpp).
# It was always here transitively under libpng; this only names the dependency.
pkg_check_modules(ZLIB REQUIRED IMPORTED_TARGET zlib)
# JPEG XL (libjxl) is OPTIONAL -- NOT REQUIRED, so a machine without libjxl still configures and
# builds; the .jxl export paths simply disable (io::jxlSupported() returns false). libjxl_threads
# backs the resizable parallel runner jxl.cpp drives. When found, MOSAIC_HAVE_JXL is defined on the
# io target below and jxl.cpp compiles its real encoder; otherwise jxl.cpp compiles inert stubs.
pkg_check_modules(JXL IMPORTED_TARGET libjxl libjxl_threads)

# ---------------------------------------------------------------------------------------------
# M4 -- the common raster codecs (docs/export-system-plan.md §2.3). EVERY ONE IS OPTIONAL, on the
# libjxl pattern: no REQUIRED, a MOSAIC_HAVE_* define when found, and inert stubs in the .cpp when
# not, so a distribution missing any of them still builds a working Mosaic that simply does not
# offer that format.
#
# libwebp needs libwebpmux too -- the plain library writes a bare bitstream, and the ICCP/EXIF
# chunks live in the extended RIFF container only the mux can assemble.
pkg_check_modules(WEBP IMPORTED_TARGET libwebp libwebpmux)
# libavif. ⚠ Which AV1 ENCODER it uses is a deliberate constraint, not a packaging detail: src/io/avif.cpp
# always sets an explicit codecChoice of AOM (libaom) or SVT (SVT-AV1) and refuses to encode when
# neither is present -- it never lets AVIF_CODEC_CHOICE_AUTO pick, because several distributions
# build libavif against a Rust encoder Mosaic does not ship and will not invoke. Nothing here
# links that encoder; the choice is enforced in code, at runtime, where it is actually observable.
pkg_check_modules(AVIF IMPORTED_TARGET libavif)
pkg_check_modules(TIFF IMPORTED_TARGET libtiff-4)
# giflib ships NO pkg-config file (it has never had one), so it is found the plain way: the header
# and the library, both or neither.
find_path(GIFLIB_INCLUDE_DIR NAMES gif_lib.h)
find_library(GIFLIB_LIBRARY NAMES gif)
if(GIFLIB_INCLUDE_DIR AND GIFLIB_LIBRARY)
    set(GIFLIB_FOUND TRUE)
endif()

mosaic_add_module(io
    SOURCES
        io.cpp
        exif.cpp
        exif_write.cpp
        png.cpp
        jpeg.cpp
        jxl.cpp
        # S42/M4 -- the common raster codecs. Each compiles either its real encoder or an inert
        # stub, depending on the MOSAIC_HAVE_* gate set below; quantize.cpp is dependency-free
        # (the palette + dither behind GIF, and behind M5's BMP/PCX/ICO writers).
        webp.cpp
        avif.cpp
        tiff.cpp
        gif.cpp
        quantize.cpp
        # S41/M2 -- the format framework: capability data + the loss diff + the typed options
        # schema + the backend registry. Pure and FLTK-free (docs/export-system-plan.md §2.1).
        caps.cpp
        document_profile.cpp
        export_path.cpp
        options_schema.cpp
        format_registry.cpp
        backends/png_backend.cpp
        backends/jpeg_backend.cpp
        backends/jxl_backend.cpp
        backends/webp_backend.cpp
        backends/avif_backend.cpp
        backends/tiff_backend.cpp
        backends/gif_backend.cpp
        # S42/M5 -- the ONE adapter over libmosaicformats (src/formats, namespace mosaicfmt): it
        # turns that library's own pixel-buffer view into common::Image and back, so the curated
        # codecs themselves stay free of every Mosaic type (§2.2, and §11 tests them standalone).
        backends/mosaicformats_backend.cpp
        brush/abr.cpp
        brush/bundle.cpp
        brush/kpp.cpp
        brush/library.cpp
        brush/mapper.cpp
        brush/preset_brush.cpp
        brush/md5.cpp
        brush/myb.cpp
        brush/png_text.cpp
        brush/preset_json.cpp
        brush/preset_xml.cpp
        brush/tip_io.cpp
        brush/zip.cpp
        mosaic/blob.cpp
        mosaic/chunk.cpp
        mosaic/codec.cpp
        mosaic/compaction.cpp
        mosaic/docio.cpp
        mosaic/docjson.cpp
        mosaic/file.cpp
        mosaic/journal.cpp
        mosaic/manifest_tokens.cpp
        mosaic/journal_session.cpp
        mosaic/lock.cpp
        mosaic/paeth.cpp
        mosaic/tagscan.cpp
        mosaic/fileinfo.cpp
        mosaic/preview.cpp
        mosaic/records.cpp
        mosaic/reedsolomon.cpp
        mosaic/salvage.cpp
        mosaic/save.cpp
    DEPS mosaic::common mosaic::core # core: the document<->container bridge (mosaic/docio)
        mosaic::formats) # M5: the dependency-free curated codecs, reached only via the adapter above

# The .mosaic checksums come from VENDORED implementations (third_party/xxhash, third_party/
# blake3) -- deliberately not system deps, so niche distros without the packages still build.
# LZ4 + zstd (system, like libpng) join with the compression-profile slice.
target_link_libraries(mosaic_io PRIVATE PkgConfig::PNG PkgConfig::TURBOJPEG PkgConfig::LZ4
    PkgConfig::ZSTD PkgConfig::ZLIB xxhash::xxhash blake3::blake3
    nlohmann::json # root/DIR payloads are JSON (spec: only the envelope is binary)
    pugixml::pugixml) # the preset XML layer (brush/preset_xml.cpp, docs/brushes.md §7)

# Gate JXL: only when libjxl was found do we define MOSAIC_HAVE_JXL (so jxl.cpp compiles the real
# libjxl encoder) and link it. Without it, jxl.cpp still compiles -- as inert stubs -- and nothing
# links libjxl, so the build succeeds on a machine that has no libjxl.
if(JXL_FOUND)
    target_compile_definitions(mosaic_io PRIVATE MOSAIC_HAVE_JXL)
    if(APPLE)
        # The macOS dependencies are cross-built as static archives (packaging/macos/build-deps.sh),
        # and a static libjxl has to name its PRIVATE dependencies too. Brotli's common half is the
        # one that bites: it sits in libbrotlienc's Requires.private, which the plain
        # (shared-oriented) resolution drops, and the link then fails on BrotliDefaultAllocFunc and
        # friends. The _STATIC_ variables are pkg-config's --static answer, which walks those --
        # including the -DJXL_STATIC_DEFINE that makes JXL_EXPORT right for a static link.
        target_include_directories(mosaic_io PRIVATE ${JXL_STATIC_INCLUDE_DIRS})
        target_compile_options(mosaic_io PRIVATE ${JXL_STATIC_CFLAGS_OTHER})
        target_link_libraries(mosaic_io PRIVATE ${JXL_STATIC_LDFLAGS})
    else()
        target_link_libraries(mosaic_io PRIVATE PkgConfig::JXL)
    endif()
endif()

# The M4 gates, all the same shape as the JXL one above: define the macro, link the library, and
# leave the .cpp compiling stubs when the library is absent. Nothing here is REQUIRED, so a
# missing codec costs exactly one format in the combobox and nothing else.
if(WEBP_FOUND)
    target_compile_definitions(mosaic_io PRIVATE MOSAIC_HAVE_WEBP)
    target_link_libraries(mosaic_io PRIVATE PkgConfig::WEBP)
endif()
if(AVIF_FOUND)
    target_compile_definitions(mosaic_io PRIVATE MOSAIC_HAVE_AVIF)
    target_link_libraries(mosaic_io PRIVATE PkgConfig::AVIF)
endif()
if(TIFF_FOUND)
    target_compile_definitions(mosaic_io PRIVATE MOSAIC_HAVE_TIFF)
    target_link_libraries(mosaic_io PRIVATE PkgConfig::TIFF)
endif()
if(GIFLIB_FOUND)
    target_compile_definitions(mosaic_io PRIVATE MOSAIC_HAVE_GIF)
    target_include_directories(mosaic_io PRIVATE ${GIFLIB_INCLUDE_DIR})
    target_link_libraries(mosaic_io PRIVATE ${GIFLIB_LIBRARY})
endif()

# Windows (S57): two path-resolving slices of this module ask the SHELL where an app-owned or a user
# folder is, rather than trusting getenv -- mosaic/journal.cpp for the recovery directory
# (FOLDERID_LocalAppData) and export_path.cpp for the picker's start folder (FOLDERID_Pictures /
# FOLDERID_Documents, which OneDrive redirects on most machines, so %USERPROFILE%\Pictures is simply
# wrong there). That costs three import libraries: shell32 for SHGetKnownFolderPath, ole32 for the
# CoTaskMemFree that releases its PWSTR, and uuid for the KNOWNFOLDERID constants themselves.
# CMake's MinGW defaults already put all three on the link line via CMAKE_CXX_STANDARD_LIBRARIES;
# naming them makes the link independent of that default, and documents why an FLTK-free, UI-free
# module reaches into shell32 at all.
if(WIN32)
    target_link_libraries(mosaic_io PRIVATE shell32 ole32 uuid)
endif()

mosaic_optimize_pixel_loops(mosaic_io) # the decode unpacks whole images
