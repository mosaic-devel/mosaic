# LE-d2 — Image (raster) patterns

> **Scope.** LE-d1 shipped the **procedural** pattern library (25 kinds, colour-editable, resolution-
> independent — `src/core/vector/pattern.cpp`). LE-d2 adds the **second honest category** from
> `docs/layer-effects.md` §7.2: **fixed-resolution bitmap tiles**, sourced **entirely from the user** —
> *Make Pattern from Selection* and *Import image*. This note is the source of truth for the tile-store /
> tile-resolver architecture and the increment plan. It is a companion to `docs/layer-effects.md` §7
> (the two-category design) and §10 (technique lineage & licences).
>
> **DECISION 2026-07-04 — Mosaic ships NO bundled pattern tiles.** The original plan floated a curated
> GIMP / CC0 stock set; the user ruled it out (see §5). Rationale: a single 1K raster tile is ~1 MB,
> against a ~5 MB release binary — bundling even a dozen would bloat the download absurdly, and the
> **25 procedural kinds already cover the built-in geometric need** (resolution-independent + colour-
> editable, strictly better than low-res bitmaps). Image patterns are therefore **user-supplied only**.
> This also makes the whole IP question moot — Mosaic distributes no third-party pattern assets.
>
> **Status (increment 1, done + on main).** The self-contained **sampler is built and green** (bilinear /
> nearest, seamless wrap, scale / angle / offset) and the **construction helpers** (`makeImagePattern`)
> exist and are unit-tested. The **UI is not yet wired** — the flyout still shows the *Image tiles
> (coming soon)* placeholder (LE-d2·2). No external assets are bundled, and none ever will be (above).

---

## 1. The data model (as-built)

`src/core/vector/pattern.hpp`:

```cpp
struct ImagePattern {
    std::shared_ptr<const common::Image> tile;  // shared, immutable, self-contained; null => transparent
    float scale = 1.0f;      // multiplier of native tile px: layer px p samples native px p/scale
    float angleDeg = 0.0f;   // rotation of the tiling about the anchor origin
    common::Vec2 offset{0,0}; // phase shift, in FRACTIONS OF ONE TILE per axis (0..1 == a full tile)
    bool operator==(const ImagePattern&) const = default;  // shared_ptr IDENTITY + fields (see §4)
};
using Pattern = std::variant<ProceduralPattern, ImagePattern>;
```

Design decisions baked in:

- **Self-contained tile, not a store handle.** The tile is a `shared_ptr<const common::Image>` held
  *directly on the pattern*, so `samplePattern` stays **pure** — no registry, no I/O, no lookup at
  sample time (the hard constraint from `docs/layer-effects.md` §7 and the header contract). A
  document-level pool (§4) is a *serialization + dedup* concern layered **on top**, never on the
  sample path.
- **Immutable (`const Image`).** Tiles are never mutated in place; a re-crop/re-import makes a new
  tile. This makes sharing across layers / undo snapshots safe and cheap.
- **`common::Image` (8-bit RGBA, straight alpha)** is the tile representation — the same container the
  rest of the pipeline already moves around (`src/common/image.hpp`), so from-selection / import / (a
  future) PNG decode all produce it with no conversion.

## 2. Sampling (as-built)

`samplePattern`'s `ImagePattern` branch (`src/core/vector/pattern.cpp`) and the file-local
`sampleImageTile` helper:

- **Layer-px contract, shared with procedural.** The renderer feeds real layer/box px (never the
  `[0,1]` a gradient gets). `layerPx` is rotated by `-angleDeg`, divided by `scale` to reach **native
  tile px**, then phase-shifted by `offset * tileSize`.
- **Seamless wrap.** Native coordinates wrap `modulo` the tile size (correct for negatives), so a tile
  repeats edge-to-edge with no seam and no clamp/mirror.
- **`antialias` picks the filter** — the same document-wide AA choice (the Move tool's AA combobox)
  the procedural kinds honour: **bilinear** (soft) when on, **nearest-neighbour** (crisp / pixelated)
  when off. Bilinear samples about texel centres (`i+0.5`) and interpolates in **premultiplied alpha**
  so a transparent texel never bleeds RGB across an edge (correct for from-selection tiles that carry
  alpha); the result is returned straight-alpha to match `ColorF`.
- **Honest quality ceiling (`docs/layer-effects.md` §7.2).** `scale > 1` magnifies the tile, so it
  softens above native (bilinear) — the standard bitmap-pattern behaviour. The flyout will show the
  native tile size so the ceiling is visible. Sharpening / super-resolution is explicitly *not* this
  feature (scalable materials are the Texture Generator's job).
- **Cost is O(1)/pixel** (a 4-tap bilinear), independent of tile size — only memory scales with the
  tile.

Coverage: `tests/test_pattern.cpp` — tiling wrap-around, negative wrap, scale magnification, bilinear
vs nearest, premultiplied no-bleed, angle rotation, offset phase, `sampleAt` reachability, and an
end-to-end Pattern-Overlay through `applyEffects`.

## 3. Building tiles — make-from-selection & import (as-built + wiring plan)

`src/core/vector/pattern.hpp` — pure, tested constructors (no I/O):

```cpp
ImagePattern makeImagePattern(common::Image tile);                                  // import / ready crop
ImagePattern makeImagePattern(const common::Image& src, long x,long y, uint32_t w,uint32_t h); // crop region
```

The region overload reuses `common::copyRegion` (out-of-bounds reads transparent); an empty
source/region yields a **null tile** (reads transparent — the safe default).

**Make Pattern from Selection (UI wiring, next increment).** The tile is the active layer's pixels
cropped to the selection's bounding box:
`makeImagePattern(activeLayer.pixels(), selBounds.x, selBounds.y, selBounds.w, selBounds.h)`.
(Selection bounds already exist in the selection model, `src/core/selection*`; a *non-rectangular*
selection would additionally punch its mask into the tile's alpha — a follow-on refinement, not
required for v1.) A sensible **max tile dimension cap** (e.g. 1024 px) should clamp/downsample huge
selections before they become a tile, to bound memory.

**Import image (UI wiring, next increment).** Decode a user-chosen PNG/JPG to a `common::Image` (the
io module, S42) and `makeImagePattern(std::move(img))`. Until S42's decoders are wired, import can be
gated behind "requires image I/O".

## 4. The tile store — serialization, dedup, identity (LE-g / S48)

Sampling needs **no** store (the tile is on the pattern). A store is needed only for **persistence,
de-duplication, and value identity**:

- **Identity problem (known gap).** `ImagePattern::operator==` compares the `shared_ptr` by **identity**,
  so two tiles with identical *content* from different sources compare unequal. That is fine for the
  render/undo footprint today but wrong for round-trip dedup and for coalescing undo across a reload.
  **Recommended fix (LE-g):** tag each tile with a **content hash** (e.g. a 64/128-bit hash of the
  RGBA bytes) computed once at construction; make equality hash-based; use the hash as the on-disk id.
- **Document `TilePool` (S48).** A document-scoped map `contentHash -> shared_ptr<const Image>`. On
  **save**, each unique tile is written **once** into the `.mosaic` container (S48 zip + manifest;
  `nlohmann/json` is already vendored) as e.g. `tiles/<hash>.png`, and every `ImagePattern` serializes
  as `{ "tile": "<hash>", scale, angleDeg, offset }`. On **load**, the pool decodes each tile once and
  hands out shared `shared_ptr`s, so N layers referencing one pattern share one buffer. In memory the
  `ImagePattern` keeps its direct `shared_ptr` (the pure fast path) plus the optional hash tag.
- **SVG export** (best-effort, LE-g): a tile becomes a `<pattern>` with an embedded (base64) `<image>`;
  `scale`/`angle`/`offset` map to `patternTransform`.

This keeps the sample path pure while giving persistence a single, content-addressed source of truth.

## 5. Bundled stock tiles — DECIDED AGAINST (2026-07-04)

The arc once planned a curated stock tile set (GIMP, then any CC0 collection). **Resolved: Mosaic
ships no bundled pattern tiles.** Two independent reasons, either sufficient:

1. **Binary weight.** A single 1K raster tile is ~1 MB; the whole release binary is ~5 MB. Bundling
   even a dozen tiles would multiply the download for low-res assets the user can trivially supply
   themselves. Absurd trade for a secondary feature.
2. **Procedural already covers the built-in need.** LE-d1's 25 kinds give a resolution-independent,
   colour-editable geometric library — strictly better than low-res bitmaps for that job. Bitmap tiles
   only add value for *photographic/material* looks, which are exactly what a user imports or lifts
   from their own image.

Consequence: **image patterns are user-supplied only** (make-from-selection + import, §3), and the
whole IP question is **moot** — Mosaic distributes no third-party pattern assets, so there is nothing
to licence-audit or record in `docs/third-party-licenses.md`.

*Research trail (for the record).* A GIMP-stock audit found its `.pat` set unsafe anyway (`gimp-data`
LICENSE makes CC0 forward-looking only and disclaims clear licensing on the shipped files; GPL-by-
default, murky 1998-era provenance). Genuinely-CC0 alternatives do exist (Kenney Pattern Pack; ambientCG
and Poly Haven material textures — all CC0, no attribution) and were sampled for review, but the
weight reason above rules out bundling regardless of licence. Should this ever be revisited, those CC0
material sets — not GIMP — are the only clean starting point.

## 6. UI — the flyout's two categories (plan)

Today the pattern flyout (`src/ui/pattern_flyout.cpp`) edits a **`ProceduralPattern`** only:
`m_pat` is a `ProceduralPattern`, `setOnChange` emits a `ProceduralPattern`, and the host dialog's
sink `m_onPatternChange` is `std::function<void(const ProceduralPattern&)>`
(`src/ui/layer_effects_dialog.cpp`). The LE-d2 placeholder is the bordered box `m_ld2` (*"Image tiles
(coming soon)"*, `pattern_flyout.cpp` ~line 353).

Wiring image tiles is therefore a **flyout-model refactor**, deferred to its own increment to avoid
churn while LE-d1 polish is in flight:

- **Widen the flyout's model to the full `Pattern` variant.** `m_pat` becomes a `Pattern`; `setOnChange`
  and the dialog sink emit `Pattern`. The left colour pane + the right per-kind controls show only for
  the `ProceduralPattern` alternative; for `ImagePattern` the right pane shows the **tile preview,
  native size, scale / angle / offset**, and the source actions.
- **Turn the placeholder into the entry point:** replace `m_ld2` with a small **category toggle**
  (Procedural | Image) plus, in the Image category, **[Make from selection]** and **[Import…]**
  buttons that call the §3 constructors, and a tile thumbnail with the native-size caption.
- **`defaultProceduralPattern` gains an image sibling** the parent seeds when a paint is switched to an
  image tile (a null-tile `ImagePattern` reads transparent until a source is chosen).
- The paint-chip mini-preview in the dialog (`layer_effects_dialog.cpp` ~line 368) already routes any
  `Pattern`; it will render an `ImagePattern` via the same `samplePattern` once seeded.

**Known routing gap to close then:** `paintAtNorm` (`src/render/layer_effects_render.cpp`) reads
`anchorToCanvas` only off `ProceduralPattern`, so an image tile currently always resolves as
**layer-glued** (or content-box). Add an `anchorToCanvas` field to `ImagePattern` (mirroring
procedural) and read it there for canvas-anchor parity.

## 7. Increment plan

| Increment | Deliverable | State |
|---|---|---|
| **LE-d2·1 (done, on main)** | Sampler (bilinear/nearest, wrap, scale/angle/offset) + `makeImagePattern` constructors + doctests + this doc | **DONE, green** |
| **LE-d2·2** | Flyout model → full `Pattern`; Image category with **Make-from-selection** + **Import** (user-supplied only); `ImagePattern.anchorToCanvas` + `paintAtNorm` routing | next |
| **LE-d2·3** | Content-hash identity + document `TilePool` + `.mosaic` round-trip (folds into S48 / LE-g) | with S48 |
| ~~**LE-d2·4** Curated stock tiles~~ | **DROPPED 2026-07-04** — Mosaic bundles no pattern assets (§5: binary weight + procedural already covers built-ins). Image patterns are user-supplied only. | cut |

## 8. References

- `docs/layer-effects.md` §7 (two-category patterns), §7.2 (honest ceiling), §7.3 + §10 (licence /
  lineage), §5.3 (overlays).
- `src/core/vector/pattern.{hpp,cpp}` (model + sampler + constructors); `tests/test_pattern.cpp`.
- `src/render/layer_effects_render.cpp` (`paintAtNorm` routing: canvas-anchor / layer-glue / box).
- `src/common/image.hpp` (`Image`, `copyRegion`); `docs/third-party-licenses.md` (provenance table).
