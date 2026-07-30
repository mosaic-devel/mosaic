# Motivational one-liners → menu-bar ticker (rework scope; not started)

> Reworks the existing "Cheesy motivational one-liners" annoyance (Settings → Annoyances). Today the
> line is a GPU tile composited into the present-pass backdrop and drifts diagonally UNDER the canvas,
> so it is almost never visible — which sucks and, when it is visible, reads as noise. This moves it to
> a quiet **ticker in the unused right of the menu bar**, keeps the gimmick, and DELETES the whole GPU
> path. It is a rework (replace render + placement), not a new feature: the content + cadence are kept.

## What ships (user-visible)
When the toggle is on, every few minutes an all-caps one-liner **slides down into the unused right
region of the menu bar, holds there for ~10 s, then slides back up and out** — a departure-board flap,
muted and understated, so the deadpan flatness undercuts the over-the-top content. **Between lines the
bar is empty**; each line lives only for its slide-in + hold + slide-out, then leaves. A line too long
to fit horizontally still crawls right-to-left during the hold (the shared `Marquee`), but the
enter/exit motion is vertical. It is only where a real menu bar exists (Linux/Windows); see macOS below.

### On-screen life of one line (as built)
The **`MenuBar` draws the ticker itself** in its own empty region past the titles (`MenuBar::drawTicker`,
called from `MenuBar::draw()`) — there is **no separate widget**. This was a deliberate correction: an
earlier cut used a `ScrollingLabel` parked over the bar as a sibling widget, coordinated by a
`setTickerHole` paint-hole. That overlap left the ticker's pixels un-refreshed while it sat idle (so
stale chrome could linger there as a "red line" until a forced redraw), and any attempt to fix the
redraw by wrapping the bar in a container broke the bar's FLTK/Wayland event routing (hover/click on the
titles). Drawing it inside the bar removes the overlap, the hole, and the event fragility in one move —
the bar already owns and correctly repaints that whole row.

`MenuBar::showTickerLine(text, holdSeconds)` starts a line; the ticker passes `kHoldSeconds = 10`
(motivation_ticker.cpp). One line's life:
1. **Enter** — the line starts translated up out of view (above the row) and **slides DOWN** into its
   resting position over ~0.35 s, smoothstep-eased.
2. **Hold** — it sits fully visible for `holdSeconds` (~10 s). A too-long line horizontally
   overflow-scrolls during this window (the shared `Marquee`).
3. **Exit** — it **slides UP** and out of view over ~0.35 s, then the bar clears the line.
4. The region is then **empty** until the next scheduled line, which slides in the same way.

The motion is driven by an eased `Fl::add_timeout` tick (~50 fps during the slide; a single wake at the
end of the hold), redraws the bar, and is **clipped to the ticker's sub-rect**, so it never trails or
overdraws the titles. The region + the too-short gate come from `tickerRegion()` (unit-tested).

## Settled decisions (user, 2026-07-01)
- **D1 Placement → the unused RIGHT region of the menu bar** (the in-window `MenuBar`, not a separate
  surface). Chosen over the status bar because a motivational line in the "serious" chrome is funnier.
- **D2 Delivery → a vertical slide-in / hold / slide-out per line** (updated 2026-07-04; superseded the
  original always-on horizontal crawl). Each line slides DOWN into the bar, holds ~10 s, then slides UP
  and out, leaving the bar empty between lines — deadpan = a mechanical departure-board flap. Reuses the
  existing `ScrollingLabel` (`presentSliding`); a line too long to fit still horizontally scrolls during
  the hold, so arbitrarily long lines in a narrow region are still handled.
- **D3 Region → ~¾ of the unused width**, right-anchored, so the crawl never pushes into the menu
  controls; **hidden entirely when the unused space is below a min width** (short window → no ticker).
- **D4 RTL → LTR for now**, one flag flips the scroll direction (and moves the region to the left) when
  bidi lands — the same deferred approach the current backdrop implementation already notes.
- **D5 macOS → absent.** No macOS support yet, and macOS should use a NATIVE menu bar (better there),
  which leaves no in-window strip to host the ticker. So on future native-menu-bar macOS the feature
  simply does not render ("macOS users motivate themselves"). No macOS code now; if ever wanted, the
  generic `ScrollingLabel` drops into the status bar in a one-liner. No design debt.

## Keep (unchanged)
- The **156 one-liners** in the `motivate` gettext domain (`po/motivate.pot`, `pot-motivate` target) +
  the size-deduced table + recent-repeat avoidance (`src/ui/motivational_lines.*`).
- The **cadence** (a random line every ~2–5 min) and the **Settings → Annoyances toggle**
  (`Settings::motivationalLines`, off by default) — only where it lives/renders changes.

## Add — the menu-bar ticker
- **Reuse `ScrollingLabel`** (`ui/widgets.hpp`; already used in the status bar for the colour-space
  name + status messages). It self-times a constant crawl, clips to its box, and reports
  `oneScrollSeconds()`. `setAlign(Right)` right-anchors it; `setText()` feeds each line.
- **`MenuBar` exposes its used width.** `MenuBar : public Fl_Menu_Bar` (`ui/menu_bar.hpp`) lays out its
  top-level items left-to-right and draws its own background (`menu_bar.cpp` `draw()`). Add a small
  accessor that measures the top-level items' total width (fl_measure each label + FLTK's item padding)
  → the right edge past which the bar is empty. The **unused width** = `bar->w() − usedRight − insets`.
- **Placement (MainWindow):** position a `ScrollingLabel` over the bar's leftover region:
  `x = usedRight + gap`, `w = ¾ · unusedWidth`, right-anchored to the bar's right inset; re-place it on
  every window/menu-bar resize (alongside the existing resize handling). **Hide** it (and skip the
  driver) when `unusedWidth < kMinTickerW`.
- **Driver:** move the line-selection + every-few-minutes cadence out of `VulkanCanvas`
  (`updateMotivation`/`spawnMotivation`) into a tiny MainWindow driver (or a `MotivationTicker` helper)
  that, on its timer, picks a line (`randomMotivationalLine()`), calls `label->setText(...)`, and shows
  the label; hides it when the toggle is off or the region is too short.
- **Deadpan styling:** muted colour (the dim inactive-menu-label tone, so it reads as chrome, not an
  accent), all-caps (kept). Each line slides in / holds ~5 s / slides out (smoothstep-eased vertical
  motion; see "On-screen life of one line" above), and the bar is empty between lines.

## Delete — the GPU backdrop path
- `shaders/canvas_present.comp`: **bindings 6 (placement SSBO) + 7 (coverage tile)** + the
  `compositeMotive`/backdrop-blend code. Reclaims two descriptor bindings.
- `render/window_renderer.{hpp,cpp}`: the motive buffer + its per-frame upload + the
  `setMotivation*`/staging members + the descriptor-pool/set-layout entries for 6/7.
- `ui/vulkan_canvas.{hpp,cpp}`: `updateMotivation()`, `spawnMotivation()`, `setMotivationalLines()`,
  and the tuning consts at the top of `vulkan_canvas.cpp`. The canvas stops driving the one-liner.
- Rewire `Settings::motivationalLines` (RunOptions + the Settings host callback in `app_window.cpp`,
  currently `m_canvas->setMotivationalLines`) to the new ticker's show/hide instead.

## Commit sequence
1. **Menu-bar ticker:** `MenuBar` used-width accessor; a `ScrollingLabel` placed in the leftover region
   (¾-width, min-width-hide, resize-tracked); the driver (line pick + cadence) feeding it; the
   Annoyances toggle wired to it. The GPU path still exists but is left inert this commit (or the toggle
   drives both briefly) — keep it reviewable.
2. **Remove the GPU backdrop path:** rip out shader bindings 6/7 + the renderer motive buffer/upload +
   the canvas driver + consts; simplify `canvas_present.comp`.

## Testing posture
Headless-testable: the used-width / ¾ / min-width-hide **layout math** (a `MenuBar` unit test), and the
driver **cadence + recent-repeat** logic (already covered for the table). `--gui-frames` stays
validation-clean after the shader binding removal (fewer bindings, not more). The **deadpan feel** — the
crawl speed, the muted styling, that it reads as understated — is the user's visual pass.
