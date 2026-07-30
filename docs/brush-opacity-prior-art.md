# Provenance — the brush engine's transcribed mechanics

*(Originally "the wash / alpha-darken opacity accumulation". Widened 2026-07-27 — see §"Posture".)*

**What this is.** A provenance record for the mechanics of Mosaic's brush engine that are faithful
re-implementations of published, dated, GPL/LGPL-licensed source: what each one implements, and
which upstream file, licence and dates it was transcribed from. It exists because Mosaic ships those
mechanics enabled by default, exactly as transcribed, rather than designing them away
(maintainer decision, 2026-07-18 for the wash path; generalized to the whole brush arc 2026-07-27).

**Why it has to exist.** The posture is *"transcribe exactly, ship enabled, credit the dated
published source"*. That only holds for as long as the transcription's provenance is on the record —
an undocumented mechanic is one whose upstream goes uncredited. So: **every mechanic transcribed
under this posture is listed here, with its source file, its SPDX licence and its observed copyright
dates.**

**What this is NOT.** Not an assessment of any third party. It records where each mechanic came
from and under which licence, and nothing else.

## What Mosaic implements

`src/core/brush/brush_engine.cpp`:

- `washAlphaDarkenAlpha()` — the per-dab opacity-**ceiling** wash accumulation step: a dab strives
  toward its own opacity (or the stroke's running average where that is higher) and never steps
  down. Taken only when a preset's Opacity option is genuinely dynamic under Wash mode
  (`optionIsDynamic`); the static path is a separate, byte-pinned branch.
- `blendAverageOpacity()` — the one-sided per-dab running average (rises to a louder dab instantly,
  decays toward a quieter one at 0.1/dab) that the step reads.

The exact arithmetic and its derivation live in `docs/brushes.md §6.2` (which refers to the upstream
neutrally as "the reference").

## The source it is transcribed from

Krita (KDE). Reference checkout: **Krita 6.0.2.1** (`KRITA_VERSION_STRING "6.0.2.1"`). The mechanism
is Krita's *indirect-painting* wash — dabs composite into a stroke-scoped temporary at `ALPHA_DARKEN`
in its default *creamy* parameterization, and a per-dab running average governs the strive-toward-the-
ceiling behavior. The upstream sources, with the SPDX license and copyright-header dates observed in
the reference checkout:

| Mosaic | Krita upstream | file | SPDX | header dates |
|---|---|---|---|---|
| `washAlphaDarkenAlpha` (alpha-channel specialization) | `KoCompositeOpAlphaDarken` — the ALPHA_DARKEN op, creamy default | `libs/pigment/compositeops/KoCompositeOpAlphaDarken.h` | LGPL-2.0-or-later | 2006, 2011 |
| `blendAverageOpacity` | `KisPainter::blendAverageOpacity` | `libs/image/kis_painter.cc` (decl. `kis_painter.h`) | GPL-2.0-or-later | 2002–2011 |
| the average applied per rendered dab | `KisDabRenderingQueue` | `plugins/paintops/defaultpaintops/brush/KisDabRenderingQueue.cpp` | GPL-2.0-or-later | 2017 |
| the wash/indirect gate + the opacity option split (`useStrength=false`) | `kis_brushop.cpp` / `KisOpacityOption` | `plugins/paintops/defaultpaintops/brush/kis_brushop.cpp` | GPL-2.0-or-later | 2002–2010 |

Mosaic is GPLv3; both GPL-2.0-or-later and LGPL-2.0-or-later upstream are license-compatible for a
re-implementation inside a GPLv3 work.

Mosaic's version is a **transcription, not a copy**: it is specialized to a single-colour source over
an alpha coverage channel (Mosaic's coverage buffer *is* the `ALPHA_DARKEN` temporary's alpha) and
re-derived in Mosaic's own float arithmetic. It reproduces the upstream *behavior*, which is exactly
why the upstream is credited here.

## The brush ENGINES, transcribed *(added 2026-07-27)*

Same reference checkout (**Krita 6.0.2.1**), same posture. Each row is a mechanism Mosaic implements
by transcription; the design record for each is the cited `docs/brushes.md` section, which states
what was transcribed, what was deliberately left out, and every deviation.

| Mosaic | Krita upstream | file | SPDX | header dates | design record |
|---|---|---|---|---|---|
| the smudge walk (`stampSmudgeDab`, `SmudgeParams`, the legacy strategy's constants) | `KisColorSmudgeOp` + its legacy strategy | `plugins/paintops/colorsmudge/` | GPL-2.0-or-later | 2002–2013 | §6.6c |
| `applyScatter` / `applyMirror` | `KisScatterOption` / `KisMirrorOption` | `plugins/paintops/libpaintop/` | GPL-2.0-or-later | 2008–2022 | §6.6d |
| the Spacing cadence scale; `sharpnessThreshold` + `applySharpnessSnap` | `KisSpacingOption` / `KisSharpnessOption` | `plugins/paintops/libpaintop/` | GPL-2.0-or-later | 2008–2022 | §6.6e |
| `applyColorDynamics` → `hsvAdjust` (the non-compatibility `HSVTransform<HSVPolicy>` branch) | `KisHSVOption` + `kis_hsv_adjustment` | `plugins/paintops/libpaintop/`, `plugins/color/lcms2engine/` | GPL-2.0-or-later | 2009–2022 | §6.6f |
| `StrokeCanvas` / `StrokePainter` — the second engine kind's shape | `KisPaintOp::paintLine` + `paintBezierCurve`'s flat-segment subdivision | `libs/image/brushengine/kis_paintop.cc` | GPL-2.0-or-later | 2002–2004 | §6.6g |
| `rasterizeDdaLine`, `rasterizeThickLine` | `KisPainter::drawDDALine`, `KisPainter::drawLine(width, antialias)` | `libs/image/kis_painter.cc` | GPL-2.0-or-later | 2002–2011 | §6.6g |
| `SketchPainter` (the point-history web, its two carried-over painter states, the distance-density and distance-opacity rules) | `KisSketchPaintOp` + `KisSketchOpOptionData` | `plugins/paintops/sketch/` | GPL-2.0-or-later | 2010 | §6.6g |
| `HairyPainter` (tip-pixels-as-bristles, the single affine transform, the ink-depletion counter and transfer curve, the per-segment additive temporary) + `linearTrajectory` | `KisHairyPaintOp`, `HairyBrush`, `Bristle`, `Trajectory` | `plugins/paintops/hairy/` | GPL-2.0-or-later | 2008–2010 | §6.6g |
| `CurvePainter` (the sliding history window, the quadratic/cubic through it, the per-path opacity) | `KisCurvePaintOp` + `KisCurveOpOptionData` | `plugins/paintops/curvebrush/` | GPL-2.0-or-later | 2008–2011, 2022 | §6.6g |
| `ParticlePainter` (the acceleration ramp, the damped chase integrator, the Wu particle) | `KisParticlePaintOp` + `ParticleBrush` | `plugins/paintops/particle/` | GPL-2.0-or-later | 2010 | §6.6g |
| `ExperimentPainter` (the whole-stroke path and its winding/hard-edge fill) | `KisExperimentPaintOp` | `plugins/paintops/experiment/` | GPL-2.0-or-later | 2010–2011, 2012 | §6.6g |
| `hatchStencil`, `hatchSpinAngle`, `hatchSeparationForParameter` | `KisHatchingPaintOp` + `HatchingBrush` + `KisHatchingOptionsData` | `plugins/paintops/hatching/` | GPL-2.0-or-later | 2008–2010 | §6.6g |
| `bakeTexturePattern` (the pattern → 8-bit mask bake: luma-over-white, brightness, contrast, the two-segment neutral point, the two cutoff policies) | `KisTextureMaskInfo::recalculateMask` | `plugins/paintops/libpaintop/KisTextureMaskInfo.cpp` | GPL-2.0-or-later | 2017 | §6.6h |
| `TextureParams` + the key set and its defaults; the embedded-pattern payload | `KisTextureOptionData` / `KisEmbeddedTextureData` / `KisTextureOption` | `plugins/paintops/libpaintop/KisTextureOptionData.cpp`, `KisEmbeddedTextureData.cpp`, `kis_texture_option.cpp` | GPL-2.0-or-later (the option), LGPL-2.0-or-later (`kis_texture_option.cpp`) | 2012, 2014, 2021, 2022 | §6.6h |
| `textureComposite` (the alpha-source multiply/subtract composite in its four with-strength forms) | `KisMaskingBrushCompositeOp` + `KisMaskingBrushCompositeOpFactory::createForAlphaSrc` | `libs/ui/tool/strokes/KisMaskingBrushCompositeOp.h`, `KisMaskingBrushCompositeOpFactory.cpp` | GPL-2.0-or-later | 2017, 2021 | §6.6h |
| `AirbrushParams`, `airbrushIntervalMs`, the walk's second (timed) cadence and `pumpStationarySpan` | `KisAirbrushOptionData` + `KisPaintOpPluginUtils::effectiveTiming` + `KisPaintOpUtils::effectiveTiming` + `KisDistanceInformation::getNextPointPosition`/`getNextPointPositionTimed` | `plugins/paintops/libpaintop/KisAirbrushOptionData.cpp`, `kis_paintop_plugin_utils.h`, `libs/image/brushengine/kis_paintop_utils.cpp`, `libs/image/kis_distance_information.cpp` | GPL-2.0-or-later; `kis_timing_information.h` is GPL-3.0-or-later | 2014, 2022 | §6.6h |
| `Curve` (the natural-cubic response curve and its `x,y;` serialization); `Curve::toLut` | `KisCubicCurve` + its `floatTransfer` | `libs/image/kis_cubic_curve.cpp` | GPL-2.0-or-later | 2007–2011 | §3.3 |
| the six procedural mask generators; the `.gbr`/`.gih`/`.abr`/`.myb` readers | the corresponding upstream generators and brush/preset readers | `libs/image/brushengine/`, `libs/brush/` | GPL-2.0-or-later | 2004–2022 | §3.4–§3.7, §7 |

⚠ **One licence note on the airbrush row.** Its timing types are spread over four upstream files and
one of them — `libs/image/kis_timing_information.h`, which carries the `LONG_TIME` constant and the
timed-spacing accessors — is marked **GPL-3.0-or-later** rather than GPL-2.0-or-later. Mosaic is
GPLv3, so that is compatible in the same direction as the rest; it is called out because it is the
one row in this table whose upstream licence is not the family the others share.

Every one of these is a **transcription, not a copy**: re-derived in Mosaic's own types and
arithmetic (a coverage channel rather than a paint device, `StrokeState` rather than
`KisPaintInformation`, `SplitMix64` rather than the upstream RNGs), specialized to what Mosaic's
engine can express, and documented deviation by deviation in the cited sections. It reproduces the
upstream *behavior*, which is exactly why each upstream is credited here. Mosaic is GPLv3 and the
upstream is GPL-2.0-or-later / LGPL-2.0-or-later, which are licence-compatible for a
re-implementation inside a GPLv3 work.

⚠ **This table is part of the ship decision, not documentation of it.** A mechanic transcribed under
this posture without a row here is a mechanic whose provenance has not been written down. Add the row
in the same commit as the code.

## Dates — what is source-anchored, and what still needs pinning

- **Source-anchored** (copyright headers observed above): the ALPHA_DARKEN composite op carries
  2006/2011; the `KisPainter` blend infrastructure 2002–2011; the dab-rendering queue 2017. All
  predate the late-2010s.
- **Not yet pinned:** the exact first-published version/commit of `blendAverageOpacity` itself and of
  the *creamy* alpha-darken flow-mode default. ⚠ `KoAlphaDarkenParamsWrapper.{h,cpp}` is a **2019**
  refactor and must NOT be mistaken for the behavior's origin — the op and the painter blend it
  wraps are the 2006–2011 sources above. To fix the earliest dates precisely, `git log` / `git blame`
  those functions on Krita's **public** history (invent.kde.org / the KDE git archive) and record the
  commit hash + date + first release tag here. That is a maintainer task, not performed in-repo.

## Posture (maintainer, 2026-07-27) — the standing ruling

**Nothing brush-facing is gated, as long as it is transcribed from the reference's published
source.** This is the 2026-07-18 opacity/flow decision below, generalized by the maintainer to the
whole brush arc on 2026-07-27: transcribe exactly, ship enabled by default, and credit the dated
published GPL source recorded above rather than designing the behavior away.

Consequences already taken, all on 2026-07-27 (`docs/brushes.md` §5, §6.6, §6.6b, §6.6g):

- the bristle-brush exclusion is **lifted** — read against the algorithm, `d)_Ink-8_Sumi-e` is not an
  instance of the technique §5 excludes, and §5 now says so factually;
- `hatchingbrush` / `y)_Screentone_Moire` is **built**;
- the three remaining stroke-history engines (`curvebrush`, `particlebrush`, `experimentbrush`) are
  **built**.

Two brush-facing mechanics that this ruling also unblocks are **not** built and are sequenced
separately, because each is its own design surface rather than part of the engine arc: the per-pixel
**paint-load channel** under smudge (upstream's `PaintThickness`, which §6.6b currently records as
dropped-never-mapped) and the **rope / pulled-string stabilizer**. Both need their own slice, their
own transcription and their own row above.

⚠ **Unchanged by the ruling, and independent of it:** this project does not characterise what any
third-party project practises — not in docs, not in code comments, not in commit messages. Naming
the dated published source a mechanic was transcribed from is exactly what this file is for, and is
the only thing it does.

## Posture (maintainer, 2026-07-18) — the original decision, for the wash path

The shipped presets carrying a dynamic Opacity option under Wash ship **enabled by default**, as the
exact transcription. A behavior-preserving-enough alternative — a max-saturation deposit that avoids
the upstream's conditional accumulation — was built, render-diffed across all 117 shipped presets,
and **rejected** for departing from the transcribed look on the soft-build presets. ⚠ **Ship the
exact transcription; this one is settled and is not to be re-opened.**
