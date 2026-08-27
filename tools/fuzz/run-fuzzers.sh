#!/usr/bin/env bash
#
# Build and run Mosaic's fuzz harnesses. Used by CI and by hand, so that "it passed locally" and
# "it passed in CI" mean the same thing.
#
#   ./tools/fuzz/run-fuzzers.sh replay        deterministic: replay the checked-in corpus, no
#                                             mutation. This is the CI gate.
#   ./tools/fuzz/run-fuzzers.sh explore 300   mutate for N seconds per harness. Finds NEW bugs;
#                                             nondeterministic, so it is never a gate.
#
# ⚠ REPLAY IS THE GATE, EXPLORE IS NOT, and the split is the whole point. A fuzzer wired into CI as
# a pass/fail on mutation is a test that fails on a coin flip and gets disabled within a month.
# What CI gates is the CORPUS: every input that ever crashed one of these parsers, replayed, must
# still not crash. That is a regression test with a fuzzer's inputs and a unit test's determinism.
# Exploration belongs on someone's machine, or a nightly, where a new finding is news rather than
# a broken build.
#
# ⚠ CLANG ONLY. libFuzzer is not a GCC feature; the project itself builds with GCC and these
# harnesses are the one place clang is required.
set -euo pipefail

MODE="${1:-replay}"
SECONDS_PER="${2:-60}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${FUZZ_OUT:-$ROOT/build/fuzz-bin}"
# The harnesses compile real sources, and src/common/version.cpp needs version_config.hpp. That
# header is produced by configure_file at CONFIGURE time, so a bare `cmake --preset ...` is enough
# -- CI does not need to build the project to fuzz it.
GEN="${FUZZ_GEN:-}"
if [ -z "$GEN" ]; then
    for cand in "$ROOT/build/linux-release/generated" "$ROOT/build/linux-debug/generated"; do
        [ -d "$cand" ] && { GEN="$cand"; break; }
    done
fi

CXX="${CLANGXX:-clang++}"
command -v "$CXX" >/dev/null || { echo "run-fuzzers: $CXX not found (libFuzzer needs clang)"; exit 1; }
[ -d "$GEN" ] || { echo "run-fuzzers: no generated/ dir -- configure and build the project first"; exit 1; }

mkdir -p "$OUT"
COMMON=(-std=c++23 -g -O1 -I "$ROOT/src" -I "$ROOT/third_party" -I "$ROOT/third_party/nanosvg"
        -I "$GEN" -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -w)

build() { # name, then sources
    local name="$1"; shift
    echo "  building $name"
    "$CXX" "${COMMON[@]}" -o "$OUT/$name" "$@"
}

echo "== building harnesses =="
build fuzz_formats "$ROOT/tools/fuzz/fuzz_formats.cpp" "$ROOT"/src/formats/*.cpp

build fuzz_meta "$ROOT/tools/fuzz/fuzz_meta.cpp" \
    "$ROOT/src/io/exif.cpp" "$ROOT/src/common/image_svg.cpp" "$ROOT/src/common/image.cpp" \
    "$ROOT/src/common/logging.cpp" "$ROOT/src/common/fs_path.cpp" -lspdlog -lfmt

build fuzz_brush "$ROOT/tools/fuzz/fuzz_brush.cpp" \
    "$ROOT"/src/io/brush/*.cpp "$ROOT"/src/core/brush/*.cpp "$ROOT"/src/common/*.cpp \
    "$ROOT"/src/formats/*.cpp "$ROOT/src/io/png.cpp" "$ROOT/src/core/selection.cpp" \
    "$ROOT/src/core/layer.cpp" "$ROOT"/src/core/vector/*.cpp "$ROOT"/third_party/pugixml/*.cpp \
    -lz -lpng -ljpeg -lspdlog -lfmt

# ⚠ Not optional for fuzz_brush. std::stable_sort's _Temporary_buffer allocates through
# operator new(nothrow) and releases through __return_temporary_buffer, and clang's ASan paired
# with GCC's libstdc++ calls that an alloc-dealloc mismatch. It is a false positive -- fifteen of
# them on core/brush/curve.cpp:58, which is a plain stable_sort -- and it costs real coverage by
# killing jobs. See tools/fuzz/fuzz_brush.cpp.
export ASAN_OPTIONS="${ASAN_OPTIONS:-alloc_dealloc_mismatch=0}"

status=0
if [ "$MODE" = "replay" ]; then
    echo "== replaying the checked-in corpus (deterministic) =="
    for pair in "fuzz_formats:formats" "fuzz_meta:meta"; do
        bin="${pair%%:*}"; dir="$ROOT/tests/fuzz-corpus/${pair##*:}"
        n=$(find "$dir" -type f 2>/dev/null | wc -l)
        echo "  $bin over $n input(s)"
        "$OUT/$bin" -runs=0 "$dir" || status=1
    done
    # fuzz_brush has no corpus: it has never crashed. Prove it still builds and runs by feeding it
    # one trivial input rather than skipping it silently.
    printf '\x00hello' > "$OUT/.brush-smoke"
    "$OUT/fuzz_brush" "$OUT/.brush-smoke" || status=1
    [ $status -eq 0 ] && echo "== replay clean ==" || echo "== REPLAY FAILED =="
else
    echo "== exploring, ${SECONDS_PER}s per harness (NOT a gate) =="
    for pair in "fuzz_formats:formats" "fuzz_meta:meta" "fuzz_brush:"; do
        bin="${pair%%:*}"; sub="${pair##*:}"
        corpus="$OUT/corpus-$bin"; mkdir -p "$corpus"
        seeds=()
        [ -n "$sub" ] && seeds=("$ROOT/tests/fuzz-corpus/$sub")
        echo "  $bin"
        "$OUT/$bin" "$corpus" "${seeds[@]}" -max_total_time="$SECONDS_PER" -rss_limit_mb=4096 \
            -max_len=65536 -artifact_prefix="$OUT/artifacts-$bin-" || status=1
    done
fi
exit $status
