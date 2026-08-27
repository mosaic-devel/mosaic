// libFuzzer entry point for the brush-preset readers -- the OTHER surface where Mosaic parses a
// third party's binary format with its own hands: MyPaint .myb, GIMP .gbr/.gih, Photoshop .abr,
// Krita .kpp, Mosaic .mbp, the PNG tip reader, and the two XML parsers behind them.
//
// RESULT SO FAR: clean. 22.8 M executions over 4,264 coverage features, no crashes. That is
// evidence, not proof, and it is worth re-running whenever one of these readers is touched.
//
// BUILD (clang only; this project builds with GCC, and libFuzzer is not a GCC feature). These
// readers are not dependency-free the way src/formats is, so the sources come in by hand:
//
//   clang++ -std=c++23 -g -O1 -I src -I third_party -I third_party/nanosvg \
//       -I third_party/pugixml -I build/linux-release/generated \
//       -fsanitize=fuzzer,address -w -o fuzz_brush tools/fuzz/fuzz_brush.cpp \
//       src/io/brush/*.cpp src/core/brush/*.cpp src/common/*.cpp src/formats/*.cpp \
//       src/io/png.cpp src/core/selection.cpp src/core/layer.cpp src/core/vector/*.cpp \
//       third_party/pugixml/*.cpp -lz -lpng -ljpeg -lspdlog -lfmt
//
// RUN -- and ⚠ ASAN_OPTIONS=alloc_dealloc_mismatch=0 IS NOT OPTIONAL:
//
//   ASAN_OPTIONS=alloc_dealloc_mismatch=0 ./fuzz_brush corpus seeds -fork=4 \
//       -ignore_crashes=1 -max_total_time=600 -rss_limit_mb=4096 -max_len=131072 \
//       -artifact_prefix=artifacts/
//
// Without it the run drowns in FALSE POSITIVES: std::stable_sort's _Temporary_buffer allocates
// through operator new(nothrow) and releases through __return_temporary_buffer, and clang's ASan
// paired with GCC's libstdc++ calls that an alloc-dealloc mismatch. Fifteen artifacts on the first
// run, every one of them core/brush/curve.cpp:58 -- which is a plain stable_sort over parsed curve
// points and is not doing anything wrong. They also cost real coverage by killing jobs: 2,143
// features with the noise, 4,264 without.
//
// SEEDS: real presets make this worthwhile. Krita ships a good corpus at
// /usr/share/krita/paintoppresets (.kpp and .myb); GIMP's brushes are .gbr/.gih under /usr/share.
// The first byte of each input selects the reader, so prefix a seed with the right selector.

#include "io/brush/kpp.hpp"
#include "io/brush/myb.hpp"
#include "io/brush/preset_json.hpp"
#include "io/brush/preset_xml.hpp"
#include "io/brush/tip_io.hpp"
#include "io/brush/zip.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 2)
        return 0;
    const std::uint8_t which = data[0] % 9;
    const std::uint8_t* p = data + 1;
    const std::size_t n = size - 1;
    std::string err;
    switch (which) {
    case 0:
        (void)mosaic::io::brush::readMyb(p, n, "fuzz", &err);
        break;
    case 1:
        (void)mosaic::io::brush::readGbr(p, n, &err);
        break;
    case 2:
        (void)mosaic::io::brush::readGih(p, n, &err);
        break;
    case 3:
        (void)mosaic::io::brush::readAbr(p, n, &err);
        break;
    case 4:
        (void)mosaic::io::brush::readPngTip(p, n, &err);
        break;
    case 5:
        (void)mosaic::io::brush::readKpp(p, n, &err);
        break;
    case 6:
        (void)mosaic::io::brush::readMbp(p, n, &err);
        break;
    case 7:
        (void)mosaic::io::brush::parsePresetXml(
            std::string_view(reinterpret_cast<const char*>(p), n), &err);
        break;
    case 8:
        (void)mosaic::io::brush::parseTipXml(std::string_view(reinterpret_cast<const char*>(p), n),
                                             &err);
        break;
    }
    return 0;
}
