add_executable(mosaic_tests
    test_smoke.cpp
    test_render.cpp
    test_gpu_caps.cpp
    test_gpu_timer.cpp
    test_gpu_budget.cpp
    test_tile_residency.cpp
    test_icon.cpp
    test_theme.cpp
    test_settings.cpp
    test_fs_path.cpp
    test_i18n.cpp
    test_logging.cpp
    test_profiler.cpp
    test_thread_pool.cpp
    test_geometry.cpp
    test_geometry3d.cpp
    test_tile_grid.cpp
    test_arrange.cpp
    test_document.cpp
    test_layer_marquee.cpp
    test_layer_mask_grid.cpp
    test_commands.cpp
    test_window_title.cpp
    test_compositor.cpp
    test_composite_scaled.cpp
    test_composite_budget.cpp
    test_device_select.cpp
    test_layer_effects_gpu.cpp
    test_histogram_gpu.cpp
    test_resample.cpp
    test_document_ops.cpp
    test_image_ops_panel.cpp
    test_composite_tile_parity.cpp
    test_tile_compositor.cpp
    test_composite_readback.cpp
    test_resident_composite.cpp
    test_adjustments.cpp
    test_blur_kernels.cpp
    test_stylize.cpp
    test_stylize_kernels.cpp
    test_adjustment_panel.cpp
    test_adjustment_crash_repro.cpp
    test_panel_arbiter.cpp
    test_layer_effects.cpp
    test_layer_effects_dialog.cpp
    test_ask_or_tell_dialog.cpp
    test_recovery_flow.cpp
    test_recovery_journal.cpp
    test_loaded_history.cpp
    test_idle_state.cpp
    test_canvas_view.cpp
    test_canvas_cursor.cpp
    test_new_document.cpp
    test_recent_files.cpp
    test_layer_panel.cpp
    test_layer_panel_fill.cpp
    test_channels_panel.cpp
    test_brush_preset_panel.cpp
    test_brush_editor.cpp
    test_tool.cpp
    test_color_state.cpp
    test_color_models.cpp
    test_color_management.cpp
    test_color_sample.cpp
    test_clipboard.cpp
    test_coverage_partition.cpp
    test_merge_down.cpp
    test_cursors.cpp
    test_cursor_apply.cpp
    test_toolbar_layout.cpp
    test_menu_bar.cpp
    test_menu_geometry.cpp
    test_menu_visibility.cpp
    test_selection.cpp
    test_selection_gesture.cpp
    test_magic_wand.cpp
    test_edge_grow.cpp
    test_select_morphology.cpp
    test_mask_stroke.cpp
    test_tab_strip.cpp
    test_to_path.cpp
    test_brush_engine.cpp
    test_brush_clip.cpp
    test_layer_grow.cpp
    test_brush_tip.cpp
    test_brush_wash_golden.cpp
    test_brush_accumulator.cpp
    test_brush_colored.cpp
    test_brush_masking.cpp
    test_brush_opacity.cpp
    test_brush_smudge.cpp
    test_brush_stroke_painter.cpp
    test_brush_texture.cpp
    test_brush_airbrush.cpp
    test_brush_stroke_seed.cpp
    test_brush_spacing.cpp
    test_brush_stroke_path.cpp
    test_brush_stroke_preview.cpp
    test_settings_card_art.cpp
    test_brush_stroke_smoother.cpp
    test_brush_curve.cpp
    test_brush_sensors.cpp
    test_brush_stroke_state.cpp
    test_brush_curve_option.cpp
    test_brush_mask_generator.cpp
    test_brush_dab_mask.cpp
    test_brush_dab.cpp
    test_brush_options.cpp
    test_brush_reticle.cpp
    test_brush_tip_outline.cpp
    test_brush_bitmap_tip.cpp
    test_brush_dab_cache.cpp
    test_brush_tablet_policy.cpp
    test_tablet.cpp
    test_tablet_x11.cpp
    test_tablet_wayland.cpp
    test_tablet_wiring.cpp
    test_curve_editor.cpp
    test_io.cpp
    test_io_formats.cpp
    test_io_quantize.cpp
    test_io_exif_write.cpp
    test_exif.cpp
    test_export_options.cpp
    test_export_loss.cpp
    test_export_path.cpp
    test_export_profile.cpp
    test_export_registry.cpp
    test_export_dialog.cpp
    # The metadata + ICC slice: the provenance rule, orientation-written-as-1, and the per-container
    # round trips (incl. a profile too big for one JPEG APP2 segment).
    test_export_metadata.cpp
    # M5 -- libmosaicformats. The first six link only formats/*.hpp, which is how "tested standalone,
    # no Mosaic deps" (docs/export-system-plan.md §11) stays visible rather than merely intended;
    # test_formats_backends.cpp is the only one that knows the registry exists.
    test_formats_bmp.cpp
    test_formats_tga.cpp
    test_formats_pnm.cpp
    test_formats_qoi.cpp
    test_formats_ico.cpp
    test_formats_hdr.cpp
    test_formats_backends.cpp
    # S35-b: the warp kernel + gesture math, and S51-b: the harvested keymap.
    test_warp.cpp
    test_keymap.cpp
    test_file_dialog.cpp
    test_icon_pack.cpp
    test_png_text.cpp
    test_preset_xml.cpp
    test_kpp_mapper.cpp
    test_myb.cpp
    test_preset_json.cpp
    test_tip_io.cpp
    test_zip.cpp
    test_bundle.cpp
    test_md5.cpp
    test_brush_library.cpp
    test_brush_library_census.cpp
    test_brush_preset_brush.cpp
    test_brush_preset_store.cpp
    test_mosaic_format.cpp
    test_mosaic_codec.cpp
    test_mosaic_file.cpp
    test_mosaic_rs.cpp
    test_mosaic_journal.cpp
    test_mosaic_journal_session.cpp
    test_mosaic_lock.cpp
    test_mosaic_save.cpp
    test_mosaic_win_io.cpp
    test_mosaic_compaction.cpp
    test_mosaic_cas.cpp
    test_save_policy.cpp
    test_mosaic_docio.cpp
    test_mosaic_preview.cpp
    test_texture_noise.cpp
    test_texture_layer.cpp
    test_texture_sky.cpp
    test_texture_lunar.cpp
    test_texture_paper.cpp
    test_texture_grass.cpp
    test_texture_materials.cpp
    test_texture_window.cpp
    test_sky_almanac.cpp
    test_sky_estimate.cpp
    test_sky_estimate_dialog.cpp
    test_texture_gen_dialog.cpp
    test_city_catalog.cpp
    test_date_picker.cpp
    test_map_picker.cpp
    test_mosaic_commit_append.cpp
    test_loaded_delta.cpp
    test_status_bar.cpp
    test_transform_gesture.cpp
    test_crop_gesture.cpp
    test_inpaint.cpp
    test_retarget.cpp
    test_fill.cpp
    test_vector.cpp
    test_vector_boolean.cpp
    test_red_eye.cpp
    test_clone_stamp.cpp
    test_pattern.cpp
    test_shape_gesture.cpp
    test_shape_library.cpp
    test_pen_gesture.cpp
    test_pen_canvas.cpp
    test_gradient_gesture.cpp
    test_gradient_paint.cpp
    test_gradient_dither.cpp
    test_gradient_automask.cpp
    test_scrub_slider.cpp
    test_line_style_preview.cpp
    test_text_model.cpp
    test_text_edit.cpp
    test_extrude_mesh.cpp
    test_extrude_render.cpp
    test_extrude_gpu.cpp
    test_texture_gpu.cpp
    test_blur_gpu.cpp
    test_type3d_panel.cpp
    test_select_morph_panel.cpp
    test_text_fonts.cpp
    test_text_render.cpp
    test_text_tokenize.cpp
    test_text_language.cpp
    test_text_hyphenator.cpp
    test_text_hyphenation.cpp
    test_text_spell_checker.cpp
    test_text_spell_scan.cpp
    test_text_spell_worker.cpp)
# zlib: test_zip.cpp builds its hostile archives with the test's own deflate.
# nlohmann: test_preset_json.cpp builds its hostile .mbp payloads by surgical JSON edits.
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZLIB REQUIRED IMPORTED_TARGET zlib)

target_link_libraries(mosaic_tests PRIVATE
    doctest::doctest
    mosaic::common
    mosaic::core
    mosaic::render
    mosaic::io
    mosaic::ui
    mosaic::platform
    PkgConfig::ZLIB
    nlohmann::json)

# The vendored profile dir, for the S12-c file-loading tests (lcms2 itself stays PRIVATE to core).
target_compile_definitions(mosaic_tests PRIVATE
    MOSAIC_ICC_DIR="${CMAKE_SOURCE_DIR}/third_party/icc-profiles")
# Absolute path to the golden-image reference dir, so the test finds it regardless of cwd.
target_compile_definitions(mosaic_tests PRIVATE
    MOSAIC_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")
# Absolute path to the io decode fixtures (S18-b), likewise cwd-independent.
target_compile_definitions(mosaic_tests PRIVATE
    MOSAIC_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
# The shipped runtime data (the default brush set): the census test replays the real bundle
# from the source tree, deliberately NOT via installedDataDir() -- tests stay hermetic.
target_compile_definitions(mosaic_tests PRIVATE
    MOSAIC_SHIPPED_DATA_DIR="${CMAKE_SOURCE_DIR}/data")
# Debug builds compile the AskOrTellDialog demo (MOSAIC_DEBUG, mirroring src/ui) so the opt-in
# MOSAIC_DEMO_DIAG driver in test_ask_or_tell_dialog.cpp can exercise the real Help-menu flow.
target_compile_definitions(mosaic_tests PRIVATE $<$<CONFIG:Debug>:MOSAIC_DEBUG>)
mosaic_apply_warnings(mosaic_tests)
mosaic_apply_sanitizers(mosaic_tests)

add_test(NAME mosaic_tests COMMAND mosaic_tests)
# ASan builds: suppress the two known-benign leak classes (FLTK putenv / fontconfig globals) so
# LeakSanitizer stays a meaningful signal for OUR leaks. Justifications live in lsan.supp.
set_tests_properties(mosaic_tests PROPERTIES
    ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp")

# corrupt_corpus self-verifies every fixture against the real reader as it writes it, so running
# it under ctest turns that guard into an automatic one: if a reader change drifts a fixture's
# verdict from what the scenario means to teach, this fails instead of the drift being caught only
# when someone next regenerates the manual corpus by hand. Guarded on the target so it self-skips
# when the tool is disabled (-DMOSAIC_BUILD_CORRUPT_CORPUS=OFF). Links no FLTK -> no LSan suppression.
if(TARGET corrupt_corpus)
    add_test(NAME corrupt_corpus_selfcheck
        COMMAND corrupt_corpus "${CMAKE_CURRENT_BINARY_DIR}/corrupt-corpus-selfcheck")
endif()
