# Local changes to vendored nanosvg

Everything else in `third_party/` is upstream verbatim. These two headers are not, and this file
exists so that an upgrade re-applies the changes instead of silently dropping them. Each site is
bracketed in the source by `MOSAIC LOCAL CHANGE` comments — `grep -n "MOSAIC LOCAL CHANGE"` finds
all of them.

## Why

An SVG reaches nanosvg from a file someone else made. `io/brush/library.cpp` rasterizes an **SVG
brush tip out of a shared preset bundle** (`svgIntrinsicSize`, then `rasterizeSvg`), and brush
packs get passed around. Fuzzing that path (`tools/fuzz/fuzz_meta.cpp`, libFuzzer + ASan + UBSan)
produced twelve witnesses, all undefined behaviour, none of them upstream's fault in any
interesting sense — they are the ordinary consequence of doing float arithmetic on numbers a file
chose.

Nothing here was an ASan finding. Nothing indexed out of bounds, and on x86-64 these conversions
produce `INT_MIN` and the loops downstream then decline to run, so the practical impact was low.
That is the hardware being kind rather than the code being right: a compiler is entitled to assume
undefined behaviour cannot happen, and to optimise on that assumption.

## The changes

**1. `nanosvg.h` — `nsvg__pathArcTo`, the arc-division count.**

`ndivs = (int)(fabsf(da) / (NSVG_PI*0.5f) + 1.0f)` where `da` comes from arc parameters in the path
data and can be NaN. `(int)` of a NaN is undefined. A legitimate arc sweep is at most `2*PI`, so
`ndivs` is at most 5 — the guard cannot change what any real drawing does. It also bounds the loop
that follows, which a large `ndivs` would otherwise run for a very long time.

**2. `nanosvgrast.h` — the active-edge stepper.** Two parts, and the second only became visible
after the first:

- `nsvg__f2i`, replacing three `(int)` conversions in `nsvg__addActive`. `dxdy` is a slope over an
  edge whose endpoints come from the file, so a near-horizontal edge makes it enormous and a
  degenerate one makes it infinite or NaN. Saturates instead; every in-range value converts exactly
  as `(int)` would.

- `nsvg__addsat`, replacing `z->x += z->dx` in `nsvg__rasterizeSortedEdges`. Saturating the
  conversion alone was **not enough**, and the fuzzer said so immediately: the two remaining
  witnesses had moved one step downstream to signed overflow in the accumulation. No static bound
  on `dx` fixes that, because `x` accumulates `dx` once per subsampled scanline, so the safe bound
  depends on the raster height. Saturating the add needs no such reasoning, and the fill path
  already clamps `x` to the raster.

## What was verified

- **The render is byte-identical.** The app icon rasterized at 16/32/64/128/256 px hashes the same
  before and after. That is the property that matters: these guards only engage on values that
  were undefined anyway.
- All twelve witnesses stop reporting.
- A fresh 430-second fuzz run over EXIF and both SVG entry points: zero crashes.

## On upgrading

Re-apply both, then re-run `tools/fuzz/fuzz_meta.cpp` and re-check the icon hashes. If upstream has
fixed these itself, drop the local change rather than keeping both — and check the *downstream*
accumulation too, since fixing only the conversions is the trap this file records.
