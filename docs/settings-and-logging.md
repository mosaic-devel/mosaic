# Settings & logging

The user-settings store and the logging facade, both landed in S5 and both living in the
`common` module (PLAN §4: `common/` owns logging and cross-cutting utilities).

## Settings store (`common/settings.*`)

A small, JSON-backed, per-user config.

- **Type:** `mosaic::common::Settings` is a plain struct of serializable, **UI-free** fields
  (`theme`, `logLevel`, `language`, plus a `kSchemaVersion`). Keeping it free of UI types lets
  the lowest module own it: `ui/` maps `theme` ↔ `ui::ThemeMode` (`parseThemeMode` /
  `themeModeKey`), and `app/` maps `logLevel` ↔ `common::log::Level`.
- **Location:** `configDir()` returns `$XDG_CONFIG_HOME/mosaic`, else `$HOME/.config/mosaic`;
  `defaultSettingsPath()` appends `settings.json`. `--config PATH` overrides it (used by tests
  and for portable runs).
- **Load:** `loadSettings()` treats a **missing file as defaults, not an error** (it reports
  `existed=false`); a malformed or wrong-typed file returns clean defaults and sets `error`.
  Unknown keys are ignored. The JSON dependency (nlohmann/json, vendored) stays in the `.cpp`
  so including `settings.hpp` is cheap.
- **Save:** `saveSettings()` writes to a sibling `*.tmp` then `rename()`s it into place —
  **atomic within the directory**, so a crash mid-write can never corrupt an existing file.
  Parent dirs are created as needed.
- **First run:** `main()` writes the defaults on the first GUI launch (when no file existed),
  so the file is discoverable and hand-editable, and the S51 settings UI has something to
  update.

The on-disk shape:

```json
{
  "language": "",
  "logLevel": "info",
  "theme": "system",
  "version": 1
}
```

### Hidden / undialoged keys

A few fields are **serializable but have no Settings-dialog control** — set them by hand-editing
`settings.json`. They are documented here as their sole record.

- **`selectBrushAddByDefault`** (bool, default `true`) — the Select brush's (S18) default combine op.
  A no-modifier stroke uses this op (`true` = Add, the natural default for a brush that *builds* a
  selection; `false` = Subtract); holding **Alt** always subtracts, the press-time-modifier
  convention the marquee/wand share. `ui/` maps it onto `VulkanCanvas::setSelectBrushAddByDefault`.
  (The "no-toggle-for-strictly-better" house rule is deliberately overridden here — the user asked
  for the preference, 2026-07-15.)
- **`antsCirculate`** (bool, default `false`) — the marching-ants direction experiment (S18,
  `docs/research-select-brush.md` §5). Off is the **default diagonal crawl** (calm, uniform). On
  makes the ants dash along the local boundary **tangent**, so they *circulate* around the contour
  like Photoshop's — computed from a raw-coverage central difference in the present pass
  (`shaders/canvas_present.comp`, behind the `pc.ants.x == 2` bit). It stays hidden (no dialog
  control) because it *shimmers* on feathered / lasso / diagonal-AA edges — it looks best on
  axis-aligned marquees. `ui/` maps it onto `VulkanCanvas::setAntsCirculate`; the default-off path is
  byte-identical to before (`pc.ants.x` stays `1.0` when the mask is active and circulate is off).

## Logging (`common/log.*`)

A thin facade over **spdlog** (PLAN §3.15) so the whole codebase logs through one place.

- **Categories:** one named logger per module — `app`, `ui`, `render`, `io`, `core`,
  `platform`. Get one with `common::log::category("render")`; it is created on first use and
  shares the global sinks. Call sites typically cache it:
  ```cpp
  spdlog::logger& uiLog() { static const auto l = common::log::category("ui"); return *l; }
  uiLog().warn("drawFrame error: {}", err);
  ```
- **Sinks + format:** a colorized stderr sink (color auto-suppressed when not a TTY), plus an
  optional append-mode file sink. Pattern: `[HH:MM:SS.mmm] [level   ] [category] message`.
- **Configuration:** `common::log::init({level, file, color})` is called once from `main()`
  after settings load. It (re)points every existing category logger, so loggers created before
  `init()` (or on a later live reconfigure) pick up the new level/sinks. Logging works at a
  default before `init()`, so nothing is lost during early start-up.
- **Levels:** `parseLevel()` / `levelName()` convert to/from `trace|debug|info|warn|error|
  critical|off` (case-insensitive, with `warning`/`err`/`crit`/`none` aliases).

### CLI control

- `--log-level LEVEL` overrides the persisted `logLevel` (`Settings.logLevel`); both fall back
  to `info`.
- `--log-file PATH` additionally appends to a file.

Diagnostic logging is **not** for hot paths (pixel kernels, the render submit loop) — those use
`std::expected`/error codes and never log per element. CLI **result** output (the `--headless`
`[headless] …` lines, `--version`, `--help`) stays on stdout/stderr via iostreams; it is program
output, not logging.
