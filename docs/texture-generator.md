# Texture Generator — Design

> **Scope.** This note settles the design of Mosaic's **Texture Generator** — a stand-alone
> `Layer ▸ Texture Generator…` modal that *synthesizes* a texture into its **own new layer**. It is
> a **suite of procedural generators** behind one dialog: a **photoreal sky** (sun + atmosphere +
> clouds, with a real perspective camera so it drops into a photo believably), **tactile paper /
> material**, and **photoreal grass** at launch, with **wood / marble / stone-concrete / canvas /
> metal** as data-level follow-ons. It is the source of truth for the **S55 (Texture Generator)**
> arc and follows the house discipline (`docs/type-tool.md`, `docs/vector-model.md`): *design from
> the published / public-domain / permissively-licensed technique, bake the published lineage into
> the source, and design resolution-independent, deterministic, and GPU-residency-aware from day one.*
>
> **It is NOT a Layer Effect.** The user settled this explicitly (2026-07-03): Texture Generator is a
> *separate, parallel* feature to Layer Effects (`docs/layer-effects.md`) — it stands alone, opens its
> own modal, and *creates a layer* rather than restyling one. (It also answers a standing gap — Mosaic
> has no "create a transparent layer" affordance; a clouds-only or sun-only generation *is* that
> transparent layer.)
>
> **No machine learning.** Project hard rule. Everything below is built from published or
> permissively-licensed technique — **no neural sky replacement, no neural texture synthesis, no
> exemplar/PatchMatch synthesis.** §9 records the lineage each generator descends from and the
> design constraints that come with it.

---

## 1. What this is (scope & vision)

### 1.1 A generator *suite*, not two features

The dialog hosts a **registry of generators**; each is a self-contained module (params + CPU
renderer + gizmos + presets). v1 ships three, architected so the fourth…nth are data-level additions:

- **Sky** — a physically-grounded **analytic sky dome** (Hosek–Wilkie radiance), a **sun** (disc +
  limb glow + aureole), **aerial-perspective haze** (Rayleigh/Mie), and **clouds** — both fast **2D
  layered** clouds *and* **true volumetric ray-marched** clouds (§9.1). A **real perspective
  camera** (horizon / FOV / roll / tilt-shift + multi-altitude cloud layers) makes the result sit
  correctly *inside a photograph* rather than reading as a pasted flat gradient (§4.5 — the user's
  "you can't just paste a sky in and have it look real" ask). Sun position is drivable **artistically**
  (drag it on a dome gizmo) *and* **physically** (date/time/lat-long → az/el). Every element is
  **independently toggleable**; disabling the dome is how you get a *transparent* clouds-/sun-only
  layer (§3.4).
- **Paper / material** — a **tactile** surface: a procedural **height field** (fiber, grain, tooth,
  laid/chain lines, wove/felt, deckle) → **normal map** → **raking-light** shading, so it reads as
  physical relief. The "expensive matte cardstock" look, with presets and paper *kinds* (§5).
- **Grass** — **photoreal grass** as a **distance-graded hybrid**: a procedural turf base carrying the
  far field, with depth-culled **blade instancing** (Bézier blades, tangent/fiber lighting, backlit
  translucency, root ambient-occlusion) for the near field, projected through a ground-plane
  homography so a lawn recedes to the horizon (§6). Evidence-backed, not a compromise (§9.3).
- **Follow-on materials** (S55-g, data-level — **BUILT 2026-07-16**): **Wood** (growth rings +
  knots + fibre), **Marble** (turbulence veining), **Stone/Concrete** (cellular aggregate +
  cracks), **Canvas/Linen** (woven height), **Metal** (brushed/anisotropic). Each is its own
  params arm reusing the §5 height→normal→shade pipeline behind the generator registry.

### 1.2 What "photoreal" means here (and what's out)

Photoreal here means **procedurally synthesized from physical/empirical models**, evaluated at the
document's real resolution — not upscaled bitmaps, not photo library assets, not ML. Concretely:

- **In:** analytic daylight radiance (Hosek–Wilkie), Rayleigh/Mie haze, a real solar-position solver,
  **2D layered *and* volumetric ray-marched clouds** (§4.3, §9.1), a **perspective camera**
  (§4.5), procedural paper micro-structure + height→normal→raked-light shading, **hybrid grass** (§6),
  a **float (`ImageF`) sky cache** for banding-free gradients (§4.4), a seed for determinism,
  resolution independence, and a live preview.
- **Out (deliberately, first pass):** ML anything; **compositing the synthetic sky *into* a user's
  photo by masking out their real sky** (that is Layer-Effects / selection territory — Texture
  Generator *makes a layer*, the user composites it);
  Bruneton multiple-scattering aerial perspective (analytic Rayleigh/Mie suffices — later upgrade,
  §9 fork 4); weather beyond clouds+haze (rain, rainbows, stars/moon — reserved hooks, not v1); the
  follow-on materials beyond paper+grass (S55-e).

### 1.3 Design pillars (house style)

1. **CPU-first, permanent CPU-only lane.** Every generator has a **CPU reference renderer** that is
   the correctness oracle + headless-test lane; a Vulkan compute lane slots in later (§8.4, S60) as a
   *renderer swap*, parity-tested against the CPU lane exactly as the 3D-text lanes are
   (`extrude_render.cpp` ↔ `extrude_gpu.cpp`).
2. **Resolution-independent + deterministic.** Params are stored resolution-independently and
   evaluated at document resolution against a **seed**; same seed + params + resolution → same pixels,
   CPU or GPU. The mandatory **Scale** slider maps generator-space to document-space (§8.3).
3. **A live, editable layer — pixels are a cache.** Parameters live on the layer (re-open the modal to
   keep tuning); pixels are a regenerated **cache**, the `TextLayer` pattern (§3). Non-destructive;
   one-click **Rasterize** bakes.
4. **Intuitive, not an alien control panel.** Progressive disclosure, strong presets, direct-
   manipulation **gizmos** (a sun on a dome, a horizon bar, a wind arrow, a raking-light dot, a grass
   camera), a *live* preview, and a **generator rail** that scales as the suite grows (§7). The simple
   path: pick a generator, pick a preset, drag one gizmo, done.

---

## 2. Session breakdown (split deliberately)

Sequenced so every session is independently **shippable and headless-testable**. The noise kit and
layer model land first with *no UI*; the CPU generators are separable subsystems; the modal+gizmos are
one session; the GPU lane folds into S60.

> **S55-a BUILT 2026-07-15.** As scoped below, plus one addition: each generator ships a
> **baseline CPU render** (gradient dome + fBm clouds + sun disc; fBm tooth → central-difference
> normal → raking Lambert; Worley-clump turf) so the whole params → `refreshTextureCaches` →
> compositor chain is exercised and **golden-pinned** end-to-end from day one. The real renderers
> (S55-b/-c/-d/-e) replace those baselines — a deliberate golden-breaking change in each session.
> Also landed here (pulled forward from later rows because the seams were open anyway):
> `.mosaic` serialization of `TextureParams` (docio/docjson; the pixel cache is never stored) and
> the layer-panel thumbnail arm. The headless op is `--headless --texture sky|paper|grass
> [--seed N] [--export out.ppm]`.

> **S55-b BUILT 2026-07-15.** As scoped below: the official BSD-3 Hosek-Wilkie reference is
> vendored at `third_party/hosekwilkie/` **trimmed to the RGB lane** (the spectral/XYZ datasets
> serve nothing we call); the clean-room NOAA/Meeus solver is `src/core/texture/solar.{hpp,cpp}`
> (validated against Meeus's own worked example, solstice/equinox declination, EoT extremes,
> both-hemisphere azimuths, horizon refraction); the §4.5 camera is `sky_camera.hpp` (pinhole,
> horizontal-FOV + pitch/roll/tilt-shift, world +X east +Y north +Z up, camera faces azimuth
> 180); the renderer is `sky_render.cpp` (per-type §4.3 catalogue constants, per-deck altitude
> planes, Kasten-Young/Beer-Lambert solar tinting, Koschmieder-style aerial perspective).
> **One deliberate deviation from §4.4's order sketch: the sun disc/aureole composites BEFORE
> the cloud decks** — the catalogue's own "Altostratus: sun a diffuse disc" row demands clouds
> over the sun; forward-scatter silver-lining carries the in-front glow instead. Display
> mapping: calibrated exposure → extended-Reinhard (white 6.0) → sRGB encode, kept float; the
> solar disc exceeds 1 (HDR headroom, golden range widened to [0,8)). Below the horizon the
> dome renders a receding ground-haze band (opaque, so a dome-on layer stays α=1). Measured
> cost: ~1 s wall / ~6 CPU-s at 1 MP with two decks — above the §8.1 guess, same order.
> SkyParams GREW (groundAlbedo/exposure/sunDiscScale/fov/pitch/roll/shiftY/wind/cloudLayers);
> docio reads absent growth fields as defaults (schema stays 1), pinned by a backward-read test.

> **S55-c BUILT 2026-07-15.** The volumetric lane (§4.3 lane B / §9.1) is
> `src/core/texture/cloud_volume.{hpp,cpp}` — an **implicit** 3D Perlin-Worley density field
> (fBm body + inverted-Worley puffs + 3-channel domain warp, carved by a coverage remap and a
> per-type height gradient, edge-eroded by a high-frequency Worley) **ray-marched** with
> single-medium **Beer-Lambert** transmittance, a dual-lobe **Henyey-Greenstein** phase, a
> **powder** (beer-powder) dark-edge term, and a secondary **light-march** toward the sun for
> self-shadow. It replaces the flat 2D projection **only for the heap/tower types (Cumulus,
> Cumulonimbus** — `cloudTypeIsVolumetric`); every other type keeps `sky_render.cpp`'s fast 2D
> lane. The marcher returns straight-alpha in-scatter + coverage so `sky_render.cpp` composites
> it through the **same** aerial-perspective fade and horizon blend as a 2D deck. Lit tops take
> the sun's hue, self-shadowed bases fall to the cool ambient — the rounded 3D form the 2D lane
> could not carry (verified by an A/B render pass). The `Rgb`/`mixRgb` radiance primitives moved
> to the shared `sky_camera.hpp` so both lanes read one definition. **SkyParams GREW**:
> `volumetricClouds` (default true; false forces every deck onto the 2D lane) — a lenient
> `getGrownB` docio read (schema stays 1), backward-read + Storm-sky round-trip pinned. The sky
> golden was **deliberately re-blessed** (`12092899735861124389` → `11872435875382124481`;
> recorded at -O3, verified byte-identical at -O0; paper/grass held EXACTLY). Cost: the default
> two-deck sky (one volumetric cumulus + one 2D cirrus) is ~1.7 s wall / ~11 CPU-s at 480×320 —
> so it is **bake-on-Create, not live** (the S55-f dialog owns the progress bar + low-res proxy;
> the pure-per-ray engine is ready for both). The §9.1 constraints hold: an implicit density
> field (NOT a polygon volume) marched with a single medium (NOT a proportion-by-intersection
> blend of a separate volume into a globally-characterized scene image); §9.7 lineage header on
> `cloud_volume.cpp`.

> **S55-d BUILT 2026-07-15.** The real paper/material renderer (§5) is
> `src/core/texture/paper_render.{hpp,cpp}` (its own TU + §9.7 lineage header; it replaces the
> S55-a baseline that lived in `texture_render.cpp`). A layered scalar **height field** — fBm
> **tooth** (anisotropic grain stretch) + a new **spectral fibre** term + the paper-**kind**
> structure — is turned into a normal by a **single-pass Sobel** (§5.3 — never an
> iterative-smoothing or weighted-pyramid variant) and shaded by an **Oren-Nayar** raked light (+ an
> optional Blinn-Phong coated sheen). The **spectral fibre** is a new **sparse-convolution Gabor
> noise** (`gabor2` in the shared noise kit, reimplemented from Lagae et al. 2009; a Gaussian
> sum, so calibrated to RMS ~0.6, `kNormGabor`), coherent (anisotropy 1) so the streaks run along
> the grain. **`PaperKind { Wove, Laid, Felt }`** selects the height structure: Wove = isotropic
> tooth; **Laid** carries BOTH ruled-line systems — fine parallel **laid** corrugation + sparse
> perpendicular **chain** ridges, jitter-wobbled and rotating with the grain angle (chain lines
> are a *component* of laid paper, not a separate kind); **Felt** adds a coarse low-frequency
> cloud. **Deckle edge** (`deckleEdge`) writes an fBm-perturbed distance-to-border **alpha** — the
> §3.4 transparent torn fringe (default off = opaque). **Print tooth** (`printTooth`) is a
> Scale-relative (not per-pixel → resolution-independent) blue-noise-style speckle that clusters
> in the tooth valleys. A **7-entry preset library** (`paperPreset*()`): Business card / Kraft /
> Laid bond / Cold-press watercolour (deckled) / Newsprint (print) / Vellum / Linen-canvas.
> **PaperParams GREW** (12 fields ADDED: kind, fiber, laidSpacing, chainSpacing, laidDepth, matte,
> sheen, deckleEdge, deckleAmount, deckleInset, printTooth, printAmount) — docio reads them
> leniently (new `getGrownEnum` for the kind; schema stays 1), backward-read + kitchen-sink
> round-trip pinned. The paper golden was **deliberately re-blessed**
> (`18218666069909297785` → `6902453368357319705`; recorded at -O3, verified byte-identical at
> -O0; sky/grass held EXACTLY). Property tests in `test_texture_paper.cpp` (Gabor determinism +
> orientation, kind structure, deckle alpha carry, print darkening, presets). Bake-on-Create; the
> S55-f dialog owns the Scale/kind/light gizmos + preset picker. This same height→normal→shade
> pipeline is the engine for the S55-g follow-on materials (a new material is a data addition).

> **S55-e BUILT 2026-07-15.** The §6 grass hybrid as scoped (`grass_render.{hpp,cpp}` own TU,
> `GrassCamera` in `sky_camera.hpp`, depth-graded Bezier-blade instancing bounded by a FIXED
> world reach, Kajiya-Kay/wrap/AO shade, banded back-to-front painter's raster byte-identical to
> serial, 16 GrassParams growth fields, 6 presets); grass golden deliberately re-blessed.
>
> **S55-f BUILT 2026-07-15.** The modal as scoped: `TextureGeneratorDialog`
> (`ui/texture_generator_dialog.{hpp,cpp}`; Layer ▸ Texture Generator…, the FillHost-style
> `TextureGenHost`, §3.3 select-to-edit) with the §7.2 generator rail, per-generator control
> stack under progressive disclosure, presets (a new `skyPreset*()` library joins paper/grass;
> "Custom" readout via spec equality), seed + Randomize, and the §4.2 solar calculator (time &
> place → sun az/el, dialog-local state). The preview pane is the extracted **`GizmoCanvas`**
> (`ui/gizmo_canvas.hpp`, shared with the 3D popup) over a **background-rendered proxy**:
> in-frame draggable sun + sun-on-dome inset, horizon bar (pitch), roll nub, FOV brackets, wind
> compass, grain ring, raking-light dome (pure mappings in `ui/texture_gizmo_math.hpp`,
> unit-tested). Two core additions power it: **`TextureWindow`** — renderers evaluate a
> BYTE-EXACT sub-rect of the full frame (test-pinned crop equality; Fit renders a proxy frame,
> 1:1 pans a document-resolution window; the Fit proxy scales Paper's pixel-sized features by
> proxy/doc so framing reads true) — and **`TextureRenderProgress`** (per-row progress + cancel,
> observation-only; goldens UNTOUCHED this session). `TextureRenderWorker` (core, SpellCheckWorker
> shape: coalesce-to-newest + epoch tags + in-flight cancel) runs both the live proxy and the
> **Create bake**: full document resolution behind a cancellable ProgressBar, then
> `applyBakedTextureCache` (new shared helper) installs the pixels so nothing re-renders
> synchronously — Create = AddLayerCommand, edit = one SetTextureCommand. Layer rows gained the
> texture type badge. Deferred: per-deck altitude/scale-bias handles (§7.3's cloud-layer
> handles) — deck rows expose enable/type/amount only; deck altitude stays data-level.
>
> **S55-f feedback round 2026-07-15** (user: value fields not the app's, "21.75 h" nonsense,
> tooltip flood, no night/moon, sun unconvincing): (1) **`ui::NumberField`** — the ONE outlined
> numeric value field (crop bar's comma-tolerant NumberInput extracted + self-styled), consumed
> by the tool bar's Number kind, the New Document W/H/DPI and the texture dialog's seed/date;
> (2) **`ui::TimeDrum`** — an HH:MM roller drum (5-minute grid, drag/wheel, wrapping) replaces
> the fractional solar-hour slider; (3) the preview's gizmos each carry their OWN tooltip
> (Fl_Tooltip::enter_area on hover change); (4) **the sky's night half**: sun elevation now
> reaches −30° — the HW dome (cooked at its 0° validity floor) decays through twilight
> (`dayBlend = (1−night01)^2.2`) into a starlit gradient with a low afterglow arch toward the
> sun's azimuth; a hash-lattice **star field** (deterministic, angular gaussians, power-law
> magnitudes) fades in past civil twilight; the **moon element** (`enableMoon` — default OFF so
> pre-night documents keep their look — az/el/scale + `starsAmount`, docio-grown) is a
> sharp-limbed disc whose **phase falls out of the real sun–moon geometry**, composited
> ADDITIVELY (the atmosphere scatters in front — a day crescent fades into blue instead of
> punching a black hole), with fBm mare, night earthshine, a phase-scaled halo, and moonlit
> (else near-black) cloud radiances feeding both cloud lanes; an in-frame draggable moon gizmo
> and a "Night & moon" section join the dialog; preset #8 "Moonlit night". (5) **the sun got
> its photographic glare** — softer limb, wide HDR-carrying aureole — the one deliberate golden
> re-bless of this round (day path otherwise byte-identical; night code is gated on elevation
> < 0, proven by a day-knobs-inert byte test).

> **Night-sky overhaul** (user visual-pass feedback: the night half must be *photoreal*, not a
> "cheap imitation"). Foundation-first, each phase its own tested commit. **(1) Ephemeris** — the
> clean-room Meeus lunar + celestial-frame solver (`lunar.{hpp,cpp}`: position ch.47, phase ch.48,
> sidereal/transform ch.12–14). **(2) Real star field** — the hash-lattice `starRadiance` replaced
> by the **Yale Bright Star Catalogue** projected by each star's true position (`star_catalog.*`,
> `gen_star_catalog.py`), so real constellations appear. **(3) Real Moon** — the fBm "mare
> mottling" ball replaced by the **NASA LRO albedo texture** (`moon_texture.*`, `gen_moon_texture.py`)
> **sphere-mapped** onto the disc: the near side is turned toward the observer by the **Meeus ch.53
> physical ephemeris** (optical libration + the position angle of the axis) off the observer clock,
> and a **Lommel-Seeliger** terminator is lit by the scene sun (with earthshine on the dark side).
> The disc is the real maria, correctly oriented, at any date/place. Day path stays byte-identical
> (the Moon is an off-by-default element), so no golden re-bless. **(4) Physical twilight** and
> **(5) dialog master-clock wiring** are the remaining phases.

> **Renderer-core batch 2026-07-16** (volumetric banding + moon relief + twilight multiple
> scattering; one sky-golden re-bless for the three:
> `15307404083731329921` → `6565761780459925611`, recorded at -O3, verified byte-identical at
> -O0; paper/grass held EXACTLY). **(1) Volumetric cloud banding** (user: "wavy strips running
> through them") root-caused by experiment to PRIMARY-march quadrature aliasing — the fixed
> 40/56-step midpoint lattice beats against the warped density field, worsening as dt stretches
> 1/dir.z toward the horizon (8× primary steps removed the strips; 8× light steps changed
> *nothing*). Fix: the step count scales with the slab slant (dt held constant in world metres,
> capped 3×) + a per-FRAME-pixel jitter of each ray's sample lattice (the ditherTPDF splitmix
> finaliser on its own channel tag — deterministic, window-crop- and parallelism-exact), so the
> residual error decorrelates into grain instead of stripes. Stripe-band spectral power over the
> banded regions drops 3–6× to the 8×-reference level; worst-case (horizon-heavy pitch-6 frame)
> cost ×2.75, steep frames pay far less. **This is the golden mover** (the default deck's cumulus
> pixels shift). **(2) Moon terrain relief**: `tools/gen_moon_elevation.py` embeds NASA's LOLA
> `LDEM_4` (PDS LRO-L-LOLA-4-GDR-V1.0) resampled onto the albedo's 1024×512 grid as int16 metres
> (`moon_elevation.{hpp,cpp}`); the Lommel-Seeliger normal tilts by the height gradient taken
> through the SAME (u, v) mapping the albedo reads — crater rims shadow hard exactly at the
> terminator, flatten by full phase, limb fade keeps the arc clean. Map orientation pinned
> empirically (Mare Crisium dark AND −3.6 km at one texel; albedo-height correlation +0.34
> aligned vs +0.20 mirrored). Moon is off by default → golden-inert (proven). **(3) Twilight
> multiple scattering**: `cookAtmosphere` bakes a 16×64 RGB **Ψ_ms table** (Hillaire 2020's
> isotropic estimate, physics reimplemented — L2 from a Fibonacci direction fan + the
> energy-conserving geometric-series closure L2/(1−f_ms)); `radiance()` adds it for every view
> sample *including Earth-shadowed ones* — the anti-solar civil-twilight sky lifts from
> dead-black to a soft blue-balanced gradient, −6° gains a directional blue ambient (+11%, blue
> leading), with `kMsGain = 5.0` truncation compensation (no ozone/ground bounce/higher orders;
> total/single-scatter ~8, inside the literature's 5–10). Day path byte-identical (gated as
> before) → golden-inert (proven); twilight renders ~1.2 s vs 0.8 s at 800×500. New tests: LOLA
> table facts + half-vs-full-phase terminator contrast; Ψ_ms finiteness + the anti-solar blue
> floor.

> **S55-g BUILT 2026-07-16** — the follow-on materials, WITHOUT the HDR-export half (`.exr`/
> AVIF-HDR off the float cache stays deferred: it is gated on the S43 colour-management lane; the
> "Bruneton if wanted" fork was superseded by the twilight multiple-scattering batch). Two parts.
> **(1) The generator REGISTRY made literal**: `GeneratorTraits` (texture_render.hpp; a
> constant-initialized table in texture_render.cpp) type-erases every cross-generator seam —
> stable name + docio/CLI token, Scale semantics (`pixelScaledFeatures` drives the dialog's
> Fit-proxy correction), default-spec seeding, render dispatch, and the preset library
> (count/name/apply/match) — so `renderTexture`, docjson's token mirror, the headless `--texture`
> op and the dialog's rail/preset/proxy plumbing all walk the table instead of switching on
> `Generator`. The dialog's giant `rebuildControls` switch became per-generator builder member
> functions over a shared `ControlsCtx` helper kit dispatched through a builder table; the preview
> gizmos key on a per-generator `GizmoLayout` (Sky / Lawn / **Surface**) with `surfaceFields()`
> exposing each flat-sheet arm's raking light + optional grain axis. Adding a generator is now:
> one params struct + one render TU + one preset library + one controls-builder + one registry row
> (+ its docio spec serializer). The refactor landed **byte-identical** (all goldens held on both
> presets). **(2) FIVE MATERIALS** (`material_render.{hpp,cpp}`, one TU: the §5 engine templated
> over a height+albedo recipe — heights padded one texel so the single-pass Sobel never clamps at
> a window edge, then the paper shade verbatim: Oren-Nayar + optional Blinn-Phong): **Wood**
> (Peachey-1985 rings — distance across the grain + |perlin|-turbulence meander, sparse
> Worley-seeded knots that bend the ring field locally and darken, along-grain Gabor fibre,
> earlywood/latewood colour ramp), **Marble** (Perlin-1985 `sin(k·x + turbulence)` veining
> sharpened into bands, two fracture families 31° apart, veins recessed under a tight polish
> lobe), **Stone** (Worley F2−F1 crack network between rounded cells + per-cell decorrelated fBm
> relief + per-cell albedo variation), **Canvas** (two perpendicular sinusoid thread systems with
> over/under checker parity — the §5.2 laid/chain machinery generalized to a true weave — thread
> wobble + thickness jitter, gap darkening), **Metal** (fBm stretched 42× along the brush axis, a
> broad Blinn-Phong lobe, a plain vertical reflection-ramp tint — no environment machinery). All
> render the 8-bit lane, opaque, with paper's Scale semantics (px-sized features at Scale 1).
> **19 presets** join: wood 4 (Oak plank / Walnut / Knotty pine / Driftwood), marble 3 (Carrara /
> Nero / Rosa), stone 4 (Granite / Concrete / Cobbles / Sandstone), canvas 4 (Cotton duck /
> Coarse linen / Fine portrait linen / Primed canvas), metal 4 (Brushed steel / Aluminium /
> Brass / Gunmetal). Docio: five STRICT born-whole spec serializers (kind tags = registry
> tokens); the generator-token table is generated from the registry. **Goldens**: five new 64×48
> seed-42 pins in test_texture_layer.cpp, release-recorded / debug-verified — wood
> `10091008802904838105`, marble `15809217301746616895`, stone `5426541401162512905`, canvas
> `16440316409148717988`, metal `15259068460268106427`; the sky/paper/grass pins did NOT move.
> Per §5.3/§9.4: single-pass Sobel only, Oren-Nayar/Blinn-Phong shading, and
> recipes drawn exclusively from the published vocabulary (Peachey 1985 rings, Perlin 1985
> turbulence/marble, Worley 1996 cellular, fBm, Gabor 2009 reimplemented, anisotropic stretch);
> lineage header on material_render.cpp. Tests: tests/test_texture_materials.cpp (structure,
> determinism, window-crop byte-equality, preset libraries) + docio round-trips + the registry
> grand-tour dialog test.

| Session | Scope | Key deliverable |
|---|---|---|
| **S55-a** | **Model + layer + noise kit** (headless) | `TextureLayer` (`LayerKind::Texture`) holding `TextureParams` + a regenerated pixel cache (§3, mirrors `TextLayer`; **float `ImageF` cache** supported for sky); `SetTextureCommand` (coalesced undo); `refreshTextureCaches()` render pass; compositor integration as a raster source. The **public-domain noise kit** — value/gradient **Perlin**, **OpenSimplex2**, **Worley/cellular**, **fBm + domain-warp**, all hash-seeded & deterministic (§8.3). A `--headless --texture …` op + golden tests. **No dialog yet.** |
| **S55-b** | **CPU sky — atmosphere + camera + 2D clouds** | Hosek–Wilkie dome + Rayleigh/Mie haze + sun disc/aureole, rendered in **float**; the clean-room **solar-position solver** (§4.2); the **perspective camera** — horizon/FOV/roll/tilt-shift + multi-altitude cloud planes (§4.5); **2D layered Perlin–Worley domain-warp clouds** with the full **type catalogue** (§4.3); per-element enable + alpha carry (§3.4). Headless golden renders. |
| **S55-c** | **CPU volumetric clouds** | The volumetric lane (§9.1): implicit Perlin–Worley density field + single-medium **Beer–Lambert** ray-march + **Henyey–Greenstein** phase + powder/dual-lobe + light-march self-shadow, per cloud type, projected through the §4.5 camera. Bake-on-Create with a progress affordance (not live); the low-res proxy previews it. Clean-room lineage header (§9.7). |
| **S55-d** | **CPU paper/material renderer** | Fiber/grain/tooth height field (laid/chain/wove/felt, deckle) → single-pass Sobel normal → **Oren-Nayar** raked-light, tint/paint, the paper preset library (§5). Generic enough that a follow-on material is a data addition. Headless golden. |
| **S55-e** | **CPU grass renderer** | The **hybrid** grass pipeline (§6): ground-plane homography → procedural turf base → Voronoi clump + density fields → single-class Poisson scatter → Bézier blades → Kajiya-Kay/wrap/AO shading → back-to-front composite; depth-graded LOD. Headless golden + a cost budget. |
| **S55-f** | **The modal + gizmos** | `TextureGeneratorDialog` (menu hook + `openTextureGenerator()`, §7.1); the **generator rail** (§7.2); the **live preview + gizmo pane** (a `GizmoCanvas` over a low-res proxy: sun-on-dome/compass, **horizon/FOV/tilt** camera gizmos, wind vector, grain-direction ring, raking-light dot, grass camera — §7.3); sliders incl. **Scale**; toggles; presets; **seed + Randomize**; Apply → full-res commit. |
| **S55-g** | **More materials** *(BUILT 2026-07-16; see the block above)* | The **generator registry** (`GeneratorTraits`) + wood/marble/stone/canvas/metal (data over §5). **`.exr`/AVIF-HDR export** off the float cache stays deferred (gated on S43 colour management); Bruneton (§9 fork 4) superseded by the twilight multiple-scattering batch. |
| **S55-h** | **Vulkan compute lane** *(BUILT 2026-07-16; see §8.5)* | Port the per-pixel/per-march kernels to compute shaders (`fill.comp`/`composite_blend.comp` pattern, VMA `rgba32f` targets); CPU↔GPU **parity golden tests**; GPU-resident cache (no readback under a GPU-resident compositor — the documented S60 seam). |

> S55-a…-f are the shippable core; -g/-h are explicitly later. -b/-c (sky), -d (paper), -e (grass) are
> independent subsystems. The durable dividing lines are: headless model+noise ↔ sky ↔ volumetric ↔
> paper ↔ grass ↔ UI ↔ GPU.

---

## 3. The layer & data model

### 3.1 A live-regenerating `TextureLayer` — pixels are a cache

The precedent is **`TextLayer`** (`src/core/layer.hpp:280`): a *model* plus a **pixel cache**
(`setCachedImage`, `contentRevision`/`m_cacheRevision` staleness key) repopulated by an app pass, with
the compositor treating the cache as a raster source (`compositor.cpp:776-783`). The `reflectionEnv`
field even proves the model can carry a per-layer **float** side-buffer
(`std::optional<common::ImageF>`, `layer.hpp:381`) — which is exactly what the **float sky cache**
needs (§4.4). `TextureLayer` copies this one-for-one, with the cache typed per generator (8-bit for
paper/grass, **float `ImageF` for sky**).

```cpp
// src/core/layer.hpp (S55-a) — a new LayerKind::Texture, sibling to Raster/Vector/Text.
class TextureLayer : public Layer {
public:
    TextureLayer(LayerId id, std::string name, texture::TextureParams params);
    const texture::TextureParams& params() const noexcept;   // the editable model
    texture::TextureParams&       mutableParams() noexcept;   // + invalidate after
    void setParams(texture::TextureParams p);                 // bumps contentRevision
    // Regenerated pixels are a CACHE the app's texture render pass populates (mirrors TextLayer).
    // 8-bit for paper/grass; a float ImageF cache for the sky (banding-free gradients, HDR export).
    const common::Image*  cachedImage()  const noexcept;      // paper/grass
    const common::ImageF* cachedImageF() const noexcept;      // sky (float; reflectionEnv precedent)
    bool cacheCurrent() const noexcept;
    // ... contentRevision()/invalidateContentBounds()/contentBounds() exactly as TextLayer ...
};
```

The **`Generator` registry** is a small tagged model; each generator's params are a variant arm, so
they never collide and the modal reflects only the active arm:

```cpp
// src/core/texture/texture_params.hpp (S55-a)
enum class Generator { Sky, Paper, Grass /*, Wood, Marble, Stone, Canvas, Metal (S55-g) */ };

struct TextureParams {
    Generator     generator = Generator::Sky;
    std::uint64_t seed = 0;      // determinism (§8.3); "Randomize" reseeds
    double        scale = 1.0;   // the mandatory Scale slider — generator-space ↔ document units
    std::variant<SkyParams, PaperParams, GrassParams /*, MaterialParams…*/> spec;
    bool operator==(const TextureParams&) const = default;   // cache-validity comparison
};
```

Storing the whole value makes `SetTextureCommand` a coalescing value-swap (the `SetTextCommand` model,
`commands.hpp:319`): a slider/gizmo drag coalesces into one undo step; re-opening the modal on an
existing `TextureLayer` seeds its controls from `layer.params()`.

### 3.2 The render pass — `refreshTextureCaches()`

Core cannot composite documents, so the **app** owns regeneration, exactly like text
(`refreshTextCaches`, `app_window.cpp:3687`). `texture::refreshTextureCaches(Document&, …)` walks the
tree and, for every `TextureLayer` whose `!cacheCurrent()`, invokes the CPU (later GPU) reference
renderer at document resolution × `scale`, then `setCachedImage(...)`. The compositor gains one
`else if (const auto* tex = layer.as<TextureLayer>())` arm at `compositor.cpp:760`, sampling the cache
through `layer.transform() * cacheImageToLayer` — identical to the text arm. **The compositor stays
generator-free.** The heavy generators (volumetric clouds, dense grass) regenerate **on Create /
Apply**, not per frame (a progress affordance, the inpaint precedent); the live preview uses the proxy.

### 3.3 New-layer semantics, creation, undo

On **Create/Apply**, `Layer ▸ Texture Generator…` makes a fresh top-of-stack `TextureLayer` via a new
`Document::makeTexture(name, params)` (the `makeText`/`makeRaster` pattern, `document.hpp:97`), inserted
with an **`AddLayerCommand`** (one undo step). The layer auto-names from the generator ("Sky", "Grass").
Re-opening the modal *with a `TextureLayer` selected* edits **that** layer (seeds from its params,
commits `SetTextureCommand`s) — the "select-to-edit" pattern. **Rasterize** bakes the cache into a
`RasterLayer` and drops the params (warned; the `text::rasterize` precedent).

### 3.4 Transparency & alpha carry (the disabled-element question)

Each generator composites its elements **internally** into a straight-alpha float `ImageF` (the
compositor's native format), bottom-to-top; *enabled* elements contribute coverage, *disabled* ones
nothing — so alpha falls out naturally:

- **Sky dome ON** → the dome fills the layer **opaque** (α=1); sun/haze/clouds shade on top → the
  "full sky" layer.
- **Sky dome OFF, clouds ON** → base α=**0**; only clouds write colour+coverage → a **transparent**
  cloud layer to drop over a photo (blend/opacity/mask all apply — it is an ordinary layer).
- **Sun only** → a transparent disc + aureole/flare — a compositable light element.
- **Paper** is opaque unless **deckle edges** carve the boundary α (§5.4). **Grass** carries α at the
  blade silhouettes over a transparent (or user-toned) ground, so a grass strip composites over other
  content with a real edge.

This is *why* Texture Generator makes its own layer: generating a partial-coverage element onto a
fresh layer *is* the "create a transparent layer" affordance the user wanted.

---

## 4. The sky subsystem

### 4.1 Atmospheric model — Hosek–Wilkie (chosen), analytic-haze companion

**Hosek–Wilkie for the sky-dome radiance** — the photorealism front-runner among analytic daylight
models, and decisively its reference implementation is **3-clause BSD** (header verified), GPLv3-
compatible. We take the BSD C (retaining its header) or reimplement from the paper.

- **Preetham (SIGGRAPH 1999)** — cheaper, equally free to implement (published; the algorithm itself
  is not copyrightable); kept as a fast/preview model, not the default (weaker sunsets, can go
  negative at low turbidity).
- **Perez / CIE** underlies both; source coefficients from Perez et al. 1993 or Radiance `gendaylit`,
  **not** the paid ISO 15469 PDF (document copyright ≠ technique copyright).

Hosek–Wilkie is a handful of transcendentals per pixel — **sub-second** (§8.1). Output is **HDR
radiance** (values exceed 1 near the sun), which is why the sky cache is **float** (§4.4).

**Aerial-perspective haze** uses **Rayleigh + Mie** scattering — published physics
(1871/1908): a Rayleigh (∝λ⁻⁴, blue) + Mie (forward, grey) optical-depth term thickens toward the
horizon and warms toward the sun. Bruneton's *Precomputed Atmospheric Scattering* (BSD-3) is the
multiple-scatter upgrade, **deferred** (§9 fork 4). **Do NOT copy Sean O'Neil's GPU Gems 2 code** (no
OSS licence) — reimplement the physics if his look is wanted.

### 4.2 Sun positioning — solver + gizmo

Two ways, **both, two-way bound**:

- **Artistic (default):** drag the sun on the **dome/compass gizmo** (§7.3) — direct az/el.
- **Physical:** a "Set by time & place" disclosure with **date, time, latitude/longitude** (+ named
  cities) → a **solar-position solver** returns az/el, moving the same gizmo. The "time-of-day" ask.

**Solar-position algorithm — clean-room NOAA/Meeus.** The load-bearing licence finding: **never ship
NREL's `spa.c`** (header: *noncommercial … no re-distribute* — doubly GPL-incompatible). The
*equations* (NREL report, Meeus *Astronomical Algorithms*, NOAA Solar Calculator) are freely
reimplementable, and a sky
needs **arc-minute**, not SPA's ±0.0003°, accuracy — so we implement a compact NOAA/Meeus solver from
the published equations (self-owned, GPLv3, no dependency). Alternatives if SPA-grade is ever wanted:
**SolTrack (LGPL-3)** or **`freespa` (GPLv3)** — both shippable.

### 4.3 Clouds — 2D layered *and* volumetric (both lanes ship), with a full type catalogue

Clouds are the make-or-break of "you wouldn't know it wasn't real," so the suite ships **both** lanes,
selected per cloud type / quality:

**Lane A — 2D layered (interactive default).** A stack of **fBm** over **Perlin** and **Worley**
noise ("Perlin–Worley") with **domain warping** for billowing/wispy structure, then **coverage**
threshold + **density** softening + a **wind** offset (direction/speed) that shears successive octaves.
Fast (a few seconds at 36 MP), and the live-preview lane.

**Lane B — volumetric ray-marched (§9.1).** An **implicit** 3D cloud **density field**
(Perlin–Worley + fBm + domain warp, shaped by coverage/altitude/type remap curves), **single-medium
Beer–Lambert** ray-march with a **Henyey–Greenstein** phase function, a **powder/dual-lobe** term, and
**light-march self-shadowing** — the *published* Nubis/Häggström/Hillaire technique, clean-room, no ML,
no engine code copied. Baked on Create (progress bar), previewed via the proxy. This is the
hero-cumulus realism ceiling; the constraints it is built under are in §9.1.

**The cloud type catalogue** (a preset over the shared knobs — altitude, coverage, frequency, density,
anisotropy, lit/shaded shaping, and which lane):

| Type | Altitude | Character / knobs | Lane |
|---|---|---|---|
| **Cirrus** | high | high-frequency, low-coverage, strongly wind-sheared wisps (anisotropic warp) | 2D |
| **Cirrocumulus** | high | fine cellular (Worley) "mackerel" ripples | 2D |
| **Cirrostratus** | high | thin uniform veil (halo tint on the dome) | 2D |
| **Altocumulus** | mid | mid-frequency cellular rolls/patches | 2D |
| **Altostratus** | mid | featureless grey sheet, sun a diffuse disc | 2D |
| **Stratocumulus** | low | lumpy low sheet, broken rolls | 2D / volumetric |
| **Stratus** | low | flat low overcast; drives the dome to the overcast tint | 2D |
| **Nimbostratus** | low | thick dark rain sheet, no base detail | 2D |
| **Cumulus** (humilis→congestus) | low base | rounded Worley-dominant heaps, lit top / shaded base (HG single-scatter) | **volumetric** |
| **Cumulonimbus** | deep | towering anvil; strong vertical density gradient + anvil spread | **volumetric** |
| **Contrails / mammatus / lenticular** | var | thin line-emitters / pouches / lens stacks — parameter presets | 2D |
| **Fog / ground haze** | 0 | low-lying Rayleigh/Mie-tinted density band | analytic |

The noise vocabulary is fixed: **Perlin (1985), Worley (1996), simplex, OpenSimplex2 (CC0)**.
**Invariant: no wavelet noise** — band-limited anti-aliased noise comes from analytic-derivative fBm
over those primitives instead (§9.4).

### 4.4 Element order, HDR & the float cache

The sky composites in a fixed order into a straight-alpha **float** buffer so highlights near the sun
don't clip:

```
sky dome radiance → aerial-perspective/haze → clouds (2D or volumetric, lit) → sun disc + aureole/flare
     (α = dome ? 1 : 0)      (modulates)              (α += coverage)                 (α += disc coverage)
```

**Float sky cache (locked, user call).** The `TextureLayer` sky cache is a **float `ImageF`** (the
`reflectionEnv` precedent proves per-layer float buffers are fine), so the sun/sunset gradient stays
**banding-free** — the classic "fake sky" tell — and **HDR export** (`.exr`/AVIF-HDR, S55-g) drops in.
An exposure control + a filmic/Reinhard tonemap (published; the ACES-style filmic curve is free)
produces the on-screen 8-bit view via the lcms2 `ColorEngine`, but the cache itself keeps the float
values. (Paper/grass caches stay 8-bit — no HDR need.)

### 4.5 Sky camera & perspective — matching a photo (the key new requirement)

A flat sky texture pasted onto a photo reads as fake because real clouds **foreshorten to a vanishing
point** and the horizon sits at a specific height/tilt. So the sky renders through a **real perspective
camera** (a pinhole projection — ancient and universal), with the clouds laid on **projected altitude
planes** rather than painted flat. Controls (the **full** model, user call):

- **Horizon (pitch)** — a draggable horizon line = camera elevation; sets where sky meets ground and
  how much dome is visible.
- **Field of view / focal length** — matches the target photo's lens; controls how fast clouds
  converge to the vanishing point.
- **Roll** — level/tilt the horizon to match a hand-held shot.
- **Tilt-shift** — a lens shift so the horizon can sit off-centre without keystoning the whole frame
  (architectural/landscape matching).
- **Multi-altitude cloud layers** — cirrus high, cumulus low, etc., each on its **own** altitude plane
  projected **independently**, so near-overhead low clouds are large and race to the horizon while high
  cirrus barely moves — real **parallax/depth** between layers, not one flat sheet.

Each cloud layer's density field is evaluated in **world space on its altitude plane**, then projected
through the camera; overhead cells are large, horizon cells compress and converge. This is what lets a
user set the horizon + rough lens of a target photo and have the generated sky *belong* to it. The
camera also drives the **haze**: aerial perspective thickens along the (now perspective-correct) view
distance to the horizon. Gizmos: a horizon bar, an FOV readout, per-layer altitude handles (§7.3).

---

## 5. Paper / material subsystem

Tactile realism comes from **relief lit at a grazing angle**, not a flat colour texture: a height field
→ normal map → raked-light shading — every step textbook and published (§9.3/§9.4), and the shading math is
*already in the tree* (`extrude_render.cpp` `shade()` — Blinn-Phong + Fresnel-Schlick; we add an
Oren-Nayar diffuse term). This same pipeline is the engine for the **follow-on materials** (wood/marble/
stone/canvas/metal, S55-g) — they differ only in the height field + tint.

### 5.1 Height field (fiber, grain, tooth)
A layered scalar height `h(x,y)` from the noise kit: **base tooth** (fBm micro-relief; amplitude =
roughness, feature size = **Scale**); **fiber/grain** (Worley/cellular + an **anisotropic** stretch
along a grain axis, the grain-direction gizmo). The **tooth model is Curtis et al.'s (SIGGRAPH 1997)
height-field-as-capacity**, reimplemented from the paper. Gabor noise (spectral fibre) is likewise
reimplemented from the paper (Lagae et al. 2009 — its reference code has no valid OSS licence).

### 5.2 Paper *kinds* — laid / chain / wove / felt (public physical facts)
**Laid** (jittered high-freq parallel stripes ~0.7–2 mm), **chain** (sparse perpendicular stripes ~25 mm),
**wove** (isotropic fBm), **felt vs wire side** (coarse-irregular vs fine-regular), **watermark**
(low-freq ± thickness mask from a user shape).

### 5.3 Height → normal → raked light (the tactile step)
**Height→normal: single-pass Sobel/central-difference only.** ⚠ **Invariant:** never an iterative
smoothing scheme and never a weighted image-pyramid normal estimator — one Sobel tap set per pixel,
nothing more. **Shading: Oren-Nayar** rough-diffuse (best matte term) over Lambert, lit by a **low-elevation raking light** so relief
throws micro-shadows; an optional faint Blinn-Phong sheen for coated stock. **Tint/paint** is a
`vec::Paint` (solid/gradient, reusing the gradient renderer), so duotone/gradient stock is free.

### 5.4 Deckle edges & optional print tooth
**Deckle/torn edge** = fBm-thresholded boundary **alpha** + a fringe band (carries the transparent
surround, §3.4). **Tactile print look** = **Floyd-Steinberg** or **void-and-cluster blue-noise** threshold masks —
generic, published screening only; no proprietary hybrid screening algorithm.

### 5.5 Presets
**Bright business-card stock**, **Kraft**, **Laid bond**, **Cold-press watercolour**, **Newsprint**,
**Vellum**, **Linen/canvas** — each a `PaperParams` value; a preset sets every knob, sliders fine-tune
(the `type3d_panel.cpp` preset pattern). Note what this is and is not: we generate a **static
texture**, not a paint-deposition engine that models grain interacting with a brush over time.

---

## 6. The grass subsystem (photoreal, hybrid)

Photoreal grass is blade-level, but the **evidence** (§9.3) — Reeves & Blau's still-image grass paper
(1985), the Perbet-Cani/Boulanger blades-near/texture-far LOD, and every production tool — converges on
a **distance-graded hybrid**, not pure blade instancing. So grass is a **procedural turf base carrying
the far field, with depth-culled blade instancing for the near field**, perspective-aware.

### 6.1 The pipeline (each step has a published source, §9.3)
1. **Camera / ground-plane homography** — a 3×3 `H` maps lawn (X,Z) → image (u,v): foreshortening,
   horizon compression, per-blade footprint placement. (Shares the §4.5 perspective machinery.)
2. **Turf base** — a Perlin/Worley colour field + a low-frequency AO/darkening field, per output pixel.
   Fills gaps *behind/between* blades (no background bleed-through), supplies low-frequency colour, and
   is the **sole representation near the horizon** (where blades collapse sub-pixel).
3. **Density + clump fields** — Worley/Voronoi clump cells (each blade inherits its cell's facing +
   tint) modulated by a Perlin density/height field; **blade count per unit *image* area falls with
   depth** — the key cost lever.
4. **Scatter** — **single-class** Poisson-disk/blue-noise points on the ground plane (+ Poisson tufts
   for clumps), projected through `H`. (Single-class deliberately — no inter-class-spaced multi-class
   sampling, §9.3.)
5. **Blade geometry** — a quadratic/cubic **Bézier** ribbon per point, width taper 1.0→0.7→0.3,
   per-blade jittered height/curvature/facing; **width-correct** sub-pixel distant blades to fight
   aliasing (Jahrmann–Wimmer).
6. **Shade each blade** — **Kajiya-Kay/Banks** tangent lighting (`Ka + Kd(1−(T·L)²)^(Pd/2) +
   Ks(1−(T·H)²)^(Ps/2)`) for sheen + **tip highlight**; **wrap/Half-Lambert + thickness `dot(V,−L)`**
   for **backlit translucency**; base→tip colour gradient × per-blade Perlin jitter (+ a few dead/yellow
   blades); **root-darkening AO** `pow((y−root)/h, 1.5)` + Reeves-Blau probabilistic self-shadow in
   dense clumps.
7. **Composite** — **back-to-front** (far→near, up toward the horizon), alpha-over the turf base.

### 6.2 Cost & gizmos
CPU cost at 24–36 MP is **single-digit-to-low-tens of seconds** and **bounded by the depth-graded
density** (you never pay for horizon blades — that's turf); it degrades gracefully. Bake-on-Create with
a progress bar; the proxy previews. Gizmos (§7.3): a **grass camera** (horizon/FOV, shared with the sky
camera widget) and a **wind/lean** vector (blade bend direction). An optional prefiltered aggregate
(Deep Appearance Prefiltering) is a later refinement for the far band.

---

## 7. UI design — the modal, the rail, the gizmos

### 7.1 Shape & wiring
A **modal** `TextureGeneratorDialog : Fl_Double_Window` (the user's call). Menu: add to **Layer** after
"Merge Down" (`app_window.cpp:219-223`) — `menu->add(_("&Layer/Text&ure Generator..."), 0,
cbTextureGenerator, win)`, a thunk beside `cbFill`/`cbSettings`, calling **`openTextureGenerator()`**
modelled on `openFillDialog()`/`openSettings()`. A `TextureGenHost` of `std::function`s (the
`FillHost`/`SettingsHost` pattern) decouples it: `foreground()`, `renderProxy(params, w, h)`,
`commit(params)` → `AddLayerCommand`/`SetTextureCommand`; `set_modal()` + `seed()` before `show()`.

### 7.2 Layout — a generator **rail**, calm not a cockpit
Because the suite **grows** (sky/paper/grass now, five more later), the generator chooser is a
**settings-style `NavItem` rail** (the `settings_dialog.cpp` machinery), not a header segmented control
— the rail scales, and the preview pane keeps the width. Then per-generator controls + progressive
disclosure:

```
┌─ Texture Generator ──────────────────────────────────────────────────┐
│ ┌─────────┐  Preset: [ Golden hour        ▾ ]          [ Randomize⚄ ] │
│ │ ▸ Sky   │ ┌──────────────────────────┬────────────────────────────┐ │
│ │   Paper │ │ CONTROLS (scrollable)    │  LIVE PREVIEW + GIZMOS      │ │
│ │   Grass │ │  ☑ Sky  ☑ Sun            │    ╭────────────────────╮   │ │
│ │  ─────  │ │  ☑ Clouds ☑ Haze         │    │  (low-res proxy)    │   │ │
│ │  Wood…  │ │  Cloud type [Cumulus ▾]  │    │    ☀ sun-on-dome    │   │ │
│ │  Marble…│ │  Quality [2D · Volumetric]│   │    ── horizon bar   │   │ │
│ │  (soon) │ │  Scale ▭▭▭▭●▭▭            │    │    ↗ wind vector    │   │ │
│ └─────────┘ │  ▸ Camera (horizon/FOV/  │    ╰────────────────────╯   │ │
│             │    roll/tilt/layers)     │   (Fit · 1:1 · pan)         │ │
│             │  ▸ Advanced (turbidity…) │                            │ │
│             ├──────────────────────────┴────────────────────────────┤ │
│             │ ⓘ Creates a new "Sky" layer.        [ Cancel ] [Create]│ │
│             └───────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────────────┘
```

- **Element toggles** (Sky/Sun/Clouds/Haze; paper: Laid/Deckle/Print; grass: Blades/Turf) are
  `CheckBox`es up top — the "independently disable-able" ask at a glance.
- **Progressive disclosure** — a compact *Basics* set always visible; **▸ Camera** (horizon/FOV/roll/
  tilt-shift/cloud-layer altitudes) and **▸ Advanced** (turbidity/ozone/albedo, per-octave cloud,
  exposure/tonemap, fibre anisotropy, blade density) collapsed by default. Presets + gizmos are the
  antidote to the "alien control panel."
- **All widgets in-tree:** `ScrubSlider` (incl. **Scale**), `Dropdown` (preset/cloud-type/paper-kind/
  quality), `SwatchChip`+`ColorFlyout` (tint/paint), `CheckBox` (toggles).

### 7.3 The gizmos (the praised `GizmoCanvas`)
The **preview pane *is* a gizmo canvas** — reuse `type3d_panel.cpp`'s `GizmoCanvas` (SDF-coverage
software-AA: `stroke`/`fillDisc`/`strokeCircle`/`fillDiamond`, hover, `grabAt`/`dragBy`): render the
low-res proxy into the ground, overlay draggable SDF gizmos, same `grabAt`→hover→`dragBy`→coalesced-
commit loop so the **preview updates live** and undo is one step per gesture.

- **Sky:** **sun-on-dome/compass** (the `Grab::Light` hemisphere + a compass ring, two-way bound to
  time/place), **camera gizmos** — a **horizon bar** (pitch), an **FOV** framing rect, a **roll**
  handle, **tilt-shift** nub, and **per-altitude cloud-layer handles** (§4.5); a **wind vector** arrow.
- **Paper:** **grain-direction ring** (the `RingZ` gizmo) + **raking-light dot** (`Grab::Light`).
- **Grass:** the shared **camera** (horizon/FOV) + a **wind/lean** vector.

### 7.4 Live preview, presets, seed
**Live preview** at a **low-res proxy** (fit-to-pane, ≤512²), frame-coalesced (the Fill dialog's
`requestPreview`/timer throttle) so a drag never pins the UI thread; a **Fit / 1:1 / pan** control
inspects grain/blades at pixel scale. On **Create**, the full-res layer generates (progress bar for the
heavy volumetric/grass cases). **Presets** set the whole `spec` (with a "Custom" readout when sliders
diverge). **Seed + ⚄ Randomize** — reproducible from (seed, params, resolution).

---

## 8. Rendering lane, performance & determinism

### 8.1 CPU reference cost (the permanent lane)
All generators are per-pixel/per-blade parallel over the compositor's `parallelFor`. At **24–36 MP**,
multithreaded: **sky dome+haze+sun** sub-second; **2D clouds** a few seconds; **volumetric clouds**
seconds-to-tens (bake-on-Create, progress bar); **paper** 1–3 s; **grass** single-digit-to-low-tens
(bounded by depth-graded density). The **live preview** runs at the proxy, so interaction stays smooth.

### 8.2 The proxy strategy
Two resolutions: a **proxy** (fit-to-pane) for live preview/gizmo drags, and **full** document
resolution on Create. Because generation is resolution-independent (§8.3), the proxy is a *faithful*
preview (same params, fewer pixels) — not an approximation that shifts on commit.

### 8.3 Resolution independence, Scale, determinism
- **Deterministic noise.** Every primitive is **hash-seeded** on `(seed, integer-lattice coord)` — no
  global state, no stream order — so output is identical at any resolution, tile order, or thread
  count, CPU or GPU (the resynthesizer's hashed-RNG discipline).
- **Scale (context-sensitive, honest).** For **paper/grass**, Scale = feature size **per physical unit**
  (grain/blade stays the same real size at 72 or 600 DPI). For **sky**, Scale = cloud feature size; the
  *dome/camera* is physical (framed by FOV/horizon, §4.5). One slider, labelled per generator.
- **Tiling.** Primary path evaluates the full canvas in one pass → no repeat to seam. A **"Tileable"**
  toggle (periodic noise) is a minor later add for exportable swatches (§9 fork 10).

### 8.4 Where the Vulkan lane slots (S60)
Per-pixel generators are ideal compute workloads; the volumetric march and grass shading map to compute
too. The GPU lane (S55-h) follows the in-tree pattern (`fill.comp`/`composite_blend.comp`, VMA `rgba32f`
targets, `docs/vulkan.md`), reading back — or, under a GPU-resident compositor, **not** reading back.
The **CPU reference stays permanently** as oracle + headless-test lane, with **CPU↔GPU parity golden
tests** (the 3D-text dual-lane discipline). Hash-seeded noise makes bit-comparable parity achievable.

### 8.5 The Vulkan compute lane as built (S55-h, 2026-07-16)

**Architecture.** `render::TextureGpu` (`src/render/texture_gpu.{hpp,cpp}`) is a persistent
compute lane in the `GpuCompositor` mould: one `VulkanContext`, a VMA allocator, two pipelines
(`shaders/texture_sky.comp`, `shaders/texture_paper.comp`) sharing one descriptor layout, a
VMA-allocated **`rgba32f` storage-image target** (grow-on-demand) and a mapped staging readback.
Each render is CPU-**cooked** in double first — the Hosek-Wilkie configs (via the vendored
reference), `cookAtmosphere`'s od/Ψ_ms tables, the projected Yale-BSC star field + screen-bin
CSR, the Meeus moon frame, the deck constants (`cloudVolumeSpec` + a transcription of the
golden-pinned `CloudTypeSpec` table and cook preamble from `sky_render.cpp` — parity tests pin
the copy) — so the shaders carry only the per-pixel math. The LRO albedo + LOLA elevation
tables upload once as packed-uint SSBOs and are sampled with the CPU's own bilinear (texelFetch
arithmetic, not hardware filtering, so the filtering matches).

**The seam.** `core::texture::setTextureRenderOverride` (the `setExtrudeRenderOverride`
precedent): `renderTexture` consults the override first and falls back to the CPU renderer on
any refusal, so `refreshTextureCaches`, the dialog's `TextureRenderWorker` proxy/bake and the
headless op all ride the lane transparently. The app installs it lazily
(`app_window.cpp`, mutex-serialised — the worker thread and the UI thread can race); the test
binary NEVER installs it, so the byte-pinned goldens stay CPU forever.

**Progress/cancel at dispatch granularity.** The window renders in 64-row bands, one submit +
fence each; `TextureRenderProgress::rowsDone` advances per band and cancellation is honoured
between bands (a cancelled render returns the all-or-nothing EMPTY result). 64-bit hashing runs
on `uvec2` lo/hi words (`umulExtended`) — exact integer transcription of `noise.hpp`, no
`shaderInt64` device dependence.

**Kernel coverage.** GPU-resident: **Sky** — the complete per-pixel path (HW dome, twilight
single-scattering + Ψ_ms, night floor, star splats, the moon incl. libration/relief/phase
modes + 4× supersampling, sun disc/aureole/glare, the **lens flare** (ghost train / halo /
starburst, cooked host-side by the same `cookLensFlare` transcription), haze, ground band, the
2D deck stack AND the volumetric march with slant-scaled steps + per-frame-pixel jitter,
tonemap + TPDF dither) — and **Paper** — the full §5 pipeline (tooth/Gabor fibre/laid/felt
height taps ×9, Sobel, Oren-Nayar + sheen, print tooth, deckle alpha), with the host quantising
through the CPU's own `q()`. CPU-fallback (the lane returns `false`): **Grass** (blade
instancing + painter's compositing is not a per-pixel kernel), the **S55-g materials**
(wood/marble/stone/canvas/metal — §5-engine recipes with their own param structs; natural
next ports), skies with **more than 8 cooked decks**, and any generator/feature the build does
not recognise.

**Parity, not bit-equality** (`tests/test_texture_gpu.cpp`, device-gated): double CPU vs float
GPU means the bar is "the same picture" — mean error ≪ one display step plus a ≤0.5% budget of
branch-flip outlier pixels. Measured on RADV (RX 6600 XT): sky meanAbs ≤ 5e-7 (max 0.003,
zero outliers), paper ≤ 1.3e-4 bytes (zero outliers); a GPU window crop is **byte-exact**
against the GPU full frame, and same-device renders are deterministic (pure per-pixel function,
no atomics/shared memory). Two float-lane traps are documented in the shaders: the Worley ring
expansion is written as straight-line **guarded shells** (a data-dependent ring loop wrapping
the nested shell scans miscompiles on RADV/ACO — whole waves' stores vanished for offset
windows; bisected via a sentinel clear), and the paper print-tooth cell corrects
`floor(fx / grain)` against exact multiplies because drivers may lower division to a
reciprocal multiply (2.5 ULP), which lands integer-exact quotients one cell low.

**Cache residency.** The result is read back into the ordinary `Image`/`ImageF` caches because
today's compositor samples `TextureLayer` caches on the CPU (`compositor.cpp`). The documented
S60 seam: the lane's `rgba32f` target already lives in device memory, so a GPU-resident
compositor takes over by consuming the `VkImage` and dropping the final
`vkCmdCopyImageToBuffer` — no speculative plumbing was built ahead of it.

**Measured cost** (RX 6600 XT vs the multithreaded CPU lane, 480×320, release, the
`bench: texture lane costs` probe in `test_texture_gpu.cpp`): default volumetric sky
4767 ms → 92 ms (**52×**); 2D-deck sky 169 ms → 3.7 ms (**46×**); night sky with stars +
moon 338 ms → 51 ms; paper 80 ms → 6.7 ms (**12×**). Cold-start adds ~10 ms (pipeline +
table warm-up); the S55-f dialog's live proxy and Create bake ride these numbers directly.

---

## 9. Technique lineage & design constraints

Two axes run through this section: the **published lineage** each generator descends from (cite it,
in the source header — §9.7), and the **constraints** that lineage is implemented under. **No ML
anywhere** — no neural sky replacement, no neural texture synthesis. Where a constraint is marked
⚠ **Invariant**, it is a hard limit on the implementation, not a preference.

### 9.1 Volumetric clouds

The lane is clean-room from **published sources only**, no engine code copied: Schneider & Vos,
*Real-Time Volumetric Cloudscapes of Horizon Zero Dawn* (SIGGRAPH 2015 / GPU Pro 7 / 2017 Nubis);
Häggström MSc thesis 2018; Hillaire, *Physically Based Sky, Atmosphere and Cloud Rendering*
(SIGGRAPH 2016); Bauer, *Atmospheric World of RDR2* (SIGGRAPH 2019). The physics is Beer–Lambert
(1729/1852), the Henyey–Greenstein phase function (1941) and Rayleigh (1871) / Mie (1908)
scattering; the noise is classic Perlin (1985) and Worley (1996).

⚠ **Two invariants on how the volume is represented and marched:**

1. **The cloud is an *implicit* procedural density field** (Perlin–Worley, analytically sampled) —
   **never a polygon set**, never a "volume object" carrying its own volume characteristics.
2. **A single-medium Beer–Lambert march accumulates transmittance through that field directly.**
   There is **no** separately-rendered, globally-characterized scene image, and **no** blend of "a
   proportion of the volume's characteristics" into one by ray-intersection extent. One march, one
   medium, straight-alpha in-scatter out.

Both are load-bearing and are restated in the `cloud_volume.cpp` header (§9.7).

### 9.2 Sky & atmosphere / solar — licences and provenance

- **Hosek–Wilkie** reference implementation is **3-clause BSD** — adopt it and retain its header.
- **Preetham / Perez / CIE** are published; take the coefficients from the papers or Radiance
  `gendaylit`, **not** the paid ISO 15469 PDF (document copyright ≠ technique copyright).
- **Bruneton** *Precomputed Atmospheric Scattering* is **BSD-3** (deferred, §10-4).
- ⚠ **Do NOT copy Sean O'Neil's GPU Gems 2 code** — no OSS licence. Reimplement the physics if his
  look is wanted.
- Solar: **clean-room NOAA/Meeus** from the published equations. ⚠ **Never ship NREL's `spa.c`** —
  its header is noncommercial / no-redistribution, doubly GPL-incompatible. **SolTrack (LGPL-3)** and
  **`freespa` (GPLv3)** are shippable alternatives if SPA-grade accuracy is ever wanted.

### 9.3 Grass — the published building blocks, and three "don'ts"

Every step of §6.1 has a published source: particle systems (Reeves 1983; Reeves & Blau 1985),
Lambert / Kajiya-Kay / wrap shading, obscurance AO (Zhukov 1998), painter's compositing (Newell 1972
/ Porter–Duff 1984), single-class Poisson-disk sampling (Cook 1986 / Bridson 2007), classic Perlin
(1985) and Worley (1996) noise, Bézier blades (Jahrmann & Wimmer, I3D 2017), blades-near/texture-far
LOD (Perbet & Cani, I3D 2001; Boulanger et al., IEEE CG&A 2009).

⚠ **Three invariants, none of which our design wants anyway:**

1. **No reference-image-guided placement with a validation loop** — scatter is driven by the
   procedural density/clump fields and the seed, never by matching against a supplied reference
   image and iterating until it validates.
2. **Single-class Poisson sampling only** — no inter-class-spaced multi-class scatter.
3. **Plain back-to-front polygon compositing** — no tiled hair-strand overlay scheme, no adaptive
   volumetric-element shading.

### 9.4 Noise, paper, height→normal, shading

Perlin (1985) and Worley (1996) noise; **OpenSimplex2 is CC0**; simplex noise; improved-gradient
noise. Curtis et al., *Computer-Generated Watercolor* (SIGGRAPH 1997) for the paper-tooth
height-field-as-capacity model, reimplemented from the paper. Gabor noise from Lagae et al. 2009,
reimplemented (its reference code has no valid OSS licence). Oren–Nayar (1994) / Blinn (1978) /
Lambert shading; Sobel & Feldman (1968) gradients; Floyd–Steinberg and void-and-cluster blue-noise
screening.

⚠ **Invariants:**

- **No wavelet noise.** Band-limited anti-aliased noise comes from analytic-derivative fBm over the
  primitives above.
- **Height→normal is a single-pass Sobel / central difference only** — never an iterative-smoothing
  or weighted-pyramid normal estimator (§5.3).
- **Screening is generic published blue noise only** — no proprietary hybrid screening algorithm.
- **No exemplar / PatchMatch texture synthesis anywhere**, and no external-image parameter, particle
  emitter, or ink/toner data path: every generator is purely procedural, seeded and analytic.

### 9.7 Source-header lineage convention

Per project precedent, each generator file carries a lineage header. Sky/paper headers as in the
prior revision; the **volumetric-cloud** and **grass** headers:

```
// Volumetric cloud texture generator — procedural, CPU-first, ML-free. Clean-room from PUBLISHED
// sources only (no engine code copied): Schneider & Vos "Real-Time Volumetric Cloudscapes of Horizon
// Zero Dawn" (SIGGRAPH 2015 / GPU Pro 7 / 2017 Nubis); Häggström MSc thesis 2018; Hillaire "Physically
// Based Sky, Atmosphere and Cloud Rendering" (SIGGRAPH 2016); Bauer "RDR2" (SIGGRAPH 2019). Physics:
// Beer-Lambert (1729/1852), Henyey-Greenstein phase (1941), Rayleigh(1871)/Mie(1908). Noise: classic
// Perlin (1985) + Worley (1996). No ML. No third-party engine source.
// INVARIANTS: the cloud is an IMPLICIT procedural density field (never a plurality of polygons), and
// a single-medium Beer-Lambert march accumulates through it directly (never a proportion-by-ray-
// intersection blend of a separate volume into a globally-characterized scene image).
```

```
// Grass texture generator — distance-graded hybrid (procedural turf base + depth-culled blade
// instancing). Technique lineage: Reeves & Blau, "Approximate/Probabilistic … Structured Particle
// Systems" (SIGGRAPH 1985, still-image grass); Perbet & Cani (I3D 2001) + Boulanger et al. (IEEE CG&A
// 2009) blades-near/texture-far LOD; Jahrmann & Wimmer (I3D 2017) + Sucker Punch GDC 2021 (Bézier
// blades); Kajiya & Kay (SIGGRAPH 1989) / Lengyel et al. (I3D 2001) tangent-fiber lighting; wrap/
// thickness translucency (Half-Lambert; GPU Gems 3); obscurance AO (Zhukov 1998); single-class
// Poisson-disk (Cook 1986 / Bridson 2007) + Voronoi/Worley clumping; back-to-front Porter-Duff.
// Noise: classic Perlin (1985) + Worley (1996). No ML.
// INVARIANTS: no reference-image-guided placement + validation loop; single-class Poisson only (no
// inter-class spacing); plain painter's compositing (no tiled hair-strand overlays, no adaptive
// volumetric-element shading).
```

---

## 10. Decisions (settled 2026-07-03) & residual questions

Every fork raised in the prior draft is now settled with the user:

1. **Live `TextureLayer`** (params editable, re-open to tune) + one-click Rasterize. **Settled: live.**
2. **Sun positioning** — **both** direct-drag *and* time/date/place, two-way bound, **both in v1.**
3. **Clouds** — **both lanes ship:** 2D layered (interactive default) *and* **volumetric**
   (§9.1, bake-on-Create). The user wanted volumetric sooner.
4. **Bruneton scattering** — **deferred** (analytic Rayleigh/Mie suffices; BSD-3 drop-in later).
5. **HDR** — **float `ImageF` sky cache now** (banding-free gradients; HDR export drops in). Paper/grass
   stay 8-bit.
6. **Solar source** — **clean-room NOAA/Meeus** (no dep). Never NREL `spa.c`.
7. **Generator chooser** — a **settings-style rail** (the suite grows; the rail scales).
8. **Preview** — **modal with an in-dialog gizmo/preview pane** (+ the proxy).
9. **Scope/name** — **"Texture Generator"** (broad, locked) as a **generator suite**: Sky, Paper, Grass
   at launch; Wood/Marble/Stone/Canvas/Metal follow-ons (S55-g).
10. **Grass technique** — **hybrid** (turf base + depth-graded blade instancing), evidence-backed (§9.3).
11. **Sky camera** — the **full** model: horizon/FOV/roll/**tilt-shift** + **multi-altitude cloud
    layers** for true parallax (§4.5).
12. **Tileable export** — non-tiling default + a later "Tileable" toggle.

**Residual (small, resolve in-session):** the exact cloud-type↔knob presets; whether the grass camera is
literally the sky camera widget or a trimmed variant; per-generator Scale labels.

---

## 11. References

- **Sky:** Hosek & Wilkie, *An Analytic Model for Full Spectral Sky-Dome Radiance* (2012) + sun addendum
  (2013), **3-clause BSD** — https://cgg.mff.cuni.cz/projects/SkylightModelling/ ; Preetham, Shirley,
  Smits, SIGGRAPH 1999; Bruneton & Neyret, *Precomputed Atmospheric Scattering* (2008/17, BSD-3) —
  https://github.com/ebruneton/precomputed_atmospheric_scattering ; Nishita 1993.
- **Solar:** NREL/TP-560-34302 (equations, free) — *not* `spa.c`; SolTrack (LGPL-3)
  https://github.com/MarcvdSluys/SolTrack ; freespa (GPLv3) https://github.com/IEK-5/freespa ; NOAA
  https://gml.noaa.gov/grad/solcalc/ .
- **Clouds (volumetric):** Schneider & Vos, *Real-Time Volumetric Cloudscapes of Horizon Zero Dawn*,
  SIGGRAPH 2015 / GPU Pro 7 / 2017 Nubis — https://advances.realtimerendering.com/s2015/ ,
  https://www.guerrilla-games.com/read/the-real-time-volumetric-cloudscapes-of-horizon-zero-dawn ;
  Häggström MSc thesis 2018; Hillaire, *Physically Based Sky, Atmosphere and Cloud Rendering*, SIGGRAPH
  2016; Bauer, *Atmospheric World of RDR2*, SIGGRAPH 2019 https://advances.realtimerendering.com/s2019/ .
- **Noise:** Perlin, *An Image Synthesizer*, SIGGRAPH 1985; Worley 1996; OpenSimplex2 (CC0)
  https://github.com/KdotJPG/OpenSimplex2 . **⚠ No wavelet noise** (§9.4).
- **Paper:** Curtis et al., *Computer-Generated Watercolor*, SIGGRAPH 1997;
  Oren & Nayar, SIGGRAPH 1994; Blinn 1978; Sobel & Feldman 1968; Gabor noise (Lagae et al. 2009).
- **Grass:** Reeves & Blau, *Approximate and Probabilistic Algorithms for … Structured Particle Systems*,
  SIGGRAPH 1985 https://dl.acm.org/doi/10.1145/325165.325250 ; Perbet & Cani, I3D 2001; Boulanger et al.,
  IEEE CG&A 2009; Jahrmann & Wimmer, I3D 2017 https://doi.org/10.1145/3023368.3023380 ; Kajiya & Kay,
  SIGGRAPH 1989; Lengyel et al., I3D 2001 https://hhoppe.com/fur.pdf ; Sucker Punch, *Procedural Grass in
  Ghost of Tsushima*, GDC 2021; AMD GPUOpen procedural grass (2024); Bridson, Poisson-disk, 2007.
- **In-tree precedents:** `src/ui/type3d_panel.cpp` (`GizmoCanvas`); `src/core/layer.hpp` (`TextLayer`
  cache + `reflectionEnv` float buffer); `src/core/commands.hpp` (`SetTextCommand`, `AddLayerCommand`);
  `src/render/compositor.cpp:760` (leaf-layer draw); `src/ui/fill_dialog.*` (modal + live preview);
  `src/ui/settings_dialog.cpp` (two-pane modal, `NavItem` rail, `set_modal`); `src/core/vector/paint.hpp`
  + `raster.cpp` (Paint + gradient renderer); `src/core/text/extrude_render.cpp` (Blinn-Phong/Fresnel CPU
  shading + CPU↔GPU parity lane); `src/render/compute_fill.*` (compute-shader pattern).
- Companions: **`docs/type-tool.md`**, **`docs/layer-effects.md`**, **`docs/vector-model.md`**,
  **`docs/compositor.md`**, **`docs/vulkan.md`**, **`docs/third-party-licenses.md`**.

## 12. The almanac clock (info panel, 2026-07-16)

The "Sky at this date & place" panel under the preview carries a small drawn **analog clock** in a
fixed slot on its right: hour + minute hands over subtle tick marks, the observer-local calendar
date beneath the face and the wall time ("HH:MM solar") beneath that. It reads the **observer's
local mean-solar time** — UTC + longitude/15, the same convention as `utcHourToLocal` in
`sky_almanac.hpp`, honest without a timezone database (solar noon reads 12:00) — with the calendar
date stepped when the offset crosses midnight. The face is tinted subtly by the almanac's state the
panel already computes (no extra almanac calls): warm in daylight (sun ≥ −0.833°), violet through
the twilight bands, blue at night (< −18°). All inks come from the live palette, so a re-theme
follows for free; the panel's label/value rows keep their layout and simply clip clear of the slot.

State lives in `ui::SkyClockState` (dialog header), refreshed by every `updateSkyInfo()` and
exposed via `skyClockForTest()` — the headless tests assert the stored clock state (local
date/time arithmetic across midnight and the day/night tint driver), never pixels.

## 13. The world-map place picker (solar section, 2026-07-16)

The solar section's curated-city dropdown (`ui::CityPicker`, retired) is replaced by a **map
place picker** (`src/ui/map_picker.*`): a compact "Place" control showing the nearest catalogued
city + the coordinates, which opens a `ui::BubbleFlyout` carrying a 360×180 equirectangular world
map. Click or drag places a **pin** — the pin is the observer's latitude/longitude, reported live
through the same sink the dropdown used (`setObserverLatLon`, so the sun/master clock/info panel
all follow), with a readout line (coordinates · nearest city · distance) under the map. Dragging
within ~5 px of a catalogued city snaps the pin onto it exactly; the catalogue
(`core/texture/city_catalog.hpp`) is unchanged and its cities draw as quiet dots (the snap
targets). The control keeps the dropdown's `showNearest` semantics: at a catalogued city it shows
the city's own name, elsewhere "Nearest: <city>".

Map data is **Natural Earth 1:110m land** (public domain; see docs/credits.md), vendored as a
generated table: `tools/gen_world_map.py` simplifies the GeoJSON rings (Douglas-Peucker, 0.25°),
quantizes to int16 centidegrees and emits `src/ui/world_map_data.{hpp,cpp}` (122 rings, 2,628
vertices, ~10.5 KB of points — script and generated file both committed, the star-catalogue
pattern). The flyout even-odd scanline-fills the rings into a land mask once (holes like the
Caspian fall out for free), tints it in live palette inks (water/land/coastline/30° graticule) and
rebuilds the tint only on a re-theme. Chassis rules follow ColorFlyout exactly: built as a
pre-show child of the dialog, dismissed by the host's `dismissActiveMapFlyoutOnOutsideClick` +
Esc, `setAvoidRect(preview)` so the live sun stays watchable while the pin drags. All interaction
claims the full press/drag/release gesture (the house click convention). Pure pieces
(`map_detail`: projection math, mask rasterizer, place labels) are unit-tested headlessly in
tests/test_map_picker.cpp, along with the pin/pick flow and the dialog wiring.

## 14. Lens flare (sky, 2026-07-16)

An optional **screen-space photographic lens flare** off the sun (`SkyParams::enableLensFlare`,
default **off** so every pre-flare document keeps its exact bytes; `flareStrength` 0..1). The
mechanism is the classic one: the sun's screen position S comes from the same `SkyCamera::project`
the stars use, and a **fixed ghost train** of soft hexagonal-aperture sprites sits at
`C + t·(S − C)` (C = frame centre) for a frozen set of `t` — including negative, past-centre
positions — with per-ghost size/energy/coating tint; **chromatic fringing** evaluates the channels
at slightly different radii (blue focuses tighter). One wide **halo ring** circles C at a radius
riding `|S − C|`, dispersion-fringed (red outside). A six-spike **starburst** (thin cosine-power
streaks, radially exponential) passes through S, its spoke phase the flare's one random draw —
frozen per seed. Visibility = strength × sun-elevation fade (the flare is the SUN's; the moon
casts none) × a smooth off-frame fade as S leaves the view (dead when the sun is behind the image
plane) × the glare block's own inherited signals: the Beer-Lambert transmittance dims a low sun's
flare and `haze01` mutes the distinct ghosts (no new occlusion machinery). Every term is a pure
analytic function of the frame pixel + constants cooked once from the full frame — no image-space
post pass — so the flare is row-parallel, window-crop byte-exact and deterministic per seed. It
composites in linear HDR **over the cloud decks** (lens light: a cloud cannot occlude a
reflection born inside the lens), additive over an opaque dome and coverage-carrying over
transparency.

**Lineage & constraints:** the screen-space ghost/halo/starburst-along-the-axis technique is
published in Kilgard's OpenGL lens-flare tutorial (1999–2000) and King's "2D Lens Flare" in *Game
Programming Gems 1* (2000) — that is the lineage this implements. Hullin et al. 2011
(physically-based flare) is background only and is ⚠ **deliberately NOT built**: no
lens-prescription simulation, no paraxial/matrix optics, no modelling of reflections between lens
elements, and no precomputed angle-indexed ghost look-up tables. The UI is a self-contained toggle +
strength pair in the sky's Advanced section.
