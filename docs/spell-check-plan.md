# Spell-checking — implementation plan (scoping only; not started)

> Companion to `docs/type-deferred-features.md` **§2** (the DESIGN + rationale — read it first). This
> file is the concrete, codebase-grounded **build plan** for the next session: the integration points
> (with file anchors), the commit sequence, the open decisions, and the test posture. Nothing here is
> implemented yet. Foundations already shipped and reused: **§0** `core/text/tokenize.*` (word
> segmentation, with `allCaps`/`hasDigit`/`cjk` flags) + `core/text/language.*` (BCP-47 resolution),
> and the `Paragraph::language` attribute (origin/main `80544e1`).

## What ships (user-visible)
Red wavy underline under misspelled words (canvas **overlay**, never baked into the text layer /
export); right-click a misspelling → suggestions at the **top** of the menu, then Add to Dictionary /
Ignore All, then the existing Cut/Copy/Paste. A Settings toggle. Off never blocks typing.

## Backend availability (verified this session)
- **Linux: enchant-2 2.8.15 present, HAS pkg-config** → `pkg_check_modules(ENCHANT REQUIRED IMPORTED_TARGET enchant-2)` (cleaner than libhyphen's `find_library`). It unifies hunspell/nuspell/aspell + finds system dicts (`/usr/share/hunspell/*.dic` present: en_US, en_US-large, pl_PL).
- **Windows**: `ISpellChecker`/`ISpellCheckerFactory` (Win8+, SDK-native). **macOS**: `NSSpellChecker` (AppKit). Both later; Linux/enchant + a mock first.

## Architecture (mirrors the Hyphenator pattern)
1. **`core::text::SpellChecker`** — FLTK-free interface, enchant behind a **PImpl** (exactly like
   `core/text/hyphenator.{hpp,cpp}` wraps libhyphen). API:
   - `bool correct(word, lang)`, `std::vector<std::string> suggest(word, lang)`,
     `void addToUserDict(word, lang)`, `void ignore(word)` (session set), `bool hasDictionary(lang)`.
   - Lazy `enchant_broker_request_dict` per resolved language (reuse `primaryLanguageSubtag` fallback +
     the dict-cache pattern from `hyphenator.cpp`). enchant owns dict lifetime via the broker.
   - **Mock backend** (fixed bad-word list) compiled in for headless tests — no system-dict dependency,
     same trick as `Hyphenator::loadDictionaryData`.
2. **Tokenisation**: reuse `tokenizeWords` (§0). Skip pure numbers, URLs/emails, CJK, and **all-caps
   acronyms by DEFAULT** (the token's `allCaps` flag; the checker consults the Text-Settings "Check
   ALL-CAPS words" toggle, default OFF = skip — decision D4). Language per paragraph via
   `Paragraph::language` → shaper/app default (locale), same chain as hyphenation.
3. **Async / incremental checker** — transient editor state, **NOT** in the document/undo:
   - Lives on the text-edit session in `vulkan_canvas` (alongside `m_textEditTarget` / `m_textSel` /
     `m_textEditCoalesce`, ~`vulkan_canvas.cpp:2653+`). A `std::vector<{begin,end}>` of misspelled BYTE
     ranges (into `TextBlock::utf8`).
   - Recompute on edit, **debounced** on the frame tick — reuse the existing thumbnail-settle precedent
     (`onFrame` settle timer + a dirty flag; the S29-c "kTextThumbSettleSec" mechanism). Recheck only
     the **edited paragraph(s)**, not the whole block, per keystroke; check a word on completion.
   - **Runs on a BACKGROUND THREAD (decision D1)**: a worker checks the edited paragraph's tokens and
     posts misspelled ranges back to the UI thread (marshal onto the frame tick). Needs: a single
     worker (or small pool), a generation/epoch counter so a newer edit **cancels** a stale in-flight
     pass, and the result apply guarded against a session that closed mid-check. enchant dict access is
     confined to the worker (its own broker) so no cross-thread enchant calls.
4. **Squiggle overlay render** — map misspelled byte ranges → glyph spans → screen segments, reusing
   the same byte→x mapping selection already uses (`text_edit.cpp` `caretXInLine` / `selectionRects`
   are the model; the squiggle wants the per-line x0..x1 of each range). Draw a **red wavy underline**
   (sine/zig-zag polyline, device space, AA, just below the baseline — reuse the decoration underline
   offset) in the **existing canvas text-overlay pass** where the caret + selection rects already draw.
   Overlay ⇒ tracks zoom/scroll, toggles instantly, never composites into pixels or exports.
5. **Context menu** — extend the Type tool's themed `ui::ContextMenu` right-click menu
   (`vulkan_canvas.cpp:~3688`, currently Cut/Copy/Paste/Select-All). When the click byte lands inside a
   misspelled range: prepend up to ~5 `suggest()` results (bold top), a divider, **Add to Dictionary**
   + **Ignore All**, a divider, then the existing items. Picking a suggestion replaces the word range
   through the **normal edit funnel** (`m_typeHost.editText` + coalesce id) so it is **one undo step**.
6. **Custom dictionary** — Add-to-Dictionary → `enchant_dict_add` (enchant persists its own personal
   wordlist; **recommended** over a bespoke sidecar). "Ignore All" → a session-scoped `std::set` the
   checker consults before flagging. (Windows/macOS backends have their own learn/ignore.)
7. **Settings** — a `bool spellCheck` on `common::Settings` + a **default text language** string (the
   app-level top of the `resolveLanguage` chain, seeded from locale, which finally WIRES the unwired
   `TextShaper::setDefaultLanguage` and feeds hyphenation too). Rail category (list at
   `settings_dialog.cpp:808`): **create a new "Text" category** seeded with these two — a lone
   spell-check checkbox does not justify a category, but Enable + Default-language do (and the R5 emoji
   font picker / future type-wide prefs join later). If only the toggle ships, fold into General
   instead. Custom-dictionary management (view/clear enchant's learned words) is the third control, and
   a **Check ALL-CAPS words** toggle (default off; D4) the fourth. A per-document language override +
   per-document spell toggle are later.

## Settled decisions (user, 2026-07-01)
- **D1 Async model → BACKGROUND THREAD.** (User overrode the main-thread recommendation.) Worker +
  epoch-cancel + safe apply, per §3 above.
- **D2 Custom dict → enchant's built-in personal wordlist** (`enchant_dict_add`); native learn APIs on
  Win/macOS. No bespoke storage.
- **D3 Check scope → edited paragraph + lazy viewport** (bounded per-keystroke work).
- **D4 All-caps → skipped BY DEFAULT + an opt-in "Check ALL-CAPS words" toggle** in the new Text
  category (revised: cheap now the category exists, and checking caps typos is a real niche use case).
  Default stays skip — the good default is never something you must toggle *into*.
- **Mixed-language**: switch dict per paragraph via `Paragraph::language` (falls out of the model).
- **STILL OPEN — Settings home** (only needed at commit 5): default to a NEW "Text" rail category
  unless the user says fold it into an existing one; does not constrain commits 1–4.

## Commit sequence (one logical change each)
1. **SpellChecker interface + enchant backend + mock + tests** (deterministic via mock; CMake
   `pkg_check_modules(enchant-2)`; `docs/third-party-licenses.md` enchant row — LGPL-2.1+). No UI.
2. **Session misspelled-range computation** — background worker (D1) + epoch-cancel, edited-paragraph
   scoped (D3), results applied on the frame tick. Keep the pure range→word logic FLTK-free + unit-test
   it with the mock (the worker/marshalling is the thin, manually-verified shell around it).
3. **Squiggle overlay** — byte-range→screen mapping + wavy-underline draw in the text-overlay pass.
4. **Context-menu extension** — suggestions + Add/Ignore, replace-via-funnel (one undo step).
5. **Custom-dict persistence + Settings toggle** (+ enable/disable wired live).

## Testing posture
Keep all word/range logic in `core::text` (doctest, mock backend). Native enchant + the GPU squiggle
overlay + the menu are manual/visual checks (per the verification division: Claude headless, user
visual). Gate any real-dict assertions on `SpellChecker::hasDictionary` (the CI-safe pattern used for
`Hyphenator::hasDictionary`).
