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
// BUILD (clang only; libFuzzer is not a GCC feature and this project builds with GCC):
//
//   clang++ -std=c++23 -g -O1 -I src -I third_party -I third_party/nanosvg \
//       -I build/linux-release/generated -fsanitize=fuzzer,address,undefined \
//       -fno-sanitize-recover=undefined -w -o fuzz_meta tools/fuzz/fuzz_meta.cpp \
//       src/io/exif.cpp src/common/image_svg.cpp src/common/image.cpp src/common/logging.cpp \
//       src/common/fs_path.cpp -lspdlog -lfmt
//
//   ./fuzz_meta corpus seeds -fork=4 -ignore_crashes=1 -max_total_time=600 \
//       -rss_limit_mb=4096 -max_len=65536 -artifact_prefix=artifacts/
//
// ⚠ COVERAGE GAP, stated so nobody assumes otherwise: extractExif -- the JPEG APP1 / PNG eXIf
// CONTAINER walk that finds the payload parseExif then reads -- is NOT fuzzed here. It calls
// sniffImageFormat, which lives in io.cpp, which drags in every image decoder in the project; the
// stub below satisfies the linker and makes that path return Unknown. The payload parser is
// covered, the walk that locates it is not.
//
// Seeds: any .svg (the app's own assets, /usr/share/icons) and any .jpg/.png with metadata. The
// first input byte selects the entry point.

#include "common/image_svg.hpp"
#include "io/exif.hpp"
#include "io/io.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

// exif.cpp's extractExif references this; the harness does not call it, but the TU must link.
// Real magic sniffing lives in io.cpp, which drags in every image decoder.
namespace mosaic::io {
ImageFormat sniffImageFormat(const std::uint8_t*, std::size_t) noexcept {
    return ImageFormat::Unknown;
}
} // namespace mosaic::io

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 2) return 0;
    const std::uint8_t which = data[0] % 3;
    const std::uint8_t* p = data + 1;
    const std::size_t n = size - 1;
    switch (which) {
    case 0: (void)mosaic::io::parseExif(p, n); break;
    case 1: (void)mosaic::common::svgIntrinsicSize(p, n); break;
    case 2: (void)mosaic::common::rasterizeSvg(p, n, 64, 64, nullptr); break;
    }
    return 0;
}
