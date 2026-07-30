# mosaic-thumbnailer (S48-b): the freedesktop thumbnailer for .mosaic files -- a SEPARATE small
# binary (the ffmpegthumbnailer shape, user call) because a file manager spawns one per file in
# a directory. Deliberately links mosaic::io + mosaic::common only: no FLTK, no Vulkan, no
# fontconfig, no display. (mosaic_io transitively carries mosaic_core for the document bridge;
# nothing in that chain opens a display or touches a GPU -- the binary reads the newest PRVW
# chunk and writes a PNG.)
include(GNUInstallDirs)

# macOS has no per-file thumbnailer protocol to spawn a binary into: since 10.15 the supported
# path is a Quick Look THUMBNAIL EXTENSION, which the system loads out-of-process from the app's
# Contents/PlugIns. So on Apple this directory builds that extension's executable instead of the
# freedesktop binary + KIO plugin below -- same PRVW read, same io+common-only dependency, a
# different host. packaging/macos/make-dmg.sh wraps it in the .appex bundle and signs it.
if(APPLE)
    # One binary, two principal classes: the thumbnail provider and the space-bar preview provider.
    # make-dmg.sh wraps it TWICE, because NSExtensionPointIdentifier is single-valued and each
    # extension point needs its own bundle. QuickLookUI carries QLPreviewProvider/QLPreviewReply on
    # macOS (QuickLookThumbnailing carries only the thumbnail half); AppKit draws the caption text.
    add_executable(mosaic-quicklook quicklook_macos.mm)
    target_link_libraries(mosaic-quicklook PRIVATE mosaic::io mosaic::common
        "-framework Foundation" "-framework CoreGraphics" "-framework AppKit"
        "-framework QuickLookThumbnailing" "-framework QuickLookUI")
    mosaic_apply_warnings(mosaic-quicklook)
    return() # the freedesktop thumbnailer and the KIO plugin are Linux-desktop mechanisms
endif()

# Windows has no per-file thumbnailer protocol to spawn a binary into either, and no plugin host in
# Dolphin's sense: Explorer's mechanism is an IN-PROCESS COM DLL implementing IThumbnailProvider,
# which the shell loads into a process of its own choosing (normally the isolated thumbnail host) and
# asks for one bitmap per file. So on Windows this directory builds that DLL instead of the
# freedesktop binary + KIO plugin below -- same PRVW read, same io+common-only dependency, a third
# host. packaging/windows/mosaic.wxs writes its registry keys; the DLL's own DllRegisterServer is the
# hand-testing escape hatch, not the shipping path.
if(WIN32)
    # A MODULE, not a SHARED library: nothing ever links against a COM server -- the shell reaches it
    # through LoadLibrary + DllGetClassObject by name -- so there is no import library worth
    # generating. The .def in the sources is what exports the four entry points undecorated (MinGW
    # takes a module-definition file as a plain linker input); the file's own header comment explains
    # why there is no __declspec(dllexport) to go with it.
    add_library(MosaicThumbnail MODULE shell_thumbnail_win32.cpp shell_thumbnail_win32.def)
    # PREFIX: MinGW's default module prefix is "lib", and the DLL's file name is not cosmetic here --
    # it is written verbatim into the CLSID's InprocServer32 value, so it has to be the name the MSI
    # and the docs both say. LIBRARY_OUTPUT_DIRECTORY because CMake files a MODULE under the LIBRARY
    # artifact rules on every platform, DLL or not; pointing it at bin/ puts the payload beside
    # mosaic.exe, which is where packaging expects to collect it (and, at install time, the only
    # place its own dependent DLLs will be found -- see below).
    set_target_properties(MosaicThumbnail PROPERTIES
        PREFIX "" OUTPUT_NAME "MosaicThumbnail"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
    # ⚠ NOT the POSITION_INDEPENDENT_CODE flip the KF6 branch below performs. That exists because a
    # dlopen'd ELF MODULE needs true -fPIC objects; PE has no such notion -- a DLL is relocated
    # through its .reloc section and every object is equally usable in one -- so the static Mosaic
    # modules are linked here exactly as mosaic.exe links them. Which also means this DLL and
    # mosaic.exe each carry their own copy of the mosaic_io/mosaic_common objects they use. That is
    # correct, not duplication to fix: they are separate images with separate static state, sharing
    # only the third-party DLLs beside them, and the shell may well have this one loaded while Mosaic
    # is not running at all.
    target_link_libraries(MosaicThumbnail PRIVATE mosaic::io mosaic::common)
    mosaic_apply_warnings(MosaicThumbnail)
    return() # the freedesktop thumbnailer and the KIO plugin are Linux-desktop mechanisms
endif()

add_executable(mosaic-thumbnailer main.cpp)
target_link_libraries(mosaic-thumbnailer PRIVATE mosaic::io mosaic::common)
mosaic_apply_warnings(mosaic-thumbnailer)
mosaic_apply_sanitizers(mosaic-thumbnailer)

# share/thumbnailers covers Nautilus, Thunar, Nemo, and Caja. Dolphin uses an entirely separate
# mechanism -- the KIO ThumbnailCreator plugin below -- and there is NO bridge between the two
# (verified on the dev machine). Authored, never executed by the dev workflow (S59 packages it).
install(TARGETS mosaic-thumbnailer RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES "${CMAKE_SOURCE_DIR}/data/desktop/mosaic.thumbnailer"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/thumbnailers")

# The Dolphin half: a KIO ThumbnailCreator plugin under qt6/plugins/kf6/thumbcreator. QUIET
# find_package keeps KF6 a soft dependency -- a machine without the KDE frameworks still
# configures and builds everything above; only Dolphin thumbnails are absent. KF6::KIOGui
# publicly carries the Qt6 targets the plugin needs (QImage rides in its API); CoreAddons is
# named explicitly for KPluginFactory.
find_package(KF6KIO QUIET)
find_package(KF6CoreAddons QUIET)
if(KF6KIO_FOUND AND KF6CoreAddons_FOUND)
    # A dlopen'd MODULE needs true -fPIC objects; the distro compiler's PIE default is not
    # enough (PC32 relocations against preemptible globals fail the shared link). Flipped only
    # when the plugin actually builds, so a KF6-less machine compiles byte-identically to
    # before; in the PIE executables these GOT loads relax back to direct addressing at link
    # time, so the main binary pays nothing.
    set_target_properties(mosaic_io mosaic_common mosaic_core blake3 pugixml hosekwilkie
        PROPERTIES POSITION_INDEPENDENT_CODE ON)
    set(CMAKE_AUTOMOC ON) # directory-scoped: only the plugin needs moc
    add_library(mosaicthumbnail MODULE kio_mosaic_thumbnail.cpp)
    set_target_properties(mosaicthumbnail PROPERTIES PREFIX "") # qt plugin naming: no "lib"
    target_link_libraries(mosaicthumbnail PRIVATE
        KF6::KIOGui KF6::CoreAddons mosaic::io mosaic::common)
    mosaic_apply_warnings(mosaicthumbnail)
    install(TARGETS mosaicthumbnail
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/qt6/plugins/kf6/thumbcreator")
    message(STATUS "KF6 KIO found: building the Dolphin thumbnail plugin")
else()
    message(STATUS "KF6 KIO not found: skipping the Dolphin thumbnail plugin (soft dependency)")
endif()
