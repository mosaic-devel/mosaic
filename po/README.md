# Translations

Mosaic uses GNU **gettext**. User-facing strings are wrapped in `_()` (translate now) or `N_()`
(mark only) in the C++ sources; `po/mosaic.pot` is the extracted English template.

`docs/i18n.md` has the full picture — this is the quick recipe.

## Layout

**Files here are templates, directories are languages.**

```
mosaic.pot        English template, main catalog     (regenerate: --target pot)
motivate.pot      English template, "motivate" domain (see motivate.md)
LINGUAS           the shipped language list          (generate: tools/i18n/gen_linguas.py)
de/mosaic.po      German
pt_BR/mosaic.po   Brazilian Portuguese
ca@valencia/…     @modifier variants keep the modifier in the directory name
```

## Improving an existing translation

```sh
cmake --build build/linux-debug --target pot     # only if UI strings changed
msgmerge --update de/mosaic.po mosaic.pot        # pull in new/changed strings
$EDITOR de/mosaic.po                             # or Lokalize / Poedit
```

Then see it running — no install step needed:

```sh
cmake --build build/linux-debug
MOSAIC_LOCALEDIR=build/linux-debug/locale MOSAIC_LANG=de ./build/linux-debug/bin/mosaic
```

**Every catalog here is a machine-assisted first draft.** They are a starting point, not a
finished localisation, and corrections from native speakers are the whole point. If you are unsure
about an entry, leave it as `msgstr ""` — it falls back to English, which is a correct outcome. A
wrong translation is worse than an empty one, because someone has to notice it first.

Watch out for three things the build will reject:

- **`printf` specifiers** (`%s`, `%zu`, `%.1f`) must match the English exactly — a mismatch crashes.
- **Menu paths** like `&File/&New...` must keep their `/` separator count. `\/` is a *literal*
  slash inside one label, not a separator.
- **Leading/trailing spaces** in fragments like `"Version "` are glued to a value at runtime.

## Starting a new language

1. Add a row to `tools/i18n/languages.py`, then run `tools/i18n/gen_linguas.py`.
2. `msginit --input=mosaic.pot --locale=<code> --output=<code>/mosaic.po`
3. Re-run CMake (it watches `LINGUAS`) and build.

## Coverage

```sh
cmake --build build/linux-debug --target i18n-stats
```

The first pass covered the core UI; the Texture Generator dialog and the Settings long-form help
are still untranslated everywhere and show English. `docs/i18n.md` describes the bulk pipeline in
`tools/i18n/` used to widen that.
