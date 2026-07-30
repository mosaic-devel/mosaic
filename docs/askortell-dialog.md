# The Ask-or-Tell dialog (`ui::AskOrTellDialog`)

A reusable themed modal for the app's "interrupt the user" moments: ask a question, warn about
something, report a result — optionally with a progress bar in between. Its two concrete consumers:

- the **HEVC caveat** on drag-and-drop open (a warning with a snarky remark and a choice) — not
  wired yet, and
- **file-corruption recovery** — WIRED (S48): `File→Open` classifies the reader's report
  (`ui::recovery_flow.hpp`, tested headlessly) and fires the flow-3a–3e + 4 faces below.
- **crash restore** — WIRED (S48): the recovery journal (`io::native::JournalSession`) autosaves
  during editing; on open (and at app start for untitled docs) `ui::recovery_journal.hpp` classifies
  the journal and fires flow 1 (Restore) or flow 2 (orphan). Flow 6 (the advisory lock) is next.

**This dialog is the app's ONLY message box.** FLTK's stock `fl_alert`/`fl_message`/`fl_choice`
family is banned from `src/`: those windows ignore the theme, the icon language and the
button-order convention, so they read as a different program. Anything that needs to tell or ask
builds a `Stage` — `MainWindow::tellError` is the ready-made "something failed" wrapper for code
that can reach it, and a dialog that cannot (it is not a `MainWindow` member) constructs an
`AskOrTellDialog` on the stack and calls `ask()` with its own window as the host. Both ICC-profile
rejections (New Document ▸ Custom…, Settings ▸ CMYK profile) were converted from `fl_alert` this
way. `fl_beep()` is *not* covered by the ban — it is a sound, not a window, and it is the right
answer where a second modal would be wrong (the file-picker-in-flight quit guard, where opening
one would hang the portal's nested wait loop).

Code: `src/ui/ask_or_tell_dialog.{hpp,cpp}`. Tests: `tests/test_ask_or_tell_dialog.cpp`.

## Anatomy

```
+--------------------------------------------------+
| [icon]  Title (bold 15)                          |
| 48x48   Body text, wrapping in the text column,  |
|         as tall as it needs to be.               |
|         [===========progress==------------]      |  <- optional
|                                                  |
|                     [ Button ] [ Button ] [ OK ] |  <- 0..3, right-aligned
+--------------------------------------------------+
```

- Fixed width (470 px, the error-dialog convention); height follows the content. The body height
  is measured (`fl_measure`) at the text column's width, so long messages grow the window.
- The **icon** (top-left, 48 px) is one of four embedded SVGs rasterized through nanosvg — see
  "Icons" below.
- **Buttons** carry arbitrary caller strings, laid out left → right and right-aligned as a row.
  The **rightmost button is the default**: it is the one accent-filled button (the FilledButton
  look) and it fires on **Enter**. Each button sizes to its label (min 84 px).
- The **progress bar** (`ui::ProgressBar`, a new shared widget in `ui/widgets.hpp`) spans the
  text column and supports **determinate** (a growing accent fill) and **indeterminate** (a
  sweeping accent segment, ~1.4 s per traverse at 30 Hz) modes. Info-style faces simply leave it
  hidden.

## The Stage model — switching faces in place

The dialog is driven by a plain-data `Stage` struct: icon + title + message + button labels +
cancel mapping + whether the progress bar shows. `present(stage)` applies one wholesale — while
hidden **or while already shown**. A multi-stage flow (the corruption-recovery shape) is just a
sequence of `present()` calls on one instance: the window relayouts, keeps its **centre**
anchored while its height changes, and never flickers through a hide/show.

Implementation note: the three buttons are a **fixed pool** that is relabelled/hidden per stage,
never deleted — so a `present()` issued from inside a button callback (the normal way to advance
a staged flow) cannot destroy the widget FLTK is still delivering the click through.

Reopen robustness: every relayout pins the WM size hints (min == max) to the *current* face
before resizing, and `present()` branches on `visible()` — normalizing a half-alive native
window (hidden for real) before re-mapping. Both guard the same failure class: a re-shown
fixed-size window coming back at a stale size (the previous face's height) because the window
system restored or clamped to remembered geometry.

## Answer semantics

| Input | Effect |
|---|---|
| Click button *i* | records result *i*, fires the `setOnButton` callback, ends a pending `run()` |
| Enter / KP-Enter | the rightmost button (none → inert) |
| Escape / WM close | the stage's **cancel target** (see below), else inert |
| `finish(r)` | programmatic result (worker completion); no callback, window stays up |

The cancel target defaults to `kCancelAuto` = the **leftmost** button (with a single button, that
button). Override `Stage::cancelButton` when the leftmost is not the safe choice (the classic
"Don't save / Cancel / Save" row wants `cancelButton = 1`), or set `kCancelNone` for a forced
choice. A stage with **zero buttons** is program-driven: Escape and the WM close are deliberately
inert so a mid-write recovery cannot be killed from the keyboard; the caller must `present()`
onward or `hide()`.

Note the dialog never hides itself after a choice — that is what lets staged flows continue in
the same window. The one-shot helpers do it for you.

## Calling patterns

**One-shot ask/tell** (blocking):

```cpp
ui::AskOrTellDialog dlg;
ui::AskOrTellDialog::Stage s;
s.icon = ui::AskOrTellDialog::Icon::Warning;
s.title = "HEVC is a licensing minefield";
s.message = "...";
s.buttons = {"Cancel", "Open anyway"};
if (dlg.ask(s, mainWindow) == 1) { /* proceed */ }
```

**Staged, blocking between stages** (`run()` returns but the window stays shown):

```cpp
if (dlg.ask(confirmStage, win) != 1) return;   // ask() hides; or use present()+run() to keep it up
dlg.present(progressStage, win);                // no buttons, progress = true
// ... chunked work on the main thread: dlg.setProgressFraction(f); Fl::check(); ...
dlg.present(summaryStage);                      // "Recovered N MB" [Close] [Open]
const int r = dlg.run();
dlg.hide();
```

**Worker-driven / non-blocking**: `setOnButton` receives every click; a worker thread posts
`setProgressFraction()` / `finish()` through `Fl::awake`. The debug exerciser below is written in
this style (timer-driven).

`present()` starts a `progress` stage **indeterminate** (duration unknown yet); the first
`setProgressFraction()` flips the bar to a growing fill. `run()` returns `kNoChoice` if the
window was hidden without a choice.

## Icons

Five professionally drawn colour SVGs in the app-icon gradient language (vertical light-to-dark
gradients, top sheen, soft rim — see `docs/icons-needed.md` identity rules), designed on a 64×64
viewBox and rasterized at 48 px into the dialog:

| `Icon::` | Asset | Design |
|---|---|---|
| `Info` | `assets/icon_info.svg` | blue circle badge, geometric "i" |
| `Question` | `assets/icon_question.svg` | violet circle badge, open-hook "?" |
| `Warning` | `assets/icon_warning.svg` | amber rounded triangle, tapered "!" |
| `CorruptFile` | `assets/icon_corrupt_file.svg` | paper page, folded corner, glitch-sheared bottom slices, small amber warning badge |
| `Restore` | `assets/icon_restore.svg` | clean paper page (same silhouette family as CorruptFile, no damage), green circle badge with a counterclockwise restore arrow |

`Restore` exists because the crash-journal offer must NOT wear `CorruptFile`: the file on disk is
healthy there — the journal is evidence of a crash, not of damage — and a "broken file" icon
would tell the user a lie at exactly the moment the dialog is trying to earn trust.

They are **embedded at build time** (`mosaic_embed_asset` in `src/ui/CMakeLists.txt` →
`<assets/icon_*_svg.hpp>`) and rasterized at runtime via `common::rasterizeSvg`, exactly like the
app icon — no runtime file dependency. The SVGs stay inside nanosvg's feature envelope: no
`<text>` (glyphs are hand-built geometry), no filters/masks/clip-paths/CSS; gradients and paths
only. The unit tests rasterize all four through nanosvg and assert real pixel coverage plus
pairwise distinctness.

## Alert sounds

`present()` asks the host for its own alert sound (`platform::playSystemSound`,
`src/platform/system_sound.{hpp,cpp}`) — the same noise a native dialog makes. The **icon** picks
it, because the icon already *is* the face's severity:

| `Icon::` | `SystemSound::` | Linux event-id chain (first that resolves) |
|---|---|---|
| `Info` | `Information` | `dialog-information` → `message` → `bell` |
| `Question` | `Question` | `dialog-question` → `dialog-information` → `bell` |
| `Warning` | `Warning` | `dialog-warning` → `dialog-information` → `bell` |
| `CorruptFile` | `Error` | `dialog-error` → `dialog-warning` → `bell` |
| `Restore` | `Question` | (as `Question`) |

`Restore` rides the *question* sound for the same reason it has its own icon: the crash-journal
offer is a choice about a healthy file, so an error tone would tell the same lie `CorruptFile`
would.

**⚠ The fallback chains are load-bearing, not defensive padding.** The Sound Naming Specification
listing an id does not mean any theme ships it — the reference `freedesktop` theme has
`dialog-information`/`-warning`/`-error` but **no `dialog-question`**, and a missing id fails
(synchronously, `CA_ERROR_NOTFOUND`) in silence. Without the chain that would mute exactly the face
Mosaic shows most often. The winning index is remembered per meaning, so the steady state is one
call.

Backends, all best-effort and none of them ours to configure:

| Platform | Backend | Note |
|---|---|---|
| Linux | **libcanberra** (optional dep) | plays the user's XDG sound theme; honours their event-sounds switch |
| macOS | `NSBeep()` | macOS has *one* alert sound by design; all four meanings correctly land on it |
| Windows | `MessageBeep(MB_ICON*)` | the four scheme sounds, one per meaning |

**There is no in-app on/off**, deliberately: every backend routes through a preference the OS
already owns, so a user who has muted UI sounds hears nothing without Mosaic knowing, and a second
switch for the same preference is the no-toggle-for-strictly-better case. FLTK's `fl_beep` is *not*
a fallback where libcanberra is missing — its X11 path is `XBell` (routed nowhere on a modern
desktop) and its Wayland path writes a BEL byte to `stderr`, which spams the launching terminal
instead of making a sound.

Two rules the implementation enforces:

- **Sound fires on the hidden → shown transition only.** `present()` is also the in-place restyle
  path, and a staged flow drives it two or three times on one window; the sound announces "the app
  is interrupting you", which happens once. A caller that wants to punctuate a later stage calls
  `platform::playSystemSound` itself.
- **The subsystem is OFF until `main()` arms it** (`enableSystemSounds(true)`), so a test binary or
  headless tool can never make the machine beep — this file's own suite presents thirteen faces.
  `systemSoundsEnabled()` exists so a test can *assert* that silence rather than assume it.

## Theming

Colours come from the live palette: the window ground is `panelBg` (re-applied via a
`ThemeSubscription`), labels sit in semantic FLTK slots (`FL_FOREGROUND_COLOR`), the neutral
buttons are `ui::FlatButton`s, and the default button/progress fill read `accent`/`onAccent`
live. Runtime re-themes (Settings → dark/light/system) restyle an open dialog in place. The
buttons' *pressed* fill follows a re-theme too — see `FlatButton::reapplyTheme`, whose contract
(chain to the base, then re-apply your own fill) the `StageButton` here obeys.

## Debug exerciser

Debug builds (`CMAKE_BUILD_TYPE=Debug`, which defines `MOSAIC_DEBUG` on `mosaic_ui` — see
`src/ui/CMakeLists.txt`) add **two** Help-menu exercisers, one per recovery flow, each wearing
that flow's settled faces verbatim (the section above) so the visual pass rehearses the real
product. They are deliberately separate — chaining the crash-restore face into the damage faces
would demo a product that does not exist:

- **Help → Test Ask-or-Tell: Crash Restore…** — flow 1's single forced-choice face (Restore
  icon, `Unsaved changes found`, Escape deliberately inert). Both answers end the demo, exactly
  like the real flow: what follows a restore is the canvas and a status line, never another
  face.
- **Help → Test Ask-or-Tell: File Recovery…** — flow 3c staged: a CorruptFile `Checking
  imaginary.mosaic` face whose indeterminate sweep **holds until "Proceed to next stage"**
  (nothing is actually being recovered, and the hold leaves time to inspect the sweep) → a
  timer-driven determinate fill under the same title (the real probe is one face whose bar
  flips mode) → the concrete-outcome damage ask (conservative accent default) — or a Warning
  face if aborted mid-flight — all as in-place restyles.

Release builds compile out the menu items and the demo code. The demo strings are deliberately
not `_()`-wrapped so the translation template stays free of dev-only text.

There is also an opt-in test driver: `MOSAIC_DEMO_DIAG=1 mosaic_tests -tc="DIAG*"` (debug
builds) drives the file-recovery demo end to end, then reopens the same window as the
crash-restore offer (asserting Escape is inert on the forced-choice face), logging
face/height/visibility at each step; `MOSAIC_DEMO_SHOT=/path.png` additionally screenshots the
reopened dialog via spectacle, capturing what the *compositor* shows rather than client-side
state. It is skipped by default — it maps windows for several seconds.

## Recovery-family flows — settled copy (2026-07-08)

The `.mosaic` recovery surface (spec: `docs/mosaic-native-format.md` §2.6/§2.8, Round 13). Copy,
icons, buttons, and cancel semantics were settled with the user on 2026-07-08 — the
document-model integration slice wires these verbatim. House rules that shaped them:

- **Tone: straight, warm, zero jokes.** Recovery is a trust moment; reassurance comes from
  specifics (counts, timestamps), not charm. Snark stays in low-stakes dialogs (HEVC caveat).
- **Terminology: "Restore" vs "Recover", never mixed.** *Restore* = the crash journal (your
  unsaved changes come back; the file is fine). *Recover* = damage (salvage from a broken file).
- **Disk honesty in every flow:** restored/recovered work arrives as unsaved in-memory state
  (dirty title per S18-d); the user's file is written only by an explicit Save (§0 hard rule) —
  and the copy says so with ONE canonical sentence, verbatim wherever it appears: `Nothing is
  written to your file until you save.` (User-corrected from an earlier "either way" phrasing —
  the invariant is app-wide, so it must read as a promise, not as a property of one choice.)
- All strings `_()`-wrapped, printf placeholders (house style); plural forms resolved at build
  time; buttons are plain words (no glyphs — see `mosaic-ui-gotchas`).

**1. Crash-journal restore offer** (open time; journal binding verifies). WIRED (S48,
`offerJournalRestore`). Icon `Restore`.
Title: `Unsaved changes found`. Body:

> Mosaic didn't close cleanly the last time this document was open. Your unsaved work — %d
> changes, the last from %s — was kept safe.
>
> Restore picks up exactly where you left off. Nothing is written to your file until you save.

Torn-tail addendum (only when replay stopped early): `The very last change was cut off and
couldn't be kept.` Buttons `[Discard changes] [Restore]`, Restore = accent default,
`cancelButton = kCancelNone` (forced choice — Discard must never fire from a stray
Escape/Enter; Restore being risk-free is why it holds the default). Counts/timestamp come from
the read-only journal replay + journal mtime, both known before presenting. **After Restore: no
summary face** — the dialog closes, the canvas shows the restored state under a dirty title, and
the status bar reports `Restored %d unsaved changes` (torn tail: `Restored %d of %d unsaved
changes — the last was incomplete`). Untitled variant (offered at app start from the recovery
dir): title `Unsaved untitled document found`, restore opens a new untitled window.

**2. Orphan journal** (binding matches nothing — the file was saved elsewhere or replaced).
WIRED (S48). Icon `Warning`. Title: `Old unsaved changes no longer match this file`. Body:

> Mosaic kept unsaved changes for this document from an earlier session, but the file has since
> been changed or replaced outside Mosaic, so they can no longer be restored into it.
>
> Keep the recovery file if you want to inspect it later; otherwise it can be discarded.

Buttons `[Keep the file] [Discard]`, Discard = accent default, Escape → leftmost (`kCancelAuto`:
Keep, the safe non-destructive answer). Satisfies §2.6 "discarded with the user's consent, never
silently."

**3. Damaged file** (open time) — the file-got-hurt-outside-Mosaic family, a DIFFERENT
mechanism from flow 1 (there the file is healthy and the app died; here the file itself took
damage) and never blended with it. The recovery machinery reports distinct conditions; most are
**tells** (no decision exists), exactly one is a genuine **ask**. One dialog instance stages
whatever combination applies.

- **3a — parity repaired everything** (`rsReconstructed > 0`, nothing lost): **no dialog.**
  Status bar: `Repaired %d damaged blocks while opening`. Data is exact-and-verified; a modal
  would be crying wolf.
- **3b — checkpoint areas lost beyond parity** (`lostEntries > 0`): a TELL — display resolution
  is automatic (where an older generation of the key survives in retained history it stands in,
  marked; otherwise blank — strictly better than blank everywhere, so no choice to offer) and
  nothing touches disk. Icon `CorruptFile`, title `This file is damaged`, body:

  > %d areas of "%s" can't be read, even after repair. Where an earlier version survived in the
  > file's history it is shown in its place; anything else is left blank. Affected: %s.
  >
  > Nothing is written to your file until you save. Saving a copy now protects everything that
  > remains.

  Single `[Open]`. The affected list names layers/regions in words (document layer maps salvage
  key flags); canvas markers over stale/blank regions are future work, not Build 1.
- **3c — damage in the committed region with intact saves past it**: the one genuine ASK, and
  it is asked AFTER assessing — salvage is read-only, so Mosaic probes first and the user
  chooses between two **concrete outcomes**, not a gamble. Progress face (icon `CorruptFile`,
  title `Checking the file…`, no buttons, indeterminate → determinate as the damage extent is
  mapped), then the ask — icon `CorruptFile`, title `This file is damaged`, body:

  > Part of "%s" can't be read.
  >
  > Mosaic can open it as it was at the last complete save (%s), or open the recovered version:
  > %d of the %d newer saves came back intact, and the areas of the lost one show older
  > content. Affected: %s.
  >
  > Nothing is written to your file until you save.

  Declared-imprecise appends `Part of the record was destroyed, so this list may be
  incomplete.` Buttons `[Open recovered version] [Open last complete save]` — the conservative
  stop keeps the accent default AND Escape (`cancelButton = 1`) per spec §2.8. No summary face
  follows: the ask already contains the outcome. When 3b losses are also present, that report
  paragraph joins this face — one dialog, not two.
- **3d — an unfinished save at the tail, nothing recoverable beyond it** (a foreign writer's
  torn save, or a sync client captured mid-append; *our own* crash-mid-save leaves a journal
  and is flow 1): a TELL — the user may believe that save succeeded, and silence would gaslight
  them. Icon `Warning`, title `The last save didn't finish`, body:

  > "%s" ends in an unfinished save — whatever was writing it stopped partway. The unfinished
  > part was set aside; this file opens at the last complete save, from %s.

  Single `[Open]`.
- **3e — no clean fallback** (roots/index destroyed; full-scan reassembly): a TELL, not an ask —
  "the last complete save" does not exist as a verified state, so there is nothing to choose
  between. Icon `CorruptFile`, title `This file is badly damaged`, body:

  > The file's structure was destroyed. Mosaic reassembled everything it could find, but some
  > content may be older than the last save, or missing. Affected: %s.
  >
  > Nothing is written to your file until you save. Saving a copy now protects everything that
  > was found.

  Single `[Open]`.
- **Composition rule:** damage faces resolve first; if a recovery journal then binds to the
  state just reached (the R11-A4 compose), flow 1's face follows **in the same window** via
  `present()`. Distinct mechanisms keep their distinct faces — the flows are staged
  back-to-back, never merged into one message.
- **Default-asymmetry note (deliberate):** flow 1 gives *Restore* the accent because journal
  content is chain-verified and exact; 3c gives the *conservative* choice the accent because
  salvage output is honest-but-imperfect (verified frames past a gap, stale flags). Different
  confidence, different default — spec §2.8 mandates the latter.
- **Test corpus:** `tools/corrupt_corpus` (S48) emits a damaged **Build-1** file per scenario
  above — a real 1920×1080 five-layer poster with seeded save history, each fixture self-verified
  against the real reader as it is written, and its self-check wired into ctest. Generate with
  `corrupt_corpus <dir>`; each file opens in Mosaic and lands on exactly the flow it demonstrates
  (3a–3e + the dual-writer 4). The old `~/Desktop/corrupted` (pre-Round-10 Python wire format,
  garbage-rejection only) is retired. Journal fixtures (flows 1/2) are **planted** with
  `corrupt_corpus <dir> --plant`: they key on the file's absolute path + `$XDG_STATE_HOME`, so a
  matching journal is written under the state dir alongside each file rather than shipped as bytes
  (run the app afterwards with the same `$XDG_STATE_HOME`). Move a planted file and flow 1 degrades
  to flow 2 by construction.

**4. Dual-writer root conflict** (rare; both lineages recovered complete — spec §2.8, Round 13).
Icon `Warning`. Title: `Two programs saved into this file`. Body:

> This file holds two separate save histories — it was open and saved in two places at once.
> Both survived intact.
>
> Mosaic will open the version that was saved first. To keep the second one too, save it as its
> own file now — a later save may discard it.

Buttons `[Save other version as…] [Open]`, Open = accent default, `kCancelNone` (this dialog is
the one reliable moment to split the lineages; a later compaction Save legitimately drops the
foreign one).

**5. Save-time tail-check refusal** (spec §2.6, Round 13 D4). Icon `Warning`. Title: `The file
changed on disk`. Body:

> "%s" was changed by another program after you opened it. Saving now would overwrite that
> other work.

Buttons `[Overwrite] [Cancel] [Save a Copy As…]`, Save a Copy = accent default,
`cancelButton = 1` (the documented "Don't save / Cancel / Save" pattern).

**6. Advisory lock (§2.10).** WIRED (S48, `openMosaicAtPath` + `io::native::AdvisoryLock`). A
second instance finds a live lock → icon `Question`, `This document is already open in another
Mosaic window`, buttons `[Cancel] [Open read-only]` (`cancelButton = 0`: Cancel is the Escape
default; read-only opens without the lock or a journal, and Save routes to Save As so it can never
overwrite the file the other window owns). The lock is an OS advisory lock (flock) on a
recovery-dir file — never the user's document — so a stale lock (the holder died) is auto-released
and the next open acquires it cleanly, folding straight into ordinary journal-backed recovery
(flow 1). Untitled documents are not locked.

## Future work

- The HEVC caveat consumer (corruption-recovery is now wired — S48).
- The damage-then-restore compose is currently staged as back-to-back dialogs (damage face closes,
  then flow 1 opens), not a single-window `present()` chain; a salvaged "recovered version" then
  crash-protects with a self-contained journal, so on reopen it surfaces as flow 2 (orphan) rather
  than flow 1 — both acceptable Build-1 corners.
- The 3c progress face: the current wiring probes synchronously and shows the ask directly (a bar
  that fills instantly is theatre); wire the progress face if/when salvage assessment is slow.
- The "Affected: …" list resolves layer names from salvage-flagged keys where it has them (3c);
  checkpoint-loss tells (3b/3e) have only a count, so they omit the list. Canvas markers over
  stale/blank regions remain future work (not Build 1).
- The S41 export flow wants `ui::ProgressBar` and possibly a loss-warning face (its severity
  icons overlap this icon family — see `docs/icons-needed.md`).
- `StageButton` is the fourth incarnation of the local accent FilledButton pattern
  (fill_dialog / layer_effects_dialog / tool_options); hoisting one shared accent button into
  `ui/widgets.hpp` is a known cleanup, skipped here to keep the change contained.
