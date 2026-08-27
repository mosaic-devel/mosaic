// libFuzzer entry point for the brush-preset readers -- the OTHER surface where Mosaic parses a
// third party's binary format with its own hands: MyPaint .myb, GIMP .gbr/.gih, Photoshop .abr,
// Krita .kpp, Mosaic .mbp, the PNG tip reader, and the two XML parsers behind them.
//
// RESULT SO FAR: clean. 21.4 M executions over 3,949 coverage features, no crashes. That is
// evidence, not proof, and it is worth re-running whenever one of these readers is touched.
//
// ⚠ Those are the numbers from the run against the VENDORED pugixml. An earlier run reported
// 22.8 M / 4,264 and had to be discarded: the harness was picking up a system /usr/include copy,
// so it was fuzzing a library the project does not ship. A clean result against the wrong code is
// not a clean result. See the note under BUILD below.
//
// ⚠ ASAN_OPTIONS=alloc_dealloc_mismatch=0 is required and run-fuzzers.sh sets it. Without it,
// std::stable_sort's _Temporary_buffer -- allocated through operator new(nothrow), released
// through __return_temporary_buffer -- reads as an alloc-dealloc mismatch under clang's ASan with
// GCC's libstdc++. Fifteen false positives on core/brush/curve.cpp:58, a plain stable_sort over
// parsed curve points, and they cost real coverage by killing jobs.
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
