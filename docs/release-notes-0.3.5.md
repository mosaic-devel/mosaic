Mosaic is a GPU-accelerated image editor for Linux, Windows and macOS, written in C++20 against
Vulkan. It is alpha software: the feature surface is broad but the polish is not, and the warnings
below are real rather than boilerplate.

![3D shapes and 3D text in Mosaic 0.3.5](https://raw.githubusercontent.com/mosaic-devel/mosaic/v0.3.5/docs/showcase-0.3.5.png)

## What changed since 0.3.4

### Shapes can be 3D

Any vector shape can now carry an extrusion: it renders as a lit, bevelled **solid** instead of a
flat fill, using the same solid model, bevels, orientation, camera, lighting and canvas
reflections the Type tool's 3D already gave a text block. It is one feature rather than two —
shapes share the mesher, both render lanes, the projected-bounds helper and the 3D popup verbatim.
Flat is still the default, and the field is written to `.mosaic` only when it is set, so every
existing document is untouched.

A shape contributes two solids: its **fill** (run 0) and its **stroke outline** (run 1), the
latter built so dashes and joins extrude as they actually draw. Two runs, because materials are
keyed by run — which is what lets a 3D shape wear a chrome outline around a matte face with no
new machinery.

**3D text improvements.** Underline and strikethrough now extrude as solids of their own at the
same depth and bevel, tagged with their run — so "U" on a 3D block means what it means on a flat
one, and a bar crossing a descender fuses into one letterform the way a real underline does.

### Colour in 3D is now just the layer's colour

This is a behaviour change, and it is the fix for three separate complaints.

A 3D layer used to carry its **own** colour, stored on the material, parallel to the colour the
layer already had. The two could not be kept in sync and were not:

- A **shape** copied its fill into that material *once*, when 3D was switched on, and then ignored
  every later colour change. Recolouring a 3D shape did nothing.
- **Text** ignored its run colours entirely and started at a flat grey, so enabling 3D repainted
  your text, and the foreground colour no longer reached it.
- 2D text carries a colour **per run**; the material carried one per layer. Going 3D silently
  flattened a multi-coloured block to a single colour.

Colour is no longer stored beside the solid. It is read from the layer at render time — a text
run's own paint, a shape's fill and stroke — so there is no second copy to go stale, recolouring
works exactly as it does in 2D, and **per-run colour works in 3D text**. The material keeps
metalness and roughness: the finish, which is the one thing 3D genuinely adds to a surface. The
3D panel's own colour swatch is gone, because the layer's colour control is now the only one.
The metal presets still carry a colour — a metal's reflectance *is* its colour — but they now set
the **layer's** colour, so picking Gold still paints it gold, and that colour is afterwards
editable, visible in the flat fill, and survives turning 3D off.

Opening a document written by an older Mosaic drops the stored 3D colour. For a shape that changes
nothing (its stored colour was a copy of the fill it now reads directly). For 3D text it is the
fix: the block reopens in the colours its runs always carried.

### The language picker

**Settings → General → Language** now lists the languages a catalog is actually installed for,
joined against a generated display-name table, so it can never offer a translation that would
silently fail to load. The choice applies at the next start-up — every label is translated when
its widget is built, so re-translating a running window would mean rebuilding the whole UI.
`$MOSAIC_LANG` still outranks the saved preference.

### 3D got much faster

The Vulkan pass that draws every 3D solid was **slower than the CPU fallback it exists to
replace**, which is why earlier work on the CPU lane was invisible from the UI. Two things about
the dispatch, neither of them the arithmetic:

- It ran **one thread per triangle**. That offers the GPU only as many threads as there are
  triangles — a few thousand, where the hardware wants tens of thousands — and makes every lane in
  a subgroup wait for whichever triangle in it has the tallest bounding box. An extruded solid
  mixes cap triangles spanning most of the tile with wall slivers a few pixels tall, so the whole
  dispatch ran at the speed of its worst triangle, twice over. Each triangle now gets a workgroup
  whose invocations stripe its scanlines.
- It **scanned** each triangle's bounding box for coverage instead of solving for it. The edge
  functions are affine along a scanline, so the covered run is two divides. On an extruded solid,
  87.8% of the samples a box scan tests fall outside the triangle.

Measured on one machine, a solid covering 80% of a 1080p canvas, GPU submit-to-fence:

| | 0.3.4 | 0.3.5 |
|---|---|---|
| shallow solid | 46.4 ms | **4.3 ms** |
| deep solid | 145 ms | **11.1 ms** |
| whole composite, shallow | 72 ms | **27 ms** |
| whole composite, deep | 178 ms | **45 ms** |

The CPU lane — the fallback, and what headless export and machines without Vulkan use — got the
same treatment plus deferred shading, so it shades once per *visible* sample rather than once per
sample that ever passed the depth test: **90 ms → 35 ms** for that solid.

Building a 3D shape's mesh, which happens on every geometry or depth edit, went **143 ms → 33 ms**
on a 41-lobe stroked rosette. Two hot spots, both of which had the shape of an all-pairs loop over
a question that was really a bounded one:

- The polygon boolean that turns a stroke into a solid classified every edge fragment against
  every other: 45 million tests, more than the rest of the kernel put together. It now sweeps in
  y and answers both sides of a fragment in one scan. **95 ms → 19 ms.**
- The mesher's bevel-clearance pass tested every vertex against every edge in the solid. It is a
  bounded-radius query and is now gridded. **42 ms → 15 ms.**

Every one of those changes is verified to produce the same pixels: the boolean is byte-identical
across self-unions under both fill rules, all four operations, stroke ribbons and 80 random
degenerate polygons; the mesher byte-identical over 10.1M lines of dumped vertex and index data;
the CPU render lane bit-identical by checksum. The Vulkan lane still matches the CPU lane to a
mean absolute difference of 6e-8.

**Depth is not exponential, despite feeling like it.** A deeper solid genuinely projects a longer
silhouette, so its cost tracks projected *area* — linear in a quantity you are dragging a
one-dimensional slider through. The old shader amplified it badly because bigger tiles meant
bigger per-triangle boxes for a single serial lane; that amplification is gone.

## Known limits

- 3D remains a **CPU-and-compute** feature with no rasterisation pipeline behind it; a very large
  or very deep solid is still work, and a full-canvas recomposite still dominates a gizmo drag on
  a big canvas.
- A gradient or pattern fill on a 3D shape shades with a single representative colour. A gradient
  that must stay a gradient on the face is what a Layer-Effects overlay is for.
- The 3D light gizmo places the lamp on the near hemisphere only: there is no back light yet.
