# Credits

## Default brush set

Mosaic ships `data/brushes/Krita_4_Default_Resources.bundle`, the Krita 4 default resource
bundle, dedicated to the public domain by its authors (its `meta.xml` declares the license
**CC-0**; see `docs/brushes.md` §4 for the full licensing review).

Its author field reads:

> Deevad with derivations of the brushes of Ramon Miranda, Razvanc, Radian, Wolthera, Storm,
> Scottyp and other.

CC-0 requires no attribution — we credit gladly anyway:

- **David Revoy** (Deevad) — the principal author of the set — <https://www.davidrevoy.com>
- **Ramon Miranda**
- **Razvanc, Radian, Wolthera, Storm, Scottyp** and the other Krita contributors named above
- **The Blender Foundation**, credited alongside Revoy and Miranda in the upstream brush-tip
  collection's README (CC-BY 3.0 there, with an explicit exception permitting unattributed use
  as a default set in open-source software — we attribute regardless)
- **The Krita project** — <https://krita.org> — whose brush engines defined the preset format
  Mosaic imports

The bundle's own attribution metadata (author, license, e-mail, website) is preserved verbatim
at import and shown with the presets; per-file provenance is recorded so what came from where
can always be proven.

## Default tool icon pack — "Smalti"

Mosaic's tool icons ship as the icon pack `assets/default_tools/` ("Smalti", after the coloured
glass a mosaic is set from; the earlier bespoke set was "Tesserae"): 33 colour SVGs covering the
whole toolset, built on **GIMP's vector color tool icons** (imported 2026-07-10, replacing the
original bespoke set).

### The GIMP color icons — CC-BY-SA 4.0

Most of the pack is taken or derived from the GIMP Default theme's color icons. Per the theme's
`COPYING`: the Color icons are licensed **Creative Commons Attribution-ShareAlike 4.0
International** (<http://creativecommons.org/licenses/by-sa/4.0/>); icons fully created for GIMP
are attributed to the **GIMP team**, and icons taken or modified from the GNOME design team's
art-libre work are attributed to the **GNOME Project** and the **GIMP team**. Please visit
<https://www.gimp.org/>.

Several files carry named creators in their embedded metadata — credited gladly:

- **Jakub Steiner** (GNOME design team) — brush, crop, eraser, eyedropper, the lasso family's
  base art, magic wand, the marquees, pen/path, smudge, text
- **Andreas Nilsson** — bucket fill
- **Klaus Staedtler** — gradient, heal (also serving the inpaint brush), zoom
- **The GIMP team** — the remainder of the color set (blur/dodge/burn base, clone stamp,
  selection brush, warp, red eye base)

(Some files still embed older CC-BY-SA 2.0 RDF metadata from their art-libre ancestry; the
theme-level `COPYING` — CC-BY-SA 4.0 for the color icons — governs.)

### Additions and modifications for Mosaic

- **hand, move** — adapted from **apple_cursor** by **Kaiz Khatri** (ful1e5),
  <https://github.com/ful1e5/apple_cursor>, **GPL-3.0** (vendored at
  `third_party/apple_cursor/`); `move` modified for Mosaic
- **inpaint_brush** — a duplicate of `heal.svg` (both tools patch pixels; deliberate until a
  bespoke set lands)
- **red_eye** — GIMP-derived art reworked in-project to fit Mosaic's tools
- **blur, dodge, burn** — all three derive from GIMP's `gimp-tool-dodge`; Mosaic adds the
  distinguishing badges (water droplet / sun / flame)
- **edge_brush** — derived from GIMP's selection-brush art (`selection_brush.svg`, GIMP team);
  Mosaic adds the distinguishing lightning badge for the edge-aware select brush
- **lasso_polygon, lasso_magnetic** — derived from GIMP's `gimp-tool-free-select`: the loop and
  tail redrawn as straight segments for the Polygonal Lasso; a horseshoe-magnet badge added for
  the Magnetic Lasso
- **shape_rect, shape_ellipse, shape_polygon, shape_star, shape_line** — Mosaic originals
  (GIMP has no shape tools), authored in-project by **Claude Fable 5** (Anthropic) and dedicated
  to the public domain (**CC0-1.0**)
- **shape_callout, shape_arrow, shape_ring, shape_cross, shape_heart, shape_banner** — the S26-c
  shape library, drawn in the same tango-blue house style as the five above, likewise Mosaic
  originals dedicated to the public domain (**CC0-1.0**)

The pack as a whole therefore mixes CC-BY-SA 4.0, GPL-3.0 and CC0-1.0 art; the per-source split
is declared in the pack's `mosaic_icon_pack.json` and here.

Every icon pack carries the same credit fields (name, artist, link, description, license) in its
`mosaic_icon_pack.json`, shown verbatim in Settings → Appearance → Icons — third-party packs are
credited the way the default one is.

## Star catalogue (night-sky Texture Generator)

The Texture Generator's night sky projects **real stars** from the **Yale Bright Star Catalogue**,
5th ed. (Dorrit Hoffleit & Wayne H. Warren Jr., 1991). Star positions, magnitudes and spectra are
astronomical facts (not copyrightable); the catalogue is a long-standing public resource of the
Astronomical Data Center.

- We consume the compact, machine-readable distillation **`bsc5-short.json`** by **Bretton Wade**
  — <https://github.com/brettonw/YaleBrightStarCatalog>, **MIT** — which also supplies the per-star
  colour temperature we tint each star with.
- `tools/gen_star_catalog.py` filters that file to the naked-eye subset (V ≤ 6.5) and emits the
  generated `src/core/texture/star_catalog.{hpp,cpp}` (positions J2000; precession omitted, well
  under the resolvable limit for a sky texture through the 2020s).

The lunar/solar ephemerides that place the stars, Moon and Sun (`src/core/texture/lunar.cpp`,
`solar.cpp`) are clean-room implementations of the published equations in Jean Meeus,
*Astronomical Algorithms* (2nd ed., Willmann-Bell, 1998) — no third-party code. The Moon's
optical libration and the position angle of its axis (Meeus ch. 53) turn the near side toward the
observer so the real surface faces the right way for the date and place.

## Lunar surface map (night-sky Texture Generator)

The Moon renders its **real near-side surface** — the actual maria, highlands and bright ray
craters — from **NASA's Scientific Visualization Studio "CGI Moon Kit"** (SVS animation 4720,
the `lroc_color_poles` colour map), an equirectangular albedo map derived from **Lunar
Reconnaissance Orbiter** (LRO/LROC) imagery by NASA GSFC and Arizona State University. NASA
material is not copyrighted; per the SVS's request we credit **NASA's Scientific Visualization
Studio**. `tools/gen_moon_texture.py` converts the 2 K colour map to an 8-bit grayscale
equirectangular map and emits the generated `src/core/texture/moon_texture.{hpp,cpp}`; the surface
pattern is a photograph of the Moon, stored as a table of that fact like the star catalogue.

The terminator's **terrain relief** — the crater rims and basin edges that roughen a real
half-moon's shadow line — shades from the **LOLA global elevation grid** (`LDEM_4`, PDS data set
LRO-L-LOLA-4-GDR-V1.0): laser altimetry by the **Lunar Orbiter Laser Altimeter** aboard LRO,
produced by the **LOLA science team** (D. E. Smith, PI, NASA Goddard Space Flight Center). NASA
material is not copyrighted; we credit **NASA / LRO / the LOLA instrument team** per NASA's
guidance. `tools/gen_moon_elevation.py` resamples the 4 pixel-per-degree grid onto the albedo
map's own grid and emits the generated `src/core/texture/moon_elevation.{hpp,cpp}` (metres above
the 1737.4 km reference sphere) — measured topography, stored as a table of that fact.

## World map (Texture Generator place picker)

The place-picker's world coastline (`src/ui/world_map_data.{hpp,cpp}`, generated by
`tools/gen_world_map.py`) derives from the **Natural Earth** 1:110m "land" polygons —
<https://www.naturalearthdata.com>. Natural Earth is in the **public domain** ("Free of known
copyright restrictions"); no permission is needed, and crediting is appreciated per their terms —
we credit gladly. Made with Natural Earth. The GeoJSON was obtained via the
`nvkelso/natural-earth-vector` GitHub mirror.
