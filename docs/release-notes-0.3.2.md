Mosaic is a GPU-accelerated image editor for Linux, Windows and macOS, written in C++20 against
Vulkan. It is alpha software: the feature surface is broad but the polish is not, and the warnings
below are real rather than boilerplate.

## What changed since 0.3.1

**Large documents open in a fraction of the time they used to.** This release is almost entirely a
performance arc, driven by one deliberately punishing test document: a 39.8-megapixel photograph
under 56 layers of 3D type, live boolean vector compounds, a nine-effect layer stack, spatial
adjustments and ten saves of undo history.

That document **did not open at all** before this release — eight threads pegged for ten minutes
with no result, twice, before the run was killed. It now opens in **31 seconds**. Nothing about the
pixels changed: every fix below is byte-identical or held to the existing golden images, and the
full test suite was green at each step.

The defects behind it were, in order of what they cost:

- **Layer resampling evaluated `sin()` per kernel tap across the whole canvas**, for layers covering
  a fraction of it — the reason the document never opened. Two fixes: hoist the weights that were
  being recomputed once per source row, and clip the walk to the region the source can actually
  reach.
- **A 34×34 layer-panel thumbnail composited its group at full document resolution** — a 31,717:1
  ratio between what was computed and what was drawn. Thumbnails are now rendered at thumbnail size,
  which is also better filtered: the reduction resolves to a real box filter instead of a single
  nearest-neighbour tap. **257× faster.**
- **A group's isolated buffer ignored how much the target could resolve**, so any reduced-size
  composite quietly rebuilt every group at full canvas resolution. This also made the 3D
  reflect-canvas snapshot — which composites small *precisely* to be cheap — 19× more expensive
  than it needed to be.
- **The entire layer-effects lane ran on one thread** while the rest of the compositor used the
  shared pool. Now banded: **2.9× faster**, goldens untouched.
- **Opening one file composited the canvas three times.** Two per-frame hooks invalidated the
  picture *after* the frame had already been composited. One composite now serves.
- **The GPU blur lane refused every document above ~16.7 megapixels** on a fixed 256 MiB policy cap
  — refusing exactly the documents whose blurs cost the most. The cap is now derived from the
  device's own memory. **11.7× on the affected adjustment.**
- **The Gaussian inner loop clamped twice per tap**, roughly 3.4 billion comparisons per pass to
  serve a few thousand edge pixels. Border and interior are now separate loops. **2.4×.**

**Wayland gets an app icon.** An AppImage cannot inherit one from the desktop environment, so the
window and taskbar showed a generic placeholder. Fixed, along with the window hints, which were
being applied to seven toplevels rather than all of them.

**This project is not sponsored or endorsed by Anthropic.**

## For anyone looking at the code

Two things landed alongside the fixes that are worth more than any single number:

The profiler now names what a composite is actually made of — per layer kind, per effect stage, per
adjustment kind — in release builds, behind `--profile` or `MOSAIC_PROFILE=1`. Several of the
defects above were invisible until the row that owned them had a name, and more than one confident
guess was contradicted by the measurement that followed it.

There are also **cost budgets** in the test suite now (`tests/test_composite_budget.cpp`). Every
defect listed above was *correct*: each one passed all 3040 test cases and was found by opening a
real document and watching a clock. The budgets assert deterministic work **counts** rather than
milliseconds — how many composites, how many layer renders, how many texels cleared — so they are
the same numbers on every machine, and they would have caught these at the commit that introduced
them.

## Before you download

- **Development is paused indefinitely, and this is alpha software.**
- **The Windows build is not the main priority and may contain bugs.**
- **The macOS Quick Look and thumbnail extensions are unverified on hardware** and have been
  reported to hang Finder. They install automatically when you open the DMG — Apple's design, not a
  choice Mosaic makes.
- **Performance still degrades on large documents**, on every platform. It degrades a great deal
  less than it did, and the remaining costs are now measured and named rather than mysterious — but
  a 40-megapixel document with this much on it is still not instant.
- A **Vulkan 1.2** driver is required on Linux and Windows. macOS runs on MoltenVK, bundled.
- Nothing is signed by a certificate authority, so each OS will object once.
