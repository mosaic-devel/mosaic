# formats -- libmosaicformats (docs/export-system-plan.md §2.2, docs/formats-curated.md): the
# hand-rolled codecs behind the curated-pro export tier -- BMP, TGA, PNM/PAM, QOI, ICO and
# Radiance HDR, encode and decode.
#
# ⚠ IT LINKS NOTHING, AND THAT IS THE POINT. No mosaic::common, no mosaic::io, no system codec --
# only the standard library. §2.2 asks for a library that is independently testable and, in
# principle, liftable out; §11 asks for it to be "tested standalone, no Mosaic deps". Both are
# properties of this dependency list, so `DEPS` staying empty is the specification, not an
# oversight. The adapter that converts common::Image <-> mosaicfmt::ImageView lives on the other
# side of the fence, in src/io/backends/mosaicformats_backend.cpp.
#
# The target is mosaic::formats and the namespace is mosaicfmt (§2.2 names the two separately).
# Nothing here has anything to do with src/io/mosaic/, which is the native .mosaic document
# container (docs/mosaic-native-format.md) -- unrelated code, confusingly adjacent name.
mosaic_add_module(formats
    SOURCES
        formats.cpp
        bmp.cpp
        tga.cpp
        pnm.cpp
        qoi.cpp
        ico.cpp
        hdr.cpp)

mosaic_optimize_pixel_loops(mosaic_formats)  # every codec here is a per-pixel loop
