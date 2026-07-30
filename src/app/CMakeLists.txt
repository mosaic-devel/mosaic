add_executable(mosaic main.cpp bench.cpp)
target_link_libraries(mosaic PRIVATE
    mosaic::common
    mosaic::core
    mosaic::render
    mosaic::io
    mosaic::ui
    mosaic::platform)
set_target_properties(mosaic PROPERTIES OUTPUT_NAME "mosaic")

# ---- Windows: GUI subsystem + the resource section (PLAN.md S57) -------------
if(WIN32)
    # WIN32_EXECUTABLE marks the image as the WINDOWS subsystem (MinGW: -mwindows) rather than
    # CONSOLE. Without it, every launch from Explorer, the Start menu or a .mosaic file association
    # flashes a black console window that then sits in the taskbar for the life of the program.
    # The cost -- a GUI-subsystem process starts with no standard handles at all, so
    # `mosaic --version` from cmd.exe would print into the void -- is paid back by
    # attachParentConsole() in main.cpp, which explains the mechanism at length.
    #
    # MinGW-w64 supplies the WinMain shim that calls our plain `int main(int, char**)` (and builds
    # argv from the command line), so nothing in main.cpp changes shape for this.
    set_target_properties(mosaic PROPERTIES WIN32_EXECUTABLE TRUE)

    # The resource compiler is a separate CMake "language", enabled HERE rather than in the top-level
    # project() call so that nothing about the Linux and macOS builds changes: they never reach this
    # branch, and enabling RC unconditionally would make every configure of theirs probe for a
    # resource compiler they have no use for. cmake/toolchains/mingw-w64.cmake has already pointed
    # CMAKE_RC_COMPILER at the right windres for the target arch.
    enable_language(RC)

    # The icon is GENERATED, not committed: make-ico.py rasterizes assets/app_icon.svg at seven
    # sizes into the build tree. Done at configure time rather than as a custom command because the
    # .rc names the file as a literal path and windres needs it on disk when it runs -- and because
    # it is one rsvg-convert call per size on an asset that changes about once a year.
    #
    # A failure is a WARNING, never an error. A missing Pillow or rsvg-convert must not stop someone
    # from building the program; it just costs them the icon.
    set(_mosaic_ico "${CMAKE_CURRENT_BINARY_DIR}/mosaic.ico")
    if(NOT EXISTS "${_mosaic_ico}")
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            execute_process(
                COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_SOURCE_DIR}/packaging/windows/make-ico.py"
                        "${CMAKE_SOURCE_DIR}/assets/app_icon.svg" "${_mosaic_ico}"
                RESULT_VARIABLE _ico_rc OUTPUT_QUIET ERROR_VARIABLE _ico_err)
            if(NOT _ico_rc EQUAL 0)
                message(WARNING "mosaic.ico could not be built (${_ico_err}) -- mosaic.exe will show "
                                "the generic Windows program icon. See packaging/windows/README.md.")
            endif()
        else()
            message(WARNING "no Python 3 interpreter found -- mosaic.exe will have no icon.")
        endif()
    endif()

    # MOSAIC_RC_VERSION is the string a user reads in Properties -> Details. It carries the git rev,
    # which the four 16-bit binary FILEVERSION fields cannot, and matches `mosaic --version` and the
    # packaged artifact names exactly.
    if(MOSAIC_GIT_REV)
        set(MOSAIC_RC_VERSION "${PROJECT_VERSION}+${MOSAIC_GIT_REV}")
    else()
        set(MOSAIC_RC_VERSION "${PROJECT_VERSION}")
    endif()
    set(MOSAIC_RC_ICON "${_mosaic_ico}")
    set(MOSAIC_RC_MANIFEST "${CMAKE_SOURCE_DIR}/packaging/windows/mosaic.manifest")
    # An ICON statement naming a file that is not there is a hard windres error, and losing the icon
    # must not also cost us the manifest -- without the manifest the process is DPI-unaware and every
    # canvas pixel gets bitmap-stretched by the compositor. So the ICON line is configured in only
    # when the .ico exists (the .in file guards it with a plain CMake @VAR@-free marker line).
    if(EXISTS "${_mosaic_ico}")
        set(MOSAIC_RC_ICON_LINE "1 ICON \"${_mosaic_ico}\"")
    else()
        set(MOSAIC_RC_ICON_LINE "// (no mosaic.ico was generated -- see the configure-time warning)")
    endif()
    configure_file("${CMAKE_SOURCE_DIR}/packaging/windows/mosaic.rc.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/mosaic.rc" @ONLY)

    # ⚠ The .rc lives in its own OBJECT library and NOT on the mosaic target, for one concrete
    # reason: CMake applies a target's compile options to every language it contains, and GNU
    # windres parses its own command line rather than forwarding it -- `x86_64-w64-mingw32-windres
    # -Wall ...` dies with "invalid option -- 'W'" before it reads a byte of input. So the moment
    # mosaic_apply_warnings() puts -Wall -Wextra -Wpedantic -Werror on the target, a .rc source on
    # that same target cannot compile. (llvm-windres happens to tolerate the flags, so this fails on
    # x86_64 only -- the more confusing half of the bug.) An OBJECT library carries no warning flags,
    # and its objects are handed to the linker individually rather than through an archive, so the
    # .rsrc section is never dropped for having no referenced symbols.
    add_library(mosaic_res OBJECT "${CMAKE_CURRENT_BINARY_DIR}/mosaic.rc")
    # A target holding nothing but resources gives CMake no language to infer a linker from, even
    # though an OBJECT library is never linked.
    set_target_properties(mosaic_res PROPERTIES LINKER_LANGUAGE CXX)
    target_link_libraries(mosaic PRIVATE mosaic_res)
endif()

mosaic_apply_warnings(mosaic)
mosaic_apply_sanitizers(mosaic)
