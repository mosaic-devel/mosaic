# Icon inventory — everything that needs final artwork (feeds S52)

A **running list** of every icon the app uses or will need, with where it appears and the sizes it
is rasterized at. **Update this file whenever a session adds an icon, a placeholder, or a
unicode/code-drawn stand-in** — S52 (icon-system finalization) works through this list, and a
human icon designer should be able to take it as a brief. Requested by the user (2026-06).

Identity rules (PLAN §3.13): **colorful, illustrative, self-describing** SVGs — never flat
monochrome line art. All icons are single-source SVG, rasterized at runtime via nanosvg, so
"size" below means the raster target(s) the design must stay legible at. Design on a 24×24
viewBox; verify at every listed size.

**Scope of that rule, clarified 2026-07-09 (user decision).** It governs icons the eye picks *one
of* from a set — **tool icons** (19 of them in a column: colour is what makes them findable) and
**dialog stage icons** (one big face carrying the emotional register). It does **not** govern
**panel chrome**: the eye/lock cells and the add/group/delete buttons in the layer dock are
one-ink marks whose whole job is to recede, follow the theme, and grey out with their control.
Three colourful chips in a 32px-tall dock strip read as clutter, and a baked-in colour cannot mute
itself when the panel is disabled. Those live under "Panel chrome" below, and they are **final**,
not placeholders — S52 should leave them alone.

## Tool icons (left toolbar + flyout rows) — GIMP color icons shipped

**Artwork shipped; bespoke replacement wanted near release.** Rasterized at **20×20** (toolbar
buttons and flyout rows, `kIconPx`); the Settings pack browser also draws them at 28. Future
HiDPI work may rasterize 28/40px — shapes stay legible small.

**2026-07-10: the S52 bespoke art was tossed (user call — it didn't make the cut) and the
default pack's 32 slots refilled with GIMP's vector color tool icons** (CC-BY-SA 4.0; credited
with per-icon provenance in `docs/credits.md` + the pack manifest). These are 16×16-grid tango
art, so tool icons are no longer on the 24-grid (the 24-grid rule above still holds for dialog
stage icons). Mosaic-side art in the pack: hand/move adapted from the vendored apple_cursor set,
badges distinguishing blur/dodge/burn (all three share GIMP's dodge base — droplet/sun/flame), a
polygonal and a magnetic lasso derived from free-select, five original shape-tool icons (GIMP
has no shape tools), and in-project reworks of inpaint_brush and red_eye. All Mosaic-authored
art is nanosvg-safe — shapes and gradients only; nanosvg silently drops `<filter>`/`clip-path`,
so the GIMP originals' soft blurs render hard-edged, which reads fine at 20px.

**Near release: post an "icon artists needed" call** (user intent, 2026-07-10) — the long-term
aim is a commissioned bespoke set; the GIMP art is the shipped-quality default until then.

The pack system itself is unchanged: every implemented tool, every future tool this file used to
list as owed (Magic Wand S17, Selection Brush S18, Blur/Dodge/Burn/Smudge S23, Pen/Path S28,
Heal S38, Red Eye S38-b, Clone Stamp, Hand, Mesh/Perspective Warp S35-b) **and a Magnetic
Lasso** — 32 SVGs, so a tool session that lands one of those picks its icon up for free
(`ui::iconKeyFor` + one ToolId switch entry). The S11 inline placeholders are DELETED — tools are
registered with the pack's art (`ui::defaultIconSvg`).

**Owed: `red_eye_sclera`.** S38-b landed the eye tool as TWO flyout variants in one slot — Red Eye
(flash) and De-redden Eye (sclera) — and both currently resolve to the reserved `red_eye` art, so a
flyout row is told apart by its name alone. The second mode wants its own glyph (a de-reddened
sclera reads naturally as an eye with the *white* treated, not the pupil). Adding it is four edits,
none of them free: a new `assets/default_tools/red_eye_sclera.svg`; the key in
`src/ui/CMakeLists.txt`'s `_mosaic_default_tool_icons` list; a bump of the 39-key census in
`tests/test_icon_pack.cpp` (which also pins a square viewBox, no `<text>`, ≥2 opaque colours and
0.10–0.95 coverage at 20 px); and a provenance row in `docs/credits.md`. ⚠ Verify any new or edited
SVG through nanosvg before trusting it — nanosvg silently drops `<filter>`/`clip-path`/`<text>` —
and beware the EmbedAssets stale-mtime trap: a changed SVG may not re-embed on an incremental build.

Pack mechanics (`src/ui/icon_pack.{hpp,cpp}`): a pack is a folder holding `mosaic_icon_pack.json`
(identity + credits; the file IS the pack marker) + one icon per tool — `<key>.svg` **or**
`<key>.png` (raster packs; SVG wins when both exist, alpha-weighted area-average scaling +
letterboxing for raster, decoded-size capped); user packs live under
`dataDir()/icon_packs/<pack>/`; resolution falls back to the default **per icon** (a one-icon
pack is legitimate). **Packs are TOOLS ONLY** — the pack surface ends at the toolbar; dialog
stage icons, panel chrome and every other glyph in this file stay out of a pack's reach. Selected
pack = `Settings::iconPack`, browsed in Settings → Appearance → Icons. The census test
(`tests/test_icon_pack.cpp`) pins: 32/32 keys, 20 px rasterization, ≥2 opaque colours per icon
(the colour identity as a hard test), coverage sanity.

## UI glyphs currently drawn in code or via unicode — need real icons eventually

| Glyph | Where | Current state | Raster size |
|---|---|---|---|
| Out-of-gamut warning | colour picker, over the preview (S12-b) | **U+26A0 unicode stand-in — user-flagged for replacement** | ~14px in a 36×22 chip |
| Swatch swap arrows | colour swatch, top-right | code-drawn | ~10px |
| Swatch reset (b/w chips) | colour swatch, bottom-left | code-drawn | ~10px |
| Slot variant triangle | toolbar buttons, corner | code-drawn (may stay code-drawn) | 6–8px |
| Group disclosure triangle | layer panel rows | code-drawn (may stay code-drawn) | ~8px |
| Combo chevron | `ui::Dropdown` | code-drawn (may stay code-drawn) | ~8px |
| Pasted-layer badge | layer panel rows, right of the name (marks unorganized pasted pixel data; clears on rename) | code-drawn placeholder (two overlapping squares, S15 fix pass) — **needs real artwork** | ~9–10px |
| Layer-type badges (vector shape, **vector path**, gradient ramp, Point/Area text, magic, adjustment) | layer panel rows, after the name | code-drawn — one ink, muted, purely passive marks. **Vector path** (added with S28's pen tool) is a bezier segment threaded through two 3px anchor squares, one hollow + one filled: the pen's own on-canvas node chrome at badge size. It exists because a path, a parametric shape and a gradient are all one `VectorLayer` and were otherwise indistinguishable in the dock | 8–14px wide, ~10–11px tall |
| Texture badge | layer panel rows (texture-generator layer) | code-drawn chip — a framed mini checkerboard, clickable like `fx` (it re-opens the generator) | 20×18px |
| `fx` badge | layer panel rows (layer has effects) | drawn as styled text | ~11px |

## Panel chrome (layer dock) — **final artwork shipped (2026-07-09, S16-g)**

**One ink, not colourful** — see the scope note at the top. Authored as pure white on transparent;
`ui::drawIcon` (`src/ui/icons.hpp`) replaces the RGB with a palette colour and keeps the
rasterizer's coverage in the alpha, so a single source serves the light theme, the dark theme, the
accent-on-hover state and the muted/disabled state, and a re-theme costs only a redraw. **Keep
them monochrome — `drawIcon` overwrites their RGB, so any colour authored in is silently lost.**
Pixels are cached per (icon, ink).

⚠ **Authored on a 16×16 grid and rasterized at exactly 16px (`ui::kIconPx`), never anything else.**
The first cut drew a 24-unit viewBox at 16px; the fractional scale put every straight edge on a
half-pixel and the user reported the whole set as *blurry*. On the pixel grid a 1px stroke is
centred on a **half-integer** and a 2px stroke on an **integer**, so each covers exactly one pixel
column. Measured fully-inked-pixel fraction at 16px: plus and folder 1.00, trash 0.96, locks 0.65,
eye_open 0.44, eye_closed 0.33 — the same art at 18px collapses to 0.00–0.47. Curves (the eye
almond, the lock shackle) cannot be grid-aligned, so they carry **1.5px** strokes: that gives them
a fully-inked core with AA'd shoulders instead of the pure-grey smear a 1px curve rasterizes to.

| Icon | Asset | Where |
|---|---|---|
| Plus | `assets/icon_plus.svg` | dock footer, New Layer |
| Group (folder) | `assets/icon_group_layers.svg` | dock footer, Group Layers |
| Trash | `assets/icon_trash.svg` | dock footer, Delete Layer (right-anchored, away from the other two) |
| Eye open / closed | `assets/icon_eye_open.svg`, `assets/icon_eye_closed.svg` | row visibility cell |
| Lock open / closed | `assets/icon_lock_open.svg`, `assets/icon_lock_closed.svg` | row lock cell (always drawn; muted when unlocked) |

Two authoring notes, both learned by rendering them offline before shipping:
- **eye-closed is a lowered lid, not a slashed eye.** A slash needs a knocked-out gap where it
  crosses the almond to read cleanly, and a one-ink alpha icon cannot punch the panel ground back
  through itself.
- **lock-open drops the shackle's right leg entirely** (the arc breaks off up-and-right). A first
  draft merely shortened it by ~1px and the two lock states were indistinguishable at 16px. The
  states must differ in **silhouette**, never in colour alone.

`tests/test_icon.cpp` pins every invariant a future asset edit could quietly break: each icon
rasterizes with ink (a dropped subpath makes nanosvg render blank *silently*), each is pure white
(the tinting contract), each lands some **fully**-inked pixels at 16px (the crispness contract —
`eye_closed` once scored a flat zero), rasterizing off-grid measurably softens them, and the
eye/lock pairs are distinct rasters.

## Dialog stage icons (AskOrTellDialog) — final artwork shipped (2026-07-06)

Rasterized at **48×48** in the dialog's top-left well (`docs/askortell-dialog.md`); designed on a
64×64 viewBox in the app-icon gradient language (vertical light→dark fills, top sheen, soft rim;
glyphs are pure geometry — nanosvg renders these, so no `<text>`/filters/masks). Verified at 48
and 128 px on dark and light grounds.

| Icon | Asset | Status |
|---|---|---|
| Information | `assets/icon_info.svg` | shipped |
| Question | `assets/icon_question.svg` | shipped |
| Warning | `assets/icon_warning.svg` | shipped |
| Corrupt file | `assets/icon_corrupt_file.svg` | shipped (page + glitch shear + small warning badge) |
| Restore | `assets/icon_restore.svg` | shipped 2026-07-08 (clean page + green badge with counterclockwise restore arrow — the crash-journal offer's face); **awaiting visual pass** |

The S41 loss-warning dialog severity icons (below) can likely reuse warning/info from this set;
only "destructive-flatten" would be net-new.

## Known future icon needs (non-tool)

- **Menu icons** — if menus ever get them (none today; decide at S53).
- **Status bar** (S13-b): colour-space chip is text; **HDR indicator/warning** (S43-c) wants a glyph.
- **Loss-warning dialog** severity icons (S41): warning / info / destructive-flatten.
- **Document tabs** (S49): close ×, unsaved-changes dot.
- **Welcome screen** art + recent-file placeholder thumbnails (S55).
- **Brush Settings panel** (S19): preset tip previews are generated, but stabilizer/symmetry/wet-edge
  toggles want glyphs.
- **App icon** (`assets/app_icon.svg`, shipped): final pass at S52; rasterized at build for platform
  icon formats — must hold up at 16, 24, 32, 48, 64, 128, 256, 512.

*Sizes recap: tool/flyout = 20px today; small chrome glyphs 8–16px; app icon 16–512px. When S52
fixes the final set, record the chosen palette/stroke conventions here.*
