# Document templates

Template presets for the New Document dialog's **Templates** gallery. Each template is an
ordinary `.mosaic` document, shipped on disk (never embedded in the binary -- templates can be
any size) and instantiated as a fresh untitled document when chosen.

Naming: `<order>-<Name>.mosaic`, e.g. `1-Birthday_Card.mosaic`, `2-Resume.mosaic`. The leading
number orders the gallery; the rest is the display name (underscores read as spaces). Files
without a number sort after the numbered ones, alphabetically.

This directory resolves through `common::installedDataDir()` (env override, install prefix,
then this source tree for dev builds); users can add their own under
`~/.local/share/mosaic/presets/`. The installed location ships with S59's packaging.

No templates exist yet -- they are to be designed by hand (see PLAN S55).
