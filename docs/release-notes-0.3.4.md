Mosaic is a GPU-accelerated image editor for Linux, Windows and macOS, written in C++20 against
Vulkan. It is alpha software: the feature surface is broad but the polish is not, and the warnings
below are real rather than boilerplate.

## What changed since 0.3.3

**This is a performance release. Nothing about it changes what Mosaic draws** — every change in it
produces the same pixels as 0.3.3 did, and most produce them bit for bit.

Measured back-to-back on one machine, both versions opening the same 39.8-megapixel, 57-layer test
document:

| | 0.3.3 | 0.3.4 |
|---|---|---|
| whole-file open | 27.8 s | **23.3 s** |
| one full composite | 17.7 s | **14.0 s** |

**Typing in 3D text was running at fifteen frames a second.** A 200-pixel extruded headline on a
1080p canvas cost 69 ms per keystroke; it now costs 34. The renderer that shades the solid was
doing all of its work on one core — it is split across cores now, by scanline rather than by
triangle, because the triangles share a depth buffer and splitting on them would have made the
picture depend on thread timing.

**Text was the single most expensive thing in a document, and most of that was avoidable.** Three
findings, in what happens to the pixels rather than in the typography:

- Every text layer was resampled with a filter whose weights were recomputed *at every pixel* —
  for the sharp kernel that means two sine calls per weight, fourteen weights per pixel. The weights
  only vary per column and per row, so they are now worked out once. **Placing text is 2.5× faster.**
- Styled body copy composited each of its runs over the *whole* text image. A paragraph with an
  italic emphasis and a coloured lead-in becomes a dozen runs, so a five-megapixel cache was walked
  a dozen times to composite a few thousand pixels each. Each run now covers only its own area.
- The per-face overlay maps that 3D text paints itself with were baked on one core: **4.5× faster.**

**Vector shapes and glyph outlines are 4.6× faster to rasterise.** The scanline filler tested every
edge of a shape against every scanline — for a 41-lobe rosette that is 441 million edge tests to
draw it once, to find the handful that actually cross each line. It now sweeps the edges as the
scanlines reach them. This is also the code that turns glyph outlines into pixels, so it shows up
anywhere there is text.

**Several adjustment layers were copying the whole canvas to arrive where they started.** A blur, a
vignette or an added-noise layer at full opacity with no mask duplicated the entire working image —
637 MB on a document this size — ran its filter on the copy, then blended the copy back over the
original unchanged. At its most visible: a small-radius blur adjustment is **17% faster**, and the
Vignette and Add Noise layers about 20%.

Where the time went, on the same document:

| | 0.3.3 | 0.3.4 |
|---|---|---|
| placing text layers | 2473 ms | **973 ms** |
| rasterising vector shapes | 1629 ms | **355 ms** |
| building text caches | 1714 ms | **1034 ms** |
| layer effects | 3842 ms | **3247 ms** |

**A crash at exit, on any machine with Vulkan validation enabled.** The GPU compute lanes were
destroyed after `main()` returned, which is after the Vulkan loader may already have shut itself
down — undefined behaviour that most drivers quietly tolerate and a validation layer does not. They
are now torn down at a defined moment, before the program returns.

**This project is not sponsored or endorsed by Anthropic.**

## For anyone looking at the code

**The measurement fixture was lying.** The tool that generates the test document set eleven
adjustment parameters by names the schema does not have — and a name that does not match is not an
error, it silently takes the default. Two of the document's adjustment layers were therefore sitting
at their defaults, which are the identity, which the compositor skips entirely. One of them was the
*spatial* adjustment the fixture exists to exercise, and its `0.00 ms` had been read off the
profiler more than once as a fact about the adjustment. It was a fact about the fixture. The
generator now asks the schema and fails the run on an unknown key or an out-of-range value.

**The benchmark could not see the problem either.** Its text case measured 28-pixel body copy in one
run — the cheapest text a document can hold. Three cases were added for the shapes people actually
report: a large headline, a block cut into styled runs, and a headline as a 3D solid. The 3D row is
where "3D text is slow for what it is" stopped being an impression and became a number.

**Six changes were reverted for measuring nothing**, which is recorded in the working notes
alongside the ones that landed, with the numbers. Parallelising a loop that streams a 637 MB buffer
does nothing on a machine whose memory is already saturated by one core; the same shape of change
applied to a loop doing real arithmetic per pixel was worth 4.5×. Telling those apart requires
measuring rather than reasoning, and one of them was measured as a 2.2-second regression before a
re-measurement showed it was the machine getting busier between runs.

Every performance change here is either bit-identical to what it replaced or byte-identical by
construction, and each is pinned by a test built to fail against the specific defect it could
introduce — verified by injecting that defect and watching the test fail, against a build confirmed
to have actually compiled.

Full suite: 3079 cases, 2,295,005 assertions. Verified under AddressSanitizer + UndefinedBehaviour
sanitizer (no reports), and under the Vulkan validation layer (no findings).

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
