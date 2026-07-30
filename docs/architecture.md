# Architecture

This document tracks the concrete code architecture. It complements `PLAN.md` §4 (the
high-level overview) and is expanded as modules gain real implementations.

## Modules (`src/`)

Dependencies flow one way; **nothing depends on `ui`**.

| Module | Target / namespace | Responsibility | Real work starts |
|--------|--------------------|----------------|------------------|
| `common`   | `mosaic::common`   | shared types: version, (later) logging, math, color, image buffers, geometry, i18n | S1 (version) |
| `core`     | `mosaic::core`     | document model: layers/groups/masks/effects, undo/redo, color state | S6 |
| `render`   | `mosaic::render`   | Vulkan backend: device/swapchain, compositor, shaders, offscreen + CPU fallback | S2 |
| `io`       | `mosaic::io`       | image format readers/writers, export loss-warning system | S41 |
| `platform` | `mosaic::platform` | OS integration: native handles, DPI, drag & drop, theme detection, tablet | S3 |
| `ui`       | `mosaic::ui`       | FLTK shell, custom widgets, tools, panels, dialogs, menus, theming | S3 |
| `app`      | executable `mosaic`| `main()`, CLI/headless entry, argument parsing | S1 |

## Conventions

- **Include root is `src/`.** Headers are included by module-relative path, e.g.
  `#include "core/core.hpp"`. Logical grouping comes from the `mosaic::<module>`
  namespaces. The build-time version header is generated to `build/<preset>/generated/`
  and included as `#include "version_config.hpp"`.
- Each module is a static library `mosaic_<name>` with an alias `mosaic::<name>`, declared
  via `mosaic_add_module()` in `cmake/MosaicHelpers.cmake` (the `common` module is
  special-cased because it also generates the version header).
- Vendored, header-only third-party libraries live in `third_party/<lib>/` (e.g. doctest,
  included as `<doctest/doctest.h>`), each keeping its upstream `LICENSE`.
- Style is enforced by `.clang-format`; static analysis by `.clang-tidy`.

## Adding a new module

1. `mkdir src/<name>`, add `<name>.hpp` / `<name>.cpp` in `namespace mosaic::<name>`.
2. Add `src/<name>/CMakeLists.txt` with `mosaic_add_module(<name> SOURCES <name>.cpp DEPS …)`.
3. `add_subdirectory(<name>)` in `src/CMakeLists.txt` (respecting dependency order).
4. Link it where needed; add tests under `tests/`.

## Build & test

See `docs/STATUS.md` for the cheat-sheet. CMake presets live in `CMakePresets.json`;
helper logic in `cmake/`. Every commit must configure, build (`linux-debug`) and pass
`ctest`.
