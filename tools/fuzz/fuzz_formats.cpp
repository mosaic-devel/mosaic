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
