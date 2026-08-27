// libFuzzer entry point for libmosaicformats (src/formats) -- the six decoders Mosaic hand-rolled,
// every one of which parses bytes from an arbitrary file.
//
// WHY THIS EXISTS. Each decoder already had hand-written negative tests, and those only cover what
// their author thought to distrust. Four minutes of fuzzing found a heap-buffer-overflow READ in
// decodeTga that all of them missed (a true-colour header declaring 8-bit depth: the reader checked
// that one byte was available and then read three). Seventeen witnesses, one bug. After the fix,
// 1.75 M executions across all six came back clean -- and coverage ROSE, because a decoder that
// stops lying about its bounds explores further.
//
// BUILD (clang only -- libFuzzer is not a GCC feature, and this project builds with GCC):
//
//   clang++ -std=c++23 -g -O1 -I src -fsanitize=fuzzer,address,undefined \
//       -fno-sanitize-recover=undefined -o fuzz_formats \
//       tools/fuzz/fuzz_formats.cpp src/formats/*.cpp
//
// RUN (seed it from real files -- the encoders in the same library will write them):
//
//   ./fuzz_formats corpus seeds -fork=4 -ignore_crashes=1 -max_total_time=600 \
//       -rss_limit_mb=4096 -max_len=65536 -artifact_prefix=artifacts/
//
// -fork=4 with -ignore_crashes collects EVERY distinct crash instead of stopping at the first,
// which is what turns a fuzz run into a triage list rather than a single bug report.
//
// ⚠ src/formats links nothing but the standard library, on purpose (see its CMakeLists), which is
// exactly what makes this harness three lines of glue. Keep it that way.

#include "formats/bmp.hpp"
#include "formats/hdr.hpp"
#include "formats/ico.hpp"
#include "formats/pnm.hpp"
#include "formats/qoi.hpp"
#include "formats/tga.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 2)
        return 0;
    const std::uint8_t which = data[0] % 7;
    const std::uint8_t* p = data + 1;
    const std::size_t n = size - 1;
    std::string err;
    switch (which) {
    case 0:
        (void)mosaicfmt::decodeBmp(p, n, &err);
        break;
    case 1:
        (void)mosaicfmt::decodeTga(p, n, &err);
        break;
    case 2:
        (void)mosaicfmt::decodePnm(p, n, &err);
        break;
    case 3:
        (void)mosaicfmt::decodeQoi(p, n, &err);
        break;
    case 4:
        (void)mosaicfmt::decodeIco(p, n, &err);
        break;
    case 5:
        (void)mosaicfmt::decodeHdr(p, n, &err);
        break;
    case 6:
        (void)mosaicfmt::decodeDib(p, n, &err);
        break;
    }
    return 0;
}
