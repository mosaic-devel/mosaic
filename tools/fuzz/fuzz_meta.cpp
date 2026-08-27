// libFuzzer entry point for the two remaining hand-fed parsers on the shared-file path: the EXIF
// reader (a hand-rolled TIFF/IFD walk, run on every JPEG and PNG import) and the SVG rasterizer
// (a thin wrapper over vendored nanosvg, which an SVG BRUSH TIP inside a shared preset bundle
// reaches -- io/brush/library.cpp rasterizes one with attacker-controlled bytes).
//
// RESULTS:
//
//   parseExif   clean. A TIFF IFD walk is offsets and counts all the way down and it held up.
//   nanosvg     twelve UBSan reports, all float -> int conversions out of range:
//                 nanosvgrast.h:878,880  a huge or infinite dx/dy stepper into an int
//                 nanosvg.h:2311         (int) of a NaN arc-division count
//               No ASan finding -- nothing indexed out of bounds -- so this is undefined
//               behaviour that x86-64 happens to render harmless (the conversions produce
//               INT_MIN and the downstream loops decline to run). It is still UB, it is still on
//               a path that eats shared files, and the compiler is entitled to assume it cannot
//               happen. See the commit that added this file for the options.
//
// BUILD AND RUN VIA tools/fuzz/run-fuzzers.sh -- do not hand-roll the command line.
//
//   ./tools/fuzz/run-fuzzers.sh replay        replay the checked-in corpus (what CI gates on)
//   ./tools/fuzz/run-fuzzers.sh explore 300   mutate for N seconds per harness
//
// ⚠ The flags are not incidental. They name every vendored include directory and the defines the
// project builds its vendored libraries with, and the script CHECKS that the vendored headers are
// the ones that answered. A hand-written command line that omits -I third_party/pugixml still
// compiles on a machine with a system pugixml installed -- against the wrong library, silently,
// with a clean fuzz run to show for it. That happened; CI caught it; the script exists so it
// cannot happen twice.

#include "common/image_svg.hpp"
#include "io/exif.hpp"
#include "io/io.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

// The real sniffImageFormat lives in io.cpp, which drags in every image decoder in the project --
// far too much to link into a fuzzer for two container walks. This stands in for it, and it
// deliberately DOES sniff rather than returning Unknown: extractExif dispatches on the answer, so a
// stub that always says Unknown makes exifFromJpeg and exifFromPng unreachable and the harness
// quietly tests nothing. Only the two formats EXIF can live in are recognised; everything else is
// Unknown, which is what the real function would say for the purposes of this path.
namespace mosaic::io {
ImageFormat sniffImageFormat(const std::uint8_t* d, std::size_t n) noexcept {
    static const std::uint8_t kPng[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (n >= 8) {
        bool png = true;
        for (int i = 0; i < 8; ++i)
            png = png && d[i] == kPng[i];
        if (png)
            return ImageFormat::Png;
    }
    if (n >= 2 && d[0] == 0xFF && d[1] == 0xD8)
        return ImageFormat::Jpeg;
    return ImageFormat::Unknown;
}
} // namespace mosaic::io

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 2)
        return 0;
    const std::uint8_t which = data[0] % 4;
    const std::uint8_t* p = data + 1;
    const std::size_t n = size - 1;
    switch (which) {
    case 0:
        (void)mosaic::io::parseExif(p, n);
        break;
    case 1: { // the JPEG APP1 / PNG eXIf CONTAINER walk that locates the payload
        std::vector<std::uint8_t> f(p, p + n);
        (void)mosaic::io::extractExif(f);
        break;
    }
    case 2:
        (void)mosaic::common::svgIntrinsicSize(p, n);
        break;
    case 3:
        (void)mosaic::common::rasterizeSvg(p, n, 64, 64, nullptr);
        break;
    }
    return 0;
}
