# Keybindings (S51-b)

Mosaic's keyboard shortcuts are **remappable**, from **Settings ▸ Keybindings**. A remap takes effect
immediately — there is no restart, and on macOS the system menu bar is re-mirrored so its
⌘-equivalents move with it.

## The defaults are Mosaic's own, not Photoshop's

⚠ **This is a deliberate reversal of the original plan.** The roadmap line for S51-b said
"Photoshop-like defaults". It is superseded (user, 2026-07-29): the shipped keymap is **harvested
verbatim from the bindings the application already had**. Nothing moved to match another editor, no
binding was invented for a command that had none, and nothing was "aligned".

That has a consequence worth stating plainly: there are commands with no shortcut at all, and there
are places where Mosaic and Photoshop differ. Both are on purpose. Anything a user has already
learned in this app still works, and anything missing is a *gap*, to be filled by that user in this
very dialog — not by us moving somebody's muscle memory.

The harvest came from the three places bindings used to live:

| Where | What it held |
| --- | --- |
| `buildMenu()` in `src/ui/app_window.cpp` | the inline `menu->add(path, <chord>, cb, …)` accelerators |
| `textEditorGuardedActions()` in `src/ui/menu_bar.cpp` | the accelerators fenced off while a text caret is live |
| `kToolDefs` in `src/ui/tool.cpp` | the single-letter tool keys |
| `MainWindow::handle`, the unclaimed-key phase | `X` swaps the active colours, `D` resets them |

`tests/test_keymap.cpp` asserts every default against the literal it was harvested from
(`FL_COMMAND + 'n'`, `FL_SHIFT + (FL_F + 5)`, …). Drift fails loudly and names the row.

## The default table

Chords are written in the canonical text form used in `settings.json`: `Ctrl+Alt+E`, `Shift+F5`,
`Ctrl+[`. `Ctrl` is the **command** modifier — Ctrl on Linux and Windows, ⌘ on macOS.

### Tools (single bare letters)

| Key | Tool |
| --- | --- |
| `V` | Move |
| `M` | Rectangular Marquee |
| `L` | Lasso |
| `W` | Magic Wand |
| `A` | Select Brush |
| `C` | Crop |
| `B` | Brush |
| `E` | Eraser |
| `J` | Inpaint Brush |
| `S` | Clone Stamp |
| `Y` | Red Eye |
| `K` | Bucket Fill |
| `G` | Gradient |
| `I` | Eyedropper |
| `U` | Rectangle (shape) |
| `P` | Pen |
| `T` | Type |
| `Z` | Zoom |

A toolbar **slot's variants share one letter** and the first one registered claims it — pressing `M`
selects the rectangular marquee, never the elliptical one; the flyout is how you reach a variant.
That is exactly `ui::toolForShortcut()`'s rule, and the keymap is *seeded from* `kToolDefs` rather
than restating it, so a tool added to that table arrives with its letter already remappable and
needs no edit to `keymap.cpp`. **The table above is a snapshot; `kToolDefs` is the source.**

### Colors

| Chord | Action |
| --- | --- |
| `X` | Swap foreground / background |
| `D` | Default colors (black / white) |

### File

| Chord | Action |
| --- | --- |
| `Ctrl+N` | New… |
| `Ctrl+O` | Open… |
| `Ctrl+S` | Save |
| `Ctrl+Shift+S` | Save As… |
| `Ctrl+Shift+E` | Export As… |
| `Ctrl+Alt+E` | Quick Export as PNG |
| `Ctrl+W` | Close |
| `Ctrl+Q` | Quit |

### Edit

| Chord | Action |
| --- | --- |
| `Ctrl+Z` | Undo |
| `Ctrl+Shift+Z` | Redo |
| `Ctrl+X` | Cut |
| `Ctrl+C` | Copy |
| `Ctrl+Shift+C` | Copy Merged |
| `Ctrl+V` | Paste |
| `Ctrl+Shift+V` | Paste in Place |
| `Shift+F5` | Fill… |
| `Ctrl+,` | Settings… |

### Image

| Chord | Action |
| --- | --- |
| `Ctrl+Alt+I` | Image Size… |
| `Ctrl+Alt+C` | Canvas Size… |

### Layer

| Chord | Action |
| --- | --- |
| `Ctrl+Shift+N` | New Layer |
| `Ctrl+J` | Duplicate Layer |
| `Ctrl+G` | Group Layers |
| `Ctrl+E` | Merge Down |
| `Ctrl+]` | Bring Forward |
| `Ctrl+[` | Send Backward |

### Select

| Chord | Action |
| --- | --- |
| `Ctrl+A` | Select All |
| `Ctrl+D` | Deselect |
| `Ctrl+Shift+D` | Reselect |
| `Ctrl+Shift+I` | Inverse |
| `Ctrl+Alt+A` | Select All Layers |

### Filter

| Chord | Action |
| --- | --- |
| `Ctrl+F` | Last Filter |

### View

| Chord | Action |
| --- | --- |
| `Ctrl+=` | Zoom In |
| `Ctrl+-` | Zoom Out |
| `Ctrl+0` | Fit on Screen |
| `Ctrl+R` | Rulers |
| `Ctrl+;` | Show Guides |

## Conflict rules

Nothing is ever taken silently. When a captured chord cannot be applied, the dialog says which rule
it hit and why — the refusals are all consequences of how FLTK dispatches keys, not house taste.

1. **Already bound.** The dialog names the other command and offers **Reassign**. Reassigning leaves
   that command with *no* shortcut (its own row's Reset brings its default back). Declining changes
   nothing.
2. **A menu shortcut must carry Ctrl or Alt.** FLTK matches `Fl_Menu_Item` accelerators *before* the
   unclaimed-key phase where the tool letters and colour keys live. A bare — or Shift-only — letter
   on a menu command would therefore swallow that letter everywhere else: the tool wearing it would
   simply stop responding, with nothing on screen to explain why. Refused even when no tool holds the
   letter today, because the next tool to claim it would die just as quietly. `Shift+F5` is
   unaffected: an F-key is not a typed character, so nothing competes for it.
3. **A tool or colour key must be one bare key.** Those are read in the unclaimed-key phase, which
   runs *only* for chords with no Ctrl/Alt/Cmd — that is what lets a focused text field keep first
   claim on your typing. The phase also reads the typed character, so it cannot tell `Shift+B` from
   `b` (which is why `Shift+B` has always picked the Brush). A modified chord there would be a
   setting that lies, so it is refused rather than stored and ignored.
4. **Reserved keys.** `Escape`, `Tab`, a plain `Return` and a plain `Space` are never capturable.
   They cancel, move focus and activate the focused control — including inside this dialog, where
   `Escape` is how you leave a capture. `Ctrl+Return` and `Ctrl+Space` are fine.

Overrides are stored **sparsely** in `settings.json` under `keymap`, as action id → chord text
(`"file.open": "Ctrl+O"`); `""` means the action was deliberately unbound. Only what you actually
changed is written, so improving a default later still reaches everyone who never touched that row.
An entry naming an action this build does not have, or a chord it cannot parse, is dropped on load —
a hand-edited file can leave the keymap odd, never broken. (A hand-edited file *can* put two actions
on one chord; the dialog cannot, and the dispatcher then simply picks the first match. The dialog is
the supported way in.)

## Scope: what this slice does NOT remap

**Canvas-local keys are not remappable, and that is a stated limit rather than an oversight.** They
live in `src/ui/vulkan_canvas.cpp` and several of them are *gesture-coupled*, which makes them a
different problem:

- `Space` — press-and-hold to pan.
- `R` — press-and-hold to rotate the view; double-tap resets the rotation.
- Arrow keys — nudge the selection outline by 1 document pixel, 10 with Shift.
- `Escape` / `Enter` / `Backspace` / `Delete` — the staged-gesture verbs: abandon an in-flight
  marquee, lasso, crop, shape or transform; commit a crop or close a polygonal lasso; take back a Pen
  node.
- The Type editor's whole editing keyboard — typing, caret navigation, selection.

`docs/wayland.md` records the trap that makes these their own pass: FLTK synthesises key **repeat**
from a timer, and `wl_keyboard_leave` sends **no KEYUP**, so a pointer gesture must never be made
hostage to the key stream. `Space`-to-pan and `R`-to-rotate are exactly that shape — a held key
gating a drag — and each already carries a hand-written "do I believe this KEYUP?" guard
(`keyPhysicallyHeld`). Routing them through a remappable table means re-deriving those guards for an
arbitrary key, which needs its own session with its own tests, not a footnote in this one.

Two smaller limits, for completeness:

- **macOS**: Quit (⌘Q) and Settings (⌘,) live in the *application* menu, which the OS builds and
  owns. They appear in the list because the chords are real everywhere else, but remapping them has
  no effect on macOS.
- The **text-editor fence** (the accelerators refused while a caret is live, `menu_bar.hpp`) follows
  the keymap: it names *actions*, and their current chords are resolved on every remap. So moving
  `Ctrl+J` elsewhere fences the new chord, not the old one.
