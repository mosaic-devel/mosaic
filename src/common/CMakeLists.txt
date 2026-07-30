# common -- shared types/utilities: version, image buffer, logging (spdlog), i18n (gettext),
# settings store (nlohmann/json), and later math/color/geometry. Special-cased (not via
# mosaic_add_module) because it also generates and exposes the build-time version header and
# pulls in several third-party libraries.

include(GNUInstallDirs)  # CMAKE_INSTALL_FULL_LOCALEDIR

configure_file(version_config.hpp.in "${CMAKE_BINARY_DIR}/generated/version_config.hpp" @ONLY)

# spdlog is required (the logging backend). Intl/gettext is best-effort: on glibc it is part of
# libc; where it is missing, i18n falls back to passing strings through untranslated.
find_package(spdlog REQUIRED CONFIG)
find_package(Intl)

add_library(mosaic_common STATIC
    version.cpp
    fs_path.cpp
    image.cpp
    image_svg.cpp
    logging.cpp
    i18n.cpp
    profiler.cpp
    recent_files.cpp
    settings.cpp
    thread_pool.cpp)
add_library(mosaic::common ALIAS mosaic_common)
target_include_directories(mosaic_common PUBLIC
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_BINARY_DIR}/generated")
target_compile_features(mosaic_common PUBLIC cxx_std_20)

# spdlog is PUBLIC: common/log.hpp includes <spdlog/spdlog.h>, so consumers need it too.
# nlohmann/json (settings.cpp) and nanosvg (image_svg.cpp) are implementation details (PRIVATE).
# Intl likewise stays behind common's tr() (PRIVATE).
target_link_libraries(mosaic_common
    PUBLIC  spdlog::spdlog
    PRIVATE nlohmann::json nanosvg::nanosvg)

if(WIN32)
    # settings.cpp asks the shell where the per-user AppData folders are instead of trusting
    # %APPDATA%/%LOCALAPPDATA% (S57): SHGetKnownFolderPath lives in shell32, the FOLDERID_* GUIDs it
    # takes are in MinGW's uuid archive, and freeing its result is CoTaskMemFree from ole32. MinGW's
    # default standard-library list already carries shell32 and ole32; naming all three keeps the
    # dependency honest rather than inherited, and `uuid` is genuinely not in that default list.
    target_link_libraries(mosaic_common PRIVATE shell32 ole32 uuid)
endif()

# profiler.cpp owns the ONE definition of the collection master switch, and its default differs by
# build type: debug ON (the Help-menu FPS readout and Timing Profiler window work with no flag),
# release OFF until --profile / MOSAIC_PROFILE=1. Same $<CONFIG:Debug> generator-expression pattern
# src/ui and src/render use. PRIVATE on purpose -- consumers never see the flag, only enabled().
target_compile_definitions(mosaic_common PRIVATE $<$<CONFIG:Debug>:MOSAIC_DEBUG>)

# i18n: where catalogs are installed, and whether gettext is available at all. On macOS and Windows
# this baked-in path names a directory in the CROSS-BUILD PREFIX that cannot exist on the user's
# machine; it stays as the tail of i18n.cpp's search order, behind the executable-relative lookup that
# actually resolves there (installedDataDir()/"locale").
target_compile_definitions(mosaic_common PRIVATE
    "MOSAIC_LOCALEDIR=\"${CMAKE_INSTALL_FULL_LOCALEDIR}\"")
# Runtime data (the shipped brush set, docs/brushes.md §4/§7): the install prefix, plus the
# source tree as installedDataDir()'s dev fallback so build-tree binaries need no install step.
target_compile_definitions(mosaic_common PRIVATE
    "MOSAIC_DATA_DIR=\"${CMAKE_INSTALL_FULL_DATADIR}/mosaic\""
    "MOSAIC_SOURCE_DATA_DIR=\"${CMAKE_SOURCE_DIR}/data\"")
if(Intl_FOUND)
    target_link_libraries(mosaic_common PRIVATE Intl::Intl)
    target_compile_definitions(mosaic_common PRIVATE MOSAIC_HAVE_GETTEXT=1)
    if(APPLE)
        # GNU libintl is a real static library here rather than part of libc: it charsets through
        # iconv and reads the user's language list from CFPreferences/CFLocale, and FindIntl names
        # neither. (On glibc, Intl_IS_BUILT_IN makes this whole branch moot.)
        target_link_libraries(mosaic_common PRIVATE iconv "-framework CoreFoundation")
    endif()
    # Windows needs no companion libraries here: GNU libintl is a DLL that carries its own imports,
    # unlike the macOS static build above. What it DOES need is that i18n.cpp reaches it through
    # wbindtextdomain() rather than bindtextdomain() -- see the comment there -- which is a gettext
    # 0.22+ entry point. packaging/windows/build-deps.sh must therefore not regress below 0.22.
endif()

mosaic_apply_warnings(mosaic_common)
mosaic_apply_sanitizers(mosaic_common)
mosaic_optimize_pixel_loops(mosaic_common)  # toFloat/toImage8/alphaBounds run per frame
