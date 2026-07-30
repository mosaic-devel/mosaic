# Module build order (dependencies flow left-to-right; nothing depends on `ui`).
add_subdirectory(common)
add_subdirectory(core)
add_subdirectory(render)
add_subdirectory(formats)  # libmosaicformats: the dependency-free curated codecs (M5). Links NOTHING
add_subdirectory(io)       # pure, FLTK-free format readers/writers (the S41 registry grows here)
add_subdirectory(platform)
add_subdirectory(ui)
add_subdirectory(app)
add_subdirectory(thumbnailer) # mosaic-thumbnailer + KIO plugin: io+common only, no display (S48-b)
