# ui -- the FLTK shell: main window, menu-bar skeleton, and the Vulkan canvas. From S3 it
# needs FLTK, and it drives the surface-backed renderer (render) via the native handle
# obtained from platform.
find_package(FLTK REQUIRED CONFIG)

# tablet_input.cpp is the FLTK half of the stylus wiring, and it carries the Linux (XInput2 +
# zwp_tablet_v2) AND Windows (WinTab + Windows Ink, S57) branches in one file -- both hang off
# Fl::add_system_handler, so they share every line of the ingest/policy/drain seam between them.
#
# macOS (S58) cannot: it reads the pen off Cocoa's NSEvent stream through a local event monitor,
# which is Objective-C++, so it is compiled from tablet_input_macos.mm INSTEAD -- the same swap
# spell_checker_macos.mm uses in core/. Same ui::TabletInput interface all three ways; the canvas is
# unchanged.
if(APPLE)
    set(MOSAIC_UI_TABLET_SOURCE tablet_input_macos.mm)
else()
    set(MOSAIC_UI_TABLET_SOURCE tablet_input.cpp)
endif()

# The system menu bar (S58-b): ui::MenuBar derives from Fl_Sys_Menu_Bar, so on macOS the menus land in
# the screen-top bar. This file adds the two halves FLTK's NSMenu mirror cannot carry -- the item
# badges (as template NSImages) and the application menu. Apple-only: it has no cross-platform
# counterpart, because every other platform draws the bar itself.
if(APPLE)
    set(MOSAIC_UI_SYS_MENU_SOURCE sys_menu_macos.mm)
else()
    set(MOSAIC_UI_SYS_MENU_SOURCE "")
endif()

mosaic_add_module(ui
    SOURCES
        ui.cpp
        theme.cpp
        widgets.cpp
        xdg_thumbnails.cpp
        icons.cpp
        paint_chip.cpp
        ask_or_tell_dialog.cpp
        loaded_history.cpp
        about_dialog.cpp
        new_document_dialog.cpp
        settings_dialog.cpp
        line_style_preview.cpp
        fill_dialog.cpp
        export_dialog.cpp
        export_preview.cpp
        select_modify_dialog.cpp
        select_morph_panel.cpp
        image_ops_panel.cpp
        adjustment_panel.cpp
        panel_arbiter.cpp
        tone_wheel.cpp
        layer_effects_dialog.cpp
        preview_pane.cpp
        keymap.cpp
        menu_bar.cpp
        ${MOSAIC_UI_SYS_MENU_SOURCE}
        menu_visibility.cpp
        motivational_lines.cpp
        motivation_ticker.cpp
        layer_panel.cpp
        history_panel.cpp
        channels_panel.cpp
        right_dock.cpp
        brush_preset_panel.cpp
        brush_editor.cpp
        tool.cpp
        icon_pack.cpp
        toolbar.cpp
        tool_flyout.cpp
        tool_options.cpp
        scrub_slider.cpp
        shape_gesture.cpp
        pen_gesture.cpp
        red_eye_gesture.cpp
        clone_stamp_gesture.cpp
        warp_gesture.cpp
        gradient_gesture.cpp
        shape_designer.cpp
        type_panel.cpp
        type3d_panel.cpp
        gizmo_canvas.cpp
        date_picker.cpp
        world_map_data.cpp
        map_picker.cpp
        texture_generator_dialog.cpp
        color_state.cpp
        color_models.cpp
        popover.cpp
        color_picker.cpp
        bubble_flyout.cpp
        color_flyout.cpp
        gradient_flyout.cpp
        pattern_flyout.cpp
        color_swatch.cpp
        brush_presets.cpp
        canvas_view.cpp
        ruler.cpp
        idle_invitation.cpp
        cursors.cpp
        cursor_apply.cpp
        selection_gesture.cpp
        transform_gesture.cpp
        crop_gesture.cpp
        curve_editor.cpp
        ${MOSAIC_UI_TABLET_SOURCE}
        vulkan_canvas.cpp
        status_bar.cpp
        tab_strip.cpp
        window_title.cpp
        timing_graph_window.cpp
        app_window.cpp
    DEPS
        mosaic::common
        mosaic::core
        mosaic::io
        mosaic::platform
        mosaic::render
        fltk::fltk)
# Deliberately NOT fltk::images: its bundled nanosvg collides at link time with our vendored one
# (multiple definition of nsvgRasterize). External clipboard-image decode registers our OWN
# Fl_Shared_Image handler backed by io::loadImage instead (runApp) -- same hook, our decoders.

if(APPLE)
    # tablet_input_macos.mm reads NSEvent's stylus fields through an NSEvent local monitor. FLTK's
    # config links the Cocoa umbrella already, but name AppKit for a clean static link (as platform/
    # and core/ do for their own .mm files).
    target_link_libraries(mosaic_ui PRIVATE "-framework AppKit")
endif()

# Bake the app icon's SVG source into the binary as mosaic::assets::app_icon_svg[]; it is
# rasterized at startup (common::rasterizeSvg) to set the window icon. The SVG is the single
# source -- swap assets/app_icon.svg to rebrand.
mosaic_embed_asset(mosaic_ui
    NAMESPACE mosaic::assets
    SYMBOL app_icon_svg
    FILE "${CMAKE_SOURCE_DIR}/assets/app_icon.svg")

# Debug-build-only UI (the Help-menu AskOrTellDialog exerciser) compiles behind MOSAIC_DEBUG,
# mirroring the $<CONFIG:Debug> generator-expression pattern in MosaicHelpers.cmake. Release
# builds ship neither the menu item nor the demo code.
target_compile_definitions(mosaic_ui PRIVATE $<$<CONFIG:Debug>:MOSAIC_DEBUG>)

# The AskOrTellDialog's stage icons (docs/askortell-dialog.md): info / question / warning /
# corrupt-file / restore, drawn in the app-icon gradient language; rasterized at 48 px into the
# dialog.
mosaic_embed_asset(mosaic_ui
    NAMESPACE mosaic::assets
    SYMBOL icon_info_svg
    FILE "${CMAKE_SOURCE_DIR}/assets/icon_info.svg")
mosaic_embed_asset(mosaic_ui
    NAMESPACE mosaic::assets
    SYMBOL icon_question_svg
    FILE "${CMAKE_SOURCE_DIR}/assets/icon_question.svg")
mosaic_embed_asset(mosaic_ui
    NAMESPACE mosaic::assets
    SYMBOL icon_warning_svg
    FILE "${CMAKE_SOURCE_DIR}/assets/icon_warning.svg")
mosaic_embed_asset(mosaic_ui
    NAMESPACE mosaic::assets
    SYMBOL icon_corrupt_file_svg
    FILE "${CMAKE_SOURCE_DIR}/assets/icon_corrupt_file.svg")
mosaic_embed_asset(mosaic_ui
    NAMESPACE mosaic::assets
    SYMBOL icon_restore_svg
    FILE "${CMAKE_SOURCE_DIR}/assets/icon_restore.svg")

# The layer dock's panel-chrome icons (ui/icons.hpp, S16-g). Unlike the dialog icons above and the
# toolbar's tool art these are ONE-INK: white on transparent, tinted to a palette colour at draw
# time. Keep them monochrome -- ui::drawIcon overwrites their RGB.
mosaic_embed_asset(mosaic_ui
    NAMESPACE mosaic::assets
    SYMBOL icon_plus_svg
    FILE "${CMAKE_SOURCE_DIR}/assets/icon_plus.svg")
mosaic_embed_asset(mosaic_ui
    NAMESPACE mosaic::assets
    SYMBOL icon_trash_svg
    FILE "${CMAKE_SOURCE_DIR}/assets/icon_trash.svg")
mosaic_embed_asset(mosaic_ui
    NAMESPACE mosaic::assets
    SYMBOL icon_close_svg
    FILE "${CMAKE_SOURCE_DIR}/assets/icon_close.svg")
mosaic_embed_asset(mosaic_ui
    NAMESPACE mosaic::assets
    SYMBOL icon_group_layers_svg
    FILE "${CMAKE_SOURCE_DIR}/assets/icon_group_layers.svg")
mosaic_embed_asset(mosaic_ui
    NAMESPACE mosaic::assets
    SYMBOL icon_eye_open_svg
    FILE "${CMAKE_SOURCE_DIR}/assets/icon_eye_open.svg")
mosaic_embed_asset(mosaic_ui
    NAMESPACE mosaic::assets
    SYMBOL icon_eye_closed_svg
    FILE "${CMAKE_SOURCE_DIR}/assets/icon_eye_closed.svg")
mosaic_embed_asset(mosaic_ui
    NAMESPACE mosaic::assets
    SYMBOL icon_lock_open_svg
    FILE "${CMAKE_SOURCE_DIR}/assets/icon_lock_open.svg")
mosaic_embed_asset(mosaic_ui
    NAMESPACE mosaic::assets
    SYMBOL icon_lock_closed_svg
    FILE "${CMAKE_SOURCE_DIR}/assets/icon_lock_closed.svg")

# The pan-gesture hand cursors (ui::panCursor), rasterized at runtime. Vendored from the GPLv3
# apple_cursor project (third_party/apple_cursor) -- hand1 = open "grab", move = closed "grabbing".
mosaic_embed_asset(mosaic_ui
    NAMESPACE mosaic::assets
    SYMBOL cursor_grab_svg
    FILE "${CMAKE_SOURCE_DIR}/third_party/apple_cursor/svg/hand1.svg")
mosaic_embed_asset(mosaic_ui
    NAMESPACE mosaic::assets
    SYMBOL cursor_grabbing_svg
    FILE "${CMAKE_SOURCE_DIR}/third_party/apple_cursor/svg/move.svg")
# The Type tool's fit-to-path hover hand (ui::fitTextCursor): apple_cursor hand2 (the flat open
# hand), shown when a click would flow text onto the vector path under the pointer (the user's own
# pick: "apple_cursor/svg/hand2.svg").
mosaic_embed_asset(mosaic_ui
    NAMESPACE mosaic::assets
    SYMBOL cursor_fit_text_svg
    FILE "${CMAKE_SOURCE_DIR}/third_party/apple_cursor/svg/hand2.svg")

# The Move tool's rotate cursor (ui::rotateCursor): the vendored apple_cursor left_side double-arrow,
# rasterized + recoloured + spun at runtime. Its blue (#0000FF) outline / green (#00FF00) inner are
# recoloured to a per-theme two-tone (black/white), and nanosvg drops its drop-shadow <filter>.
mosaic_embed_asset(mosaic_ui
    NAMESPACE mosaic::assets
    SYMBOL rotate_cursor_svg
    FILE "${CMAKE_SOURCE_DIR}/third_party/apple_cursor/svg/left_side.svg")

# The four-way MOVE arrow (ui::moveCursor): the vendored apple_cursor all-scroll, recoloured to the
# theme two-tone at runtime exactly like the rotate cursor. Substituted for the stock FL_CURSOR_MOVE
# on WAYLAND only -- breeze_cursors symlinks the Xcursor name `move` to `dnd-move`, a closed
# grabbing hand, so hovering a Move-tool selection announced a drag that was not happening (X11
# resolves the same request to XC_fleur, which is what this art is).
mosaic_embed_asset(mosaic_ui
    NAMESPACE mosaic::assets
    SYMBOL move_cursor_svg
    FILE "${CMAKE_SOURCE_DIR}/third_party/apple_cursor/svg/all-scroll.svg")

# The Type tool's rotating I-beam cursor (ui::textCursor): the vendored apple_cursor xterm glyph,
# recoloured (blue #0000FF outline / green #00FF00 inner) and spun to the local baseline at runtime,
# exactly as the rotate cursor is. Its 256-square clip-path is stripped so a turned I-beam's ends are
# not cropped (docs/type-tool.md §6.1).
mosaic_embed_asset(mosaic_ui
    NAMESPACE mosaic::assets
    SYMBOL text_cursor_svg
    FILE "${CMAKE_SOURCE_DIR}/third_party/apple_cursor/svg/xterm.svg")

# The default tool icon pack "Smalti" (S52, assets/default_tools/): 39 tool SVGs + the pack
# manifest, embedded like the rest of assets/ so the per-icon fallback pack can never be missing.
# User packs are runtime folders (dataDir()/icon_packs/<pack>/); only the default is baked in.
# One symbol per key: dt_<key>_svg (+ dt_pack_json), consumed by icon_pack.cpp's embedded table.
set(_mosaic_default_tool_icons
    move marquee_rect marquee_ellipse lasso lasso_polygon lasso_magnetic crop brush eraser
    inpaint_brush bucket_fill gradient eyedropper shape_rect shape_ellipse shape_polygon
    shape_star shape_line shape_callout shape_arrow shape_ring shape_cross shape_heart
    shape_banner text zoom magic_wand selection_brush edge_brush blur dodge burn smudge
    pen_path heal red_eye clone_stamp hand warp)
foreach(_key IN LISTS _mosaic_default_tool_icons)
    mosaic_embed_asset(mosaic_ui
        NAMESPACE mosaic::assets
        SYMBOL dt_${_key}_svg
        FILE "${CMAKE_SOURCE_DIR}/assets/default_tools/${_key}.svg")
endforeach()
mosaic_embed_asset(mosaic_ui
    NAMESPACE mosaic::assets
    SYMBOL dt_pack_json
    FILE "${CMAKE_SOURCE_DIR}/assets/default_tools/mosaic_icon_pack.json")

# icon_pack.cpp parses pack manifests (vendored single-header nlohmann, the settings.cpp precedent).
target_link_libraries(mosaic_ui PRIVATE nlohmann::json)
