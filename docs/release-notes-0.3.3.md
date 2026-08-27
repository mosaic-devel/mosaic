Mosaic is a GPU-accelerated image editor for Linux, Windows and macOS, written in C++20 against
Vulkan. It is alpha software: the feature surface is broad but the polish is not, and the warnings
below are real rather than boilerplate.

## What changed since 0.3.2

**0.3.2 shipped with a bad regression, and this release exists mostly to fix it.**

In 0.3.2, editing text did not update the canvas. Opening a document drew it correctly, and then
nothing you did to the type ever appeared: typing, changing a font or a size, turning any 3D
parameter, hovering a font to preview it, clicking between text layers. The pixels were being
re-rendered perfectly every frame and thrown away.

The cause was a performance change in 0.3.2 that moved the text re-render *before* the frame's
composite. The re-render reports the region it changed exactly once, and moving the call moved that
one report out of reach of the code that acts on it — so the canvas was asked to repaint a region
of nothing. If you have 0.3.2, this is the release to take.

**A document with two reflect-canvas 3D text layers never stopped working.** Each layer's mirror is
a snapshot of the canvas below it, and refreshing one marked the other stale, forever — a
full-canvas recomposite every third of a second, on a document nobody was touching. On the test
document that is fifteen seconds of CPU on repeat. Two mirrors facing each other have no stable
state to settle into, so the feedback is now cut after one bounce, which is what the effect always
approximated anyway.

**Saving a large document froze the app for sixteen seconds.** Every save built a full-resolution
composite in order to produce a 256-pixel thumbnail for the file — 39.8 megapixels computed and
99.8% of it discarded, on the interface thread. It now composites at thumbnail size: **16.3 s → 24
ms**, and the thumbnail is better filtered for it.

**The New Document dialog took about a second and a half to open** if a large document was in the
recent list. It read each recent file twice, in full, one byte at a time, to find two small records
near the end. It now reads a 46-byte header per record and skips the rest: **1457 ms → 68 ms** per
recent file.

**Large documents composite about 21% faster.** Measured back-to-back on one machine against the
same 39.8-megapixel test document 0.3.2 was tuned on:

| | 0.3.2 | 0.3.3 |
|---|---|---|
| whole-file open | 35.4 s | **29.9 s** |
| one full composite | 24.0 s | **19.0 s** |

Almost all of that is one change: a 39.8-megapixel working buffer is 637 MB, and the compositor
built dozens of them per composite by asking the system for memory and then filling it with zeros —
memory the kernel had already zeroed. It now takes the pages as they come and touches only the part
each layer actually draws into. Two Gaussian blur passes were also rewritten (byte-identical
output); those show up mainly on machines without a usable GPU blur, where the blur adjustment is
**2× faster**.

⚠ Those two rows are both slower than the numbers in the 0.3.2 notes, which said 31 s. Same
document, same code, busier machine. The honest comparison is the one above — both figures measured
minutes apart on the same hardware — and the ratio is the part that travels.

## Reading files that came from somewhere else

Mosaic parses several formats with its own hands rather than a hardened library: BMP, TGA, PNM, QOI,
ICO and Radiance HDR, the `.mosaic` container itself, and brush presets (MyPaint, GIMP, Photoshop,
Krita). Documents and brush packs get shared, so those parsers were fuzzed for the first time. Four
findings, all fixed:

- **A malicious `.tga` could make Mosaic read memory past the end of its buffer**, and those bytes
  land in the decoded image. For an image editor that is the serious end of the class: the stray
  memory becomes pixels, and the pixels get exported and posted. A true-colour header declaring an
  impossible bit depth was accepted, then read as though it were a different depth.
- **A 2 MB `.mosaic` file could exhaust the machine's memory.** Chunks were capped individually at
  256 MB with no limit on the total, and a crafted file holding 160 of them drove one open to 23.6
  GB. Chunk sizes are now bounded by what each kind of chunk can actually hold, and a whole open has
  a budget derived from the file's own size.
- **A crafted `.mosaic` could steer a metadata reader past content it should have found**, by lying
  about a chunk's length. The reader now distrusts the length of any chunk that fails its checksum.
- **A crafted `.svg` — including one used as a brush tip inside a shared preset pack** — produced
  undefined behaviour in the vendored SVG library on values taken from the file. Guarded, with the
  rendering of well-formed drawings verified unchanged.

None of these is known to be exploitable for code execution, and file data is never placed in
executable memory. They are fixed because "not known to be" is not the same as "cannot be".

**This project is not sponsored or endorsed by Anthropic.**

## For anyone looking at the code

The text regression above passed all 3040 tests, because every test in the suite composited a
document *once* and asked whether the pixels were right. None asked the question a user asks, which
is about the second composite: *I changed something — did the canvas change?*

`tests/test_edit_propagation.cpp` asks it now, for every kind of edit the application can make —
3D type, flat type, brushing, vector objects, adjustments, layer effects, texture generators, masks,
layer chrome. Each is held to four things: the edit changes the composite at all, every changed
pixel lies inside the region the code claimed it would change, patching just that region reproduces
a full recomposite, and undo restores the pixels exactly.

There is also fuzzing now (`tools/fuzz/`), and the inputs that once crashed these parsers are kept
in `tests/fuzz-corpus/` and replayed by CI on every push under AddressSanitizer. The CI gate
replays; it does not mutate. A fuzzer gated on random mutation fails on a coin flip and gets
switched off within a month, whereas a corpus of real crashes is a regression test with a unit
test's determinism.

## Before you download

- **Development is paused indefinitely, and this is alpha software.**
- **The Windows build is not the main priority and may contain bugs.**
- **The macOS Quick Look and thumbnail extensions are unverified on hardware** and have been
  reported to hang Finder. They install automatically when you open the DMG — Apple's design, not a
  choice Mosaic makes.
- **Performance still degrades on large documents**, on every platform. A 40-megapixel document
  under this many layers is faster than it was and still not instant.
- A **Vulkan 1.2** driver is required on Linux and Windows. macOS runs on MoltenVK, bundled.
- Nothing is signed by a certificate authority, so each OS will object once.
