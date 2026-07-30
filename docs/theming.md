# Theming & the system-adaptive look

How Mosaic themes its UI, why it works the way it does on FLTK 1.4, and how to consume the
palette from custom widgets. Implemented in S4; the palette grows as widgets land.

## The FLTK 1.4 reality (important)

PLAN §3.3/§3.5 originally assumed FLTK 1.4 shipped a subclassable **`Fl_Scheme`** OOP theming
system (with host-mimicking schemes like Aqua/Fluent/Sweet). **It does not** — in FLTK 1.4.5
`Fl_Scheme` is only a *name registry* (its header says the constructor is "not yet
implemented"), and the built-in schemes are just `base`, `gtk+`, `gleam`, `plastic`, `none`.

So Mosaic does **not** subclass a scheme. Instead the theming engine (`src/ui/theme.*`):

1. Picks a flat **base scheme** (`gtk+`) for widget geometry.
2. Drives a **token palette** onto FLTK's global **color map** (`Fl::background`,
   `Fl::background2`, `Fl::foreground`, `Fl::set_color(FL_SELECTION_COLOR, …)`, …).
3. Registers a few **custom flat boxtypes** (`FL_FREE_BOXTYPE` slots) for a cohesive,
   modern, flat look.
4. Styles **tooltips** (`Fl_Tooltip::color/textcolor/delay/enable`).

If a future FLTK gains a real OOP scheme system, this is the layer that would adopt it; the
token palette stays regardless.

## Token palette

`ui::Palette` (in `ui/theme.hpp`) names colors by **role**, never by raw FLTK slot, so widgets
never hardcode RGB. Tokens: `windowBg`, `panelBg`, `canvasBg`, `controlBg`, `controlHover`,
`controlActive`, `controlSelected`, `text`, `textMuted`, `accent`, `onAccent`, `border`,
`tooltipBg`, `tooltipText`, plus a `dark` flag.

`controlSelected` is the *slight* ground for a row that is in a multi-selection but is not the
active one (the layer dock's grey-dot rows). It sits at **half a hover step** from `panelBg` in
each palette rather than at a fixed delta, because the light ramp is far more compressed than the
dark one — an absolute step is invisible on light or a slab on dark. Precedence is
`active > hover > selected`, so hover feedback survives on a row already in the set.

Two built-in palettes ship today — `darkPalette()` and `lightPalette()` — tuned to the app
icon's palette (canvas ground `#171B2B`, default accent the icon blue `#5E7EFF`).

## Modes & system adaptation

`ui::ThemeMode { System, Dark, Light }`:

- **Dark / Light** — fixed built-in palettes (deterministic; no OS calls).
- **System** — follows the host: `platform::detectColorScheme()` and
  `platform::detectAccentColor()` (see `platform/system_theme.*`). Detection is **best-effort
  and never blocks**:
  - **Color scheme:** the cross-desktop **XDG settings portal**
    (`org.freedesktop.appearance / color-scheme`: 0 none, 1 dark, 2 light), falling back to
    GNOME `gsettings`. Mosaic defaults to **dark** unless the host explicitly prefers light
    (a pro-creative-app convention).
  - **Accent:** the portal's `accent-color` (a tuple of doubles), folded into `accent` when
    present. On KDE this picks up e.g. Breeze blue `#3DAEE9`.
  - Implemented by shelling out to `gdbus`/`gsettings` (each wrapped in `timeout`); no D-Bus
    library dependency yet. A richer/native detector can replace it later without touching
    callers.

`resolvePalette(mode)` returns the effective palette; `applyTheme(pal)` pushes it into FLTK
and is idempotent (safe to call again to re-theme). The selectable theme **mode UI** lands
with settings in S5/S51.

## Custom boxtypes

Registered by the first `applyTheme()` and exposed as `Fl_Boxtype` globals:

| Boxtype | Look | Use |
|---|---|---|
| `MOSAIC_FLAT_BOX` | solid fill, no border | bars, backgrounds (e.g. the menu bar) |
| `MOSAIC_PANEL_BOX` | solid fill + 1px border | **floating** panels (popovers, dialog fields) |
| `MOSAIC_BUTTON_UP_BOX` | rounded fill + border | buttons at rest |
| `MOSAIC_BUTTON_DOWN_BOX` | rounded fill + accent border | pressed / active |

The draw callbacks read the active palette for borders/accents; the widget's own `color()`
supplies the fill.

## Border-edge ownership (docked chrome)

When two adjacent docked elements each draw their shared edge, the 1 px hairline doubles to
2 px (user feedback, 2026-06: "visual agony"). So docked chrome never uses the full
`MOSAIC_PANEL_BOX` border; `ui::Panel` draws a flat fill plus only the **edges it owns** (an
`Edge` bitmask via `borderEdges()`), and **exactly one element owns each junction line**:

- tool options bar → `EdgeTop | EdgeBottom` (the menu|bar and bar|body junctions)
- left toolbar → `EdgeRight` (toolbar|canvas)
- Layers dock → `EdgeLeft` (canvas|dock)
- status bar → its own top hairline (body|status)
- window-edge sides draw nothing — the WM frame is the border there

Free-standing/floating panels (the `Popover`) keep all four edges. New docked surfaces (the
Properties tab, future docks) must pick their owned edges by the same rule, never `EdgesAll`.

## Consuming the palette from widgets

Custom widgets read `activePalette()` at construction. Two base classes exist now
(`ui/widgets.*`), the groundwork for the docks (S10) and tool buttons (S11):

- **`ui::Panel`** — themed container (`panelBg` fill + owned hairline edges, see above).
- **`ui::FlatButton`** — flat rounded button with a hover highlight and no focus rectangle.

Pattern for new widgets: in the constructor, pull `const Palette& p = activePalette();` and set
`box(MOSAIC_*)`, `color(fl_rgb_color(p.<token>.r, …))`, `labelcolor(…)`. For hover/active
states, override `handle()` and swap colors on `FL_ENTER`/`FL_LEAVE` (see `FlatButton`).

The **Vulkan canvas** participates too: its clear color is `activePalette().canvasBg`, so the
GPU-drawn area matches the FLTK chrome.

## DPI / HiDPI

FLTK 1.4 applies its own UI scaling (`Fl::screen_scale`, env `FLTK_SCALING_FACTOR`), so widget
coordinates stay in logical units and the theme needs no per-DPI work. The Vulkan swapchain
sizes itself from the surface's pixel extent independently (see `docs/vulkan.md`).

## Adding a theme

Add a `Palette` factory (like `darkPalette()`), and—once the settings UI exists—surface it in
the theme picker. Because everything reads tokens, no widget code changes.
