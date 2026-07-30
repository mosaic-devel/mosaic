#pragma once

#include "io/format_backend.hpp"
#include "io/io.hpp"

#include <memory>

// io/backends -- one factory per format backend. The registry (io/format_registry.cpp) calls
// these explicitly; nothing self-registers (see the note there for why that matters in a static
// library). A new format adds one factory here, one file beside this one, and one line in
// FormatRegistry::instance().
namespace mosaic::io {

// The RenderInput -> EmbeddedMetadata translation every metadata-capable backend performs: the
// typed common::ExifData becomes an EXIF payload (io/exif_write.hpp), the profile path becomes
// validated ICC bytes, and the density rides along. Shared rather than copied per backend so the
// strip-metadata toggle is honoured in exactly one place. Defined in format_registry.cpp.
[[nodiscard]] EmbeddedMetadata buildMetadata(const RenderInput& input);

// PNG, via the libpng encoder in io/png.cpp (io::encodePng). Always available.
[[nodiscard]] std::unique_ptr<FormatBackend> makePngBackend();

// JPEG, via the libjpeg-turbo encoder in io/jpeg.cpp (io::encodeJpeg). Always available.
[[nodiscard]] std::unique_ptr<FormatBackend> makeJpegBackend();

// JPEG XL, via the libjxl encoder in io/jxl.cpp (io::encodeJxl). Registered unconditionally, but
// its available() answers io::jxlSupported() -- libjxl is an optional build dependency.
[[nodiscard]] std::unique_ptr<FormatBackend> makeJxlBackend();

// The M4 quartet. All four are registered unconditionally and gate themselves through
// available(): libwebp, libavif, libtiff and giflib are each an OPTIONAL build dependency, and a
// machine without one still builds Mosaic -- the format is simply not offered.
[[nodiscard]] std::unique_ptr<FormatBackend> makeWebpBackend();
[[nodiscard]] std::unique_ptr<FormatBackend> makeAvifBackend();
[[nodiscard]] std::unique_ptr<FormatBackend> makeTiffBackend();
[[nodiscard]] std::unique_ptr<FormatBackend> makeGifBackend();

// The M5 curated-pro tier, all six of them fronting libmosaicformats (src/formats, namespace
// mosaicfmt) from one adapter translation unit, backends/mosaicformats_backend.cpp. None of them
// is build-optional: the codecs are ours, so available() is unconditionally true and a distro
// missing every image library still exports all six. (Six backends, seven ids' worth of formats:
// FormatId::Pnm covers PBM, PGM, PPM and PAM, which are one family behind one variant option.)
[[nodiscard]] std::unique_ptr<FormatBackend> makeBmpBackend();
[[nodiscard]] std::unique_ptr<FormatBackend> makeTgaBackend();
[[nodiscard]] std::unique_ptr<FormatBackend> makePnmBackend();
[[nodiscard]] std::unique_ptr<FormatBackend> makeQoiBackend();
[[nodiscard]] std::unique_ptr<FormatBackend> makeIcoBackend();
[[nodiscard]] std::unique_ptr<FormatBackend> makeHdrBackend();

} // namespace mosaic::io
