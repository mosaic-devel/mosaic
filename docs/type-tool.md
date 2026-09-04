# Type Tool — design

> **Scope.** This note settles the design of Mosaic's Type tool — the rich-text data model,
> font/shaping stack, on-canvas editing, the transient "Type panel" popup, and the 3D
> extrusion engine — and picks the text-rendering algorithms. It is the
> source of truth for **S29 (Type core)**, **S30 (Type advanced: warp / fit-to-path /
> rasterize)**, and the **S30-c…/3D sub-sessions**. It is the companion to
> `docs/vector-model.md` (the vector stack this tool plugs into) and follows the same
> discipline: *design from the public-domain technique, bake the technique lineage into the
> source, design resolution-independent and GPU-residency-aware from day one.*
>
> **Deferred advanced features** — hyphenation, spell-checking, and vertical writing-mode — are
> scoped separately in **`docs/type-deferred-features.md`** (each is its own session; S29-c built only
> the foundations).
>
> **Why a whole doc.** A good Type tool is a killer feature; a bad one kills the project. This
> tool is treated generously and planned before a line is written. We will **split it across as
> many sessions as it needs** (user call, 2026-06-25) — we are not landing it in one shot.

---

## 1. What was settled (design conversation, 2026-06-25)

1. **Real 3D, not faux-3D.** 3D text is a genuine extruded **mesh** rendered through Vulkan
   with a perspective camera, vertex normals, a real light model, and a depth buffer — because
   the asks (rotate in *any* direction, controllable lighting, per-face Layer Effects, a mini
   3D scene with draggable handles) are exactly the things 2.5D fakery breaks on. The cost is a
   small, bounded 3D renderer living **beside** the 2D compositor, not inside it (§10).
2. **Per-run styling reaches the *face*, not the *geometry* — the z-fight-free middle ground.**
   The whole text block extrudes as **one watertight solid** with **one shared depth, bevel and
   front-plane** (so there is no z-fighting and no internal seams), while **per-run material**
   (colour / metalness / roughness / face paint) varies across that surface. A run can look like
   gold next to glass; it cannot be at a different depth. Depth & bevel are **per-layer (per-
   object)** properties; material is **per-run, defaulting to inherit the object material** — so
   the simple case shows *zero* extra controls (§8.4, §10.4). This is the "rich but not a fighter-
   jet cockpit" answer the user asked for.
3. **GPU residency from day one (design, not build).** Full GPU residency is a deferred project
   goal (it blocked the vector GPU renderer — `docs/vector-model.md` §5.3). The Type model is
   designed so residency is a *renderer swap, not a model rewrite*: the model stores
   **resolution-independent** data (shaped glyph runs + outlines + extrusion params); pixels are
   a **cache**. The 2D text path can later move to GPU per-pixel coverage (Slug / MSDF) and the
   3D mesh is **inherently** GPU-resident (upload once; rotate/light = matrix & uniform updates,
   no per-frame CPU re-mesh). See §5.4 and §10.5.
4. **Affinity authoring, fixed.** Drag-to-create makes an **Area (frame) text** box of any size
   (text wraps inside it). A plain **click** makes **Point text** at the size in the Text-Size
   control — *not* the size of the last box. A Settings toggle restores Affinity's "reuse the
   last box size" behaviour for those who want it (§7). This directly fixes the annoyance the
   user named.
5. **Editing happens on the canvas, in real time.** You type *into the glyphs on the canvas*,
   with a live caret and selection; the chrome (toolbar, menus, options bar) **must not eat the
   keystrokes**. This needs an explicit text-editing *session* that owns keyboard focus and an
   IME (§6). Mosaic is **not** GIMP's modal text box.
6. **A transient bottom-right "Type panel" popup.** When a text object is selected, a popup
   appears in the **bottom-right** of the canvas — **no comic-book pointer** (it is transient and
   contextual, not anchored to a widget). It carries the character/paragraph controls and, *only
   when 3D is enabled*, a **mini 3D scene** with handles at its top. The hottest few controls live
   on the tool **context bar**; the popup is the "everything" surface (§8).
7. **Per-run rich text** — font / size / weight / italic / strikethrough / underline / colour /
   tracking / baseline can all change *within a selection*, including a different font for part of
   the text (§3). This composes with 3D via decision #2.

### 1.1 Second review round (settled 2026-06-25, same conversation)

8. **A rotating I-beam cursor.** The text-hover cursor turns to the local baseline orientation
   (rotated layer / on-path tangent / warped baseline), reusing the Move-tool rotate-cursor
   machinery and the vendored `xterm.svg` (§6.1).
9. **Fit-to-path gets on-canvas range handles.** Start/end/flip brackets pick *exactly* which span
   of the path/shape the text conforms to (§9).
10. **Fonts resolve cross-platform, never hardcoded.** The OS default family + the OS fallback
    cascade (macOS/Windows/Linux share almost no fonts); the **emoji font is user-selectable in
    Settings** because a machine may have several (§4.2).
11. **Everything reviewed is folded into the sessions, not parked.** All §13 additions are
    **accepted** and distributed across S29/S30 (§2). **Layer Effects on 3D text is a real
    scheduled session (S30-e)**, not "future" (§12).
12. **GPU residency may be pulled forward.** It is still the deferred blocker (vector-model §5.3),
    but as more features feel laggy without it the project may bring it forward; the Type model is
    already residency-shaped (§5.4, §10.5), so that is a renderer swap, not a remodel.

---

## 2. Session breakdown (split deliberately)

The tool is sequenced so every session is independently **shippable and headless-testable**
(the project's verification division: Claude runs the headless harness, the user does visual
verification — `docs/`-recorded). 3D is split out because it is a subsystem, not a feature.

| Session | Scope | Key deliverable |
|---|---|---|
| **S29-a** | Text **model + shaping + render**, headless | `TextLayer` rich-run model (§3); FreeType+HarfBuzz wiring; the **cross-platform `FontDB`** — OS-default family, OS fallback cascade, emoji-font enumeration (§4.2); shape→layout→**Contours** (§5); **colour-glyph** (COLR/CPAL/bitmap) render; CPU rasterize via the S25 vector rasterizer; AA modes incl. off. Golden tests. |
| **S29-b** | **On-canvas editing + authoring UX** | Live caret/selection, the editing **session** + focus + IME hooks + bidi-aware caret (§6); the **rotating I-beam cursor** (§6.1); drag=Area / click=Point + the Settings toggle (§7); `SetTextCommand` coalesced undo. |
| **S29-c** | The **Type panel popup** + context-bar split | Bottom-right transient popup + caret-occlusion flip (§8); character/paragraph sections; the **5 hot controls** on the bar (§8.2); **variable-axis sliders, OpenType-feature toggles, optical/metric kerning, AA-mode** controls; the **emoji-font picker** in Settings (§4.2). |
| **S30** | **Warp/bend + fit-to-path (+ range handles) + rasterize** | Bend handle; **text-on-path with on-canvas start/end/flip brackets** via `samplePathAt` (§9, vector-model §4); right-click Rasterize / Convert-to-path (§11). |
| **S30-c** | **3D engine (geometry + render)** | `Vec3/Vec4/Mat4/Quat` math; Contours→mesh (cap + walls + bevel, one watertight solid); **near-ortho camera + perspective slider**; toggle-gated lighting; depth buffer; the Vulkan 3D pass; rotate-any-axis; per-run material; design-space UVs (§10). |
| **S30-d** | **3D scene UI + transform gizmo + lights** | The mini-scene with handles in the popup; on-canvas 3D transform; light controls behind the toggle (§10.3). |
| **S30-e** | **3D-text Layer-Effects integration** | Per-face effect mapping (front-face design-space eval; optional wrap-to-sides) + wiring 3D text into the Layer-Effects pipeline (§12). Paired with the general Layer-Effects feature. |

> S29-a/-b/-c may merge or split further in practice; the dividing lines (headless model ↔
> interactive editing ↔ panel UI) are the durable ones. Nothing above is parked as "future" — the
> only deliberately-deferred item is stepped-relief 3D (§10.4, §14), and even that is specced.

---

## 3. The text data model

All coordinates are **layer-local** (em / text units), placed into the document by
`layer.transform()` — identical to `RasterLayer` and `VectorLayer` (`docs/vector-model.md` §2).
The current `TextLayer` (`src/core/layer.hpp`) is a one-string stub; S29 replaces its body
(keeping the class and `LayerKind::Text`).

### 3.1 Runs — rich text without a tree

Text is a **flat string of Unicode** plus a list of **style runs** over byte ranges. A flat
run-list (not a paragraph/span tree) is enough for everything Mosaic needs, trivially
round-trips to SVG `<tspan>` / our native format, and keeps editing (split/merge runs on an
edit) simple.

```cpp
// src/core/text/text_model.hpp  (S29-a)
struct FontRef {                         // resolved against the font DB at shape time
    std::string family = "Sans";         // e.g. "Inter", "Noto Serif"
    float       weight = 400;            // CSS-style 1..1000 (variable-font aware, §3.4)
    bool        italic = false;          // synthesised-oblique fallback if the face lacks one
    float       widthAxis = 100;         // OpenType 'wdth' (condensed..expanded), variable fonts
    std::map<std::string,float> variations; // any other OpenType variable axis (tag → value)
};

struct CharStyle {
    FontRef  font;
    float    sizePx       = 24.0f;       // em size in layer units (px at the layer's base scale)
    ColorF   fill         = {0,0,0,1};   // a run's paint is a vec::Paint (solid/gradient) — see §3.3
    bool     underline    = false;
    bool     strikethrough= false;
    float    tracking     = 0.0f;        // extra letter-spacing, 1/1000 em
    float    baselineShift= 0.0f;        // super/subscript, px
    std::vector<std::string> features;   // enabled OpenType features ("liga","smcp","ss01"…) §3.4
};

struct StyleRun { std::size_t begin, end; CharStyle style; };  // half-open byte range into text

struct Paragraph {                       // paragraph-level (between '\n's)
    enum class Align { Left, Center, Right, Justify } align = Align::Left;
    float leading      = 1.2f;           // line height, ×em (or absolute if leadingAbsolute)
    bool  leadingAbsolute = false;
    float spaceBefore = 0, spaceAfter = 0;
    float indentFirst = 0, indentLeft = 0, indentRight = 0;
    enum class Direction { Auto, LTR, RTL } direction = Direction::Auto;  // bidi base dir
};
```

### 3.2 The block

```cpp
enum class TextFrame { Point, Area };    // §7 — click = Point (grows), drag = Area (wraps)

struct TextBlock {
    std::string             utf8;        // the text (one logical string; '\n' splits paragraphs)
    std::vector<StyleRun>   runs;        // cover [0, utf8.size); split/merged on edit
    std::vector<Paragraph>  paragraphs;  // one per '\n'-delimited paragraph (parallel to splits)
    TextFrame frame = TextFrame::Point;
    Vec2      areaSize = {0,0};          // Area only: the wrapping box (Point ignores)
    AntiAlias aa = AntiAlias::Grayscale; // §4.3
    // 3D (S30-c) — std::nullopt ⇒ a flat 2D text layer (the default).
    std::optional<Extrude> extrude;      // §10.1
};
```

`TextLayer` holds **one** `TextBlock` (mirroring "one object per vector layer"), plus the
caches below. A run-list invariant helper keeps `runs` covering and non-overlapping after every
edit (the model's single most error-prone bit — it gets its own unit tests).

### 3.3 Per-run paint reuses the vector `Paint`

A run's colour is **not** just `ColorF` — it is a `vec::Paint` (`docs/vector-model.md` §2.3:
`NoPaint` / `SolidPaint` / `Gradient`). So a run can be gradient-filled the same way a shape is,
"convert to path" preserves it exactly, and a future per-run stroke is the vector `Stroke`. The
`fill` field above is the common-case shorthand; the model stores `vec::Paint`.

### 3.4 OpenType features & variable fonts (the "pro" surface)

HarfBuzz exposes both for free, and they are what separates a *good* type tool from a toy — so
they are in the model from day one, behind progressive disclosure (§8.3):

- **Variable axes** (`wght`, `wdth`, `opsz`, custom) — continuous weight/width, not a bold
  toggle. `FontRef::weight/widthAxis/variations` carry them; the panel shows a slider per axis
  the selected face actually exposes.
- **OpenType features** — ligatures, small caps, oldstyle/lining figures, fractions, stylistic
  sets (`ss01`…), contextual alternates. `CharStyle::features` is a curated, per-run on/off set.

These are *data only* in S29-a (shaping honours them); the UI to toggle them is S29-c, tucked in
the panel's advanced strip so the simple case never sees them.

---

## 4. Fonts & shaping — FreeType + HarfBuzz

Both are in the PLAN's dependency list (§6.2) but **not yet wired into CMake**; S29-a adds them.

### 4.1 Licensing (clean for GPLv3)

| Lib | Role | License | GPLv3-compatible? |
|---|---|---|---|
| **FreeType** | font rasterization / outline access | FTL **or** GPLv2+ (dual) | Yes — take the **GPLv2+** option, GPLv3-compatible |
| **HarfBuzz** | complex-script shaping | MIT (Old) | Yes |
| **fontconfig** (Linux) | system font enumeration / fallback | MIT-like | Yes |

Mosaic is GPLv3 (memory: GPLv3 image editor), so FreeType's **GPL arm** is taken — no FTL
attribution-clause friction. Record in `docs/third-party-licenses.md` at wire-up.

### 4.2 The pipeline

```
TextBlock ──HarfBuzz──▶ shaped runs ──FreeType──▶ glyph outlines ──flatten──▶ Contours ──▶ raster
          (per StyleRun)  (glyph ids,            (per glyph id)    (§5)        (vector-model
                           positions, clusters)                                 rasterizer, S25)
```

- **Shaping** runs **per `StyleRun`** (one HarfBuzz buffer per run, since font/features change at
  run boundaries), then runs are **bidi-reordered** and line-broken into a layout. HarfBuzz gives
  RTL, CJK, Indic, ligatures, mark positioning — the things a hand-rolled layout gets wrong.
  Vertical (CJK) text is a future axis; the model's `Paragraph::direction` reserves the hook.
- **Line breaking** — UAX #14 line-break opportunities (a small classifier table, or ICU if we
  later add it) for Area text; Point text never wraps. Justify distributes space at break points.
- **Font enumeration & fallback (cross-platform, no hardcoded names).** A `platform/FontDB`
  interface (isolated like the rest of `platform/`) backed by **fontconfig** (Linux), **CoreText**
  (macOS), **DirectWrite** (Windows). It resolves four things: *enumerate* installed families;
  *resolve* a family by name; the **OS default UI/sans family** (San Francisco / Segoe UI /
  whatever `sans-serif` maps to — *never* a baked-in name, since the three platforms share almost
  no fonts); and a **fallback font for a missing codepoint**, delegating to the OS fallback cascade
  (CoreText and DirectWrite both do this well; fontconfig query on Linux). So the default font and
  the fallback chain are *whatever the machine actually has* — if Noto is installed fontconfig will
  reach for it, but we neither ship nor assume it.
- **Colour glyphs / emoji.** FreeType renders COLR/CPAL layered glyphs and embedded-bitmap
  (CBDT/sbix) emoji; the run model already lets a glyph carry colour, so emoji render in S29 rather
  than waiting. Because a machine may have **several** emoji fonts (the OS emoji font, Noto Color
  Emoji, Twemoji…), the **emoji font is user-selectable in Settings** (`Type ▸ Emoji font`, default
  = the OS emoji font; home recorded in the Type/Fonts settings group per the settings-coherence
  rule). `FontDB` enumerates emoji-capable faces for that picker.

### 4.3 Anti-aliasing modes (incl. **off**)

`AntiAlias { None, Grayscale, Subpixel }`:

- **None** — bilevel, hard edges. The user explicitly wants this (pixel-art / retro). Trivial:
  threshold the coverage at the rasterizer.
- **Grayscale** — analytic coverage AA (the S25 vector rasterizer already does this — FreeType
  smooth / AGG signed-area lineage, public-domain). **Default.**
- **Subpixel (LCD)** — RGB-stripe coverage. Caveated: it only works on an opaque, axis-aligned,
  unrotated, fully-resolved background, so it is **auto-disabled** when the layer is rotated,
  semi-transparent, 3D, or composited over transparency, silently degrading to Grayscale. Offered,
  but not the default, precisely because those caveats bite in a layered editor.

---

## 5. Rendering lane & IP

### 5.1 The seam — text emits the same `Contours` everything else does

This is the load-bearing reuse (`docs/vector-model.md` §1.3, §4): a shaped, positioned glyph is a
set of closed outlines; `flatten()` turns them into `Contours`; **fill, stroke, hit-test, bounds,
mask coverage, SVG export, 3D extrude, and convert-to-path all consume `Contours`.** So Type adds
*no new geometry primitive* — it adds a producer of the existing one. Convert-to-path (§11) is
"emit the Contours as a `vec::Path`"; 3D extrude (§10) is "give the Contours a back face and walls."

### 5.2 The conservative lane (build first)

CPU: HarfBuzz shape → FreeType outline → `flatten` → the **S25 CPU vector rasterizer** (analytic
AA; solid + gradient fill; per-run paint). Every piece is classic/public-domain and already
built. This is the S29-a floor — robust, deterministic, golden-testable.

### 5.3 Slug / MSDF — the GPU-resident quality upgrade

- **Slug — per-pixel Bézier coverage — DEDICATED TO THE PUBLIC DOMAIN (now usable).** Eric
  Lengyel / Terathon irrevocably dedicated it to the public domain effective **2026-03-17**.
  Reference vertex/pixel shaders are released **MIT**; independent reimplementations exist
  (`mightycow/Sluggish`, `diffusionstudio/slug-webgpu` (WebGPU), HarfBuzz-GPU). ⚠ The **Slug
  *Library* remains a commercial product** — we do **not** use it; we implement the **free
  algorithm** from the public-domain shaders / paper. Gives atlas-free, resolution-independent,
  crisp-at-any-zoom text directly from outlines — ideal for the GPU-resident renderer and the
  S30-b Vector document type.
- **MSDF — Green 2007 (Valve), msdfgen (MIT).** ⚠ Design from **Green 2007's basic method**, not
  from one of the recent narrow "SDF-of-text" pipelines (`docs/vector-model.md` §5.1). The atlas
  alternative to Slug.

**Plan:** ship the conservative CPU lane (S29-a); adopt **Slug** for the GPU-resident text path
when the compositor goes GPU-resident (S60). The data model is identical for both — only the
rasterize backend swaps.

### 5.4 GPU-residency-aware design (the explicit ask)

The model separates **what to draw** (resolution-independent) from **drawn pixels** (a cache),
exactly so residency is a later renderer swap:

- **`ShapedBlock` cache** — the result of HarfBuzz+layout (glyph ids, positions, per-glyph run
  link). Invalidated only when text/runs/area/font change — **not** on zoom, move, recolour, or
  re-light. This is the unit a GPU renderer uploads once.
- **Glyph outlines** are control points, uploadable as an SSBO (the Slug/Loop-Blinn input) — no
  per-frame CPU flatten. The CPU `flatten` lane stays for hit-test / bounds / export / bring-up.
- **Crisp at any zoom** — because the source is outlines, the renderer re-evaluates coverage at
  *device* resolution. In a Raster document the text layer composites into the pixel grid (zoom
  magnifies pixels — correct there); in a **Vector document (S30-b)** it re-renders at view
  resolution. The model already supports both (`docs/vector-model.md` §1, S30-b).

### 5.5 Source-header lineage convention

Per the project precedent, every text/3D renderer file carries a short lineage header:

```
// Text rendering. Technique lineage (see docs/type-tool.md §5):
//   - Shaping: HarfBuzz (MIT). Outlines: FreeType (GPLv2+ arm).
//   - Coverage (CPU): analytic scanline (FreeType smooth / AGG — classic, public).
//   - Coverage (GPU, optional): Slug per-pixel Bézier — DEDICATED TO PUBLIC DOMAIN
//     2026-03-17 (MIT reference shaders). NOT the commercial Slug Library.
//   - SDF (optional): Green 2007 basic method (msdfgen, MIT).
```

---

## 6. On-canvas editing — the input model

The hard requirement: **type into the glyphs on the canvas, live, and do not let the chrome eat
keys.** FLTK sends keystrokes to the focused widget; the menu bar, options bar, and popup are all
focusable widgets. So:

- **A `TextEditSession`** is created when the Type tool enters edit on a block. While it lives,
  the **canvas sub-window takes and holds keyboard focus** (`Fl::focus(canvas)`), and the session
  consumes `FL_KEYBOARD` / `FL_PASTE` / IME events. The popup's own text fields (e.g. the font
  size box) explicitly **return focus to the canvas** on commit so they never steal the typing
  stream. Menu accelerators that would collide (e.g. plain letters) are suppressed while a session
  is active — only modified accelerators (⌘/Ctrl-…) pass through.
- **IME / dead keys / CJK input** via FLTK's compose/marked-text path (`Fl::compose()` +
  `FL_KEYBOARD` `event_text()`), with the candidate window pinned to the caret (§6.2); the session
  shows the pre-edit (composition) string inline. *Non-negotiable* for a world-class type tool and
  designed in now even if a basic build lands ASCII-first.
- **Caret & selection** are model positions (byte offsets) rendered from the layout: a blinking
  caret (timer), shift-arrow / drag selection, word/line double/triple-click, home/end, and
  **bidi-aware** caret movement (visual vs logical order). Selection spanning mixed runs is what
  makes "set part of the text to another font" work — the panel edits the `CharStyle` of the
  selected range, splitting runs at the boundaries.
- **Undo coalescing** — typing coalesces into pause/word-bounded `SetTextCommand`s (not one per
  keystroke), mirroring the vector tools' coalesced `SetVectorObjectCommand`. Style changes from
  the panel are their own coalesced commands. The whole block is the command's footprint (small;
  S36-b selective-undo friendly).
- **Cursor** — an I-beam over editable text, the type-create cursor otherwise (`ui/cursors`).

Enter commits a newline; **Esc / click-away / tool-switch commits the block** (text editing is
*not* destructive-on-cancel — an empty block is discarded). The block stays a live `TextLayer`
afterward; re-selecting with the Type tool re-enters editing (the "select-to-edit" pattern the
Shape tool already uses — `core::topmostVectorLayerAt`, extended to text).

### 6.1 The rotating I-beam cursor

The text-hover cursor **rotates to the local baseline orientation** — upright for flat text, turned
for a rotated layer, **tangent to the curve** for text-on-path, following the warped baseline for
bent text — so the insertion preview always aligns with the text it will edit. It reuses the
Move-tool rotate-cursor machinery wholesale (`ui/cursors.cpp` `rotateCursor` / `applyRotateCursor`):

- **Art:** the vendored **`third_party/apple_cursor/svg/xterm.svg`**, embedded as a generated
  asset. It already follows the project's cursor convention — **`#00FF00` (green) = the I-beam
  fill, `#0000FF` (blue) = the outline** — *the same placeholders the rotate cursor recolours*, so
  the two-tone theming is the identical two `replaceAll`s (blue → black/white outline, green →
  white/black inner by `darkMode`), giving a cursor that reads on any canvas.
- **Rotation:** baked into the SVG (a `rotate(deg cx cy)` on the art group) and **bucketed**
  (≈64 steps, like the rotate cursor) so the image only re-rasterizes when the orientation or theme
  changes. The angle source is the local baseline tangent (layer transform / `samplePathAt` tangent
  / warp tangent), not the Move tool's box geometry.
- **Two quirks carried over** (from the rotate-cursor code): nanosvg mis-parses **commas** in a
  transform's argument list, so the baked args are **space-separated**; and `xterm.svg`'s outer
  group is a `clip-path` group whose 256-unit rect would **crop the I-beam ends** once rotated ~45°,
  so the rotation is applied to an **unclipped** wrapper (or the clip is stripped) rather than the
  first `<g>` blindly.

A `ui::textCursor(angleRad, dark, scale)` mirrors `rotateCursor`; the canvas caches + buckets it
exactly as `applyRotateCursor` does, with the hotspot at the I-beam centre.

### 6.2 IME activation — off except while editing (Wayland pitfall, verified 2026-06-25)

Field testing (KDE Plasma, **native Wayland**, Fcitx5 "experimental Wayland runner") confirmed the
trap the §13 risk warned about, and it is a **current bug, independent of the Type tool**: FLTK
enables the Wayland text-input on whatever window holds keyboard focus, so the instant the canvas
`take_focus()`es (it does on every click, to receive Space/R — `vulkan_canvas.cpp` FL_PUSH) it
becomes an **active text-input client**. With no spot set, the Fcitx candidate window appears at the
surface origin **(0,0)**; worse, Fcitx then **intercepts plain keys** (Space, `r`…) as
composition/commit input, so the canvas shortcuts (space-to-pan, r-to-rotate) break. Selecting a
passthrough engine hides the popup but the surface stays text-input-*enabled*, so the keys are still
routed away and the shortcuts stay broken. Real `Fl_Input` fields (Crop W:H, hex, rename) instead
place the IME correctly — FLTK calls `fl_set_spot` for them — proving the infrastructure works and
the canvas is simply opting in wrongly.

**The rule:** the canvas is **not** a text-input client except while a `TextEditSession` is live.
FLTK gives the exact controls (`/usr/include/FL`):

- `Fl::disable_im()` / `Fl::enable_im()` — toggle the seat's text-input. Keep IME **disabled** while
  the canvas has focus and no edit session is active; enable it when an edit session starts.
- `fl_set_spot(font, size, x, y, w, h, win)` / `fl_reset_spot()` — pin the candidate/preedit window
  to the **caret** (canvas-window coords) while editing and on every caret move. *(Note:
  `Fl::insertion_point_location` is `__APPLE__`-only in 1.4 — Linux uses `fl_set_spot`.)*

Session flow: enter → `enable_im()` + `fl_set_spot(caret)`; caret move → `fl_set_spot(caret)`;
commit/Esc/blur → `fl_reset_spot()` + `disable_im()`. **Regression to guard:** the toggles look
global, so the gating must be focus-scoped so that disabling IME for the canvas does not also kill
it for the working `Fl_Input` fields. This is empirical FLTK-per-focus behaviour → an **S29-b
spike**, not an assumption. (This canvas/IME conflict can also be fixed *now*, ahead of S29, since
it breaks Space/R under Fcitx-Wayland today.)

**Env / platform note.** Xwayland showed *no* IME anywhere (no canvas hijack, but also none in the
Crop boxes): the Wayland runner serves only the Wayland text-input frontend, so X11/Xwayland clients
(FLTK's X11/XIM backend) need Fcitx's **XIM** frontend with `XMODIFIERS=@im=fcitx`. So **native
Wayland is the primary IME target**; X11/XIM works once the env is set. Per the verification
division, this GUI-side checking is the user's; Claude stays on the headless harness.

---

## 7. Authoring — Point vs Area, the Affinity fix

The user's annoyance ("later text is stuck at the last box's size") is the classic **Point vs
Area** distinction, made explicit:

- **Click** (no drag) → **Point text**: an insertion caret at the click, at the **Text-Size
  control's** current value. The box **grows with the text**; no wrapping. *This is the fix* —
  click-created text always uses the slider, never the last box.
- **Drag** → **Area text**: a wrapping frame of the dragged size; text reflows inside it; the
  frame is resizable later via the selection handles. Use any size you like.

**Settings toggle** (`Type ▸ "New click text reuses the last text box size"`, default **off** —
Mosaic behaviour) restores Affinity's "inherit the last size" for users who prefer it. Per the
settings-dialog-coherence note, its **home is the Type/Tools settings group** (recorded as the
toggle's home so it doesn't become an orphan). Converting Point↔Area is a panel/right-click action
(Point→Area wraps the grown bounds; Area→Point drops the frame).

A draft outline + baseline preview shows during the drag/hover (reusing the shape-draft overlay
mechanism); commit creates the `TextLayer` and opens a `TextEditSession` immediately so you can
just start typing.

---

## 8. The Type panel (bottom-right transient popup) + context-bar split

### 8.1 Shape & behaviour

A **transient popup** appears in the **bottom-right of the canvas** whenever a text object is
selected/edited, and dismisses when it is deselected or the tool changes. It reuses the
`ui::Popover` child-sub-window host (real sub-window → takes focus, draws above the Vulkan
canvas, themable) but with **no comic-book bubble** (`enableBubble` *not* called — it is not
anchored to a widget, so a pointer would be meaningless) and a **new bottom-right placement**
mode added to `Popover::place()` (today it does right-of-anchor and below-anchor; this adds
"pinned to a canvas corner"). It **flips to bottom-left** if the caret/selection is itself in the
bottom-right region, so it never occludes what you are typing (a small but real polish).

> **Reconciliation with the PLAN.** PLAN §S11-b promises a *Character/Paragraph* panel reached via
> the options bar's **"More…"** bridge. **This popup *is* that panel**, relocated to the canvas
> corner per the user's wish. It is now labelled **"Style…"** on the bar (the "More…" name is gone);
> 3D lives in its **own** popup off a sibling **"3D…"** button, not inside this one. The context bar
> still carries the hot controls and the "Style…" button that summons the panel; we do not build two
> Character/Paragraph panels.

### 8.2 Context bar vs popup — who owns what

Progressive disclosure is the antidote to the "fighter-jet cockpit" the user fears. The split
(revised S29-c rev 1/2 — the bar was trimmed once the panel existed; Bold/Italic/Align read better
in the panel than as cramped bar letters):

- **Tool context bar (always visible, just the two hottest + two buttons):** Font family and Text
  size, then a **"Style…"** button (opens the Type panel) and a reserved **"3D…"** button (opens the
  separate 3D popup, §8.4). AA mode stays in the model (panel-bound, R4). The paint swatch (reusing
  the shape tools' `PaintSwatch`, §7.2 of vector-model) is deferred. *Bold/Italic/Alignment moved
  off the bar into the panel.*
- **Type panel popup (the Character + Paragraph "everything" surface), grouped sections:**
  - **Character** — family, style/weight (variable-axis sliders when the face has them), size,
    tracking, leading, baseline shift, underline/strike, the run paint (solid/gradient via the
    vector paint editor), and an **advanced strip** for OpenType features / variable axes.
  - **Paragraph** — alignment incl. justify, indents, space before/after, direction.
  - *3D is **not** a section here — it is its own popup off the bar's "3D…" button (§8.4).*

Every control edits *the current selection's* `CharStyle`/`Paragraph` (or the block defaults when
nothing is selected — the same dual-state "defaults vs selection" the Shape options bar uses,
keyed by an edit-target id). Edits land as coalesced `SetTextCommand`s and reflect back live.

### 8.3 Keeping it calm

- The panel is **sectioned and scrollable**, not one tall wall. Character / Paragraph are sections;
  the advanced OpenType strip is collapsed by default.
- 3D is a **separate popup** off the bar's "3D…" button (§8.4) — the 2D user never sees a 3D control,
  and the Character/Paragraph panel stays compact instead of carrying a whole 3D scene.
- One soft default light means 3D "just works" with zero light-fiddling; the multi-light controls
  hide behind the lighting toggle (§10.3). *Sensible defaults beat exposed knobs.*

### 8.4 The 3D scene popup (off the bar's "3D…" button)

3D gets its **own** popup (revised S29-c rev 9 — the Character/Paragraph panel is already large, so
the live 3D scene does not belong inside it). The bar reserves a **"3D…"** button beside "Style…"
(built greyed until the 3D editor lands, S30-c/-d); clicking it opens a separate popover showing a
small live **3D viewport** of the text object with on-handle manipulation:

- **Orbit handles / a trackball gizmo** rotate the object in any axis (drag = orbit; the standard
  three ring-handles for constrained X/Y/Z).
- **A depth handle** (drag the front face toward/away) sets extrusion depth; a **bevel handle**
  sets the bevel size/profile.
- **A light gizmo** (a draggable light-direction widget on a sphere) when lighting is enabled.

It renders through the same 3D pass as the canvas (§10), just to an offscreen target shown in the
panel — so what you sculpt is exactly what composites. The full **on-canvas 3D transform gizmo**
(S30-d) mirrors these for direct manipulation on the document.

---

## 9. Warp / bend + fit-to-path (S30)

Both reuse the vector arc-length sampler (`docs/vector-model.md` §4, `samplePathAt`):

- **Bend handle** — a single on-canvas handle warps the baseline along a smooth arc (Affinity's
  text-warp); glyphs are placed by **position + tangent** along the warped baseline. The warp is a
  baseline transform, so it composes with per-run styling untouched.
  - ⚠ **The arc's anchor depends on the frame** *(2026-07-14)*. **Point**: the text's own advance
    span, anchored at the first baseline (the original behaviour, golden-pinned). **Area**: the
    **FRAME drives** — the reference arc is the box's *top edge* (`x0=0, y0=0`, length =
    `areaSize.x`) and every line rides a parallel arc at its own depth, so the whole box warps as
    one annular sector, two type sizes in the same box bend **identically**, and the frame chrome
    (top edge, bowed bottom, radial sides) samples the same family. Before this, the arc followed
    the text span/first baseline even for Area — the user's report read "only the bottom line of
    the box conforms to the bend… as if it conforms to the text size instead of the Area box" — and
    the chrome bowed *only* the bottom edge, on its *own* circle. The perpendicular reference is
    `bentArc.baseY`, read by placement and edit chrome alike (`CurveSampler::refY`) — one rule, one
    home. Deep boxes under strong bends still pinch at the arc centre (`clampBendOffset`), which is
    where the warped lines themselves collide. **The overset clip is the warped sector too**
    *(same day)*: `BentArc::sectorContains` (pointAt's exact inverse) cuts per pixel against the
    box the user sees, `bentSectorBounds` clamps the cache extent — the flat-rect clamp used to
    shear the arch flat. Unbent frames keep the rect clamp byte-identically.
- **Fit-to-path** — dragging a text object onto a vector/path layer flows it along that path. The
  baseline is a **`LayerId` reference** to the path (not a copy), so moving/editing the path
  **re-flows the text non-destructively**. Because *any* `Geometry` flattens to `Contours`,
  text-on-a-circle / on-a-star / on-a-custom-path are the **same feature** for free.
- **On-canvas range handles** — the conforming text is bounded by **two brackets that slide along
  the path** (a *start* and an *end*), plus a **centre/flip bracket** that drags the run along the
  path and **flips it to the other side** of the curve. Each bracket is just an **arc-distance `s`**
  along the flattened `Contours`; `samplePathAt(s)` gives the position+tangent to draw it and to lay
  the glyphs out within `[sStart, sEnd]`. So you choose *exactly* which span of the path/shape the
  text rides — the whole ellipse, or just its top arc — by dragging, not guessing. This is the
  Illustrator/Affinity text-on-path bracket model, and it reuses the §6.1 rotating cursor (each
  bracket's tangent is already in hand).

Warp + path are **2D baseline transforms**; with 3D enabled the warped/path-fitted baseline is the
input to the extrude (the solid follows the curved baseline). *(As built 2026-07-07: this
composition SHIPPED — the warped glyph outlines feed the mesher directly, and the editing chrome
composes warp-then-plane-projection, so bend and fit-to-path both work on extruded blocks. The
fit-to-path entry is a Type-tool click near a path's spine; overflow on an open path extends
straight past its ends, and a closed single contour wraps.)*

### 9.1 The bent block's chrome is ONE geometry *(2026-07-29)*

Two handle defects, one root: pieces of the box chrome were still expressed against the **flat,
unbent** box while the glyphs and the drawn frame went through bend → 3D projection. The rule the
fix installs is that **every** piece of bent chrome goes through one named mapping,
`ShapedBlock::BentArc::warp(flat)` — the arc point at the flat x's distance along the arc, stepped
down the local normal by the flat depth, i.e. *exactly* what `applyBend` places each glyph with, and
`sectorContains`'s exact inverse. It is the **identity, bitwise**, for a straight or absent arc, so
unbent text is untouched.

- **The bend handle now turns with the block.** Its pill hung `kTextBendDrop` **screen-down** from
  the bar's apex (`outScreen.y += …`), so a rotated layer — or a rotated *view* — turned the bar out
  from under a handle that stayed bolt upright, and the drawn stem pointed at a place that was not on
  the bar. The drop direction now comes from the **bar's own local normal**, carried through the same
  world-transform + cap-projection funnel the bar itself rode, so layer rotation, view rotation,
  mirroring (which genuinely flips which screen side is "under the baseline") and the 3D cap all come
  along; only the *length* stays a screen constant, because the handle is a UI affordance. The apex
  is now **returned** by `textBendHandle` instead of being re-derived by the caller as
  `pill − (0, drop)`, which was the same screen-vertical assumption in a second place.
  ⚠ The pill's *art* (the rounded box and its ↕ glyph) is still drawn screen-upright by
  `canvas_present.comp`, like a cursor; only its placement conforms.
- **`textRotateCorners` tests BENT before 3D.** It tested `extrude` first and anchored the band to
  an **axis-aligned** extent (the rendered ink's bbox), so a block that was *both* extruded and bent
  got an AABB around an arch — corners in empty space diagonally off the arc ends, which is the very
  defect round 3 had already fixed for flat bent text. A bent block now takes its hotspots from the
  bent box's corners whether or not it is extruded, and for a 3D block those corners are projected
  onto the visible cap through the same `ExtrudePlaneMap` the solid renders with. A **non-bent** 3D
  block keeps the ink-bounds anchor (the 2026-07-14 round-2 answer) unchanged.
- **A bent Area block's box corners are warped too**, in `textEditBoxCorners` — so the resize handle,
  the Move band, the rotate hotspots and the drawn frame are one geometry rather than two expressions
  to keep in step. Move/Resize hit-testing for a bent Area frame mirrors the bent-Point bar's
  (`hitBentAreaFrame` / `hitBentPointBar`): the band is measured off the **polyline that is drawn**,
  so a strong bend can no longer put the grabbable edge a whole sagitta from the arch.
  ⚠ **Point text is deliberately not warped here.** Its box is `contentBounds`, and `applyBend`
  has already written the *warped* bbox there (`layoutBounds` returns `ShapedBlock::bounds`), so a
  second warp would double-count; `textRotateCorners` owns that case, unchanged apart from also
  projecting through the cap now.

Still flat by design, and unchanged: the hover I-beam over a **non-edited** 3D layer uses flat
`contentBounds`, and the box **move/rotate gesture math** is flat-frame — the 2D box is what places
the text (§10.3).

---

## 10. The 3D engine (S30-c/-d)

A real, bounded 3D renderer beside the 2D compositor. The text layer renders to an **offscreen
RGBA target** and then **composites as an ordinary layer** — so the 2D compositor stays 2D and
3D text drop-shadows/blends/masks like anything else.

### 10.1 The extrude parameters

```cpp
// src/core/text/extrude.hpp  (S30-c)  — std::optional<Extrude> on the block (§3.2)
struct Bevel { enum class Profile { Flat, Round, Convex, Concave } profile = Profile::Round;
               float size = 0.0f; int segments = 3; };
struct Material { ColorF albedo{0.8f,0.8f,0.8f,1}; float metalness=0, roughness=0.5f;
                  /* later: emissive, a paint/texture face fill */ };
struct Light { Vec3 direction; ColorF color{1,1,1,1}; float intensity=1.0f; };

struct Extrude {
    float depth = 20.0f;                 // shared by the whole block (no per-run depth — §1.2)
    Bevel bevelFront, bevelBack;         // independent front/back bevels
    Material material;                   // per-layer default; runs may override (§10.4)
    // Orientation: a quaternion (any-axis, gimbal-free) + perspective camera.
    Quat  orientation = Quat::identity();
    float perspective = 10.0f;           // vertical FOV; small ≈ near-ortho default; →0 = true ortho
    bool  lightingEnabled = true;        // the toggle; off ⇒ flat self-lit faces
    std::vector<Light> lights = { /* one soft key light by default */ };
    ColorF ambient{0.25f,0.25f,0.25f,1};
};
```

This is the **only** new math the model needs: `Vec3/Vec4/Mat4` + a `Quat` (added to
`common/geometry.hpp` in S30-c; today it has only `Vec2/Affine2D/ColorF`). Orientation is a
**quaternion** so "rotate in any direction" is genuinely gimbal-free.

### 10.2 Mesh generation (z-fight-free by construction)

From the block's `Contours` (the §5 seam):

1. **Front cap** — triangulate the (possibly holed, even-odd) contours with **earcut/libtess2**
   (already vendored for the vector fill, `docs/vector-model.md` §5.1), at `z = +depth/2`.
2. **Back cap** — the same triangulation mirrored to `z = -depth/2`.
3. **Side walls** — quad strips connecting front to back along every contour edge, with proper
   normals (sharp at corners, optionally smoothed along curves).
4. **Bevels** — extra ringed segments at the front/back edges per `Bevel::profile`.

Because **all runs share one depth and one front plane**, this is **one watertight solid** with
**no coplanar overlaps → no z-fighting** (the user's hard "z-fighting is bad" constraint, designed
out rather than patched). The mesh is regenerated only when text/contours/depth/bevel change —
**not** on rotate, light, or recolour (those are matrix/uniform updates), which is what makes it
GPU-resident-friendly (§10.5).

### 10.3 Camera, lighting, transform

- **Camera** — a perspective camera (FOV from `Extrude::perspective`). The **default is
  near-orthographic** (a small FOV — calm, graphic, distortion-free, right for logos/UI), with the
  FOV exposed as a **perspective slider** for drama on demand; `→0` is true ortho. Model =
  `orientation` quaternion → `Mat4` (settled 2026-06-25).
- **Lighting (gated by the toggle)** — off ⇒ faces are flat-shaded with their material albedo
  (clean, fast, predictable). On ⇒ a small **Blinn-Phong / PBR-lite** model (albedo + metalness +
  roughness + N lights + ambient). Blinn-Phong and basic PBR are **textbook public-domain**. One
  soft key light by default so enabling 3D looks good immediately.
- **Transform** — the on-canvas 3D gizmo (S30-d) writes `orientation` (+ the layer's 2D
  `transform()` still positions/scales the whole thing in the document). Rotation is any-axis via
  the quaternion; there is no separate "3D transform vs 2D transform" mode confusion — 2D
  transform places the *rendered result*, the gizmo orients the *solid*.

### 10.4 Per-run material — the middle ground, concretely

Per-run material is a sparse **override map**: `std::map<run-index, Material>` on the `Extrude`,
empty by default. A run with no entry inherits `Extrude::material`. So:

- The simple case (one material for the whole word "TITLE") shows **one** material control and
  produces one mesh with one draw.
- The rich case (gold "SALE", glass rest) assigns materials to runs; the mesh is **partitioned
  into per-material index ranges** of the *same* vertex buffer and drawn in a few passes — **no
  geometry change, no z-fighting, no seams** (the faces are coplanar and contiguous; only the
  shader material differs across them).

This gives "as rich as possible" without the multi-depth mesh, the seam walls, or the per-run
depth UI that would have produced both z-fighting and the fighter-jet cockpit. *(A future "stepped
relief" mode — runs at different depths joined by real connecting walls — is noted as a deferred
possibility, explicitly out of scope to keep the solid watertight.)*

### 10.4b Decorations extrude too (fix, 2026-08-28)

Underline and strikethrough are **solids of their own**, at the block's shared depth and bevel and
tagged with their run, so they take that run's material like everything else. Before this the 3D
lane skipped the decoration bars entirely — the "U" and "S" toggles were silently inert the moment
a block was extruded, which read as the toggles being broken rather than as the feature being
absent. A bar is a rectangle, and a rectangle is exactly what the mesher already handles for a
glyph outline, so the two lanes now compute the bars through one shared walk
(`forEachDecorationBar` in `text_render.cpp`) and can no longer disagree about where they go. Bars
that intersect a descender fuse into one solid — which is what a real underlined letterform does.

The bent-block exclusion is unchanged in both lanes: a flat bar across an arc would cut through it,
and a swept quad strip is a later refinement.

Separately, the **metrics** behind those bars were partly guesswork. `underline_position` is now
honoured only when the face actually states one — a `post` table with a zero there is an unfilled
field, not a request for a bar on the baseline, and taking it literally drew the underline through
the glyph bottoms on the faces that leave it unset. And strikeout now comes from the OS/2 table's
`yStrikeoutPosition` / `yStrikeoutSize`, which FreeType does not surface on `FT_FaceRec` the way it
surfaces the `post` underline pair; before this it was a fixed 0.26 em guess, which sits wrong on
any face with an unusual x-height.

### 10.5 GPU residency (designed-in)

- The **mesh lives in a GPU vertex/index buffer**, uploaded once and re-uploaded only on a
  geometry-affecting edit. Rotate / light / recolour = **uniform updates**, zero re-upload — this
  is inherently the residency model the project is heading toward.
- The 3D pass writes to an offscreen RGBA+depth target that the compositor samples as a layer.
  When the compositor becomes GPU-resident (S60), this target is **already a GPU texture** — no
  readback. The CPU mesh path (for headless golden tests / export) stays as the bring-up/test lane,
  the same dual-lane discipline as the 2D renderer.

### 10.6 The same 3D on shapes (2026-08-28)

None of §10 is typographic: the mesher's input is `vec::Contours`, the §5.1 seam. So a **shape**
(and a pen path) extrudes through the identical mesher, render lanes, camera, materials,
reflections and popup — see **docs/vector-model.md §11**, which covers the one shape-specific
question (which solids an object contributes) and the mesh cache a cache-less vector layer needs.
The `Extrude` type stays here; the "3D…" button and `ui::Type3dPanel` now serve both tools.

---

## 11. Rasterize / convert-to-path / export

- **Rasterize** (right-click, S30) — render the block (2D or 3D) to pixels and replace the
  `TextLayer` with a `RasterLayer`. Destructive; the warned, baked form.
- **Convert to path** (S30) — emit the shaped outlines as a `vec::Path` (`VectorLayer`),
  preserving per-run paint (§3.3). Loses editability of the *text*; keeps the *vector* geometry.
  3D has no 2D-path equivalent (it would convert the flat front face only — offered with a clear
  warning).
- **SVG / native export** — runs map to `<text>`/`<tspan>` (paint, features, variations carry as
  CSS/`font-variation-settings`); the **native `.mosaic` format (S48)** stores the full editable
  `TextBlock` so text round-trips losslessly (and so a Vector document, S30-b, keeps it live). 3D
  exports as rasterized pixels in SVG (no SVG 3D) but keeps `Extrude` natively.

---

## 12. Layer Effects on 3D text — scheduled (S30-e)

"Layer Effects" (Affinity's *Layer ▸ Effects…*: gradient overlay, stroke, drop shadow, glow,
bevel…) is a general, cross-cutting feature owned by its own future session. **The 3D-text *
integration* with it is not "future" — it is scheduled as S30-e** (user call 2026-06-25), paired
with whenever the general Layer-Effects panel lands; S30-e cannot ship before that prerequisite,
but it is specced and committed here, not parked. The user's worry: a Gradient Overlay must **not**
lazily smear over the whole rendered 3D pixel rectangle. The contract this doc fixes now, so the 3D
model already carries what effects will need:

- **Effects are defined in the glyph "design space" (2D UV), then mapped onto faces.** A gradient
  overlay is evaluated in the text's *unextruded 2D coordinate frame* and applied to the **front
  face** by default (the face the user "sees" the design on); the side walls/bevel inherit a
  shaded continuation, **not** a re-projected copy of the gradient (which is the "looks insanely
  weird" outcome the user named). An *optional* "wrap onto sides" mode exists for users who want
  it, off by default.
- **The mesh therefore carries per-vertex UVs in design space** (cheap to generate during §10.2)
  so any future effect has a sane domain to sample — this is the one thing 3D must bake in now to
  not block effects later. Drop shadow / glow operate on the composited 2D result (after the 3D
  pass), so they need nothing special.

S30-e builds the per-face mapping + the wiring; the model here guarantees it has a sane domain
(design-space UVs) to build on. The only thing it waits on is the general Layer-Effects feature,
not a design question.

**As built (S30-e, 2026-07-16).** Exactly the contract above, with one implementation choice worth
recording: the overlays are not evaluated per fragment in the render lanes — they are **baked into
per-material "overlay albedo" maps** (`core/text/extrude_overlay.{hpp,cpp}`): each texel is the
colour → gradient → pattern overlay stack composited (own blend mode + opacity, the canonical
z-order) over that material's constant albedo, in the design domain the mesh UVs are normalized
over, at the device texel density. Both lanes (CPU rasterizer + the Vulkan compute pass, overlay
maps on binding 8) then sample ONE map bilinearly at the perspective-correct interpolated UV — so
blend-mode math lives in exactly one place and lane parity is by construction. The sampled colour
*replaces the albedo before shading* (diffuse fill and metal F0 alike): the design is lit with the
surface it sits on, which is what makes the walls' "shaded continuation" read as one solid.
- **Faces:** the front cap always; `Extrude::overlayWrapSides` (the panel's "Wrap effects onto
  sides", off by default) wraps the WHOLE solid — the back cap takes the design map too (it reads
  mirrored from behind, like the back of a painted sign), and the walls/bevels take a SECOND
  per-material map in the **unrolled side domain** (outline arc length × depth,
  `ExtrudeVertex::side`). Sampling the flat design UV on a wall repeats one outline point's colour
  down the whole depth — any pattern stretches into ruler lines (feedback 2026-07-16) — while the
  unrolled domain is the label-wrapped-around-the-solid surface a pattern tiles undistorted in.
  Per wall texel: patterns evaluate at real unrolled design px; gradients keep the design-space
  continuation (the outline point the texel's arc position maps back to, via the mesh's
  `SideStation` return map), so a gradient still flows around the solid like paint.
- **Sampling semantics** mirror the 2D effect renderer: gradients span the design domain
  normalized to [0,1]²; patterns tile in real design px, phase-anchored at the domain's top-left
  (`anchorToCanvas` has no meaning on a rotated 3D face and is treated as layer-anchored).
- **The compositor strips the consumed overlays** from `applyEffects` for extruded text — applying
  them again would be precisely the projected-rectangle smear this section forbids — while drop
  shadow / glow / stroke / bevel / satin still operate on the composited 2D result, as specced.
  Flat text is untouched: its overlays remain the 2D pass's business, byte-identical.
- **Cache:** the three overlays joined the text pixel-cache validity key
  (`TextLayer::cachedOverlays`), so a Layer-Effects edit re-renders the solid without a block
  edit; fill-opacity dims the whole solid *including* its baked design (the design is ON the
  surface — a deliberate, documented divergence from the flat-layer rule that overlays stay full).

---

## 13. Accepted additions (folded in 2026-06-25)

Offered as critique, **reviewed and accepted by the user**, and now distributed into the sections
and sessions noted (§2) — none of these is "future":

- **Point vs Area named explicitly** (§7) — this *is* the clean fix for the "stuck size" annoyance;
  it also gives resizable wrapping frames for free.
- **Variable fonts + OpenType features** (§3.4) — cheap via HarfBuzz, and genuinely the line
  between a *good* type tool and a toy. Strongly recommended for S29-c; behind progressive
  disclosure so they don't clutter.
- **Optical vs metric kerning** — offer both (metric from the font; optical computed from
  sidebearings, Affinity-style). A small, high-perceived-quality touch. Proposed for S29-c.
- **Subpixel AA caveats** (§4.3) — offered but defaulted off, because its preconditions (opaque,
  unrotated, non-3D) are routinely violated in a layered editor; grayscale is the honest default.
- **Bidi/IME designed in now** (§6) — retrofitting RTL/CJK/IME into a caret model is painful;
  cheap to design for up front even if a first build is ASCII-first.
- **Emoji / colour fonts (COLR/CPAL, sbix)** — pulled **into S29**: FreeType renders the colour
  layers/bitmaps and the emoji font is Settings-selectable (§4.2). (Vertical CJK text stays a
  reserved hook, §4.2 — the one genuinely-large axis nobody asked for yet.)
- **The panel must not occlude the caret** (§8.1) — corner-flip; small but real.

---

## 14. Decisions (2026-06-25 review) & residual questions

**Resolved this round:**

1. **Context-bar vs panel boundary** — *settled:* the 5 hot controls are Font, Size, Bold/Italic,
   Align, AA + the 3D toggle (§8.2); everything else is in the panel. Retune once the bar exists.
2. **Default font & fallback** — *settled:* resolved **cross-platform from the OS**, never a baked-in
   name (§4.2); the **emoji font is user-selectable in Settings** (a machine may have several).
3. **3D camera default** — *settled:* **near-orthographic** with a perspective slider (§10.3).
4. **Stepped-relief 3D** (per-run depth via real connecting walls) — *deferred by decision* (§10.4):
   the shared-depth solid is the model; revisit only if it proves limiting. Specced, not parked.
5. **Layer Effects on 3D text** — *scheduled as **S30-e*** (§12), paired with the general
   Layer-Effects feature; no longer "future."

**Residual (small, resolve in-session, not blocking):**

- The exact per-OS default-family choice and emoji-font default — confirm against real machines
  during S29-a (it is OS-resolved, so this is verification, not design).
- **The canvas/IME focus-gating spike** (§6.2) — disable IME on the canvas without breaking the
  working `Fl_Input` fields; native Wayland is the primary IME target. **Fixable ahead of S29** — it
  breaks Space/R under Fcitx-Wayland today (verified 2026-06-25).
- **Whether GPU residency is pulled forward** (§1.1 #12) — a project-level call tracked outside this
  doc; the Type model is residency-shaped either way.

---

## 15. References

- **Slug** — Lengyel, *US 10,373,352*, **dedicated to the public domain 2026-03-17** (form SB/43);
  MIT reference shaders. https://sluglibrary.com/ · https://terathon.com/blog/decade-slug.html ·
  reimpls: github.com/mightycow/Sluggish, github.com/diffusionstudio/slug-webgpu
- **HarfBuzz** (MIT) — https://harfbuzz.github.io/  · **FreeType** (FTL/GPLv2+) —
  https://freetype.org/
- **Green**, *Improved Alpha-Tested Magnification for Vector Textures and Special Effects*,
  SIGGRAPH 2007 (Valve) — SDF foundation. **msdfgen** (MIT) — https://github.com/Chlumsky/msdfgen
- **UAX #14** (line breaking), **UAX #9** (bidi) — Unicode, public.
- **earcut.hpp** (ISC) / **libtess2** (SGI-B) — triangulation, already vendored (vector-model §5.1).
- Companion: **`docs/vector-model.md`** (the vector stack, the `Contours` seam, the renderer IP
  landscape this builds on) and **`PLAN.md`** S29 / S30 / S30-b.
