cmake_minimum_required(VERSION 3.25)

project(Mosaic
    # 0.3.0 -- the first TAGGED, published release (PLAN.md item 9, decision 2026-08-24).
    #
    # This supersedes the "perma-alpha, frozen at 0.2.17" rule. That rule existed because a
    # 0.<phase>.<session> number could not honestly describe a build of a non-linear roadmap, and
    # nothing was ever published, so the git commit was the only identifier that meant anything.
    # Publishing changes that: a downloaded binary has no git tree to interrogate, and a bug report
    # has to be able to name a build. So the version now moves only when something is RELEASED,
    # which is what SemVer asks of it. The short git commit is still appended (0.3.0+g<rev>) and is
    # still the exact identifier; the number in front of it is now the human-facing one.
    VERSION 0.3.0
    DESCRIPTION "A professional, GPU-accelerated, cross-platform image editor"
    HOMEPAGE_URL "https://github.com/mosaic-devel/mosaic"
    LANGUAGES CXX)

# ---- Guardrails -------------------------------------------------------------
if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR
        "In-source builds are not allowed. Use a preset, e.g. `cmake --preset linux-debug`.")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# ---- Build version: project version + short git commit ----------------------
# MOSAIC_GIT_REV ("g<hash>" / "g<hash>-dirty", empty without git) is baked into the version header
# (src/common) and shown by `mosaic --version`. Configure re-runs when a commit lands (the reflog
# moves), so the hash tracks HEAD. See PLAN.md §8.9.
set(MOSAIC_GIT_REV "")
find_package(Git QUIET)
if(Git_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE _git_hash OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _git_rc ERROR_QUIET)
    if(_git_rc EQUAL 0 AND _git_hash)
        set(MOSAIC_GIT_REV "g${_git_hash}")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" diff --quiet HEAD --
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" RESULT_VARIABLE _git_dirty)
        if(NOT _git_dirty EQUAL 0)
            set(MOSAIC_GIT_REV "${MOSAIC_GIT_REV}-dirty")
        endif()
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/.git/logs/HEAD")
endif()
if(MOSAIC_GIT_REV)
    message(STATUS "Mosaic version: ${PROJECT_VERSION}+${MOSAIC_GIT_REV}")
else()
    message(STATUS "Mosaic version: ${PROJECT_VERSION}")
endif()

# ---- Global C++ settings ----------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ---- Windows API surface (S57) ----------------------------------------------
# These belong to MOSAIC's project scope, deliberately NOT to cmake/toolchains/mingw-w64.cmake:
# a toolchain file is also read when cross-building the third-party stack, and these three are
# OPINIONS about the Win32 API surface rather than properties of the target.
#
#   UNICODE/_UNICODE  make the generic Win32 macros (RegOpenKeyEx, GetModuleHandleEx, ...) resolve
#                     to their -W variants, which is the only way a path with non-ASCII characters
#                     survives. ⚠ Forcing this on a dependency BREAKS the ones written against the
#                     ANSI API: the Vulkan-Loader carries char* strings and does its own UTF-8
#                     conversion, so every generic macro became a wide call taking a char* and it
#                     failed with a screenful of incompatible-pointer errors. Earned, not theoretical.
#   NOMINMAX          drop <windows.h>'s min/max macros so they cannot shadow std::min/std::max.
#                     Also not safe to impose globally -- a C dependency may use those macros.
#
# The version floor (_WIN32_WINNT/WINVER) is the opposite case and DOES live in the toolchain:
# every library in the payload must agree on it or a struct grows a field on one side of a link.
if(WIN32)
    add_compile_definitions(UNICODE _UNICODE NOMINMAX)
endif()

# Keep all build artifacts under a single, predictable tree.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# Emit compile_commands.json for clangd / clang-tidy.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---- Options ----------------------------------------------------------------
option(MOSAIC_BUILD_TESTS "Build unit tests" ON)
set(MOSAIC_SANITIZE "" CACHE STRING
    "Sanitizers to enable as a comma list, e.g. address,undefined (empty = none)")

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # spdlog bundles fmt 10.2.1, whose consteval format-string parser chokes under a very new clang
    # -- but only on the brace-nested literal "{} [{}({})]" in the exception path
    # SPDLOG_LOGGER_CATCH ("call to consteval function ... is not a constant expression"). Normal log
    # calls compile fine. SPDLOG_NO_EXCEPTIONS drops exactly that call site (spdlog then reports
    # logging errors via its error handler instead of throwing -- fine for an app). (S58)
    #
    # Keyed on the COMPILER, not on APPLE, which is what S58 wrote when osxcross was the only clang in
    # the build: S57's Windows-on-ARM target is llvm-mingw, and it reproduced this identically -- 36
    # instances of the same diagnostic from the same spdlog line. Two unrelated platforms failing the
    # same way is evidence about the compiler, not about the platforms. (⚠ FMT_USE_CONSTEVAL=0 does
    # NOT help; fmt 10.2.1 ignores it. Do not re-try that.)
    add_compile_definitions(SPDLOG_NO_EXCEPTIONS)
endif()

include(MosaicHelpers)
include(EmbedShaders)   # GLSL -> SPIR-V -> embedded C++ header pipeline
include(EmbedAssets)    # binary assets (e.g. SVGs) -> embedded C++ header pipeline
include(Dependencies)   # system-first dependency probe + report (informational early on)

# ---- Targets ----------------------------------------------------------------
# SYSTEM: vendored headers are exempt from the mandatory -Werror wall -- their diagnostics
# are upstream's business; ours build clean (mosaic_apply_warnings).
add_subdirectory(third_party SYSTEM)
add_subdirectory(src)
add_subdirectory(tools)  # optional dev tools (inpaint_bench); self-disables unless its option is ON

if(MOSAIC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

add_subdirectory(po)  # i18n template-extraction ('pot') target; self-disables without xgettext

# ---- Install ----------------------------------------------------------------
# Runtime data: the CC-0 default brush set (docs/brushes.md §4, docs/credits.md). Installed
# under CMAKE_INSTALL_DATADIR/mosaic -- the prefix common::installedDataDir() is compiled
# against; dev builds read the same files from data/ via its source-tree fallback.
include(GNUInstallDirs)
install(FILES data/brushes/Krita_4_Default_Resources.bundle
    DESTINATION "${CMAKE_INSTALL_DATADIR}/mosaic/brushes")

# The vendored FOGRA39-based default CMYK press profile. core/color_management.cpp resolves it as
# installedDataDir()/icc-profiles/ISOcoated_v2_300_eci.icc and falls back to a compile-time DEV path
# that does not exist on a user's machine -- so without this rule every INSTALLED build (AppImage
# included) silently had hasCmyk() == false and hid its CMYK UI. Loaded from disk on purpose: a
# distro that objects to the profile's redistributable-but-non-DFSG licence can delete this one file
# and get exactly that graceful degradation (docs/third-party-licenses.md).
install(FILES third_party/icc-profiles/ISOcoated_v2_300_eci.icc
    DESTINATION "${CMAKE_INSTALL_DATADIR}/mosaic/icc-profiles")

# Linux desktop integration (S48-b; overlaps S59 packaging, laid here first): the binary, the
# .desktop entry that makes `mosaic %f` reachable from a file manager, the shared-mime-info
# declaration of image/x-mosaic (magic on the container preamble + *.mosaic glob), and the
# hicolor icons -- the app icon for the .desktop's Icon=mosaic, a document-flavoured variant
# under the freedesktop mimetype icon name image-x-mosaic. These rules are AUTHORED, never run
# by the dev workflow: packaging (S59) or an explicit `cmake --install` executes them, and the
# post-install cache refreshes (update-mime-database, update-desktop-database,
# gtk-update-icon-cache) are the package manager's hooks, not build steps.
install(TARGETS mosaic RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES data/desktop/mosaic.desktop
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
install(FILES data/desktop/mosaic.xml
    DESTINATION "${CMAKE_INSTALL_DATADIR}/mime/packages")
install(FILES assets/app_icon.svg
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
    RENAME mosaic.svg)
install(FILES assets/mimetype_icon.svg
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/mimetypes"
    RENAME image-x-mosaic.svg)

mosaic_print_summary()
