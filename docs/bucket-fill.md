# Bucket fill (S21) — design

This note covers the S21 bucket-fill tool: the core flood engine (`core::bucketFillCoverage`), and
how the interactive tool turns a click into one undoable `core::FillCommand`.

## 1. What S21 delivers vs. what already existed

The fill *result* pipeline was built by **S39** (Edit ▸ Fill…) and is reused wholesale:

- `render::computeFill` / `render::computeFillPaint` (`src/render/region_fill.cpp`) — the pixel
  producer: composite a solid colour or any `core::vec::Paint` (gradient / pattern) over a layer
  region under a blend mode + opacity + coverage + Protect-alpha. **Pure, already unit-tested.**
- `core::FillCommand` (`src/core/commands.hpp`) — a region-scoped `SetLayerPixelsCommand` whose only
  distinction is the History label `"Fill"`. Byte-exact undo/redo.
- The **Fill dialog** (`src/ui/fill_dialog.cpp`), including the **Pattern…** content with its live
  `PatternFlyout` editor and `computeFillPaint` preview/commit. S39 shipped this fully — the old
  "(with S21)" greying is already gone; S21 did **not** need to touch it.

S21 adds the missing **interactive** half:

- **`core::bucketFillCoverage`** (`src/core/fill.{hpp,cpp}`) — the flood engine. Pure, FLTK-free,
  unit-tested. Turns a seed pixel + tolerance into a per-pixel fill-coverage mask.
- **The Bucket tool canvas path** (`VulkanCanvas::pushBucketFill` → `MainWindow::bucketFillClick`) —
  click → flood → intersect with the active selection → fill the active foreground over the covered
  region via `computeFill` → **one `FillCommand`** ("Fill" reads once in History).
- An **Anti-alias** toggle added to the bucket's tool-options (the flood engine supports it).

## 2. The flood engine

`bucketFillCoverage(src, seedX, seedY, params) -> std::vector<uint8_t>` (row-major, `src`-sized).

- **Metric.** Shares the S17 wand metric `core::wandColorDistance` (luma-weighted RGB, lighter alpha
  term, normalised to [0,1]; the S43-b managed-ΔE swap seam). The tool bar's 0–255 tolerance slider
  maps onto [0,1]. Sharing the metric keeps the wand and the bucket answering "same colour?" the
  same way.
- **Connectivity.** `contiguous` → a 4-connected **scanline/span flood** from the seed (whole row
  runs at once, only run-starts seeded into the rows above/below — the same worklist shape as the
  wand's flood). `!contiguous` → the global predicate (every within-tolerance pixel, "fill all").
- **Fill vs. selection mask.** This is the one deliberate difference from `magicWandSelection`: the
  flooded **interior is solid 255** (a bucket fills opaque *inside* the region), and only the
  region's **outer boundary** earns a soft ramp when `antialias` is on. A selection instead caps its
  interior at the marching-ants threshold and fringes below it — right for ants, wrong for a fill —
  so the wand code is *not* reused for the fill; the two share only the metric + the span flood.
- **Anti-alias.** An unfilled pixel 4-adjacent to the region earns a sub-0.5 coverage from the
  linear distance ramp (0.5 at d==T → 0 at d==T+band, band = 0.02 in metric units). Gating the
  feather on the *hard* flood keeps the soft band from bridging into a disconnected same-colour
  region (the same guard the wand uses, `research-selection.md §5`).
- **Empty results.** A bad seed, an empty image, or a coverage-free result all return an empty
  vector so the tool can no-op cleanly (never a fill of nothing).

The tool then intersects this mask with the active selection (mapped into layer-pixel space through
the layer transform, exactly like `buildFillContext`), crops to the covered bbox, and hands the
region to `computeFill`. Fill therefore **respects the selection** (whole active layer if none) and
Protect-alpha is available through the shared fill core.

## 3. Lineage, and what is deliberately not built

Every mechanism here is long-published graphics practice:

- **Seed / flood fill (scanline + span).** The recursive and scanline seed-fill algorithms are
  foundational computer graphics from the 1960s–70s (Smith, *Tint Fill*, SIGGRAPH 1979; the
  span/scanline variant is textbook Foley & van Dam).
- **Tolerance / colour-distance flood ("magic-wand"-style fill).** Filling the connected region of
  *near*-seed-colour pixels shipped in consumer paint programs by the mid-1980s (MacPaint 1984's
  paint bucket, Deluxe Paint, early Photoshop). We additionally reuse Mosaic's own
  `wandColorDistance` (S17, `research-selection.md`) — no new metric is introduced.
- **Contiguous vs. global ("fill all matching pixels").** A boolean over the same predicate;
  "select/fill by colour" is equally ancient.
- **Anti-aliased fill edge.** A one-pixel coverage ramp from a distance-to-threshold value is a
  trivial, long-published smoothing; it is the same construction S17 already uses for the wand.
- **Pattern fill.** Tiling a repeating motif into a filled region is 1980s-era practice (MacPaint
  patterns; PostScript/PDF tiling patterns, published spec). Mosaic's pattern evaluator
  (`core::vec::samplePattern`, procedural, resolution-independent) shipped under **LE-d**; the fill
  only multiplies its output by coverage.

**⚠ Deliberately NOT implemented here, and not to be added:** content-aware / "smart" fill (that
belongs to the inpaint path, `docs/inpainting-research.md`), any edge-aware / geodesic / graph-cut
region growing, and gradient-mesh fills. The bucket does **plain colour-tolerance flood + paint**
and nothing more — a design boundary, not an omission.
