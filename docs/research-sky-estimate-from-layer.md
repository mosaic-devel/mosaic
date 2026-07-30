# Estimate from Layer — Design Document

**Feature:** a one-click "Estimate from layer" action in the Sky generator (Texture Generator dialog, S55) that analyzes the active layer's pixels and best-effort conforms the sky generator's settings — horizon/camera, sun position (or clock time via almanac inversion), turbidity, cloud coverage, exposure — to a photograph. A separate toggle additionally (a) auto-masks the photo's sky region and (b) photometrically harmonizes the non-sky region to the generated sky, executed only on dialog ACCEPT.

**Hard constraints:** no ML runtime; portable C++ (SSE2 at most); classical image processing only; degrade to "no change + tell the user", never garbage. Citations below are algorithm provenance.

---

## 0. Repo infrastructure inventory (verified, with file paths)

Everything below was read from source; the design maps directly onto it.

| Infrastructure | Location | Facts that matter |
|---|---|---|
| Sky parameters | `src/core/texture/texture_params.hpp` (`SkyParams`) | `pitchDeg` (18 default), `rollDeg`, `fovDeg` (62, horizontal), `shiftY` (tilt-shift, +down), `sunAzimuthDeg`/`sunElevationDeg` (180 = frame centre), `turbidity` (1–10), `cloudCoverage`, `exposure` (EV), `obsYear/Month/Day/HourUtc/LatitudeDeg/LongitudeDeg`, `enableMoon`, `moonPhaseMode`. Growth rule: fields only ever ADDED. |
| Camera model | `src/core/texture/sky_camera.hpp` (`SkyCamera`) | Pinhole; camera faces compass south (az 180 = frame centre); `fromParams`, `rayAt(px,py)→world dir`, `project(dir)→px,py` (exact inverse). "Positive roll tilts the horizon clockwise on screen"; `shiftY` moves horizon down without pitching rays. World: +X east, +Y north, +Z up; compass az = `atan2(d.x, d.y)`. |
| Solar solver | `src/core/texture/solar.hpp` | `sunPosition(UtcTime, lat, lon)` → az/el, ~0.01° accuracy, pure. |
| Almanac | `src/core/texture/sky_almanac.hpp` | `computeSkyAlmanac`, `sunRiseSetTimes` (transit = solar noon, polar `alwaysUp/alwaysDown` flags), `sunEventsAtAltitude(date, lat, lon, targetAltDeg)` — **a ready-made almanac inverter for any target elevation**, `applyMasterClock(sky, t, lat, lon)`. |
| Atmosphere (forward model) | `src/core/texture/atmosphere.hpp` | `cookAtmosphere(sunDir, turbidity)` → baked transmittance table; `radiance(viewDir)` pure per-ray. Nishita 1993 single scattering; used below horizon. Day dome is Hosek–Wilkie (`sky_render.cpp:584`). |
| Display mapping | `src/core/texture/sky_render.cpp:124–164, 940–953` | linear radiance → `exposure` EV scale → extended-Reinhard `tonemap()` → `srgbEncode()` + TPDF dither. Probe renders and photo statistics must be compared **through this same mapping** (or both decoded to linear). |
| Render entry | `src/core/texture/texture_render.hpp` | `renderTexture(params, w, h, window, progress)` — pure function of frame coords; `TextureWindow` byte-exact crops; `TextureRenderProgress` (atomics: rowsDone/rowsTotal/cancel, aborts between rows). |
| Worker pattern | `src/core/texture/render_worker.hpp` (`TextureRenderWorker`) | One thread, requests coalesce to newest, epoch-tagged results, cancel-in-flight, `progressFraction()`; FLTK-free; UI polls via re-armed timeout (`TextureGeneratorDialog::pollOnce`). |
| Parallelism | `src/core/texture/parallel_rows.hpp` | `parallelRows(count, fn)` — band-parallel, bit-identical to serial. |
| Selection engine | `src/core/selection.hpp/.cpp` | 8-bit coverage mask; `magicWandSelection(src, x, y, WandParams)` — scanline flood, hard flood + 1-px AA boundary ramp; `wandColorDistance(a, b, useAlpha)` — normalized luma-weighted RGBA distance in encoded space, **the designated swappable metric seam**; `grown/shrunk` (exact FH-EDT signed-distance offset, `offsetBy`), `feathered(radius)` (separable Gaussian, σ=radius), `smoothed`; `kAntsCoverageThreshold = 128`. |
| Mask plumbing | `src/core/selection.hpp:137–163` | `maskFromSelection(layer, sel, docW, docH)` — resamples doc-space selection onto the layer's mask grid through its transform. |
| Commands | `src/core/commands.hpp` | `SetSelectionCommand`, `SetLayerMaskCommand(id, mask, label)`, `SetLayerPixelsCommand` (whole or region, byte-exact undo), `AddLayerCommand`, `CompositeCommand` (one undo step). |
| Adjustment layers | `src/core/layer.hpp:461–494`, `src/render/compositor.cpp:586–658` | `AdjustmentLayer` carries a `std::map<std::string,double>` params bag; compositor's `applyAdjustment` handles opacity, layer mask, clip-to-below coverage, `parallelFor` — but only Invert/Grayscale/BrightnessContrast have math today; Exposure/HueSaturation/ColorBalance/Curves **pass through** (S32–S35). |
| Doc-space layer image | `src/ui/app_window.cpp:5820–5835` | `activeLayerDocImage(scratch)` / `wandMergedSource(scratch)` — exactly the source resolution the magic wand uses, including the "active layer has no pixels" user hint. Reuse verbatim. |
| Dialog | `src/ui/texture_generator_dialog.hpp/.cpp` | `TextureGenHost { foreground, commit }` (needs growth); `applyEdit(mutate)` + `syncControls()` + `requestProxy()` is how presets land; sky sections: `sky:clouds`, `sky:night`, `sky:camera`, `sky:solar` ("Sky by date & place"), `sky:advanced`; bake path `create()/pollOnce()/cancelBake()` with footer progress; moon-source ephemeris/manual latch. |
| Bubble popup precedent | `src/ui/bubble_flyout.hpp` | `BubbleFlyout` base: comic triangle, anchor gap, auto-flip, `setAvoidRect` (keep the preview clear), build-before-host-shown rule, native-Wayland plain-panel fallback. Subclassed by ColorFlyout/GradientFlyout. |
| Pixels | `src/common/image.hpp` | `common::Image` = RGBA8 straight alpha, sRGB-encoded; `common::ImageF` float RGBA. |
| **EXIF** | `src/io/io.hpp:35`, `src/io/jpeg.cpp` | **Not preserved.** io.hpp explicitly defers "text/EXIF/pHYs chunks" to the S41/S42 FormatBackend; `jpeg.cpp` installs no marker reader. FOV-from-EXIF is future work (§2.3). |

---

## 1. Pipeline overview

```
                         ┌─ click "Estimate from layer" ─┐
photo (active layer,     │  EstimateWorker thread         │
doc space, RGBA8 sRGB) ─►│                                │
                         │ S0 proxy build  (≤1024 wide)   │
                         │ S1 horizon      (pos + tilt + confidence)
                         │ S2 sky prior + DP border (proxy segmentation)
                         │ S3 sun detect   (disc/glow → az,el)
                         │ S4 sky statistics + forward-model probe match
                         │       → sun elevation, turbidity, exposure, coverage
                         │ S5 almanac inversion (only if date-&-place latch on)
                         └─► one applyEdit() → syncControls() → requestProxy()
                              + info-panel summary with per-quantity confidence

on ACCEPT, if "mask & harmonize" toggle on (after the normal full-res bake):
   S6 full-res sky mask   (prior + flood + DP reconcile + FH-EDT/Gaussian feather)
   S7 photometric harmonization parameters (from generated sky's known illuminant)
   S8 commit: ONE CompositeCommand
        [ AddLayer(sky texture, below photo)
        , SetLayerMask(photo, ¬skyMask)              — feathered
        , AddLayer(PhotometricMatch adjustment, clipped to photo) ]
```

Every stage emits `(value, confidence ∈ [0,1])`. A stage below its floor leaves its parameters **untouched** and says so. If S1 fails and S2's sky fraction is ≈0, the whole estimate aborts with "No sky or horizon found in ‹layer›. Settings unchanged." — the degrade-to-no-change contract.

All analysis runs on a working proxy of the doc-space layer image (long edge ≤1024, box-filtered — decimation is also a cheap noise/JPEG-artifact filter, which the horizon and statistics stages want). Only S6 touches full resolution.

---

## 2. Horizon estimation (S1)

### 2.1 Survey of classical approaches

1. **Two-class statistical line search** — Ettinger, Nechyba, Ifju & Waszak, *"Vision-Guided Flight Stability and Control for Micro Air Vehicles"* (IROS 2002) and *"Towards Flight Autonomy: Vision-Based Horizon Detection for Micro Air Vehicles"* (2002): parameterize a line by (bank angle, intercept); for each candidate, split the frame into "above"/"below" pixel sets and maximize a class-separation criterion built from the two RGB covariance matrices. 99.9% correct at 30 Hz on 2002 hardware; the criterion is intrinsically robust because it uses *all* pixels, not just edges.
2. **Edge + Hough dominant line** — Sobel gradient, orientation-gated thresholding, Hough transform (Duda & Hart, CACM 1972), then a robust line refit with RANSAC (Fischler & Bolles, CACM 1981).
3. **Per-column boundary + robust fit** — take the DP sky-border polyline (§5) and fit a line to it robustly.
4. Comparative marine-horizon literature — Gershikov, Libe & Kosolapov, *"Horizon Line Detection in Marine Images: Which Method to Choose?"* (IJAIS 6(1–2), 2013) — found **edge+Hough best in angle, regional-covariance best in position**. That result decides the hybrid below.

### 2.2 Recommendation (AS SHIPPED): DP sky-border polyline + RANSAC line fit (primary) + edge/Hough (cross-check)

> ⚠ **Design override (standing constraint C-A1).** The draft of this document recommended the
> Ettinger covariance class-separation line search as the primary method. It was REPLACED before
> implementation, and must not be reinstated: Ettinger's criterion computes per-class color
> statistics over candidate global splits of the frame, which is a global-intensity-split family
> this project deliberately stays out of. The shipped primary is instead a **gradient/colour-prior
> border with a line fit** — no histograms, no global intensity threshold.

**Primary (position-accurate).** The §5 DP sky-border polyline (color-prior + edge energy — no
histograms, no global intensity threshold) is computed on a coarse whole-frame pass (long edge
≤256), then a robust line is fitted to its border points: RANSAC (500 iterations, 2-pt samples,
inlier band max(2 px, 1%·H), tilt capped at ±40°) with a least-squares polish on the inliers.
When the border is ragged (mean adjacent-column jump > 2%·H — trees, roofs), the fit moves to
the **lower-quartile border points** (largest y = the least-occluded evidence; a quartile of
border ROW GEOMETRY, not of pixel intensity), confidence is capped at 0.6 and the summary says
"skyline used as horizon" (per §2.4).

Because a two-lobe chroma prior cannot span a twilight sky's chroma arc (dim gray zenith
through deep orange glow), the coarse horizon pass blends one additional lobe-free structural
term into the DP input: **top-connected smoothness** (per column, the cumulative gradient
maximum from the top row — the gradient-domain border idea of Shen & Wang 2013). Purely
gradient-based; C-A1 untouched.

**Sanity gate (cheap physics, unchanged):** the "sky" class must actually look like sky — mean
luminance(A) > mean luminance(B) OR blueness(A) > blueness(B), and mean local-gradient(A) <
mean local-gradient(B), all CLASS MEANS over the fitted split (no distribution). If the winner
violates all three, the "horizon" is a table edge or wall seam → fail the stage. (This is the
indoor/closeup rejector.)

**Cross-check (angle-accurate).** On the 1024-proxy: Sobel luminance gradient → keep pixels
above a FIXED gradient threshold (0.08 encoded-luma units, one fixed relaxation step to 0.04 —
never a distribution-derived "top N%", C-A1 conservatism) whose orientation is within 30° of
horizontal → Hough over (tilt ±40°, intercept) → strongest peak → collect per-column topmost
inlier points within ±5 px of the peak line → RANSAC (2-pt samples, inlier band ±2 px) →
least-squares polish on inliers.

**Fusion (unchanged in shape):** if both succeed and agree (|Δv_h| < 3%·H and |Δτ| < 2°),
output position from the border fit, tilt from Hough/RANSAC (per Gershikov's finding). If only
one succeeds, use it at reduced confidence. If they disagree, prefer the border fit but cap
confidence at 0.45 ("low confidence" band).

### 2.3 Confidence score

```
conf_horizon = w1·margin + w2·inlierFrac + w3·agreement + w4·skyPlausibility
  margin        = fraction of ALL border points within ±2%·H of the fitted line
                  # the border's own line-peakedness replaces the draft's J-ratio (§2.2 override)
  inlierFrac    = cross-check RANSAC inliers / candidate boundary points
  agreement     = exp(−(Δv_h/0.03H)² − (Δτ/2°)²)                # 1 when both methods coincide
  skyPlausibility = fraction of the 3 physics gates passed
  weights w = (0.35, 0.2, 0.25, 0.2)
```

- **conf ≥ 0.65** → apply pitch/roll silently.
- **0.4–0.65** → apply, flag "low confidence" in the summary line.
- **< 0.4** → leave `pitchDeg/rollDeg/shiftY` untouched; summary: *"No horizon found — camera left unchanged."* Downstream stages that need the horizon (sun az/el mapping, position prior) fall back to the **current** generator camera.

### 2.4 Known failure modes

| Case | Behavior |
|---|---|
| Indoor / closeup | Physics gates fail → stage aborts cleanly (by design). |
| Occluded horizon (city skyline, forest) | The visible skyline sits *above* the true horizon → pitch biased up. Mitigation: fit the line to the **lower quartile** of the DP border points (§5) when the border's roughness (per-column |Δb|) is high; cap confidence at 0.6 and note "skyline used as horizon". |
| Sloped terrain / ocean swell tilt | Accepted — matching the apparent horizon is the right answer for compositing. |
| Sky-only photo (>90% sky prior) | Skip line fit; set nothing; report "all sky — horizon left unchanged"; segmentation still valid (mask = everything). |
| Strong vignette / graduated ND | Ettinger's covariance criterion tolerates it (global color split); Hough may lock onto the filter edge — the agreement term suppresses that. |

---

## 3. Camera parameter mapping (S1 → SkyParams)

### 3.1 Horizon → pitch and roll (closed form + exact refine)

From `SkyCamera::fromParams`/`project` (verified derivation): a level world direction seen at the centre column projects to normalized row

```
v_h = 0.5 · (1 + 2·shiftY + tan(pitch)/halfTanY),   halfTanY = tan(fovDeg/2 · π/180) · H/W
```

(`v_h` in [0,1], 0 = top; pitch > 0 pushes the horizon below centre.) Inverting, with the estimate deliberately **resetting `shiftY` to 0** (one measured DOF — the horizon row — cannot determine two parameters; pitch is the semantically-primary one and the tilt-shift stays a manual artistic control):

```
pitchDeg = atan( (2·v_h − 1) · halfTanY ) · 180/π          # clamp to the generator's UI range
rollDeg  = τ_cw                                            # measured image tilt, clockwise-positive
```

where `τ_cw = atan2(y_right − y_left, x_right − x_left)` on the fitted line in image coords (y down) — positive when the right end is lower, which is the screen-clockwise sense that `SkyCamera` documents for positive roll. ⚠ Implementer: pin the sign with a unit test that renders `project()` of level directions at roll = +5° and asserts the measured τ_cw ≈ +5° — do not trust prose (mine included).

Because the closed form is first-order in roll, finish with an **exact 2-parameter Gauss–Newton refine** (≤5 iterations): parameters (pitch, roll); residuals = image-space distance between the fitted horizon line's two endpoints and `SkyCamera::project()` of the corresponding level world directions. This kills convention bugs and is exact at any roll. FOV is held at its current value throughout (see 3.2), which is fine: for the horizon *line*, pitch and FOV are confounded only through `halfTanY`, and matching v_h at the current FOV still puts the rendered horizon exactly where the photo's is — the property the user actually sees.

### 3.2 FOV: the honest assessment

Single-image FOV cues, reviewed:

- **Vanishing points** (Caprile & Torre, IJCV 1990): needs man-made scenes with two orthogonal VP families, fails on landscapes — exactly this feature's target photos. Fragile, wrong silently. **Rejected.**
- **Sun-position-over-time fitting** (Lalonde, Narasimhan & Efros, *"What Do the Sun and the Sky Tell Us About the Camera?"*, IJCV 88(1), 2010) recovers focal length — but from **image sequences**, not one frame. **Not applicable.**
- **Sky-appearance model fitting** (same paper) can in principle constrain FOV from one frame, but the authors themselves report it is the weakest of their cues; with unknown white balance and our low quality bar it would be noise. **Rejected.**
- **EXIF `FocalLengthIn35mmFilm`** (CIPA DC-008): `fovDeg = 2·atan(18/f35)·180/π` (36 mm frame width, horizontal FOV — matching `fovDeg`'s horizontal definition). This is the **only trustworthy source**.

**Repo status (verified):** Mosaic's loaders do not read or keep EXIF — `src/io/io.hpp:35` explicitly defers metadata chunks to the S41/S42 FormatBackend, and `src/io/jpeg.cpp` installs no APP1 marker handling. **Recommendation: leave `fovDeg` untouched, always, and note in the summary line "Field of view unchanged (no lens metadata)." When S41/S42 lands EXIF plumbing, wire the formula above behind the same estimate button — the design needs no other change** (the Gauss–Newton refine already treats FOV as a held constant).

---

## 4. Sun detection & time estimation (S3–S5)

### 4.1 Visible sun: detection by saturation cratering + radial glow fit

Lineage: sun-disc extraction for robot navigation — Cozman & Krotkov, *"Robot Localization Using a Computer Vision Sextant"* (ICRA 1995) — thresholded brightest-region centroiding of the solar disc; the radial-falloff validation is standard glare/glow analysis.

On the proxy, within (a 5-px dilation of) the sky region from S2:

```
1. clipped mask C = { px : min(R,G,B) ≥ 250 }                        # "saturation crater"
2. connected components of C (scanline, 8-conn); for each component:
     reject if area < 4 px  or  equiv. radius > 0.25·W
     roundness = 4πA/P² ≥ 0.5                                        # sun+glow blobs are compact
     radial profile: mean luminance over rings r ∈ [r0, 4·r0] from the centroid
       must be monotone-decreasing; fit L(r) = a·exp(−r/b) + c, accept R² ≥ 0.8
3. if ≥ 2 candidates survive: keep the one with the largest a·b (brightest, widest glow);
   if it is not ≥ 3× the runner-up's a·b → ambiguous → treat as "sun not visible"
4. expected physical disc radius check: r_disc ≈ 0.00445 · f_px,  f_px = 0.5·W/halfTanX
   (0.255° radius); accept crater radius in [r_disc·0.5, r_disc·20] (glow inflates it)
```

Map centroid → direction with the **post-S1 camera** (estimated pitch/roll, current FOV): `d = SkyCamera(est).rayAt(px, py)`; then `el = asin(d.z)·180/π`, `az = atan2(d.x, d.y)·180/π` (compass; frame centre = 180 by construction, so `sunAzimuthDeg = az` directly encodes the in-frame offset — no extra arithmetic).

**Failure modes & rejectors:**
- *Overexposed sky*: if > 40% of the sky mask is near-clipped, skip sun detection entirely (position meaningless), fall to §4.2 which will also mostly fail → coarse "bright day" bucket.
- *Specular highlights, streetlights*: excluded by the sky-mask gate + roundness + radial-monotony.
- *Sun behind thin cloud (bright smear, no crater)*: detect as fallback — brightest 0.5-percentile luminance blob inside sky with radial monotony but no clipping; confidence halved, elevation only (azimuth still usable), label "sun position approximate (behind cloud)".
- *Moon at night*: if the photo's sky median luminance is very low (night signature) and the crater is near `r_disc` with little glow, report it as the **moon** and (stretch goal) set `enableMoon/moonAzimuthDeg/moonElevationDeg` instead; v1 may simply say "bright disc treated as moon — sun left unchanged".
- *Sun's reflection on water below the horizon line*: rejected by the sky-mask gate.

### 4.2 Sun not visible: analysis-by-synthesis against our own renderer

The generator **is** a forward model (Hosek & Wilkie 2012 above the horizon, Nishita 1993 single-scattering below — `atmosphere.hpp`), so instead of hand-authored "twilight vs golden vs midday" heuristics we match the photo's sky against **rendered probes**. Precedent for fitting a physical sky model to a photo's sky pixels to recover sun position: Lalonde, Narasimhan & Efros, IJCV 2010 (they fit the Perez 1993 all-weather model; we fit our own dome, strictly better calibrated to what we will render). See also Preetham, Shirley & Smits (SIGGRAPH 1999) for the turbidity parameterization this inherits.

**Photo signature** (from S2's proxy sky mask, sRGB-decoded to linear via a 256-entry LUT):
- `chromaH`, `chromaZ`: median (r,g,b)/(r+g+b) chromaticity in a **horizon band** (rows within 8% of the border, sky side) and a **zenith band** (top 15% of sky rows);
- `gradLum = medianLum(horizon band) / medianLum(zenith band)`;
- `asym`: left-vs-right horizon-band luminance asymmetry (sub-horizon/low-sun azimuth cue);
- `sat`: median saturation of sky pixels; `medLum`: median linear luminance (exposure matching only).

Features are ratio/chromaticity-based deliberately: a global von-Kries white-balance shift moves `chromaH` and `chromaZ` together, so the **difference** `chromaH − chromaZ` and `gradLum` survive unknown camera WB far better than absolute color does.

**Probe set:** `renderTexture()` at **64×64**, dome+haze only (`enableClouds=false`, `enableSun=false`, `enableMoon=false`, stars off via `starsAmount=0`), camera = estimated pitch/roll & current FOV, exposure 0, over the grid

```
el ∈ {−18,−15,−12,−9,−6,−4,−2,0,2,5,10,15,20,30,45,60}   × turbidity ∈ {1.5, 2.5, 4, 6, 9}
```

= 80 probes. Day probes (Hosek–Wilkie) are ~1–2 ms each at 64²; sub-horizon probes pay one `cookAtmosphere` table bake each (~5–20 ms) → worst case ≈ 0.5 s total, in the worker, cancellable between probes; the signature table is cached per (pitch, roll, FOV, aspect) bucket for the session, so re-estimates are free. Extract the same signature per probe (probes rendered through the same display mapping the photo implicitly lives in, then decoded with the same LUT — apples to apples).

**Match:** weighted nearest neighbor + local 3-point parabolic refinement along el:

```
D(probe) = 4·‖(chromaH−chromaZ)_photo − (…)_probe‖² + 1·‖chromaZ_photo − chromaZ_probe‖²
         + 2·(log gradLum_photo − log gradLum_probe)²
elEst, turbEst = argmin; conf from D margin to runner-up and absolute D
exposureEV     = log2(medLum_photo / medLum_bestProbe)          # clamp [−6, +4]
```

**Cloud coverage (coarse, best-effort):** fraction of sky pixels whose chromaticity sits closer to the neutral axis than the matched clear-sky model predicts at their row → `cloudCoverage = clamp(cloudyFrac · 1.1, 0, 1)`. If `cloudyFrac > 0.85` and `gradLum` is flat → overcast: set `cloudCoverage ≈ 0.95`, and **degrade the elevation claim** to a 3-bucket luminance guess (day / twilight / night) because an overcast sky's color signature is nearly elevation-invariant — say so in the summary. Cloud *type* is not estimated (out of scope; the default deck stack stays).

**Elevation resolution honesty:** above el ≈ 30° the clear-sky signature is nearly constant (the sky looks "midday" from 35° to 75°). Report bucket "midday", pick el = min(35°, day's transit altitude), confidence capped at 0.5. Between −18° and +25° the twilight/golden gradient is highly discriminative — this is where the method genuinely shines, and conveniently where users most want time-of-day matching.

### 4.3 Almanac inversion: elevation → clock time

Runs **only when the dialog's date-&-place mode is active** (the `sky:solar` section / moon-source ephemeris latch — the existing first-set-wins mechanics in `setMoonSourceInternal`). Otherwise the estimate stamps `sunElevationDeg`/`sunAzimuthDeg` manually and this stage is skipped.

The repo already contains the general inverter: `sunEventsAtAltitude(date, lat, lon, targetAltDeg)` returns the ascending (morning) and descending (afternoon) crossings of any elevation, polar-aware. The implementation:

```
invertTime(date, lat, lon, elEst, currentHourUtc):
    t  = sunRiseSetTimes(date, lat, lon)            # transit + polar flags
    elMax = t.transit.transitAltitudeDeg
    if elEst > elMax − 0.5:                          # unreachable → clamp to solar noon
        return { hour: t.transit.hourUtc, note: "sun never reaches {elEst}° here on this date;
                 set to solar noon ({elMax}°)" }
    ev = sunEventsAtAltitude(date, lat, lon, elEst)  # rise = morning solution, set = afternoon
    if ev.rise.alwaysDown:  clamp to transit, note polar night
    if ev.rise.alwaysUp:    clamp to midnight-sun minimum, analogous note
    pick = whichever of {ev.rise.hourUtc, ev.set.hourUtc} is valid and
           nearer currentHourUtc;  both valid & equidistant → afternoon (ev.set)
    return { hour: pick, alt: other solution, note: "morning/afternoon ambiguous" }
```

Then `applyMasterClock(sky, {y,m,d,hour}, lat, lon)` stamps coherent sun + moon + phase. Cost: microseconds.

**The two-solutions ambiguity, handled as UI copy** (a single photo cannot resolve it): default = the solution nearest the currently-set clock time, tie-break afternoon. Summary line: *"Time set to 17:42 (sun 12° up, afternoon assumed — morning equivalent 06:31)."* Consider making "06:31" a clickable swap link in the info panel; it's one `applyEdit`.

**Azimuth conflict in date-&-place mode, stated honestly:** the generator's camera faces due south by convention (no yaw parameter), so in ephemeris mode the sun's frame position follows the clock and generally will not land where the photo's sun sat. Manual mode reproduces framing exactly (`sunAzimuthDeg` **is** the frame-relative control); ephemeris mode matches *time*, not framing. Summary line in that mode: *"Sun placed by date & place; the photo's sun sat 23° left of centre — switch to manual sun to match framing exactly."* (Adding a camera-yaw field to `SkyParams` would dissolve this — a generator feature decision outside this document; noted since `SkyParams` growth is additive-safe.)

---

## 5. Sky segmentation (S2 proxy / S6 full-res)

### 5.1 Survey (classical only)

- **(a) Seeded region growing** — Adams & Bischof, IEEE TPAMI 16(6), 1994; instantiated with Mosaic's own scanline flood + `wandColorDistance` metric (the S43-b-swappable seam). Great boundaries where color separates; leaks catastrophically through low-contrast horizon haze if used alone.
- **(b) Per-column DP border polyline** — Lie, Lin, Lin & Hung, PRL 26(2), 2005; threshold-optimized gradient-domain variant: Shen & Wang, IJARS 2013 (95% accuracy, 1000-image set, ~150 ms — classical, no learning). One boundary point per column, smoothness-regularized: structurally leak-proof, handles ragged tree/roof skylines, but cannot represent sky through apertures or below overhangs.
- **(c) Pixel priors (color/position/texture)** — Luo & Etz, IEEE TIP 11(3), 2002 (physical sky-color gradient model; their full system includes a small trained classifier — we take only the physical color-model insight); Zafarifar & de With, ACIVS 2006 (fully classical probabilistic combination of color + position + texture — the closest published recipe to what ships here). Hoiem/Efros/Hebert geometric context (2005) is noted and **excluded** (boosted classifiers = ML).

### 5.2 Recommended hybrid

**Prior map** (proxy, then full-res where needed), per pixel, log-linear blend:

```
P(x,y) ∝ Pos(y)^1.0 · Color^1.2 · Texture^0.8 · Lum^0.5
  Pos     = σ( (horizonRow(x) − y) / (0.05·H) )              # above fitted horizon line
  Color   = max( G_blue(chroma), G_grayBright(chroma, lum) ) # two Gaussian lobes in linear-RGB
                                                             # chromaticity: "blue sky" and
                                                             # "bright neutral cloud/overcast"
  Texture = exp(−(meanGrad7×7 / g0)²), g0 = 4/255 per px     # sky is smooth at proxy scale
  Lum     = percentile-normalized luminance, clamped ≥ 0.35
```

Lobe parameters seed from canonical values and are **re-fitted once** from the top-quartile-P pixels (one EM-lite iteration) — Zafarifar's adaptive-model idea without any offline training.

**DP border** (proxy): for column x, boundary row `b(x)` minimizing

```
E = Σ_x [ vertEdgeReward(x, b(x)) + α·Σ_{y<b(x)} (1−P(x,y))/b(x) + β·Σ_{y≥b(x)} P(x,y)/(H−b(x)) ]
    + Σ_x λ·|b(x) − b(x−1)|,     |b(x) − b(x−1)| ≤ smax
α = β = 3, λ = 2/px, smax = 0.15·H;  search band = horizon ± 0.35·H (whole frame if S1 failed)
```

Left-to-right DP, `O(W · band · smax)` ≈ 10 ms at proxy. (Lie et al.'s multi-stage-graph formulation with Zafarifar-style regional terms replacing raw edge maps.)

**Full-res consolidation** (S6, ACCEPT-time only):

1. Upsample `b(x)` linearly to full width.
2. Seed grid: every 64 px in the region ≥ 24 px above the border with P > 0.9; flood each unvisited seed with `magicWandSelection`-machinery, adaptive tolerance: initial 0.08 → re-flood once at `clamp(2.5·σ_seedRegion, 0.06, 0.20)`. Union = sky core. Cost proportional to sky area.
3. Final hard mask = `(above border) ∧ (floodUnion ∨ P_fullres > 0.8)`. The P-term recovers cloud pixels a conservative flood missed; the border term stops flood leaks cold.
4. **Holes policy:** non-sky islands enclosed by sky smaller than 0.02%·frame are filled (dust, distant birds); larger enclosed objects (chimney, head against sky, power-line clusters) **stay foreground**. Sky components not connected to the main sky body and smaller than 0.1%·frame are dropped (specular roofs). One connected-components pass.
5. **Sky through apertures** (arch, window, between branches): out of v1 (the DP border cannot represent it; a global color match would eat blue cars). Documented limitation; the user paints the mask by hand there. (A later "include openings" sub-toggle can add: non-contiguous wand match against the fitted sky color model, tight tolerance, components gated fully above the horizon line.)
6. **Edge finish:** the flood's native hard-flood + 1-px AA ramp, then `Selection::feathered(r)` with `r = clamp(diag/1500, 1.0, 3.0)` px; `smoothed(2)` optionally first to de-staircase the DP border. All existing `selection.cpp` code.

Output is a document-space `core::Selection`; the ACCEPT path converts per-layer with `maskFromSelection()` (handles transformed layers correctly).

### 5.3 Segmentation confidence / abort

- sky fraction ∈ [2%, 98%] (else "nothing to mask" / "all sky");
- mean P inside mask ≥ 0.6; mean P outside ≤ 0.35 (separation);
- DP per-column energy below threshold for ≥ 70% of columns (ragged high-energy border = fine structures → warn "mask will be approximate around trees/hair");
- below floor → the toggle's path reports *"Couldn't isolate the sky — mask & harmonize skipped"* and the plain parameter estimate still stands.

### 5.4 Runtime at 4K (3840×2160, 8.3 MP), measured-class estimates

| Pass | Complexity | Single-thread | With `parallelRows` (8C) |
|---|---|---|---|
| Proxy build + priors + DP + horizon + sun + stats (S0–S4, proxy) | O(1 MP) | 60–120 ms | (proxy stages not worth threading) |
| Probe rendering (first run, night grid) | 80 × 64² + table bakes | 150–500 ms | ~80–200 ms |
| Full-res prior + floods (S6) | O(8.3 MP) | 200–350 ms | 60–120 ms |
| Connected components + holes | O(8.3 MP) | 40 ms | — (sequential ok) |
| FH-EDT + Gaussian feather | O(8.3 MP), separable | 120–160 ms | 40–60 ms |
| **Totals** | | **estimate click ≈ 0.3–0.7 s; ACCEPT extra ≈ 0.4–0.6 s** | ≈ half that |

Everything is linear passes over flat RGBA8/float rows — SSE2-autovectorizable; no per-pixel allocation anywhere.

---

## 6. Photometric harmonization (S7–S8)

### 6.1 What we know that blind methods don't

The generated sky is not a mystery image: at commit time we hold the **ground-truth sun elevation, sun color through atmospheric transmittance, and mean dome radiance** (integrate one 32×32 dome-only probe). "Estimate the illuminant of image B" — the error-prone half of Reinhard-style transfer — becomes a table lookup. Only the *photo's* side needs blind estimation.

### 6.2 The transfer (all global, one fused pass)

Work in linear RGB (`ImageF`, sRGB decode LUT), foreground pixels only (mask = ¬sky, coverage-weighted).

**(a) White balance — von Kries diagonal adaptation** (von Kries 1902; optionally Bradford-sharpened, Lam 1985 / CAT02, Moroney et al. 2002 — recommend plain linear-RGB von Kries for v1):

```
L_target = normalize( w_dir(el)·sunTransmittanceColor(el, turb) + (1−w_dir)·meanDomeColor )
           w_dir: 0.8 day (el>15°), 0.5 golden (0..15°), 0.0 below horizon
L_source = normalize( 0.5·grayEdge₁(foreground) + 0.5·whitePatch₉₉(foreground) )
gains g  = clamp( (L_t/‖L_t‖₁) / (L_s/‖L_s‖₁), 0.6, 1.6 ) ^ strength
```

Source illuminant: **gray-edge** (van de Weijer, Gevers & Gijsenij, IEEE TIP 2007 — 1st-order, σ=2, Minkowski p=6) blended with 99th-percentile **white-patch** (Land's Retinex, 1977), fallback **gray-world** (Buchsbaum 1980) when the foreground has too few gradients; near-clipped and near-black pixels excluded. (Finlayson & Trezzi's *Shades of Gray*, 2004, unifies these — p=6 is their recommendation.)

**(b) Exposure/contrast — Reinhard statistics transfer on log-luminance only** (Reinhard, Ashikhmin, Gooch & Shirley, IEEE CG&A 2001; luminance-only so hue is carried solely by (a); full 3-channel / Pitié distribution transfer deliberately rejected for v1 — too aggressive):

```
ΔEV  = k(el_target) − k(el_photoEst)          # k(el): precomputed table of mean dome log-luminance
                                              # from OUR forward model, exposure-normalized
scale = 2^clamp(ΔEV, −6, +2)
logL' = μ_s + ln(scale) + (logL − μ_s)·clamp(σ_t/σ_s, 0.7, 1.3)
rgb'  = rgb · exp(logL' − logL); extended-Reinhard shoulder on the result to keep highlights
```

σ_t comes from the same table (twilight compresses scene contrast). The −6 EV floor is the "artistic night, stays readable" clamp.

**(c) Vertical luminance gradient (optional, subtle):** multiply by `1 + a·(1 − 2y/H)`, `a = clamp(0.4·ΔskyBrightness, −0.12, +0.12)`; disabled below harmonization confidence 0.5.

**(d) Twilight/night response — the "time control" promise, made precise.** Grounding: Thompson, Shirley & Ferwerda, JGT 7(1), 2002 (day-for-night: blue shift + brightness rescale); Jensen et al., *Night Rendering*, UUCS-2000-016, 2000; scotopic luminance from Ward Larson, Rushmeier & Piatko, IEEE TVCG 3(4), 1997; Purkinje-shift perceptual shape: Kirk & O'Brien, SIGGRAPH/TOG 30(4), 2011.

```
ρ = smoothstep(el_target: −2° → −14°)                    # rod fraction, 0 by day, 1 past nautical
V' = Y · (1.33·(1 + (Y+Z)/X) − 1.68)                     # scotopic luminance (Ward Larson 1997);
                                                         #   XYZ from linear RGB
night = V' · (0.42, 0.55, 1.00) · nightGain              # rod signal as desaturated blue-gray
out   = mix(rgb_after_abc, night, ρ);  then saturation × (1 − 0.5·ρ)
```

**Explicit contract for "night from a day photo" (goes in docs and near the toggle):** global dimming, blue-shift, contrast compression, and the generated sky's own glow/stars/moon **will** read as night. What will **not** appear: lit windows, streetlights, headlights, neon; daytime cast shadows keep their direction and hardness; old sun's specular glints remain; reflections of the sky in water/glass still show the *old* sky. This is a color-grade, not a relight.

### 6.3 Non-destructive landing (repo-verified options)

`AdjustmentLayer` carries a `std::map<std::string,double>` params bag; compositor `applyAdjustment` already handles opacity, layer masks, clip-to-below, parallelism — but only Invert/Grayscale/BrightnessContrast have math today (Exposure/ColorBalance/Curves pass through until S32–S35, `compositor.cpp:624–632`).

**Recommendation — mint `AdjustmentKind::PhotometricMatch`.** Every §6.2 quantity is a scalar: 3 WB gains, μ_s, ΔEV, σ-ratio, gradient `a`, ρ, 3 night-tint components, saturation, strength — a perfect fit for the params bag; one new `case` in `applyAdjustment` implements the fused transfer (with the sRGB↔linear LUT pair; the accumulator is already float). Serialization: adjustment kinds round-trip by name through docio — a new name token is additive schema growth. Undo/redo needs zero new machinery.

**Commit shape (single undo step):**

```
CompositeCommand "Sky from ‹layer›" [
  AddLayerCommand( TextureLayer(sky params, baked via applyBakedTextureCache), index = below photo ),
  SetLayerMaskCommand( photoLayer, maskFromSelection(photo, ¬skySelection), label = "Mask sky" ),
  AddLayerCommand( AdjustmentLayer(PhotometricMatch, params…, clipToBelow = photo) )
]
```

Sky **below**, photo masked to its foreground above: feathered foreground edge composites over the new sky (correct fringing), sky plate stays whole and editable, each piece independently disable-able afterwards — strictly better than sky-on-top.

The existing `TextureGenHost::commit` (params + baked, one AddLayer/SetTexture) grows an optional conform payload — or a parallel `commitConform` hook — wired where the host is built (`app_window.cpp:3757–3807`).

**Fallback if minting an AdjustmentKind is deferred:** bake the transfer with a whole-layer `SetLayerPixelsCommand` inside the same CompositeCommand — one undo step, but destructive; acceptable MVP, not the recommendation.

---

## 7. UI flow, threading, budgets

**Button.** Top of the sky control stack, above `sky:clouds` — a full-width action row "Estimate from layer…", behaves exactly like a preset (one `applyEdit`). Disabled with a hint when the host reports no usable source (mirrors the wand's "active layer has no pixels" message, `app_window.cpp:5828`; same copy family: *"Rasterize it, or select a pixel layer."*).

**Hover bubble.** New `EstimateBubble : BubbleFlyout` (ColorFlyout/GradientFlyout pattern): ≤160 px thumbnail of the active layer + its name + one line, *"Analyzes this layer; sets horizon, camera, sun & sky to match."* Base rules already encoded: construct before host shown, `setAvoidRect(previewArea)`, native-Wayland plain-panel fallback, Esc closes. Thumbnail fetched lazily on first hover through the host, cached against the layer's `contentRevision`.

**Host growth.** `TextureGenHost` gains:

```cpp
struct SourceLayer { std::string name; common::Image docImage; };   // doc-space, via activeLayerDocImage
std::function<std::optional<SourceLayer>()> sourceLayer;            // nullopt = disable button
// plus the conform payload on commit (§6.3)
```

**Threading.** A small `EstimateWorker` mirroring `TextureRenderWorker`'s shape (one thread, epoch-coalesced, cancel checked **between stages** and between probe renders, `progressFraction()` from fixed stage fractions: proxy .10 / horizon .25 / segmentation .45 / sun .60 / probes .90 / invert 1.0). Calls `renderTexture` for probes — pure, safe off-thread. UI reuses the dialog's poll-timeout + footer progress + Cancel plumbing (`pollOnce`/`cancelBake` precedent). Worker mandatory (probe stage worst case ~0.5 s); never on the UI thread.

**Results landing.** One `applyEdit` (label "Estimate from layer") stamping only parameters whose stage confidence cleared its floor → `syncControls()` → `requestProxy()`. Dialog transaction semantics give the undo story; additionally keep a one-slot pre-estimate `SkyParams` snapshot surfaced as a "Revert estimate" link in the summary. The info panel (`updateSkyInfo` precedent) shows: per-quantity values, confidence badges, the morning/afternoon note with clickable alternative, and every "left unchanged" honesty line.

**The mask+harmonize toggle.** A checkbox near the footer, sky generator only, enabled only when a source layer exists **and** the last estimate's segmentation confidence cleared its floor: *"Also mask the photo's sky and match it to this sky."* Runs **only on ACCEPT**: after the normal full-res bake completes behind the existing progress bar, S6–S8 continue on the same progress scale (bake 0–70%, mask 70–90%, harmonize 90–100%), all cancellable; then the CompositeCommand commits and the dialog hides. Live preview never runs segmentation (the proxy preview may cheaply overlay the *proxy* mask outline as a hint — stretch goal).

**Budget summary:** click ≈ 0.3–0.7 s (worker); ACCEPT adds ≈ 0.4–0.6 s at 4K single-thread, ~half with `parallelRows`; hover thumbnail ≈ 10 ms; all UI-thread work stays under one frame.

---

## 8. Pushback: out of scope without ML (set expectations here)

1. **True relighting / cast-shadow synthesis or removal.** Requires depth/geometry/intrinsic decomposition — solved only with learned priors. The photo's shadows keep their daytime direction under a sunset sky. Scoped out; the harmonization is a *global color grade* and the UI copy must never promise more.
2. **Date or location estimation from the photo.** Cozman & Krotkov (1995) needed timed image *sequences*; one frame's sun position at unknown time constrains nothing usable. Place+date stay pinned to the user's values; only clock time is solved.
3. **FOV without EXIF.** Vanishing points need man-made scenes and fail silently on landscapes; single-frame sky-appearance FOV fitting is too weak. Mosaic currently discards EXIF at load (verified). **FOV is never touched.** When S41/S42 lands metadata, `FocalLengthIn35mmFilm` → `fovDeg = 2·atan(18/f35)` becomes a five-line addition.
4. **Segmentation through fine structures** (hair, bare branches, chain-link, power lines): partial at best — no alpha matting in v1. The output is a *starting* mask; the confidence system warns when the border is ragged. Sky through apertures likewise v1-out.
5. **Reflections and translucency:** water, windows and eyes keep reflecting the *old* sky; not detectable classically with acceptable reliability.
6. **Blown skies:** a fully clipped white sky carries no chroma/gradient signal — degrades to "bright/overcast" defaults and says so.
7. **Night sources in day-for-night:** no lit windows, lamps, or light pools will be synthesized. The moon/stars/sky glow come solely from the generated sky layer.
8. **Morning vs. afternoon:** physically unresolvable from one frame; explicit user-visible choice, defaulting nearest-current-time then afternoon.
9. **White-balance ambiguity:** unknown camera WB shifts every color cue; band-ratio features and the gray-edge/white-patch blend mitigate but cannot eliminate — reduced confidence, honestly reported.
10. **Cloud reproduction:** coverage estimated coarsely; cloud *type*, placement and shapes not matched.

---

## 9. Bibliography (algorithm provenance, with dates)

**Horizon / sky boundary:** Ettinger, Nechyba, Ifju & Waszak, IROS 2002 + *Towards Flight Autonomy*, 2002; Duda & Hart, CACM 1972 (Hough); Fischler & Bolles, CACM 1981 (RANSAC); Gershikov, Libe & Kosolapov, IJAIS 6(1–2), 2013; Lie, Lin, Lin & Hung, PRL 26(2), 2005; Shen & Wang, IJARS 2013.

**Sky pixel models:** Luo & Etz, IEEE TIP 11(3), 2002; Zafarifar & de With, ACIVS 2006; Adams & Bischof, IEEE TPAMI 16(6), 1994; Otsu, IEEE TSMC 1979; Felzenszwalb & Huttenlocher, distance transforms, TR 2004 / ToC 2012 (already in `selection.cpp`).

**Sun & sky illumination:** Cozman & Krotkov, ICRA 1995; Lalonde, Narasimhan & Efros, IJCV 88(1), 2010 + IJCV 2012; Perez, Seals & Michalsky, Solar Energy 1993; Preetham, Shirley & Smits, SIGGRAPH 1999; Hosek & Wilkie, SIGGRAPH 2012 (vendored); Nishita et al., SIGGRAPH 1993 (in `atmosphere.*`); Meeus, *Astronomical Algorithms*, 2nd ed. 1998 (in `solar.*`/`lunar.*`).

**Color transfer / constancy / night:** Reinhard, Ashikhmin, Gooch & Shirley, IEEE CG&A 2001; Ruderman, Cronin & Chiao, JOSA A 1998; Pitié, Kokaram & Dahyot, ICCV 2005 / CVIU 2007 (surveyed, rejected for v1); von Kries 1902; Lam (Bradford) 1985; CIECAM02/CAT02 2002; Buchsbaum 1980 (gray-world); Land 1977 (Retinex); Finlayson & Trezzi, CIC 2004 (Shades of Gray); van de Weijer, Gevers & Gijsenij, IEEE TIP 2007 (gray-edge); Thompson, Shirley & Ferwerda, JGT 7(1), 2002; Jensen, Premože, Shirley, Thompson, Ferwerda & Stark, UUCS-2000-016, 2000; Ward Larson, Rushmeier & Piatko, IEEE TVCG 1997; Kirk & O'Brien, SIGGRAPH/TOG 30(4), 2011; Caprile & Torre, IJCV 1990 (surveyed, rejected).

**Excluded as ML:** Hoiem/Efros/Hebert geometric context (2005), Fefilatyev et al. (2006), Tao et al. SkyFinder (2009), all deep sky-replacement work (2019+) — listed so the implementation agent doesn't reach for them.

---

**Implementation-order suggestion:** (1) `EstimateWorker` + S0/S1 horizon → pitch/roll with the sign-pinning unit test; (2) S2 proxy segmentation + confidence plumbing + summary UI; (3) S3/S4 sun + probes + exposure/turbidity; (4) S5 almanac inversion + the mode split; (5) the toggle path: S6 full-res mask → `maskFromSelection`; (6) `AdjustmentKind::PhotometricMatch` + S7/S8 commit; (7) `EstimateBubble`. Each step lands user-visible value independently and is headless-testable (synthetic renders from the generator itself make perfect fixtures: render a known sky, feed it back, assert recovered parameters — a closed-loop test no external dataset can match).

---

## 10. As built (phase 1, 2026-07-16): corrections and calibrations against the real code

The S0–S7 engine, the `SkyEstimateWorker`, the `AdjustmentKind::PhotometricMatch` compositor
case and the §6.3 commit builder shipped in `src/core/texture/sky_estimate.*`,
`sky_estimate_worker.*`, `sky_estimate_commit.*` and `src/render/compositor.cpp`; the closed-loop
battery lives in `tests/test_sky_estimate.cpp`. The dialog wiring (§7's button, bubble, host
growth, toggle) is phase 2. Deviations from the draft above, each verified by the closed-loop
tests:

- **§2.2 horizon override** — see the rewritten §2.2: the Ettinger covariance search was
  replaced pre-implementation by the DP-border + RANSAC primary (standing constraint C-A1).
- **C-A1-conservative substitutions.** Every "percentile" in the draft became a
  distribution-free stand-in: the prior's Lum term normalizes by the *mean of the ≥-mean pixels*
  (two mean passes); the Sobel edge gate is a fixed threshold with one fixed relaxation; the
  EM-lite refit selects supporters at a fixed fraction of the prior map's own maximum. (The S7
  white-patch 99th percentile stays — color constancy on the foreground is neither horizon
  detection nor segmentation, so C-A1 does not reach it.)
- **Color model.** Lobe scoring is a *flat-top* Gaussian (full score within one Mahalanobis
  unit): a sky dome's chromaticity is a smooth FAMILY, and a plain Gaussian around the refit
  mean scored the dome's own gradient as "not sky". The refit pads sigma (+0.02, floors
  0.05/0.035) because supporters are the prior's top slice, chromatically narrower than the
  real family. A refit that moves the second lobe > 0.05 off neutral marks it `grayAdaptive`
  and stands its brightness gate down (a dusk sky is legitimately dim).
- **DP calibration.** The regional terms consume sqrt(P) — the raw prior's absolute level
  (legitimately ~0.3 over a dim twilight sky) must not read as "more ground than sky"; the
  compression keeps the ordering and lets the edge term decide. λ was recalibrated to 14/H
  (scale-invariant) against these normalized terms — the draft's 2/px belonged to unnormalized
  sums. The pairwise term is minimized exactly with two monotonic-deque sliding minima,
  O(W·band). The fine pass bands around the fitted line only when the line CLEARED its 0.4
  floor (the draft's "whole frame if S1 failed", enforced); the whole-estimate abort tests the
  UNBANDED coarse fraction plus prior separation, so a rejected line cannot manufacture a sky
  fraction.
- **§4.2 probe match, exposure-consistent synthesis.** The renderer's night floors (airglow,
  twilight-glow fill) are constant in DISPLAY space, so a twilight dome's apparent gradient and
  chroma *change with exposure* — an exposure-0 probe cannot match a camera-exposed twilight
  photo. Each probe therefore renders twice: once at 0 EV to imply the exposure that matches
  the photo's median luminance, then again AT that exposure for the signature (skipped when
  |EV| ≤ 0.5). The final exposure estimate = probe EV + residual, clamp widened to ±6 EV (the
  draft's +4 cap predates exposure synthesis). D() gained one gentle brightness-plausibility
  term (0.02·ΔEV²) — golden-hour-at-high-turbidity and civil-twilight-at-low-turbidity are
  chromatic near-twins that 2 EV of median-luminance disagreement separates.
- **Match confidence.** The runner-up for the margin term is the best probe of a different
  family along the APPLIED axis (another elevation on the full grid; another turbidity when the
  sun disc pinned the elevation) — turbidity neighbors at one elevation are physical near-twins
  and must not read as elevation ambiguity. The absolute-D guard is soft (scale 0.5): a twilight
  match legitimately lands near D ≈ 0.2 (the photo's 8-bit quantization floor).
- **Sun honesty.** When no horizon was accepted, the disc maps through the CURRENT generator
  camera, so its confidence is capped at 0.45 with a "camera unknown" note.
- **S6.** An all-sky estimate (fraction > 0.98) short-circuits to a full-coverage Selection
  (§2.4's "mask = everything") — floods anchored to bright seeds would honestly under-cover a
  graded dome. The full-res texture term widens its box mean with the proxy decimation factor
  so "smooth at proxy scale" keeps its meaning.
- **S7.** σ_t/σ_s is read as the MODEL's contrast ratio between the two elevations (the ln-lum
  σ of the two k(el) dome probes), keeping the whole target parametric (C-C1); the draft's
  wording left the σ pairing ambiguous. The compositor's highlight shoulder is anchored at
  max(original, knee) and compresses only actual raises — an empty params bag is an exact
  no-op.
- **§3.1 verified.** The v_h formula and the roll sign are as drafted: τ_cw (atan2 of dy/dx in
  image coordinates, y down) equals `rollDeg` — pinned against `SkyCamera::project` in
  `test_sky_estimate.cpp` before any prose was trusted. `shiftY` resets to 0 on apply, FOV is
  never touched (no EXIF plumbing existed when this shipped; the S41/S42 EXIF read slice has
  since landed on main and §3.2's five-line wire-up is now unblocked for a later session).
- **Failure mode added to §2.4's table:** a twilight sky over a ground *as bright as the glow
  itself* (warm floodlit foreground) defeats the border honestly — the estimate degrades to
  "unchanged" with the low-confidence note; dark-silhouette dusk grounds (the common case)
  recover fine.
- **Measured (1280×720 dome-only fixture, no sun disc — the worst case, forcing the full
  80-probe grid — 8-core, pixel loops optimized in both presets):** estimate S0–S5 ≈ 3.8–3.9 s,
  S6 ≈ 0.23–0.26 s, S7 ≈ 0.01–0.02 s. The S0–S5 figure is dominated by the sub-horizon probe
  renders: the twilight integrator gained multiple scattering mid-build (main's night-overhaul
  round) and the exposure-consistent synthesis renders each twilight probe twice, so the
  §5.4 draft budget (0.5 s worst) no longer holds for the sun-less path — the stage runs in
  the cancellable worker, and the design's own session signature cache (§4.2, keyed on
  camera bucket) is the phase-2 lever if a click-to-summary latency of ~4 s bothers in
  practice. A visible sun disc pins the elevation and collapses the grid to 5 probes
  (≈ 0.1 s). Closed-loop recovery achieved:
  pitch ±1.5°, roll ±1.2° (signs pinned), visible-sun az/el ±2.5°, sun-less elevation ±3.5°
  across el ∈ [−6°, +8°] with exposure within ±2.5 EV and turbidity within one grid step
  (coupled look-alike bands — the recovered triple reproduces the photo's appearance, which is
  the estimate's actual contract), segmentation IoU > 0.93 with the holes policy pinned
  pointwise.

## 11. Phase 2 (dialog wiring, 2026-07-17): placement decisions

§7 shipped in `src/ui/texture_generator_dialog.*` (inside the S55-g per-generator builder
pattern) + the host growth in the app window; tests in `tests/test_sky_estimate_dialog.cpp`.
Two placement decisions differ from §7's sketch, recorded here:

- **The summary lives in the control stack, not the info panel.** The under-preview
  SkyInfoPanel is a fixed-~96px DRAWN widget already full with the almanac rows + the analog
  clock, and it takes no clicks -- the estimate summary (up to ~8 lines) plus the Revert button
  and the morning/afternoon swap (both interactive) sit instead directly under the "Estimate
  from layer" action row at the top of the scrolling sky stack, where they read as one block
  with the control that produced them.
- **The session probe-signature cache was dropped.** §4.2's cache was keyed on (pitch, roll,
  FOV, aspect); the exposure-consistent probe synthesis (§10) makes twilight signatures a
  function of the photo's implied exposure too, so the key no longer holds and the cache stops
  being the simple win it was scoped as. Re-estimates in one session are rare; the estimate
  runs on the cancellable worker either way.

EXIF consumption (§3.2's future work) is live: the S41/S42 read slice landed on main, File →
Open and Open-as-Layer stamp `Layer::setExif`, and the estimate treats FocalLengthIn35mmFilm
as a measured FOV (the whole pipeline runs at it) while DateTimeOriginal + GPS prefill the
observer under the existing first-set-wins moon-source latch, the local wall time serving as
the morning/afternoon tie-break.
