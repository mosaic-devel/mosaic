# Fuzz regression corpus

Every input in here once crashed one of Mosaic's hand-written parsers. None of them crash now. CI
replays the lot on every push (`tools/fuzz/run-fuzzers.sh replay`, the `fuzz` job), so a change
that reintroduces one of these bugs fails the build instead of shipping.

The first byte of each file selects the entry point inside its harness; the rest is the input. That
is why these are not `.tga` or `.svg` files and cannot be opened directly — they are fuzzer inputs,
not documents.

## `formats/` — 17 inputs, `tools/fuzz/fuzz_formats.cpp`

All seventeen are the same bug, which is worth keeping as seventeen: a **heap-buffer-overflow read**
in `decodeTga`. A true-colour TGA header declaring 8-bit depth gave `bytesPerPixel == 1`, so the
reader checked that one byte was available and then read three. Fixed by rejecting a pixel depth
its image type cannot have.

## `meta/` — 19 inputs, `tools/fuzz/fuzz_meta.cpp`

Undefined behaviour in vendored nanosvg, reachable because an **SVG brush tip inside a shared
preset bundle** gets rasterized (`io/brush/library.cpp`). Three sites, all float-to-int on values a
file chose: a NaN arc-division count, an infinite scanline stepper, and `arc / da` with `da == 0` in
the stroke-cap tessellator. Fixed in `third_party/nanosvg/` — see `MOSAIC-PATCHES.md` there.

⚠ Twelve of these were found first; the other seven only appeared once the harness stopped stubbing
`sniffImageFormat` to `Unknown`, which had made the EXIF container walk unreachable and, with it,
a whole region of the SVG code. A harness that cannot reach the code is a harness that reports
nothing, confidently.

## What is not here

`fuzz_brush` has no corpus because it has never crashed — 22.8 M executions clean. The replay gate
still runs it against one trivial input so that a harness which stops *building* is caught.

Real seed material is deliberately not checked in: Krita ships a good preset corpus at
`/usr/share/krita/paintoppresets`, GIMP's brushes are under `/usr/share`, and neither is ours to
redistribute. `explore` mode seeds from this directory and whatever else you point it at.

## Adding to it

When a fuzz run finds something, commit the artifact here with the fix. Keep the file even after
the bug is dead — that is the entire point of a regression corpus.
