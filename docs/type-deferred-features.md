# Type: deferred advanced features — scope & design

> **Status: SCOPED, not yet built.** This is the design/scope note for the three sizeable Type
> features deliberately split out of S29-c (the Type panel rounds R1–R3, which laid the foundations:
> on-canvas editing, the selection-style funnel, the Type panel, justify). They are real subsystems,
> not panel tweaks, so each gets its own session. Companion to `docs/type-tool.md` (§8 panel, §10 3D).
>
> The three: **(1) Hyphenation**, **(2) Spell-checking**, **(3) Vertical writing-mode**. They share a
> common foundation — a **per-paragraph language attribute** — so build that first (§0), then the
> features in any order. None is on the linear roadmap; they slot in like S37/S39 did.

---

## 0. Shared foundation — the language attribute (build FIRST)

All three features are language-specific (hyphenation patterns, spell dictionaries, and vertical
glyph orientation are per-language/script). Today the model carries `Paragraph::direction` but no
language. Add it once, up front:

- **Model**: `std::string language;` on `Paragraph` (BCP-47 tag, e.g. `"en-US"`, `"de"`, `"ja"`).
  Empty ⇒ inherit the document default. Add a document-level default language (Settings + per-document
  override), seeded from the OS locale.
- **UI**: a language picker in the Type panel Paragraph section (and/or a document-language setting).
  Most users never touch it; the default-from-locale is the calm path.
- **Why per-paragraph**: a document can mix languages (a quote in French inside English body); the
  spell-checker and hyphenator must switch dictionaries per run/paragraph. Per-paragraph is the
  pragmatic granularity (per-run is overkill for v1; revisit if needed).
- **Tokenisation**: both hyphenation and spell-check need word segmentation. Implement once, shared:
  Unicode word boundaries (UAX #29) over the UTF-8 — skip numbers, URLs/emails, all-caps acronyms
  (configurable). Keep it in `core::text` (FLTK-free, unit-tested).

---

## 1. Hyphenation (hyphenated vs unhyphenated mode)

**Goal.** Optional automatic hyphenation so justified text fills lines evenly (the screenshot's
"the·········time" rivers come from space-justify with few words/line — only hyphenation fixes it).
Also improves ragged-right wrapping. A per-paragraph **Hyphenate** toggle; off by default.

**Library.** Prefer **libhyphen** (the "hyphen" library: Liang's 1983 TeX algorithm + per-language
pattern files; used by LibreOffice/Firefox/Chromium). Tri-licensed GPL2/LGPL2.1/MPL1.1 — use under
**LGPL-2.1+ or MPL** for GPLv3 compatibility (note in the lineage header). Liang's algorithm has
been published since 1983. System dep where present
(`/usr/share/hyphen/hyph_*.dic`); bundle the common dictionaries (en, de, fr, es, it, …) for
Windows/macOS and as a fallback.
- *Alternative considered:* vendoring Liang's algorithm + patterns directly (no dep). More work,
  more bytes; choose only if libhyphen packaging is a problem on a target platform.

**Model.** `bool hyphenate = false;` on `Paragraph`. Uses `Paragraph::language` (§0) to pick the
pattern file. `lefthyphenmin`/`righthyphenmin` (min chars before/after a break) from the dictionary
or sane defaults (2/3).

**Shaping/layout integration** (`core::text::shaping`):
- Hyphenation matters only where text **wraps** → Area text (and, later, vertical Area). Point text
  doesn't wrap; no hyphenation there.
- In the wrap loop (today `shaping.cpp` ~L302–328): when a word overflows the line and no whitespace
  break fits, query the hyphenator for break opportunities **inside** the word; pick the last break
  that fits, emit a **hyphen glyph** (shaped in the run's face, its advance counted), continue.
- Penalties/quality: avoid hyphenating the last word of a paragraph, avoid >2 consecutive hyphenated
  lines, respect min-before/after. (Liang gives candidates; we apply the line-break policy.)
- Justify interaction: more break points ⇒ less per-space stretch ⇒ the loose-line problem goes away.
  The hyphen sits at the line's right edge; the laid width still fills `availContent`.

**UI.** A "Hyphenate" toggle in the Type panel Paragraph section (paired with the language picker).
Optionally a context-bar toggle later.

**Edge cases.** Existing soft/hard hyphens (U+00AD soft hyphen as a manual override; respect it),
URLs/emails (never hyphenate), numbers, all-caps, very short words, CJK (no hyphenation — wraps
anywhere; gate by script).

**Testing.** Unit-test break points for known words per language (deterministic); golden-image for a
justified paragraph hyphenated vs not.

---

## 2. Spell-checking (squiggles + suggestions) — "the whole orchestra"

**Goal.** Red wavy underline under misspelled words; right-click a misspelling → suggestions at the
**top** of the menu, then Add-to-Dictionary / Ignore, then the usual Cut/Copy/Paste. The squiggle is
**editor chrome, not document content** — it is NOT baked into the text layer pixels (drawn as a
canvas overlay over the glyph spans), so it never exports/compositing-bakes and toggles instantly.

**Backends — native where possible, behind one interface.** Define `core::text::SpellChecker`
(FLTK-free): `bool correct(word, lang)`, `std::vector<std::string> suggest(word, lang)`,
`void addToUserDict(word)`, `void ignore(word)`, available languages. Implementations via `#ifdef`:
- **Windows**: the Spell Checking API — `ISpellCheckerFactory` / `ISpellChecker` (Win8+). Native, no
  dep; respects the user's installed languages.
- **macOS**: `NSSpellChecker` (AppKit). Native; also gives suggestions + learn/ignore.
- **Linux**: **enchant** — the unifying layer over hunspell/nuspell/aspell that exactly solves the
  "Linux has 10 trillion spell checkers" problem (it finds whatever backend + system dictionaries the
  distro ships). Depend on enchant-2; it pulls hunspell dictionaries (`/usr/share/hunspell`). (Direct
  hunspell is the fallback if enchant is unavailable.) All FOSS.

**Async / incremental.** Spell-checking must never block typing. Run a background pass that tokenises
(§0) the changed region, checks each word on **word completion** (not mid-word), and produces a list
of misspelled **byte ranges**. Debounce like the thumbnail settle (rev 11). Recheck only the edited
paragraph(s), not the whole document, per keystroke.

**Model/state.** The misspelled-range set is **transient editor state** on the text session (not in
the document/undo). A per-document/user **custom dictionary** (persisted in Settings or a sidecar
file) backs Add-to-Dictionary; "Ignore" is session-scoped.

**Rendering.** Map misspelled byte ranges → glyph spans → screen segments (the shaper already maps
runs↔glyphs and has decoration offsets). Draw a **red wavy underline** as a canvas overlay under each
span (a sine/zig-zag polyline in device space, AA, below the baseline — reuse the underline offset
metric). Overlay, so it tracks zoom/scroll and never bakes into pixels.

**Context menu.** Extend the text-edit right-click menu (the themed `ContextMenu`): when the click
lands on a misspelled word, prepend **up to ~5 suggestions** (bold the menu top), a divider, **Add to
Dictionary** + **Ignore All**, a divider, then the existing Cut/Copy/Paste. Selecting a suggestion
replaces the word range via the normal edit funnel (one undo step).

**Settings.** Enable/disable spell-check; language (defaults from `Paragraph::language` §0 / locale);
manage the custom dictionary. Possibly a per-document toggle.

**Edge cases.** Mixed-language paragraphs (switch dict by `language`), numbers/URLs/code skipped,
repeated-word detection (optional), performance on huge text (only the visible + edited region need
squiggles; check lazily by viewport).

**Testing.** A mock `SpellChecker` backend (fixed word list) for headless tests of tokenisation,
range→glyph mapping, and the menu assembly. Native backends are smoke-tested per platform manually.

---

## 3. Vertical writing-mode (vertical-rl / CJK)

(Carried from the S29-c R10 deferral — recapped here so all three live in one place.)

**Goal.** A `writing-mode` toggle: `horizontal-tb` (today's default) vs `vertical-rl` (CJK vertical
text: glyphs stack top-to-bottom, columns run right-to-left). A tool-context-bar toggle.

**Why it's big.** The whole shaping/layout pipeline is horizontal today: `ShapedGlyph.pen` on a
baseline with a scalar horizontal `advance`; `ShapedLine` carries `baselineY`/`ascent`/`descent`/
`width`. Vertical mode needs:
- **Model**: `enum class WritingMode { HorizontalTb, VerticalRl };` on `TextBlock` (block-level).
- **Shaping**: HarfBuzz vertical direction (`HB_DIRECTION_TTB`) + **vertical metrics** (the font's
  `vmtx`/`VORG`; fall back to synthesised metrics when absent). **Glyph orientation per UAX #50**
  (Vertical_Orientation): CJK/kana upright, Latin/digits rotated 90° (or upright per text-orientation
  — at least handle the common `mixed`). Optional tate-chu-yoko (short Latin runs upright) is a later
  refinement, not v1.
- **Layout**: "lines" become **columns**; column advance runs right-to-left; leading becomes
  inter-column spacing; alignment/justify act along the vertical axis. The `ShapedLine`/bounds model
  needs an axis abstraction (or a parallel vertical path).
- **Caret/selection/hit-testing**: caret is horizontal between glyphs; Up/Down vs Left/Right key
  semantics rotate; selection rectangles are per-column.
- **Rendering**: the rasterizer consumes positioned glyphs, so mostly falls out once layout produces
  vertical positions — but verify the device-space tessellation + decoration drawing.
- **UI**: a context-bar **Vertical** toggle (per the original ask); Area boxes grow leftward.

**Scope discipline for v1.** Vertical-rl + mixed orientation (UAX #50) + vertical Area wrapping +
caret/selection. Defer: tate-chu-yoko, vertical-lr, ruby, full bidi-in-vertical.

---

## Sequencing & dependencies

1. **§0 language attribute + shared tokeniser** — small, unblocks 1 & 2.
2. **Hyphenation** — adds `libhyphen` + bundled `hyph_*.dic`; wrap-step change; one toggle.
3. **Spell-checking** — adds `enchant-2` (Linux) + native Win/macOS; async checker; overlay render;
   context-menu extension; custom-dict persistence. The biggest of the three (cross-platform + async +
   UI).
4. **Vertical writing-mode** — the deepest shaping/layout rework; independent of 1–2 (but shares §0
   for script/orientation by language).

**New build deps:** `libhyphen` (+ dictionaries), `enchant-2`/`hunspell` (Linux); Windows Spell
Checking API + AppKit `NSSpellChecker` are SDK-native (no third-party). All FOSS;
record each in the consuming file's lineage header and confirm GPLv3 compatibility (libhyphen via
LGPL-2.1/MPL; enchant LGPL-2.1+; hunspell LGPL/GPL/MPL).

**Testing posture:** keep all language logic in `core::text` (FLTK-free, doctest + golden-image);
mock the spell backend; the native spell backends and the GPU overlay are manual/visual checks.
