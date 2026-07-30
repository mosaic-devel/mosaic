# The "motivate" catalog

This is a **satellite gettext domain**, separate from the main `mosaic` one, holding the 100
all-caps one-liners behind **Settings → Annoyances → "Cheesy motivational one-liners"** (the toggle
that drifts encouragement diagonally under the canvas every few minutes).

It lives in its own domain on purpose: 100 jokes have no business in `po/mosaic.pot`, where they'd
swamp the real UI strings a translator cares about. So nothing here touches the main catalog.

## How it fits together

- **Source:** `src/ui/motivational_lines.cpp`. Each line is wrapped in `MOTIVATE_("...")` — an
  extraction marker that is *identity at runtime* (like `N_`).
- **Extraction:** `MOTIVATE_` is **not** one of the main `pot` target's keywords, and
  `motivational_lines.*` is explicitly removed from its source list, so `po/mosaic.pot` never sees
  these strings. They are extracted separately:
  ```sh
  cmake --build build/linux-debug --target pot-motivate   # writes po/motivate.pot
  ```
- **Runtime:** `main()` binds the domain with `common::i18n::initDomain("motivate")`; a line is
  translated at the point of use via `common::i18n::dtr("motivate", line)` (see
  `randomMotivationalLine()`). With no catalog installed, the English `msgid` is shown — which is
  the whole point while we are English-only.

## Starting a translation (future-you)

Same recipe as the main catalog, just the `motivate` domain. It lives beside `mosaic.po` in the
language's own directory (S54 layout: files in `po/` are templates, directories are languages), and
the build compiles and installs it automatically once it exists:

```sh
cd po
msginit --input=motivate.pot --locale=de --output=de/motivate.po
$EDITOR de/motivate.po                                        # fill in the msgstr "" entries
```

The `.mo` basename **must** be `motivate` (the domain name). It installs next to the main catalog:
`<prefix>/share/locale/<lang>/LC_MESSAGES/motivate.mo`. Run-from-build-tree override is the same
`MOSAIC_LOCALEDIR` the main catalog uses — both domains resolve their directory the same way.

> Translating these faithfully is, as the original author put it, "a nightmare" — the humour is
> deeply English and all-caps. That is a problem for future-you. Today, English-only is fine.
