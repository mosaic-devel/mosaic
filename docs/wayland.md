# Native Wayland

The reference for Mosaic's native-Wayland backend, which became **the default in S59-a**. How the
swapchain gets onto a `wl_subsurface` is in `docs/vulkan.md`; this file covers everything the flip
changed *around* the canvas — what moved, what broke, and what is deliberately still parked.

## 1. The switch itself

`platform::preferWaylandBackendIfUnset()` (`src/platform/native_window.cpp`) pins
`FLTK_BACKEND=wayland` when the user has not chosen a backend **and** `WAYLAND_DISPLAY` is set.
Three rules it holds to:

- **A user-set `FLTK_BACKEND` always wins.** `FLTK_BACKEND=x11` is the supported escape hatch and
  the one thing to try first when a Wayland-specific problem is suspected.
- **Pure Xorg is untouched.** No `WAYLAND_DISPLAY` ⇒ the variable is not written at all, and FLTK
  picks X11 on its own, exactly as before.
- **No settings toggle.** The backend is a session-level property, not a preference; there is
  nothing sensible to show a user in a Settings pane, and the project has a standing
  no-toggle-for-strictly-better rule. The env var is the whole interface.

The value is *written* rather than left to FLTK's own "Wayland if `WAYLAND_DISPLAY`" default so
that the choice is ours and inspectable: code that has to know the backend **before any window
exists** has to read the environment, because `platform::activeBackend()` probes `fl_wl_display()`
and needs a live display.

### What the flip changes

| Area | Before (XWayland) | After (native) |
|---|---|---|
| **KDE file dialogs** | `kdialog --attach <xid>` — a real XWayland→XWayland modal transient (`file_dialog.cpp`) | that branch is **dead** (`activeBackend() == X11` is false); every KDE user moves to the xdg-desktop-portal path, parented via `zxdg_exporter_v2` as `wayland:<handle>` |
| **Tablet** | X11/XI2 valuators on top of the server's own pointer motion | `zwp_tablet_v2`; binding the manager makes KWin **stop** emulating pointer events, so `TabletPointerSynth` produces every `Fl::handle()` call (`docs/tablet.md` §4) |
| **Window / taskbar icon** | `_NET_WM_ICON` from `Fl_Window::icon()` | inert — Wayland has no per-window pixel-icon protocol; the icon comes from **app_id → `.desktop` → `Icon=`** (§3) |
| **Popovers / dropdowns** | sub-windows map whenever | the "a sub-window's first `show()` must come *after* the parent maps" rule is now the default path for every popover, flyout and dropdown |
| **Clipboard** | offers arrive on pointer enter | offers only arrive on **keyboard-focus** enter |
| **Resize** | FLTK chrome flashes **black** for a frame or two (§12, X11-only) | gone — an argument *for* the flip |
| **HDR output** | impossible (XWayland/Xorg are SDR-only) | possible; S43 is unblocked |
| **Cursors** | stock cursors always resolve; HiDPI is never exercised | §2 |

## 2. Cursors

Two real, Wayland-only defects sat behind the "hovering around chrome seems offset in some cases"
report. Both are *wrong cursor images*, not wrong event coordinates — FLTK's Wayland pointer path
accumulates sub-window offsets and divides by `Fl::screen_scale` exactly like X11, FLTK 1.4.5 has
no `wp_fractional_scale_v1` support at all (so `Fl::screen_scale` is 1), and Mosaic does no
screen↔widget conversion of its own. **Do not go hunting for coordinate math.**

### 2.1 Device pixels vs logical units (the double-scale)

`Fl_Wayland_Window_Driver::set_cursor_4args` treats the `Fl_RGB_Image`'s `w()`/`h()` **and** the
hotspot as **logical** units and multiplies both by the window's buffer scale. Mosaic's cursor
builders rasterize at the *device* scale for crispness, so handing FLTK the device numbers renders
the cursor at `scale ×` its intended size and puts the hotspot `scale ×` too far from the point the
art aims at. For the two **off-centre** hotspots — the pan hands and the fit-to-path hand — that is
a ~10–15 px gap between where the hand points and where the click lands on a 2× output. X11 never
showed it because `NativeSurfaceHandle::scale` is hard-coded to 1 there (`docs/vulkan.md`).

The contract now lives on `ui::CursorImage` (`src/ui/cursors.hpp`): every builder reports the
**logical box** and **logical hotspot** alongside the device bitmap. Callers pass the full-resolution
data to `Fl_RGB_Image`, set its *drawing* size with `Fl_RGB_Image::scale(logicalW, logicalH, 0, 1)`
— FLTK's own HiDPI image idiom — and hand `logicalHot{X,Y}` to `Fl_Window::cursor()`. At scale 1
(X11, and macOS, where `cursorBuildScale()` is pinned to 1) every one of those steps is an identity.

Secondary, and the reason the report said *sometimes*: the cursor caches are keyed on nothing, so
moving the window between a 1× and a 2× output left a cursor built for the wrong scale. The caches
are now dropped in `VulkanCanvas::resize()` on a content-scale change.

### 2.2 Stock cursors a theme may not ship

`Fl_Wayland_Window_Driver::set_cursor(Fl_Cursor)` resolves stock cursors by their **legacy X11
Xcursor names** and returns 0 when the theme lacks the file; `Fl_Window::cursor()` then calls
`fallback_cursor()`, which for `FL_CURSOR_NWSE` / `FL_CURSOR_NESW` / `FL_CURSOR_NONE` builds a
**15×15 XPM with a dead-centre (7,7) hotspot** — inside a theme whose real arrows are 24 px and
point from near their top-left corner. `FL_CURSOR_NWSE`/`NESW` ask for `fd_double_arrow` /
`bd_double_arrow`, and **`breeze_cursors` — the KDE default — ships neither** (Adwaita does). Mosaic
requests exactly those for the Move / Crop / Shape / Type **corner handles**. X11 cannot fail this
way: `XCreateFontCursor` always answers.

`ui::nwseCursor()` / `ui::neswCursor()` are the substitute — the same apple `left_side` double-arrow
the rotate cursor uses, baked to ±45°, so a corner handle and the rotate band beside it read as one
family. The canvas swaps them in **only on the Wayland backend**; making it unconditional would
change the resize cursors for every X11 user, whose theme resolves them correctly.

Related, not currently a divergence: FLTK maps `FL_CURSOR_HAND` to `hand1` on Wayland but
`XC_hand2` on X11. In `breeze_cursors` both symlink to `pointer`, so the two agree — but a theme
where they differ gives the two backends different hotspots for Mosaic's most-used chrome cursor.

### 2.3 The seat's cursor is app-global

On Wayland `seat->default_cursor` is per-application, not per-window: whatever any widget last set
is what the pointer shows, and a drag-and-drop resets to it. Combined with `updateToolCursor`'s
`if (want == m_cursorState) return;` early-out, that is the mechanism behind the standing
"rotate cursor won't revert on canvas re-enter" report — which was marked **WON'T FIX explicitly
because X11 was the default backend.** That justification expired with the flip. The canvas now
forgets its cursor state on `FL_ENTER` and re-sends unconditionally; on X11 that costs one
redundant, idempotent `cursor()` call per canvas entry.

### 2.4 Every path that names the tool cursor (the enumeration)

Two *different* things can vanish, and a report of "the cursor disappeared" has to say which before
anything else is worth doing:

- **the OS pointer**, set through `Fl_Window::cursor()` (mouse) and `zwp_tablet_tool_v2.set_cursor`
  (pen). These are separate protocols with separate lifetimes — binding the tablet manager makes
  KWin stop emulating pointer events for the pen, so `wl_pointer` requests never reach it;
- **the brush reticle**, which is not a cursor at all but geometry the present pass draws from the
  overlay SSBO. It disappears when `syncBrushReticle` stops being fed, never because of anything
  in this section.

`VulkanCanvas::updateToolCursor(inside)` is the single decision point for the OS pointer, and every
exit from it sets **both** devices. The exits, in order:

| Exit | Mouse | Pen |
|---|---|---|
| `want == 15` (rotate) | `applyRotateCursor()` | `setToolCursor(FL_CURSOR_DEFAULT)` |
| `want == 21` (I-beam) | `applyTextCursor()` | `setToolCursor(FL_CURSOR_INSERT)` |
| `want == m_cursorState` | — (dedup) | — (dedup; the pen re-asserts itself, see below) |
| everything else | one `cursor()` call | `setToolCursor(tabletCursorFor(want))` |

The two early returns above the dedup exist because those cursors' *art* changes while their state
number does not; both were once bugs precisely because they returned before the pen was told.

**Everything that calls `cursor()` from outside `updateToolCursor` must invalidate `m_cursorState`
to `-2` and tell `m_tablet`.** The dedup is a comparison against a cached number, so a raw
`cursor()` elsewhere leaves the cache describing a shape the window no longer wears, and the next
`updateToolCursor` returns having set nothing — the pointer then keeps the wrong shape (an arrow
over a brush tool, which is supposed to hide it) until an unrelated state change happens by. The
documentless idle state's `FL_ENTER` / `FL_LEAVE` hand cursors and `setIdleEnabled(false)` are the
three that did this; `FL_ENTER` on the live canvas already used the `-2` sentinel and is the model.

**The pen's cursor is per tool and the compositor forgets it on every `proximity_out`.** So
`TabletWayland` tracks what each `zwp_tablet_tool_v2` was last actually *sent*
(`ToolEntry::applied`), not just what the app last asked for. An app-wide value dedup was wrong in
both directions: a change made while the pen was off the tablet sent nothing yet was recorded as
current, and a later request for that same value was then dropped as a no-op — so the pen came back
wearing the compositor's default (KWin: a crosshair) with nothing left to re-assert it. That is a
third path to "the pen cursor is randomly wrong", distinct from the two fixed in `4b2953e` /
`2372e04` (the swallowed `proximity_out` leave) and from §2.1 (the scale-keyed caches).

### 2.5 The chrome audit — and why "patch FLTK" is the wrong lever *(2026-07-29)*

The "chrome hotspot" report came back a third time, with the question *"do we need to patch FLTK?"*.
**On Linux we cannot**, and the constraint is structural, not a preference: `src/ui/CMakeLists.txt`
does `find_package(FLTK REQUIRED CONFIG)` and links `fltk::fltk` — **the distro's own
`/usr/lib/libfltk.a`**. `packaging/macos/patches/` (which carries
`fltk-1.4.5-open-panel-nil-path.patch`) is applied by the **osxcross** build only and reaches no
Linux binary. "Patch FLTK on Linux" therefore means *vendoring FLTK and building it from source in
the Linux build* — a real build-system change, with a real cost (a from-source FLTK per configure,
a patch set to carry forward at every distro bump, and Mosaic's Linux binary no longer sharing the
distro's security updates). That is a decision for the user, not a fix to slip into a bug session.

**And it is not needed for anything found so far.** The audit, against FLTK 1.4.5's own source and
this machine's actual theme (`~/.config/kcminputrc` and `gsettings … cursor-theme` both say
`breeze_cursors`, which is what `libdecor_get_cursor_settings` feeds `wl_cursor_theme_load`):

| Mechanism | Reachable from CHROME? | Evidence |
|---|---|---|
| §2.1 device/logical double-scale | **No** | it lives in `set_cursor_4args`, i.e. only for *custom image* cursors — and every custom-image cursor in the app is set by `VulkanCanvas`. Grep every `cursor(...)` outside it: chrome sets **only stock `Fl_Cursor`s**, which never touch that arithmetic |
| §2.2 fallback 15×15 XPM | **No** | only `FL_CURSOR_NWSE`/`NESW`/`NONE`/`WAIT`/`HELP` have XPM fallbacks, and only the first two miss in breeze. **Nothing in chrome requests them** — the canvas does, and already substitutes `ui::nwseCursor`/`neswCursor` |
| stock-name hotspot arithmetic | **No** | `do_set_cursor` divides the theme image's own hotspot by `seat->pointer_scale` and sets the cursor surface to that same buffer scale; `try_update_cursor` reloads the theme whenever the scale changes. Self-consistent |
| **theme art vs. the name FLTK asks for** | **YES** | see below |
| §2.3 app-global seat cursor | yes, but produces a **stale** cursor, never an offset one | `pointer_enter` re-asserts `Fl::first_window()`'s `custom_cursor`, and `fl_cursor()` targets `Fl::first_window()` rather than the widget's own toplevel |

So the remaining chrome defect of this family is **not hotspot arithmetic at all** — it is FLTK
asking for an Xcursor *name* whose art in the installed theme points somewhere other than its
hotspot. Two, concretely:

- **`FL_CURSOR_MOVE`.** Wayland asks for `move`; breeze symlinks it to **`dnd-move`, a closed
  grabbing hand with a dead-centre hotspot** (12,12 of 24). X11 asks for `XC_fleur` → breeze
  `fleur`, a four-way arrow correctly centred on its crossing (11,11 of 24). The hand's *apparent*
  point is its fingertips, ~10 px up-left of where the click actually lands — which is exactly what
  "the hotspot is offset" feels like. `ui::moveCursor()` was built for this on 2026-07-28 and wired
  into the canvas. **Every site is on the substitution as of 2026-07-29** — see §2.6, which is also
  where this bullet's original claim (that the Export preview was the *only* remaining one) turned
  out to be wrong.
- **`FL_CURSOR_HAND`, latent.** FLTK maps it to `hand1` on Wayland and `XC_hand2` on X11. In breeze
  both symlink to `pointer`, so the two agree *here* — but in **Adwaita `hand1` symlinks to
  `grab`** (an open hand) while `hand2` is the pointing finger. On a theme that follows that
  convention, every clickable label, swatch and chip in the chrome shows a *grab* hand on Wayland
  and a *pointer* on X11, with different hotspots. Not a bug on this box; a trap for the next
  report, and one more argument for naming our own art.

**What is NOT established.** This pass could not reproduce the symptom: the headless-Wayland harness
has a seat with `capabilities: 0` (no pointer at all), so it cannot drive a cursor, and nothing
here was verified against a live session. Before the next iteration, the report has to name **which
cursor, over which widget, offset how far, in which direction, at what output scale, and whether
`FLTK_BACKEND=x11` shows it too** — an X11/Wayland divergence points at the theme-name table above,
a both-backends offset points at Mosaic's own art in `src/ui/cursors.cpp`. And bear §5's warning in
mind: FLTK 1.4.5 has a **write-after-return on `Fl::e_text`**, so "it did something odd" reports on
this backend are not automatically deterministic.

The `ui::CursorImage` contract is now pinned end-to-end instead of piecewise
(`tests/test_cursors.cpp`): for every builder at scales 1/2/3, `image` is exactly
`logical{W,H} × scale`, the hotspot is inside the art in *both* coordinate systems, and **FLTK's own
`hot × buffer_scale`** lands within **one device pixel** of the device hotspot the builder
rasterized to. The bound is absolute, not proportional, which is the point: §2.1 was an error that
*grew with the scale*, and that is now the thing a test refuses.

### 2.6 The substitution has one implementation now *(2026-07-29)*

§2.5 named `ExportPreview::setCursorFor` as "the one chrome site still on the stock request". That
was wrong, and the way it was wrong is the more useful finding: **two more chrome widgets were
asking for `FL_CURSOR_MOVE` too**, and both are pointer-precision affordances where a hand pointing
10 px off its own hotspot is exactly the felt symptom —

- `Extrude3dViewport` (`src/ui/type3d_panel.cpp`), the Type 3D popup's **free-orbit trackball**;
- `TexturePreviewPane` (`src/ui/texture_generator_dialog.cpp`), the texture generator's **pan**.

Both sit *beside* handles that legitimately show `FL_CURSOR_HAND`, so on breeze the two states were
not merely mis-aimed, they were **the same hand** — the affordance could not tell "orbit freely"
from "grab this ring" at all. Neither was reachable by the audit's method: it read the table of
*mechanisms* and asked which could reach chrome, rather than enumerating chrome's actual calls.

The real defect underneath is that the substitution was **per-site code**. Written twice (canvas,
Export preview) and missing twice, with no type to forget to use. It is now one class,
`ui::MoveCursor` (`src/ui/cursor_apply.{hpp,cpp}`), owning the backend test, the rasterized cache,
the stock fallback and the null-window guard; `ui::makeCursorImage` moved there from the canvas's
anonymous namespace, so the §2.1 logical-box idiom also has exactly one copy. A widget that wants a
move cursor now holds a `MoveCursor` and calls `apply(window())` — **the substitution is what you
get by default, and there is no version of it to write incorrectly.**

`apply(Fl_Window*)` builds at `chromeCursorScale(win)` = the window's own Wayland buffer scale
(`platform::windowBufferScale`), so chrome gets the crisp, correctly-sized pointer the canvas
already had; the Export preview's hard-coded 1× — and the "slightly soft arrow" its comment
apologized for — is gone. `tests/test_cursor_apply.cpp` pins the handover §2.1 turns on: `w()/h()`
report the **logical** box while `data_w()/data_h()` stay at **device** resolution, for every
builder at 1/2/3×, plus the nullptr-on-failure contract every caller's stock fallback depends on.

**The PEN is deliberately not substituted.** `VulkanCanvas::tabletCursorFor` collapses state 10 to
`FL_CURSOR_MOVE` → `TabletCursor::Move` → `WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE`, and under
`wp_cursor_shape_v1` the **compositor** picks the art, so KWin on breeze may well draw the same
grabbing hand there. Substituting would mean drawing the pen's cursor into our own `wl_surface`
(buffer, damage, scale, per-tool lifetime) rather than naming a shape — a feature, not a fix, and
the same v1 limit that already collapses the op badges, the pan hands and the rotate arrow (§2.4).

**Still not verified live, and still not reproduced.** This closes the sites that provably make the
request §2.5 identified as wrong; it does not prove the user's report was any of them. The six facts
§2.5 asks for are still what a next iteration needs.

## 3. Desktop integration

Wayland compositors match a toplevel's **`app_id`** to an installed `.desktop` file to find its
`Icon=`, its name, and its task-manager grouping. FLTK maps its `xclass` onto `app_id` (and onto
`WM_CLASS` on X11, which is the same match there), so `Fl_Window::default_xclass("mosaic")` runs
next to the backend pin, before the first window exists. `data/desktop/mosaic.desktop` already
declares `Icon=mosaic` and `StartupWMClass=mosaic`, and `CMakeLists.txt` already installs it plus
the **scalable** hicolor icons — so the chain is complete as soon as the app is *installed*. Running
from the build tree, with no `.desktop` in the search path, still shows a generic icon: that is
expected and is a packaging (S59) matter, not a bug.

### 3.1 The icon an AppImage cannot get that way *(2026-08-26)*

The chain above has a floor: **it requires the app to be installed.** A shipped AppImage never is,
and v0.3.1 duly showed a generic placeholder in the Wayland taskbar and titlebar while looking
perfectly correct on X11. That asymmetry is the whole story:

- **X11 carries the icon in the client.** `Fl_Window::icon()` becomes `_NET_WM_ICON` — literal
  pixels on the window — so a binary run from anywhere shows its icon with nothing installed.
- **Wayland has no equivalent.** There is no `_NET_WM_ICON`, and FLTK's Wayland driver implements
  no `icons()` override at all, so the `Fl_Window::icon()` / `default_icon()` pair that
  `app_window.cpp` sets before `show()` is a **silent no-op** on this backend. The only route left
  is `app_id` → installed `.desktop` → `Icon=`, which is exactly the route an AppImage cannot take.

`xdg-toplevel-icon-v1` is the protocol that gives the pixels back, and `src/platform/
wayland_toplevel_icon.cpp` speaks it: one icon object, `set_name("mosaic")` for compositors that can
resolve the theme name plus real ARGB8888 buffers in a sealed `memfd` pool for those that cannot,
built once and reused for every toplevel (`ui::applyToplevelHints`, called right after each
`show()`). Sizes come from the compositor's own `icon_size` events when it sends any.

**Coverage is partial and that is upstream's doing, not ours.** KWin implements the protocol;
**Mutter does not yet** ([GNOME/mutter#4100][m4100]), so GNOME sessions fall through to the
`.desktop` chain and an uninstalled AppImage still shows a placeholder there. Nothing in this repo
can change that; when Mutter lands it, Mosaic gains GNOME for free.

Still open, all packaging-side: PNG raster icon sizes (some environments do not rasterize SVG),
an AppStream `metainfo.xml`, and a reverse-DNS application id. For **packagers** — and for anyone
who wants an AppImage to behave like an installed app — `packaging/linux/desktop-entry/` holds a
ready-made `.desktop` + icon install, which is the only thing that also gives a launcher entry,
MIME associations and task-manager grouping.

[m4100]: https://gitlab.gnome.org/GNOME/mutter/-/issues/4100

## 4. Deliberately not done

- ~~**`xdg_dialog_v1`**~~ — **DONE 2026-08-26**, together with `xdg-toplevel-icon-v1` (§3.1) and
  for the same reason: both needed the window's `xdg_toplevel`, and both got it the same way. See
  §4.1 below for how, and `src/platform/wayland_dialog.cpp` for the result. Still cosmetic — the
  dialogs were always genuinely modal, they just did not dim their parent.
- **Deriving `NativeSurfaceHandle::scale` on X11.** See `docs/vulkan.md`; it would turn on every
  HiDPI path for X11 users in one untested step.

### 4.1 How the toplevel was reached, and why not the other ways *(2026-08-26)*

FLTK 1.4.5's public `FL/wayland.H` exposes display / xid / surface / compositor / buffer_scale / gc
/ glcontext and stops — no `xdg_toplevel`. That one gap blocked *both* protocols above, and
1.4.5 is still the newest release (Apr 2026) with master unchanged, so waiting was not a plan.
Three routes existed; the third is the one taken.

- **Hand-declare FLTK's private `struct wld_window`** and read the frame out of it. Rejected. The
  layout is version- *and* build-config-dependent, and `find_package(FLTK CONFIG)` means we link
  whatever the distro built. Reading the wrong union member is a wild pointer, not a failed feature.
- **Interpose `libdecor_decorate`** — define the symbol ourselves, recover the real one via
  `dlsym(RTLD_NEXT)`, keep a `wl_surface → libdecor_frame` map, then use the *public*
  `libdecor_frame_get_xdg_toplevel()`. Genuinely layout-free, and still rejected: **FLTK vendors
  libdecor** (`CMake/options.cmake` falls back to the bundled copy whenever system libdecor is
  < 0.2.0 or its plugin dir is missing), and in that configuration the symbol is inside
  `libfltk.a` — our definition either collides at link time or `RTLD_NEXT` finds nothing. Which of
  the two happens depends on how the distro built FLTK, which is not a thing to ship.
- **Patch FLTK to expose it** — `packaging/linux/patches/fltk-1.4.5-wayland-toplevel-accessor.patch`.
  FLTK **already computes this**: `Fl_Wayland_Window_Driver::xdg_toplevel()` handles the decorated
  (libdecor-owned) and undecorated cases and answers NULL for sub-windows, menus and tooltips. It is
  simply a member of a class in a private header. The patch adds the free function that exposes it
  and nothing else — no new logic, no behaviour change — which is why it is written to be
  upstreamable and dropped again.

**This does not overturn §2.5.** That section's constraint — "on Linux we cannot patch FLTK,
because `find_package(FLTK CONFIG)` links the distro's own `libfltk.a`, and changing that means
vendoring FLTK into the Linux build" — still holds, and nothing here changes it. The patch is
applied **only** where a from-source FLTK already existed: the release AppImage job, which has
built FLTK 1.4.5 itself since day one because Ubuntu 24.04 ships 1.3.8. Local builds, distro
builds and CI still link the distro's unpatched `libfltk.a`, exactly as before. §2.5 also said the
call belonged to the user rather than to a bug session; it was theirs, on 2026-08-26.

**A patched FLTK is not required to build Mosaic.** The accessor is referenced through a **weak
symbol** (`src/platform/wayland_toplevel.hpp`): an unpatched FLTK resolves it to null, the two
backends report themselves unavailable at runtime, and nothing fails to compile or link. That is
deliberate and load-bearing for CI, which builds against Arch's stock `fltk` package — every line
of both backends is still compiled and `-Werror`'d there; only the final request is inert. The
release AppImage, which builds FLTK from source, gets the patch and the feature.

The one ELF subtlety worth knowing: a weak *undefined* reference does not by itself pull a member
out of a static archive. It resolves only because the object defining it —
`Fl_Wayland_Window_Driver.cxx.o` — is already linked in for the window driver itself. If that ever
stopped being true the accessor would read null on a patched build, i.e. fail toward "feature off"
rather than toward breakage.

## 5. The keyboard is not a state machine you can infer

**FLTK's Wayland backend SYNTHESISES key auto-repeat from a timer, and that is structurally
different from X11.** `Fl_Wayland_Screen_Driver.cxx`: on `FL_KEYDOWN` it arms `KEY_REPEAT_DELAY`
(0.5 s), then `key_repeat_timer_cb` calls `Fl::handle(FL_KEYDOWN, window)` every
`KEY_REPEAT_INTERVAL` (0.05 s) while `last_keydown_serial` still matches. Three consequences, all of
which have bitten:

- the repeat **re-enters our key handler from a timeout, mid-drag**, against the window captured at
  press time (`Fl::focus()->top_window()` — the main window, since the canvas is a sub-window);
- `Fl::e_keysym` / `e_text` / `e_state` are **whatever the last real event left**. Press a mouse
  button during a held key and `pointer_button` overwrites `e_keysym` with `FL_Button + b`, so the
  repeats that follow arrive claiming to be that button. Our handler declines them, and FLTK then
  escalates the same event to `FL_SHORTCUT` — 20 times a second, through the menu bar's accelerator
  matcher;
- `Fl::e_text` points at a **stack buffer inside `wl_keyboard_key`, which has already returned**, and
  `Fl::handle_`'s case-swap retry *writes through it* (`*c = isupper(*c) ? tolower(*c) : ...`) on any
  alphabetic key that nothing consumed. That is a write-after-return in FLTK, reachable by holding
  a letter key. Upstream's, not ours, but it is why "R does something odd" reports should not be
  assumed deterministic.

And in the other direction: **`wl_keyboard_leave` clears the whole held-key set and delivers
`FL_UNFOCUS` — never a `KEYUP`.** A menu popup, a portal file dialog or the compositor moving focus
while a key is held means that key's release is *never reported at all*.

So a gesture that infers "the key is held" from pairing a KEYDOWN with a KEYUP is wrong on this
backend in both directions, and Mosaic had one of each:

- **Space-pan moved a little, then stopped, with the pan cursor still showing.** `onKeyUp(' ')`
  cleared `m_panning` as well as `m_spaceDown`, which made a *pointer* drag hostage to the key
  stream: one spurious or duplicated Space KEYUP killed the motion while `m_spaceDown` stayed (or
  was re-set by the next repeat), so the cursor kept saying "panning" over a dead drag. Fixed by
  scope: a pan begins on `FL_PUSH` and ends on `FL_RELEASE`, like every other drag in the file.
- **R-rotate rotated a little, then reset while R was still held.** The double-tap-reset guard was
  `if (!m_rotateDown) // ignore auto-repeat`, an X11 assumption (repeats arrive as bare KEYDOWNs
  with no intervening KEYUP). A KEYUP for a physically-held key slips straight through it; two such
  up/down pairs inside `kDoubleTapSeconds` — 7 apart at 20 Hz — read as a double tap.

**The rule now: the window system is the authority on what is held.** `Fl::event_key(int)` reads the
compositor's own key vector on Wayland (`search_int_vector(key_vector, k)`) and `XQueryKeymap` on
X11; `VulkanCanvas::m_spaceDown` / `m_rotateDown` are only ever a cache of it.

- a KEYUP for a key the system still reports down is **refused** (consumed, but not believed);
- the pointer events **resync** the cache, so a release that was never delivered cannot strand the
  canvas in pan or rotate mode;
- an in-flight drag belongs to the pointer and runs to `FL_RELEASE` regardless.

⚠ `Fl::event_key(int)` opens the display to answer, so it is routed through
`VulkanCanvas::keyPhysicallyHeld`, which short-circuits when no window is mapped (and carries the
test seam `setHeldKeyQuery` — the only way to produce "a KEYUP for a key that is still down" in a
headless test; `tests/test_canvas_cursor.cpp` pins both defects that way).

## 6. What only a human on a real session can check

Headless verification cannot reach any of this. On a native-Wayland session:

1. Hover the **pan hand** (hold Space) and the Type tool's **fit-to-path hand** over a HiDPI
   output — the fingertip/palm must sit exactly on the click point, and the cursor must be the same
   size as the system arrow, not double it.
2. Drag the window from a 1× output to a 2× one and back; the cursor must re-sharpen, not stay
   blurry or drift.
3. Hover a Move/Crop/Shape/Type box **corner** under `breeze_cursors` — expect Mosaic's diagonal
   double-arrow, correctly aimed, not a small centred one.
3b. Hover the **move** states under `breeze_cursors` — a Move-tool selection's body, the Export
   preview's image, the Type **3D popup's orbit area**, and the **texture generator's preview**
   (§2.6). All four must show the same four-way arrow, aimed at its own centre, and must be
   visibly *different* from the hand shown over the 3D rings / texture handles right next to them.
   A closed grabbing hand anywhere here means a site got missed again.
4. Rotate a selection with the Move tool, leave the canvas, come back — the cursor must revert.
5. Open **File ▸ Open** on KDE: the portal picker must stack modal over the main window, and the
   main window must not be clickable while it is up.
6. Draw with a **tablet**: hover, toolbar, menus and dialogs must all respond to the pen, not just
   the canvas stroke.
7. Open every popover, flyout and dropdown at least once — they are sub-windows, and sub-window
   mapping order is the classic native-Wayland trap.
8. `cmake --install` to a prefix, add it to `XDG_DATA_DIRS`, and confirm the **taskbar icon**.
9. Resize the window and confirm the chrome **black flash** is gone.
10. Hold **Space** and pan a long way, slowly, past the 0.5 s point where FLTK's synthetic repeat
    starts — the document must keep following the pointer for the whole drag, and releasing Space
    mid-drag must finish the pan rather than abandon it (§5).
11. Hold **R** and rotate slowly for several seconds — the view must never snap back to 0°. Then
    tap R twice quickly with no drag between: *that* must reset it.
12. With Space or R held, open a menu (or a file dialog) and close it again — the canvas must come
    back out of pan/rotate mode rather than staying stuck in it, since no `KEYUP` was ever sent.
13. **Tablet**, the cursor one: hover a brush tool (pointer hidden, reticle shown), lift the pen
    clear off the tablet and bring it back several times, and switch tools between lifts. The pen
    must return wearing the cursor the *current* tool asks for every single time — never KWin's
    crosshair, never the previous tool's shape (§2.4).
