# Developer tools, built only on request. inpaint_bench drives the inpainting engine on real
# images from disk (paths passed on the command line) so no large test images are vendored.
option(MOSAIC_BUILD_INPAINT_BENCH "Build the inpaint_bench perf driver" OFF)

if(MOSAIC_BUILD_INPAINT_BENCH)
    add_executable(inpaint_bench inpaint_bench.cpp)
    target_link_libraries(inpaint_bench PRIVATE mosaic::core mosaic::common mosaic::io)
    mosaic_apply_warnings(inpaint_bench)
    mosaic_apply_sanitizers(inpaint_bench)
endif()

# tablet_spike is the S19 Arc C Wayland risk-retirement spike (docs/tablet.md §4): a
# standalone zwp_tablet_v2 client plus a uinput virtual stylus that proves, against the live
# compositor, that a self-bound wl_seat delivers tablet events and that input regions route
# them to the parent surface. Dev-only, needs a Wayland session + /dev/uinput access to run.
option(MOSAIC_BUILD_TABLET_SPIKE "Build the Wayland tablet input spike" OFF)

if(MOSAIC_BUILD_TABLET_SPIKE)
    enable_language(C) # the project is C++-only; wayland-scanner emits C
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SPIKE_WAYLAND REQUIRED wayland-client)
    pkg_get_variable(SPIKE_PROTO_DIR wayland-protocols pkgdatadir)
    pkg_get_variable(SPIKE_SCANNER wayland-scanner wayland_scanner)
    set(_spike_gen "${CMAKE_CURRENT_BINARY_DIR}/wayland-gen")
    file(MAKE_DIRECTORY "${_spike_gen}")
    set(_spike_gen_src "")
    foreach(_proto IN ITEMS
        "${SPIKE_PROTO_DIR}/stable/xdg-shell/xdg-shell.xml"
        "${SPIKE_PROTO_DIR}/unstable/tablet/tablet-unstable-v2.xml")
        get_filename_component(_name "${_proto}" NAME_WE)
        add_custom_command(
            OUTPUT "${_spike_gen}/${_name}-client.h" "${_spike_gen}/${_name}.c"
            COMMAND "${SPIKE_SCANNER}" client-header "${_proto}" "${_spike_gen}/${_name}-client.h"
            COMMAND "${SPIKE_SCANNER}" private-code "${_proto}" "${_spike_gen}/${_name}.c"
            DEPENDS "${_proto}" VERBATIM)
        list(APPEND _spike_gen_src "${_spike_gen}/${_name}.c" "${_spike_gen}/${_name}-client.h")
    endforeach()
    add_executable(tablet_spike tablet_spike.cpp ${_spike_gen_src})
    target_include_directories(tablet_spike PRIVATE "${_spike_gen}" ${SPIKE_WAYLAND_INCLUDE_DIRS})
    target_link_directories(tablet_spike PRIVATE ${SPIKE_WAYLAND_LIBRARY_DIRS})
    target_link_libraries(tablet_spike PRIVATE ${SPIKE_WAYLAND_LIBRARIES})
    target_compile_features(tablet_spike PRIVATE cxx_std_20)
    mosaic_apply_warnings(tablet_spike)

    # Stage 2: the same questions with FLTK owning the connection/event loop and the REAL
    # WaylandSubsurface as the child, plus the pointer-emulation question (Q4). Links
    # mosaic::platform, which since the zwp_tablet_v2 backend landed already carries the generated
    # protocol private-code -- so this target takes only the client HEADER (for the inline proxy
    # wrappers), never its own copy of the .c, or the tablet interface symbols would be defined
    # twice.
    add_executable(tablet_spike_fltk tablet_spike_fltk.cpp
        "${_spike_gen}/tablet-unstable-v2-client.h")
    target_include_directories(tablet_spike_fltk PRIVATE "${_spike_gen}"
        ${SPIKE_WAYLAND_INCLUDE_DIRS})
    target_link_directories(tablet_spike_fltk PRIVATE ${SPIKE_WAYLAND_LIBRARY_DIRS})
    target_link_libraries(tablet_spike_fltk PRIVATE mosaic::platform
        ${SPIKE_WAYLAND_LIBRARIES})
    target_compile_features(tablet_spike_fltk PRIVATE cxx_std_20)
    mosaic_apply_warnings(tablet_spike_fltk)

    # scanner-generated C is not held to the project's warning bar
    set_source_files_properties(${_spike_gen_src} PROPERTIES COMPILE_OPTIONS "-w")

    # Stage 3 (Arc C step 3): the X11/XI2 live probe -- proves TabletX11's server-side half
    # (init/hotplug/select/cookies) against XWayland and fact-checks the device-naming and
    # valuator-label claims the backend bakes in. Same uinput pen, same session requirement.
    pkg_check_modules(SPIKE_X11 REQUIRED x11 xi)
    add_executable(tablet_spike_x11 tablet_spike_x11.cpp)
    target_include_directories(tablet_spike_x11 PRIVATE ${SPIKE_X11_INCLUDE_DIRS})
    target_link_directories(tablet_spike_x11 PRIVATE ${SPIKE_X11_LIBRARY_DIRS})
    target_link_libraries(tablet_spike_x11 PRIVATE mosaic::platform ${SPIKE_X11_LIBRARIES})
    target_compile_features(tablet_spike_x11 PRIVATE cxx_std_20)
    mosaic_apply_warnings(tablet_spike_x11)

    # The JAGGED-STROKE diagnostic: the same uinput pen, but driving the REAL ui::TabletInput on a
    # real FLTK window, to measure the three named suspects behind the staircased X11 stroke (the
    # synthesized fallback, motion compression, sub-pixel loss). Links mosaic::ui because the whole
    # point is to measure the SHIPPED wiring and not a re-implementation of it.
    add_executable(tablet_diag_x11 tablet_diag_x11.cpp)
    target_link_libraries(tablet_diag_x11 PRIVATE mosaic::ui)
    target_compile_features(tablet_diag_x11 PRIVATE cxx_std_20)
    mosaic_apply_warnings(tablet_diag_x11)

    # Which position source is faithful: the server's event_x/event_y, or the device's own valuators?
    add_executable(xi2_valuator_probe xi2_valuator_probe.cpp)
    target_include_directories(xi2_valuator_probe PRIVATE ${SPIKE_X11_INCLUDE_DIRS})
    target_link_directories(xi2_valuator_probe PRIVATE ${SPIKE_X11_LIBRARY_DIRS})
    target_link_libraries(xi2_valuator_probe PRIVATE ${SPIKE_X11_LIBRARIES})
    target_compile_features(xi2_valuator_probe PRIVATE cxx_std_20)
    mosaic_apply_warnings(xi2_valuator_probe)

    # pen_driver drives the REAL mosaic binary with a uinput STYLUS (and nothing else -- see its
    # header). Pair with $MOSAIC_TABLET_PROBE to capture the StrokeInput stream the engine actually
    # received, on either backend.
    add_executable(pen_driver pen_driver.cpp)
    target_include_directories(pen_driver PRIVATE ${SPIKE_X11_INCLUDE_DIRS})
    target_link_directories(pen_driver PRIVATE ${SPIKE_X11_LIBRARY_DIRS})
    target_link_libraries(pen_driver PRIVATE ${SPIKE_X11_LIBRARIES})
    target_compile_features(pen_driver PRIVATE cxx_std_20)
    mosaic_apply_warnings(pen_driver)
endif()

# corrupt_corpus emits the manual-testing corpus of damaged .mosaic files (one per recovery
# scenario, self-verified against the real reader at generation time -- see the file header).
# ON by default: it is the user-facing test fixture for the S48 recovery surface.
option(MOSAIC_BUILD_CORRUPT_CORPUS "Build the .mosaic corrupt-corpus generator" ON)

if(MOSAIC_BUILD_CORRUPT_CORPUS)
    add_executable(corrupt_corpus corrupt_corpus.cpp)
    target_link_libraries(corrupt_corpus PRIVATE mosaic::core mosaic::common mosaic::io)
    mosaic_apply_warnings(corrupt_corpus)
    mosaic_apply_sanitizers(corrupt_corpus)
endif()

# torture_doc emits the S60 measurement fixture: ONE deliberately punishing .mosaic document (a
# 40 MP photograph under ~70 layers of 3D type, layer effects, live vector booleans, spatial
# adjustments and real appended history). See the file header. ON by default and cheap to build --
# it is the fixture the performance work is measured against, and a fixture nobody can regenerate
# is a fixture that rots.
option(MOSAIC_BUILD_TORTURE_DOC "Build the .mosaic performance-fixture generator" ON)

if(MOSAIC_BUILD_TORTURE_DOC)
    add_executable(torture_doc torture_doc.cpp)
    target_link_libraries(torture_doc PRIVATE mosaic::core mosaic::common mosaic::io)
    mosaic_apply_warnings(torture_doc)
    mosaic_apply_sanitizers(torture_doc)
endif()

# mosaic_stats answers "where did the bytes go" for one .mosaic file: per chunk type, per layer
# kind, per layer, checkpoint vs history. The companion to torture_doc -- a fixture whose size
# cannot be explained is a fixture that teaches the wrong lesson.
option(MOSAIC_BUILD_MOSAIC_STATS "Build the .mosaic byte-census tool" ON)

if(MOSAIC_BUILD_MOSAIC_STATS)
    add_executable(mosaic_stats mosaic_stats.cpp)
    target_link_libraries(mosaic_stats PRIVATE mosaic::core mosaic::common mosaic::io)
    mosaic_apply_warnings(mosaic_stats)
    mosaic_apply_sanitizers(mosaic_stats)
endif()
