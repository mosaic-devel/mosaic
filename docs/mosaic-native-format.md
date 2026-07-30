# `.mosaic` Native Format — Scoping Plan

Covers **S48** (`.mosaic` native *Save*, PLAN.md: "the one native format must round-trip both
Raster and Vector history"). This is the implementation spec — the level of detail matches
`docs/export-system-plan.md` — synthesized from a standalone, 11-round empirical research project
done outside this repo (six container designs benchmarked for corruption resilience/save-speed/
compression; four-plus undo-history designs benchmarked for storage/timing/corruption; ~500
adversarial test checks across both; a dedicated licensing pass, independently re-verified
here). A separate narrative write-up of that research — the story, not the spec — lives at
`docs/mosaic-native-format-research.md`. This document does not cover S41/S42/S44 (the `Export`
pipeline and interchange formats — see `docs/export-system-plan.md`) or S30-b (the future
vector-only document type, which this format reserves a code for but does not build out).

This is a **scoping document, not implemented code.** Nothing in `src/` changes as a result of this
pass.

**Revision note (2026-07-07):** the first pass of this document answered several remaining
questions (tile size, the fast compression tier, root-slot sizing, the retention-budget default,
the `HIST` chunk's `parent` field) by reasoning from the existing research rather than testing
them — inconsistent with the standard the rest of this design was held to. A follow-up round
(**Round 10**, `findings.md` in the standalone research project) went back and tested every one of
them empirically, using the same correctness-before-trust discipline as the original nine rounds.
This revision reflects those real results, including one place (§0, compression) where the
measurement argues against the first pass's proposed answer.

**Revision note 2 (2026-07-07, review round):** a full review of this document surfaced one
product-semantics gap and several spec-level inconsistencies. The user resolved the product
question with a hard rule — **the user's file is never written without their permission; an
explicit Save (Ctrl+S) is the only thing that touches it** — which moves incremental autosave out
of the document file entirely, into an app-owned recovery journal (§2.6). The review's design
consequences (journal binding, salvage-past-a-damaged-frame semantics, explicit-link frames, and
the Reed-Solomon padding number §2.7 now cites) were **empirically tested first** (**Round 11**,
`scripts/round11_journal_salvage.py` in the standalone research project, 16 checks green plus the
standing gates re-run) — same discipline as Round 10, not reasoned-only amendments. The remaining
review items (structured chunk keys §2.2, checkpoint copy-through §3.3, the bimodal per-state cost
caveat §3.4, advisory locking §2.10) are layout/arithmetic/policy changes with no untested
empirical claim in them; where a number was involved (§2.2 header overhead) it is an exact rescale
of a Round 10 measurement, flagged as such.

**Revision note 3 (2026-07-07, Save-semantics round):** the user proposed — correctly — that
**File→Save should commit, not rewrite**: append the session's new states to the file as a
committed batch (O(changed data)), with "Save As" and an occasional threshold-triggered
compaction doing the full write. This keeps revision 2's hard rule intact (the file is still
written *only* at an explicit Save — what changed is *how*), and it re-activates the research's
own nine-round-hardened WAL machinery inside the document file, now gated behind user intent
instead of autosave. Tested before adoption (**Round 12**, `scripts/round12_save_commit.py`, 12
checks green, gates re-run): Save atomicity under tearing, the crash window (torn Save + intact
journal loses nothing), commit-granularity journal binding, in-file salvage carry-over, index-free
full-scan on appended files — plus two findings worth naming: appending costs **-0.9%** vs the
full rewrite it replaces (under retained history the full save was mostly re-writing chunks it
keeps — §2.6), and the §2.2 generation rule was *demonstrated* load-bearing when the harness
briefly violated it and full-scan silently returned stale content. The same round measured the
journal's real write volume at 64px to settle autosave cadence (§2.6) — SSD wear is a non-issue
at any plausible setting.

**Revision note 4 (2026-07-07, dual-writer round):** the user asked whether "two Mosaics saving
into one file" was ever truly resolved — it was designed (§2.10's lock) but never *run*.
**Round 13** (`scripts/round13_dual_writer.py`, 9 checks green, Rounds 11/12 re-run green) ran
it and found one real hole: salvage would have silently **blended two writers' lineages** into a
document neither ever saw (D2). Fixes adopted: lineage-aware salvage (§2.8 — partition frames by
link-chain, primary = seed-rooted winner, foreign lineages recovered separately and never
merged) and the O(1) **pre-Save tail check** (§2.6 — a non-racing foreign append, truncation, or
inode swap refuses loudly before writing). §2.10 now states the layered result plainly: lock =
prevention, tail check = detection, deterministic chain winner = containment, lineage salvage =
recovery — the worst case for simultaneous writers is a loudly-reported conflict with both sides
recoverable.

---

## 0. Decisions locked

- **The user's file is written only by an explicit Save. Never otherwise.** (User decision,
  2026-07-07.) Incremental autosave goes to an app-owned **recovery journal** in the OS state
  directory (§2.6), bound to the exact commit it extends; an explicit Save is the *only* writer
  of the user's path. Consequences: closing without saving genuinely discards (delete the
  journal — the user's file never contained the unsaved work); the S18-d "unsaved" window title
  is *literally true about the bytes on disk*; cloud-sync clients see the document change exactly
  once per deliberate Save; and crash recovery restores unsaved work as unsaved *in-memory*
  state, never by silently writing the user's file. Round 11-tested: the sidecar carries every
  WAL-mechanism property over unchanged, and the journal's chain seed makes a stale journal
  structurally unreplayable, not merely policy-rejected.
- **File→Save commits; it does not rewrite.** (User decision, 2026-07-07; Round 12-tested.)
  An ordinary Save **appends** the states accumulated since the last Save as one committed,
  explicit-link batch closed by a `CMIT` frame — O(changed data), milliseconds instead of
  seconds on a large document, and atomic: a torn Save opens at the previous commit, never a
  half-saved hybrid. The **full write** (build-fsync-rename, §2.6) happens for Save As (a new
  path needs a complete file), the first save of an untitled document, and **automatically when
  a Save trips the compaction threshold** — never in the background, so every byte written to
  the user's path stays behind an explicit Save. Measured (Round 12): under retained history the
  append-file is the *same size* as the full rewrite it replaces (−0.9% in the test — the full
  save was mostly re-writing chunks it keeps), so compaction's actual deliverables are parity
  refresh, budget eviction, and structural tidiness, not space. No user-facing knob: Save is
  Save ([[mosaic-no-toggle-for-strictly-better]] applies).
- **Autosave cadence: idle-triggered, coalesced, and unconstrained by SSD wear — measured.**
  (Round 12, §2.6.) A deliberately heavy painting hour (600 strokes, ~4,800 tile after-images at
  64px) journals ~60MB logical; even at a pathological per-stroke fsync cadence, an 8-hour day
  consumes ~0.19% of a 600TBW consumer drive's per-day endurance budget. Flash life is therefore
  *not* the constraint; fsync latency, CPU, and battery are. Policy: autosave only when unsaved
  changes are pending, on a few seconds of idle, with a ceiling during continuous activity and a
  minimum gap (§2.6) — typically 1–6 journal writes/minute while painting, zero when idle or
  clean.
- **Journal frames are explicit-link, not cumulative-chain.** Each journal frame carries a
  standalone checksum plus the previous frame's checksum as a checked 8-byte LINK field (§2.2,
  §2.6) — detection power for torn tails and reordering is identical (Round 11, tested), but a
  structurally destroyed frame localizes to a gap instead of rendering every later, physically
  intact frame unverifiable by construction. This is what makes honest salvage (§2.8) possible:
  the measured difference between recovering 12/12 states with precise per-key flags and
  abandoning 13 intact frames.
- **Chunk identity is a structured 16-byte KEY + 64-bit generation, little-endian** (§2.2) — the
  earlier 4-byte `chunk_id` could not hold a collision-free encoding of Mosaic's 64-bit `LayerId`
  plus tile coordinates, and an opaque sequential id would have broken full-scan recovery's
  self-describing property. This resolves what §9 previously carried as the last open question.
- **Checkpoint copy-through** (§3.3): an explicit Save re-encodes only the history that is new
  since the last checkpoint (journal states, LZ4→zstd once); already-checkpointed history chunks
  are verify-then-copied byte-verbatim. Save cost stays O(current content + new history), not
  O(session length). Only an H2↔H4 mode switch re-encodes the whole retained history.
- **Advisory per-document locking** (§2.10): a lock file in the recovery directory — never an
  attribute or handle on the user's file — makes accidental double-open safe (read-only second
  opener, explicit takeover, stale-lock detection feeding the crash-recovery path).
- **Container shape: a bespoke, self-describing chunked binary stream (PNG/EBML lineage), NOT a ZIP
  wrapper.** This **supersedes** the ZIP-based sketch currently in PLAN.md §3.16 and the S48 entry
  ("like ORA/KRA": mimetype-first-entry, `manifest.json`, tiled blobs) — see §5 for the full
  reconciliation. The research measured ZIP's structural fragility directly (its one authoritative
  index, the central directory, is written *last* and is the first thing lost on truncation, with
  zero built-in redundancy — corroborated by a real Mozilla CI incident and by Krita's own,
  ZIP-based `.kra` format's real bug-tracker history of corrupted-file reports) against a chunked
  alternative that recovered 84–88% of data on average across a corruption battery where the ZIP
  baseline recovered ~17%.
- **The manifest, directory, and history-commit payloads stay JSON.** Only the outer envelope
  changes. `nlohmann/json` is already vendored (`docs/third-party-licenses.md:14`); every existing
  doc that assumes a JSON manifest (`docs/type-tool.md:650`, `docs/layer-effects.md:175,466`,
  `docs/type-vertical-writing-mode.md:43-44`, `docs/export-system-plan.md:322`) needs no revision.
- **Undo/redo history is LINEAR, not tree-structured.** The research built and fully tested a
  tree (undo, then a new edit, creates a branch; abandoned branches survive with their own
  time-boxed retention policy) — that design is explicitly **out of scope** for the real
  implementation. Every mainstream raster editor (Photoshop, GIMP, Krita) uses linear undo, and
  Mosaic should too. This was decided in conversation after the empirical research concluded; it
  does not reflect a flaw the research found, it's an ordinary product decision made on top of it.
  See §3 for what this does and doesn't change.
- **History-encoding default: H2 (retained-journal)** — each undo state stores only its dirty
  tiles' after-images, which is exactly the content the recovery journal (§2.6) already writes at
  autosave time. "Keeping history" costs nothing extra at the moment it matters (autosave); at
  Save time it costs one re-encode of just the new states (copy-through, §3.3); the ongoing cost
  is bounded, tunable storage overhead. **History is on by default, no settings toggle** — there
  is no genuine trade-off here for a toggle to arbitrate.
- **Checksums/compression:** xxh3-64 per-chunk (fast tier), BLAKE3 for the root (strong tier); **LZ4
  for the fast/autosave tier, zstd (levels ~3 / ~19) for balanced/max** — a from-scratch GF(256)
  Reed-Solomon coder for checkpoint-time-only erasure parity. All licence-clean and
  GPLv3-compatible (§4). **Measured, not assumed (§2.4):** dropping LZ4 in favor of zstd's own fastest
  level was considered and tested directly — LZ4 is genuinely faster in both directions (1.4–1.8x
  faster to compress, 2.7–2.8x faster to decompress than either zstd fast-tier candidate measured),
  at the cost of a meaningfully worse ratio. Given this design's own stated priority puts "fast
  saving, including autosave" above "good compression," and decompress speed specifically matters
  for the journal-replay-on-reopen and undo-walk paths, **LZ4 stays** — this is a case where testing the
  alternative confirmed the original choice rather than replacing it.
- **Tile size: 64px**, not the research prototype's 256px default. **Measured, not assumed (§2.1):**
  tile size was never actually varied across the original nine rounds — 256px was a code-comment
  assumption. Tested directly: the compression-ratio cost of going smaller is negligible (128px
  measured slightly *better* than 256px; 64px only 1.38% worse), while a single small, localized
  edit costs **13.8x more** to autosave at 256px than at 64px, because this design has no sub-tile
  diffing — the whole containing tile is re-encoded on any touch. 64px wins clearly on every axis
  that matters for a paint tool's actual (frequent-small-edit) usage pattern.
- **Root slot: 128KB, with a "write the real root as an ordinary chunk if it doesn't fit" overflow
  path.** Designed, implemented, and tested (§2.3) — the prototype's fixed 64KB slot had no overflow
  handling at all, a real latent bug for any document whose directory/history table grows past it.
- **Retention budget: unlimited by default, backstopped by a generous (several-hundred-MB) safety
  net**, grounded in a measured per-state cost (§3.4) rather than a guess.
- **Spatial filter: Paeth** (PNG's), not a simpler Sub filter — closes most of the compression gap
  with plain per-layer PNG. Needs a real SIMD-vectorized decoder in C++ (a solved problem; not the
  Python research prototype's numpy trick — see §8).
- **Build plan (§6):** Build 1 ships the container + linear H2 history + the interactive undo/redo
  hot path — "Save works, reliably, with undo history that survives a reopen." Build 2 adds H4
  content-addressing + adaptive H2↔H4 switching (§3.9) — this needs **no telemetry**: it measures
  each document's own churn locally, at checkpoint time, and is sequenced second purely to keep
  Build 1 small, not because it's waiting on data. H3 (xor-delta compaction) and history-region
  Reed-Solomon parity are real, tested designs that are **not in the build plan** (§3.8) — available
  later if a concrete need arises, not scheduled.

---

## 1. Goals & non-goals

**Goals, in priority order** (unchanged from the research brief):

1. **Nuclear-bunker resiliency** — detect any corruption, recover from substantial corruption, no
   single point of failure.
2. **Fast saving, including autosave** — cost scales with changed data, not document size.
3. **Good compression**, with multiple encoding profiles so autosave never pays a high-CPU
   compression cost.

Throughout: no licensing risk; **the user's file changes only on explicit user action**
(the §0 hard rule — autosave and crash recovery must never write to it); a single flat file at
rest (the transient recovery journal is app-owned state in the OS state directory, deleted on
clean close — never a sidecar dropped next to the user's data); case-insensitive-filesystem safety
(no internal string-name addressing — see §2.2); tolerance of naive cloud-sync clients uploading a
file mid-write; a `documentType` field (raster+vector today; vector-only and future types
reserved); a small embedded preview; forward-compatible unknown-chunk preservation; backward
compatibility.

**Non-goals, explicitly:**

- **Multi-writer/daemon coordination.** SQLite WAL's shared-memory `-shm` index (needed for
  concurrent readers/writers on one host) is deliberately **not** adopted — Mosaic has no
  multi-process/multi-writer problem to justify it, and WAL's own documentation says this exact
  mechanism is what breaks under networked storage.
- **Network-filesystem-native operation.** The design assumes local-disk-then-sync (Dropbox/
  OneDrive/iCloud-style), not a file opened live over NFS/SMB by multiple writers.
- **Tree-structured/branching undo.** Superseded by the linear-undo decision (§3.1).
- **Blind (non-erasure) error correction.** The directory already identifies *which* chunk is
  damaged, so recovery only ever needs the cheaper known-erasure-position Reed-Solomon decode path,
  never a blind error locator.

### 1.1 How this works end-to-end (a walkthrough)

Before the byte-level detail below, the operational shape in plain terms — what actually happens
when an edit gets saved:

1. **While editing**, the user's file is untouched. Current pixels/vectors and the undo stack live
   in memory, same as today.
2. **Autosave** (frequent, cheap): grab only the tiles/vectors that changed since the last
   autosave. Each one gets the Paeth filter (§2.5) applied, fast-compressed (§2.4's `fast`
   profile), and wrapped in its own self-describing, checksummed chunk (§2.2) — compression
   happens per piece, not as one step over an assembled blob, which is exactly what lets one
   damaged piece stay damaged without taking the rest down with it. These frames get **appended to
   the recovery journal** (§2.6) — an app-owned file in the OS state directory, *never* the user's
   file — each carrying an explicit link to the previous frame's checksum, plus one small `HIST`
   commit frame (§3.2) that closes this as an official undo state. Cost: proportional to what
   changed, not document size.
3. **Explicit Save** (the only writer of the user's path — and, ordinarily, *cheap*): append the
   states accumulated since the last Save as one committed batch — each state's dirty
   tiles/vectors re-encoded once at `balanced` (§3.3), its `HIST` frame, and a closing `CMIT`
   commit frame, all explicit-link-chained onto the file's committed region (§2.6). Cost:
   O(changed since last Save), milliseconds. Only then is the journal reset (rebound to the new
   commit) — so a crash *during* Save still loses nothing. A torn Save opens at the previous
   commit: Save is atomic at batch granularity.
4. **Full write** (Save As, the first save of an untitled document, or a Save that trips the
   compaction threshold): build a whole *new* file — current content at `balanced`/`max`,
   retained history by **copy-through** (§3.3: committed chunks verified and copied
   byte-verbatim; the H2-vs-H4 encoding choice, §3.9, is made here), Reed-Solomon parity (§2.7,
   computed only here), fresh root/directory (§2.3). Write to a temp path, fsync (file, then its
   directory), atomically rename over the target (§2.6) — a crash mid-write leaves the previous
   good file completely untouched.
5. **Reopening**: read the root (small, tells you where the directory is) → read the directory →
   replay the committed append region forward from `wal_start_offset`, batch-transactionally
   (§2.8). If a bound recovery journal survives for this document (a crash — a clean close
   deletes it), verify its binding and offer to restore: replayed work arrives as **unsaved
   in-memory state** (dirty title and all, per S18-d) — the user's file stays untouched until
   they choose Save. If the root/directory itself is damaged: fall back to scanning the whole
   file linearly — every chunk announces its own type/key/checksum, so this works with no index
   at all, just slower.

---

## 2. Container format spec

### 2.1 Preamble

A fixed 16-byte preamble at byte 0: an 8-byte magic sentinel + a format-version byte + a
`documentType` code byte + 6 reserved bytes. Purely a convenience for instant type-sniffing (file
managers, a "is this even a `.mosaic` file" check) — **never load-bearing**. A destroyed preamble
must not block recovery; the full linear chunk-scan fallback (§2.8) doesn't need it at all.

`documentType` codes: `0` = raster+vector (the only type actually built); `1` = vector-only
(reserved for S30-b, not exercised until that document type exists).

### 2.2 Chunk framing

Every chunk after the preamble is self-describing, fixed 46-byte header + optional link field +
payload + checksum suffix. **All integer fields are little-endian, at fixed offsets.**

```
MAGIC(8) TYPE(4) FLAGS(1) PROFILE(1) KEY(16) GENERATION(8) UNCOMPRESSED_LEN(4) PAYLOAD_LEN(4)
| [LINK(8) — present only when FLAGS bit 1 is set] | payload (PAYLOAD_LEN bytes) | checksum (8 or 32 bytes)
```

This supersedes the research prototype's `CHUNK_ID(4) GENERATION(4)` fields, for a reason that
was previously carried as §9's one open question: a 4-byte chunk id provably cannot hold a
collision-free encoding of Mosaic's 64-bit `LayerId` (`src/core/layer.hpp:33`) plus tile
coordinates, and the escape hatch — opaque, file-local sequential ids — would break §2.3's own
ground truth, because full-scan recovery can only reconstruct a document "from the self-describing
chunks alone" if a tile chunk itself says which layer and which tile it is. So the identity is
structured instead of derived:

- **KEY(16)**, interpreted per TYPE: `TILE` = `layer_id(u64) + tx(u32) + ty(u32)`; `VECT` =
  `layer_id(u64) + 8 zero bytes`; `HIST` = `state_id(u64) + 8 zero bytes`; `PRTY` =
  `stripe_index(u64) + 8 zero bytes`; `BLOB` (Build 2) = the first 128 bits of the content's
  BLAKE3 hash (the full 32-byte hash rides at the head of the payload; dedup equality decisions
  are always made on the full hash, the KEY prefix is only the chunk's file-level identity);
  `MFST`/`PRVW`/`ROOT`/`DIR`/`RPTR`/`JHDR` = all-zero (singletons — generation disambiguates).
  A chunk's logical identity is `(TYPE, KEY)`; "highest generation wins" resolves versions of the
  same identity exactly as before.
- **GENERATION(8)**: the undo-state id that wrote the chunk. State ids are a single per-document
  monotonic u64 counter, never reset (Round 10-verified, including across Flatten History — only
  states consume ids, so `parent == state − 1` in §3.2 is untouched by this change). A
  checkpoint's own generation (root/directory/manifest/parity chunks) is the newest retained
  state id at Save time, which keeps the §3.2 rule "full-scan recovery takes the
  highest-generation manifest, whether from `MFST` or a `HIST` snapshot" a single comparable
  ordering. **This rule is demonstrated load-bearing, not bookkeeping** (Round 12, A5): when the
  harness briefly violated it — a new state reusing an id at the checkpoint's generation —
  full-scan's "highest generation wins" tied, kept the older tile, and silently returned stale
  content. Only states consume ids; nothing else ever mints one.
- **LINK(8)**: journal frames only (§2.6) — the previous frame's checksum, sitting uncompressed
  between header and payload, covered by this frame's own checksum. (The Round 11 prototype
  embedded it at the head of the compressed payload instead; coverage and detection semantics are
  identical, but keeping it outside compression lets a chain walk read links without
  decompressing.)

Framing overhead becomes 46 + 8 = 54 bytes per ordinary chunk (62 for journal frames), up from
the prototype's 38. At 64px tiles this is bounded by an exact rescale of Round 10's measured
overhead (same chunk counts, same payload bytes, only the constant changes): **under 0.76% of
file size** — still noise. In exchange, canvas size and layer count can never overflow the id
space, and the collision-prone arithmetic id formula (§8) is gone entirely.

- **MAGIC**: an 8-byte sentinel, reserving a leading bit-pattern and banning degenerate all-0/all-1
  forms (the EBML self-synchronization idea) so a linear scan can resync to the next real chunk
  after *any* corruption — including a corrupted length field, which is PNG's one real structural
  weakness (a reader has no independent way to verify the length field itself before trusting it).
  A resync candidate is only trusted once its own checksum verifies; a coincidental MAGIC-byte
  collision inside compressed tile data is possible in principle but was empirically checked at
  zero occurrences across 28MB of real compressed content — reassuring, not a hard guarantee, which
  is exactly why the checksum check (not the magic match alone) is the actual defense.
- **Checksum**: 8-byte xxh3-64 for ordinary chunks; 32-byte BLAKE3 for `ROOT` chunks specifically
  (the ZFS "importance-weighted" idea — the small, precious root gets the stronger check).
  **The checksum covers every header field after MAGIC (type, flags, profile, key, generation,
  both lengths), the LINK field when present, and the payload — never just the payload.** This is
  load-bearing, not a stylistic choice: a bit flip isolated to `KEY` or `GENERATION` alone,
  leaving the payload and its own checksum untouched, would otherwise verify as "valid" while
  silently misattributing a correct, checksum-passing payload to the *wrong* logical tile.
- **FLAGS**: bit 0 = critical/ancillary (PNG's chunk-casing idea — an unknown chunk type can still
  be safely skipped by a future/older reader without a type registry, satisfying "forward-compatible
  unknown-chunk preservation" directly); bit 1 = linked (the 8-byte LINK field is present — journal
  frames, §2.6; **explicit-link, not the cumulative chain the research prototype used**: the frame's
  checksum stands alone, and the chain is validated by comparing LINK against the previous frame's
  actual checksum. Round 11 measured why: identical detection power for torn tails and reordering,
  but a structurally destroyed frame stops poisoning every later, physically intact frame — the
  difference between salvaging 12/12 states and abandoning 13 intact frames, §2.8); bit 2 =
  filtered (a spatial predictor, §2.5, was applied to the payload before compression); bit 3 =
  delta (reserved for history XOR-delta encoding, §3.8 — unused unless/until H3 ships).
- **Chunk types**: `MFST` (manifest), `PRVW` (preview — shipped at S48-b: payload = LE32 width,
  LE32 height, then width×height×4 Paeth-filtered RGBA bytes; 256px longest edge, written at
  `Profile::Max`, parity-uncovered per §2.7. A DERIVED artifact, never document content: loaded
  history skips its dirty keys, `applyChunksToDocument` refuses it, and compaction drops
  superseded copies instead of retaining them as undo states), `TILE`, `VECT`, `ROOT`, `DIR`, `RPTR`
  (root-slot overflow pointer, §2.3), `PRTY` (Reed-Solomon parity), `HIST` (history commit
  record, §3.2), `BLOB` (content-addressed history, §3.9 — Build 2), `JHDR` (journal binding
  header — recovery-journal files only, §2.6), `CMIT` (closes one commit-appended File→Save
  batch, §2.6 — all-zero KEY, generation = the saved state id).
- **Tile size: 64px.** Decided, not inherited from the prototype's 256px default — that value was
  never actually tested against alternatives (a code-comment assumption, not a measured one).
  Measured directly on real content: the compression-ratio cost of going smaller is negligible
  (128px measured slightly *better* than 256px; 64px only 1.38% worse — noise-level differences),
  per-chunk framing overhead stays under 0.76% of file size even at 64px (measured at 0.53% with
  the prototype's 38-byte frames, rescaled exactly to this spec's 54-byte frames), and — the
  deciding factor —
  a single small, localized edit costs **13.8x less** to autosave at 64px than at 256px, because
  this design has no sub-tile diffing: any edit inside a tile forces the whole tile to be
  re-filtered and re-compressed at the next autosave, so tile size directly sets the floor cost of
  "autosave cost scales with changed data" (§1, goal 2) for the common case. 64px also shrinks the
  worst-case Reed-Solomon stripe loss (§2.7) proportionally — a real, non-hypothetical benefit for
  the specific corruption scenarios where recovery genuinely isn't 100% (§2.8). Store this as the
  document manifest's own `tileSize` field so a possible future revision doesn't require a hard-coded
  constant, but 64 is the value to ship with.
- **Chunk addressing is purely numeric** (the structured `(TYPE, KEY)` identity above, plus its
  offset in the directory) — the container has **no internal string-path/filename surface at
  all**, which is what rules out the whole class of case-insensitive-filesystem collision bugs
  that hits ZIP/directory-bundle formats (APFS/NTFS both default to case-insensitive; a real,
  documented failure class for name-addressed archives). The research prototype's arithmetic
  chunk-id derivation formula is superseded outright by the KEY design — see §8.

### 2.3 Root and directory

The root is doubly-replicated: once in a **128KB reserved slot** near the start of the file, and
twice more at the end of the checkpoint region (protecting the commit marker itself from being
the thing that's torn on a crash or an interrupted sync). The end-of-checkpoint replicas sit at
EOF immediately after a full write; commit-appended Saves (§2.6) then land after them, so root
discovery is "slot, else backward magic-resync from EOF, else full scan" — and, importantly, the
roots do **not** go stale as Saves append: the checkpoint root stays the correct anchor, and the
commits are found by replaying forward from it (Round 12, A0/A5). Only a full write mints a new
root. A `DIR` chunk alongside the root holds the actual directory: `{(type, key) -> offset,
length, compression profile}` for every chunk written in that checkpoint, plus (if history is
retained) the history tables described in §3.2. Commit-appended states are *not* in the
directory — they are indexed by the replay walk, which is O(appended-since-checkpoint) and
bounded by the compaction threshold (§2.6).

**Slot overflow.** A fixed reserved slot needs an answer for "what if the real root doesn't fit" —
the research prototype's own fixed 64KB slot had no such handling at all, a real latent bug (a
document with a large enough directory/history table would silently desynchronize every offset
computed before that point). Designed and tested here: try to write the real, full root chunk
directly into the 128KB slot; if it fits (the common case — a typical document's root is a few KB),
done, one read, no indirection. If it doesn't fit, write a tiny, **always-fits** pointer chunk
(`RPTR`, just an offset) into the slot instead, and place the real root chunk as an ordinary chunk
immediately after the reserved region. **A reader needs no new logic to handle this** — the existing
full-scan fallback below already does a position-agnostic linear search for any valid `ROOT`-type
chunk and picks the highest generation, so even if the slot's pointer chunk is itself destroyed, the
real root (and the two end-of-file copies) are still found exactly as before. Tested: a typical
small document's root fits directly (no overflow); a synthetic document with enough Reed-Solomon
stripe entries to force a root past 128KB correctly overflows to a pointer + separate chunk, and
recovers correctly whether reached via the slot pointer, via full-scan after the pointer is
destroyed, or from the end-of-file copies alone after *both* the slot and the overflow chunk are
destroyed.

**The container version is checked, and it is checked twice.** The preamble's version byte (§2.1) is the only thing that can identify a container whose *framing* changed — by definition no chunk in such a file will verify, so the root cannot speak for it. But those 16 bytes carry no checksum, so a gate that trusted them alone would let one rotted byte lock a user out of an intact document. So the root carries `format_version` as well, under its BLAKE3 checksum, and the rule takes two facts:

- **A root verified.** The framing is ours, whatever the preamble says. The root's claim wins; a corrupted preamble byte is a non-event. If the *root* names a newer version, refuse.
- **No root verified, and the preamble claims a newer version.** Both facts agree on one explanation. Refuse honestly — *"this file needs a newer Mosaic"* — rather than degrade into recovery and tell the user their perfectly good document was destroyed. That sentence is the worst one a format built on never-being-silently-wrong could produce, and it cannot be retrofitted into readers already in the wild.

A refused file is not a damaged file: nothing is lost, nothing is counted, the append region is never replayed, and compaction refuses to fold it. Additive changes that keep the framing bump the manifest `schema` (§3.1) instead, which is the finer-grained wall.

**The manifest is replicated too.** The root tells you where things are; the `MFST` chunk tells
you *what they are* — canvas size, colour state, the layer tree, which layer owns which tile.
Lose it and a thousand intact, checksum-valid tile frames are worthless, because nothing says
how to assemble them. It is also small (a few hundred bytes for a five-layer document), which
made it the file's sharpest single point of failure: one flipped byte in ~0.07% of the file
rendered the document unopenable while every other frame verified. So `buildCheckpoint` writes
it **twice** — once in its ordinary content position, once after the parity chunks, hundreds of
kilobytes away, so no single burst of contiguous damage can take both. The reader needs no new
concept for this: two frames sharing `(TYPE, KEY, GENERATION)` are, by the format's own rule
that *generation is what versions a key*, the same logical chunk. Whichever verifies answers;
the other is dropped (never mistaken for retained history, §3.3). Losing one replica is not
damage and is not reported as such; losing every copy of a chunk is counted once, by identity.

**Both the root and the directory are accelerators, never the sole path in.** If every root replica
is destroyed, a full linear scan of the file (magic-resync, verify each chunk's checksum, resolve
ties by "highest generation wins" per `(TYPE, KEY)` identity) reconstructs the same information from the
self-describing chunks alone — this was tested directly (destroy all three replicas simultaneously,
full recovery still occurs) and is the direct descendant of the "the index is optional, the full
scan is ground truth" principle both EBML/Matroska (Cues) and PAR2 converge on independently.

The root payload carries (at minimum): `generation`, `document_uuid` (minted at document
creation, stored in the manifest and mirrored here so recovery-journal binding and lock naming
(§2.6, §2.10) never require parsing the manifest first), `base_dir_offset`, `wal_start_offset`
(where the committed append region begins — commit-appended Saves are replayed forward from here
on open, §2.8), `rs_params` (the Reed-Solomon stripe map, §2.7), and `mode` (which history
encoding wrote this file — `"journal"` for H2, `"cas"` for H4 once Build 2 lands, §3.9; this is
also what makes adaptive switching cheap — a reader just reads `mode` and dispatches, and a
checkpoint under a different mode is not a migration, just a different encoder run on the same
in-memory history).

### 2.4 Compression profiles

Four profiles, recorded per-chunk so a file can freely mix them chunk-by-chunk:

| Profile | Codec | Used for |
|---|---|---|
| `store` | none | incompressible content (store-if-larger fallback engages automatically) |
| `fast` | LZ4 | autosave path — near-zero CPU cost |
| `balanced` | zstd, level ≈3 | default explicit Save |
| `max` | zstd level ≈19, or Brotli | Export / idle-time background recompaction |

A **store-if-larger fallback** is mandatory: if compression made a chunk bigger (noisy/incompressible
content, or re-compressing an already-compressed inner stream), store it raw instead — mirrors
Krita's own per-tile fallback and is pure waste to skip.

**Why `fast` is LZ4, not zstd's own fastest level — measured, not assumed.** The obvious
simplification (one fewer third-party dependency) was tested directly on real, Paeth-filtered tiles,
correctness-gated before trusting any number:

| Codec | Compress | Decompress | Ratio |
|---|---:|---:|---:|
| LZ4 (chosen) | 38.0ms | 7.2ms | 1.430x |
| zstd level=1 | 69.3ms | 20.6ms | 2.378x |
| zstd level=-5 (fast mode) | 54.4ms | 19.4ms | 1.662x |

LZ4 is genuinely faster in both directions (1.4–1.8x faster to compress, 2.7–2.8x faster to
decompress than either zstd fast-tier candidate) at the cost of a meaningfully worse ratio. Given
this design's own stated priority order puts "fast saving, including autosave" above "good
compression" (§1), and decompress speed specifically matters for the journal-replay-on-reopen and
undo-walk paths (not just write speed), **LZ4 earns its keep as a distinct dependency** — this is
one of the few places in this document where testing an alternative confirmed the original choice
rather than overturning it.

### 2.5 Spatial filter (Paeth)

Before compression, each raster tile's raw RGBA bytes get PNG's Paeth spatial predictor applied
(per-pixel choice of left/up/upper-left neighbor, whichever the linear predictor `a+b-c` lands
closest to). Measured to close most of the compression gap with a naive one-PNG-per-layer baseline
— materially better than not filtering at all (~25-30% smaller on real photographic content), and
competitive with (slightly ahead of, at the `max` profile) plain PNG.

Paeth's **decode** direction has a genuine 2D sequential dependency (`recon[y,x]` needs
`recon[y,x-1]`, `recon[y-1,x]`, and `recon[y-1,x-1]` already reconstructed) — this is exactly why
naive Paeth decoders have a reputation for being slow/unvectorizable. **A real C++ implementation
should use a straightforward SIMD-vectorized decoder — this is a solved problem, libpng's own
decoder is not slow.** Do not port the research prototype's Python-specific "anti-diagonal
wavefront" numpy trick verbatim (see §8) — it exists because pure-Python/numpy has no other way to
vectorize a 2D sequential dependency; a compiled implementation has better tools available (the
research's own numba-JIT experiment, ~1.35ms for a 256×256 tile after warmup, is the evidence this
is not a performance risk at any realistic document size). Paeth's **encode** direction has no
sequential dependency at all (every pixel's neighbors are already-known raw values) and vectorizes
as trivially as any simpler filter would.

### 2.6 Write paths: commit-append Save, the full write, and the recovery journal

**File→Save — commit-append (the ordinary case; user decision + Round 12).** Append the states
accumulated since the last Save onto the file's committed region as **one batch**: each state's
dirty tiles/vectors (re-encoded once to `balanced` here — Save is human-initiated and can afford
zstd-3 on just the delta, §3.3), its `HIST` frame, and a closing **`CMIT` commit frame** carrying
the saved state id — all explicit-link frames (§2.2). The first batch after a full write seeds
its link from the checkpoint root's checksum; every later batch continues from the previous
batch's `CMIT` checksum, so the committed region is one verifiable chain from the root forward.
`fdatasync` the file, **then** reset the journal (rebound to the new `CMIT`'s checksum) — this
ordering is load-bearing: a crash *during* Save leaves a torn batch (discarded, atomic — a
batch's states apply only once its `CMIT` validates, Round 12 A1) plus a still-intact journal
holding the same states, so the crash window loses nothing (A2). Cost: O(changed since last
Save) — milliseconds, on the operation users hammer reflexively; Krita/GIMP/Photoshop all pay a
full rewrite here. **The pre-Save tail check (Round 13):** immediately before appending, the
writer re-verifies that the file still ends exactly where its in-memory state says — `st_size`,
inode identity, and one read of the last commit's checksum at the known offset, O(1). Any
mismatch (a foreign append, a truncation, another instance's compaction having replaced the
inode under a stale handle) makes the Save **refuse and surface a conflict** instead of writing
a batch that would be dead on arrival. Check-then-append is not atomic — the lock (§2.10) exists
for the true race — but this catches every non-racing violation, tested (D4a–d).
**Sync-capture caveat, stated honestly:** a cloud-sync client that uploads
mid-append ships a copy that opens at the *previous* commit — degraded, bounded, and self-healing
by construction, where the same event against a ZIP central directory is fatal. The local file is
never the torn one (the tear exists only in the copy), and a Save still produces exactly one
change-notification burst.

**The full write — Save As, the first save of an untitled document, and threshold compaction.**
Build a complete new file (current content at `balanced`/`max`, retained history by copy-through
§3.3, parity §2.7, fresh root/directory §2.3), fsync the file, atomically rename over the target,
then **fsync the parent directory** — without that last step, POSIX permits the rename itself to
be lost on power failure. `rename()`/`ReplaceFile` is a single metadata operation, so a watching
sync client never observes an intermediate state, and a crash mid-write leaves the previous good
file completely untouched. **Compaction is a Save, never a background job:** when a Save finds
the committed append region past a threshold (parity-debt driven — the appended region carries no
Reed-Solomon coverage until folded in — or when the retention budget requires eviction), *that*
Save performs the full write instead. Round 12: the threshold trips, parity coverage is restored
(a flipped tile reconstructs again), the journal rebinds to the new root. No user-facing knob —
Save is Save. And compaction's deliverable is **not space**: measured, the append-file is the
same size as the full rewrite it replaces (−0.9% in the test), because under retained history the
full save was mostly re-writing chunks it keeps anyway; what compaction actually buys is parity
refresh, budget eviction, and a fresh directory.

**Recovery journal — incremental autosave, in an app-owned file.** Append only the changed
tile/vector frames plus a small **linked** `HIST` commit frame (§3.2) to the journal, then
`fdatasync`. Cost is O(changed data), not O(document size). Specifics:

- **Location: the OS state directory** (Linux: `$XDG_STATE_HOME/mosaic/recovery/`, with the
  platform equivalents elsewhere), named by `document_uuid` plus a hash of the canonical document
  path — never a sidecar dropped next to the user's file. Rationale, in order: the §0 rule (no
  writes into user-visible space without permission), cloud-sync hygiene (synced folders don't
  churn with journal appends), **untitled documents get crash protection too** (a UUID exists
  before a path does — impossible under the old append-to-the-document design), and read-only or
  removable media still get autosave. The path-hash component means a *copied* document file
  (same UUID, different path) gets its own journal and lock rather than fighting over one.
  Crash recovery being machine-local is the accepted trade — that is what crash recovery is.
- **Binding.** The journal opens with a `JHDR` chunk recording `document_uuid`, the id of the
  **commit it extends**, and that commit's checksum — the checkpoint root's BLAKE3 after a full
  write, the newest `CMIT` frame's checksum after an ordinary appended Save — and, the
  load-bearing half, the frame chain's **link seed is that same checksum**. Round 11-tested at
  root granularity and Round 12-tested at commit granularity (A3): a journal left over from a
  *different* save of the file fails link verification at frame 0 — structurally unreplayable
  before any policy field is even consulted — and the `JHDR` fields reject it independently
  (belt and suspenders, both layers tested separately).
- **Frames are explicit-link** (§2.2, FLAGS bit 1): each frame's checksum stands alone; LINK
  carries the previous frame's checksum. A replaying reader verifies each frame *and* the link
  match, and by default **stops at the first invalid, incomplete, or link-mismatched frame** — no
  separate "was this torn?" flag anywhere, exactly the SQLite-WAL property, with the salvage
  upside §2.8 describes when damage lands mid-journal instead of at the tail.
- **Cadence — measured, and unconstrained by SSD wear (Round 12).** The journal is
  full-fidelity (every undo state's after-images are written — coalescing merges *fsyncs*, not
  bytes; Save re-encodes history from the in-memory session regardless), so cadence is purely a
  latency/CPU/battery question, and the flash-wear worry was measured away: a deliberately heavy
  painting hour (600 strokes, ~4,800 tile after-images at 64px) journals **~60MB logical at the
  LZ4 tier**; with the analytic ~32KB NAND/FS floor per sync, even *per-stroke* fsync (600/hr)
  totals ~79MB/hr physical — an 8-hour day is **~0.19% of a 600TBW consumer drive's per-day
  endurance budget**. Policy: autosave only when unsaved changes are pending; trigger on a few
  seconds of input-idle; a ceiling (~30–60s) forces a write during continuous activity; a
  minimum gap (~5–10s) coalesces bursts. Typically 1–6 journal writes/minute while painting,
  zero when idle or clean. Exact seconds are Build 1 tuning, not format.
- **Growth.** A save-averse user's journal grows with their unsaved session. That is
  acceptable — replay cost is O(journal), and the content is exactly the history they'd be
  keeping anyway — but the journal is app-owned, so the implementation *may* compact it (an
  atomic rewrite of the journal file itself, folding retention eviction, on a size threshold)
  without touching the user's file. Allowed, not required for Build 1. The *file's* committed
  append region grows with Saves instead, and the compaction threshold above bounds that.
- **Lifecycle.** Created lazily at the first autosave after an edit; **reset** (truncated and
  rebound to the newest commit) after every successful Save — append or full, and only *after*
  the Save's bytes are durable (the Round 12 A2 ordering); **deleted on clean close** —
  whether the user saved or deliberately discarded, a clean exit leaves no journal, so "close
  without saving" genuinely discards. A journal found at open time *is* the crash evidence:
  verify its binding against the file just opened and offer restoration (the ask/tell dialog,
  `docs/askortell-dialog.md`, is the intended consumer). Restored work arrives as unsaved
  in-memory state — dirty title per S18-d — and touches the user's file only when they Save.
  A journal whose binding matches nothing (the file was saved again elsewhere, or replaced) is
  reported and discarded with the user's consent, never silently replayed.

**Implementation-critical detail, found the hard way in the research (a real bug, not a
hypothetical risk):** link state must be **threaded through the live editing session in
memory** — and there are now *two* of them: the journal's running link (continued by every
autosave) and the file's committed-region link (the newest `CMIT` checksum, continued by every
appended Save). Neither may be re-derived by scanning on every call; deriving each by scanning is
a *one-time* cost, paid when a document is opened for editing this session. Getting this wrong
(re-seeding on every call) is invisible in any test that only writes once — it silently makes
every write *after the first* look torn to a reader and get discarded, a real, substantial
data-loss bug that only a many-sequential-writes stress test exposes (§7).

### 2.7 Reed-Solomon parity

A from-scratch GF(256) Cauchy-matrix systematic Reed-Solomon coder, `k=8, m=2` by default (~25%
redundancy, within PAR2's typical range), computed **only at checkpoint/full-save time, never
autosave** — this is real, non-trivial CPU cost, and "CPU is the currency" throughout this design's
save-path decisions. Striped over current-content **tile/vector** chunks in groups of `k` — not the
manifest, which is protected by replication instead (§2.3): a stripe pads every shard to its
longest member, so folding a manifest into one would inflate that whole stripe by the manifest's
size, and it would still only survive `m` losses. Because the
directory already identifies exactly *which* chunk is damaged, only the cheap **known-erasure-
position** decode path is ever needed — never a blind error locator, which would need roughly twice
the redundancy for the same correction capability. If more than `m` shards in a stripe are lost,
recovery correctly and honestly reports that stripe as unrecoverable — reconstruction is never
silently wrong, only ever exact-and-verified or explicitly declined.

At 64px tiles (§2.2), a full stripe loss (more than `m`=2 of the 10 shards in one `k`=8 group
damaged beyond repair — a severe, concentrated corruption event, not an isolated bit flip, which
parity already reconstructs for free) costs at most 32,768px (~181×181), versus 524,288px
(~724×724) at the prototype's original 256px tiles — a direct, quantified benefit of the smaller
tile size for exactly the corruption scenarios where recovery genuinely isn't 100% (§2.8).

**Padding waste, measured (Round 11).** Striping variable-size compressed chunks means every
shard in a stripe is padded to its longest member — a cost the research had flagged only for the
not-scheduled history-region parity, but which applies to this current-content parity just the
same. Measured on Round 10's own 2048×2048 real-photo canvas (zstd-balanced, Paeth-filtered,
framed shards): file-order striping costs **+30.3% over the `m/k` parity floor at 256px tiles,
+11.5% at 64px** — so the smaller tile genuinely mitigates it (more uniform shard sizes), and the
effective parity overhead ships at ~28% of data rather than the ideal 25%. Size-sorted stripe
assignment (the "size-bucketed striping" already listed as researched-but-not-scheduled, §6)
would cut the excess to +0.3%; the number is now on record so that decision is a measured one
when/if it's ever revisited.

### 2.8 Recovery algorithm

1. **Fast path (the checkpoint).** Find the best root (slot, else backward magic-resync, else
   full-scan — §2.3) → read the directory → for each entry, verify its checksum and decompress;
   anything that fails is recorded as lost. Reed-Solomon-reconstruct any lost entries whose
   stripe still has ≥`k` surviving shards (parity + data).
2. **Committed-region replay.** Replay commit-appended Save batches forward from
   `wal_start_offset`, **batch-transactionally**: verify each frame standalone plus its link
   continuity, and apply a batch's states only once its `CMIT` frame validates — a torn final
   Save (crash or a sync client's mid-append capture) yields exactly the previous commit, never
   a half-saved hybrid (Round 12, A1). This is part of ordinary opening, not just disaster
   recovery: it is how the newest saved content is reached.
3. **Journal replay** (only when a bound recovery journal exists — a crash, §2.6): verify the
   `JHDR` binding and the link seed against the newest commit just reached, then replay forward,
   transactionally per `HIST` frame, **stopping at the first invalid, incomplete, or
   link-mismatched frame**. Always offered to the user, never silent (§2.6 lifecycle).
4. **Full-scan fallback** (root/preamble also destroyed, or the fast path throws): a single linear
   MAGIC-resync scan across the whole file, resolving `(TYPE, KEY)` collisions by "highest
   generation wins," reconstructing manifest/preview/tiles/vectors directly from self-describing
   chunks with no index at all — **including commit-appended content** (Round 12, A5; that check
   also demonstrated, by briefly violating it, that §2.2's generation rule is what makes this
   resolution correct rather than coincidental).

**Salvage past a gap — a separate, explicitly-labeled mode, with honesty rules (Round 11-tested
in the journal, Round 12-tested in the file's committed region — same machinery, both
locations).** Stopping at the first bad frame is the conservative-correct default, and Round 11
proved *why* it must never be quietly relaxed: a naive scanner that skips whatever fails
and applies the rest returns a "successful" document with **silently wrong content** — the
damaged state's keys hold stale, pre-edit pixels while every later edit applies (check B2). But
the conservative stop also provably over-discards: one damaged mid-journal frame threw away 12
physically intact later states in the same test (B1), and one damaged frame in an old committed
batch cost every later commit (Round 12 A4a) until salvage recovered them with the lost state's
key flagged exactly (A4b). The explicit-link frame design (§2.2) is what reconciles the two —
every intact frame verifies independently, so damage localizes to a gap — under these rules:

- A state is salvageable **only if** its `HIST` frame and every chunk in its dirty set
  individually verify. Reconstruction is never accepted on faith.
- Skipping a damaged state and continuing is permitted **only with per-key staleness flags**: if
  the damaged state's `HIST` survives, its dirty list names exactly which keys now hold stale
  content (Round 11: flagged keys == mismatching keys, *exactly* — B4). If the `HIST` itself is
  destroyed, the dirty list is unknown and the salvager must degrade to **declared-imprecise** —
  flag what surviving frames reveal and say the flag set is a lower bound, never fake precision
  (B4b).
- **Salvage never blends lineages (Round 13).** Individually-valid frames are not enough —
  *whose chain they belong to* matters: a dual-writer file contains two internally-consistent
  chains, and a salvager that applies every verifiable state silently produces a document
  neither writer ever saw (demonstrated, D2 — the B2 hazard one level up). Salvage therefore
  partitions frames into **link-chains**: the primary is the chain rooted at the expected seed
  (identical to conservative replay's winner); every other chain is a **foreign lineage** —
  fully recovered, separately reported, never merged. In the frame-interleaved true-race case
  this recovers *both* writers' batches completely (D5b), surfaced as a root conflict for the
  user to resolve explicitly.
- Salvage is presented to the user as recovery-with-a-gap (which states, which keys/regions),
  through the same ask/tell dialog as ordinary journal restoration. The default answer remains
  the conservative stop.

Under the cumulative-chain design the research prototype used, none of this was possible for
structural damage: the chain-link value dies with the destroyed frame, and all 13 subsequent
intact frames were unverifiable *by construction* (B3b) — that measured dead-end is the reason
the journal moved to explicit-link frames rather than inheriting the prototype's chain verbatim.

Measured in the research: **84–88% of data recovered on average** across a battery of bit-flip,
large-corrupted-region, torn-write, crash-mid-write, destroyed-header, and truncation scenarios —
against ~17–18% for both a naive ZIP baseline and a "one compressed blob" baseline. A 129-check
adversarial edge-case sweep (degenerate/empty/truncated-at-every-3%/compound-corruption/excess-
erasure/all-replicas-destroyed inputs) found zero crashes and zero silent-wrong-data results once
the header-checksum-coverage requirement in §2.2 was actually implemented correctly. Round 11
extends the same standard to the journal: 16 further checks (binding, staleness, torn tails,
payload-level and structural mid-journal damage, destroyed commit frames, frame reordering), zero
silent-wrong-data results across both replay and salvage modes.

### 2.9 Versioning

Reserve one explicit top-level container/format-version field (the preamble byte, §2.1) with a
clear reject-if-newer-major-version policy on open — but let sub-formats version independently
rather than conflating everything into one number: the manifest's own JSON schema version, and (if
a spatial-filter or compression-profile encoding ever needs to change incompatibly) that encoding's
own version tag inside its chunk. This mirrors Krita's own three-independent-version-axes design
(container version, per-node schema convention, binary tile-codec version) rather than a single
global number that forces every sub-format to bump together. The recovery journal carries the
same container-version byte in its preamble; its `JHDR` payload is JSON and versions with the
manifest schema.

### 2.10 Concurrency: advisory per-document lock

Multi-writer coordination stays a non-goal (§1) — but *accidental double-open of the same
document by two Mosaic instances* is an ordinary user mistake, not a daemon architecture, and the
design owes it an answer. The document file itself is safe by construction (it is only ever
replaced atomically, never appended to), so the actual hazards are (a) two instances fighting
over one recovery journal and (b) last-Save-wins silently discarding the other instance's
session. The answer:

- An **advisory lock file** in the recovery directory (§2.6), keyed like the journal
  (`document_uuid` + path-hash) — **never a lock, attribute, or open handle held on the user's
  file itself** (§0 rule, and network-filesystem locking is exactly the swamp §1 declines to
  enter). It records PID, a boot/session identifier, and a heartbeat mtime.
- Second opener while the lock is fresh: open **read-only**, say why, offer explicit takeover
  (which flags the first instance's next Save to re-check, so a stolen lock degrades to a
  loud conflict rather than a silent overwrite).
- A stale lock (dead PID, boot-id mismatch, expired heartbeat) alongside a surviving journal *is*
  the crash-recovery signal (§2.6) — the two mechanisms share one directory and one identity
  scheme on purpose.

This is OS-integration behavior, verified at build time (§7) rather than in the Python harness —
listed here because the journal design creates the shared resource, so the lock belongs to the
format's contract, not to UI polish.

**The lock is the top of a tested defense-in-depth, not the load-bearing member (Round 13).**
Advisory locks can always be defeated — containers, kill −9, a deleted state dir — so the format
beneath must be safe on its own, and now measurably is: **prevention** (this lock), **detection**
(the pre-Save tail check, §2.6 — a non-racing foreign append or truncation refuses loudly instead
of writing a dead batch), **containment** (a second writer forks the explicit-link chain;
conservative replay follows exactly one deterministic lineage — the interloper's batch is dead
bytes, not poison, D1/D5a), and **recovery** (lineage-aware salvage recovers *both* writers'
batches separately, even from a frame-interleaved true race, and never blends them — §2.8,
D3/D5b). Two Mosaics saving into one file simultaneously can therefore cost, at absolute worst, a
loudly-reported conflict whose both sides are recoverable — never corruption, never silence,
never a merged document nobody made.

---

## 3. Undo/redo history persistence (linear)

### 3.1 What changed from the research, and why

The research built and fully tested a **tree-structured** history: undoing, then making a new edit,
creates a second branch off the same parent, and the abandoned branch survives (with its own,
separately-budgeted, time-boxed retention policy modeled on Git's reflog-expiry asymmetry) rather
than being silently destroyed. **This tree design is out of scope for the real implementation.**
Mosaic's undo/redo will be **linear**: a new edit after an undo permanently discards whatever was
undone past — exactly what Photoshop, GIMP, and Krita all do, and what users of any of them already
expect.

Concretely, this drops from scope (real, tested, just not needed):

- Branch-eviction logic and the specific crash-class bug the research found and fixed there (an
  eviction planner that assumed "newest N states by append order" is always a contiguous, walkable
  chain — true only for a linear history, and reachable through completely ordinary use: undo, then
  edit, then reopen).
- The asymmetric, Git-reflog-style retention policy for abandoned branches (a separate, smaller,
  time-boxed allowance) — there are no abandoned branches to retain.
- Tree-walking (`path_to` across divergence points, grouping nested branches with their outer
  branch's fate).

**Everything else carries over unchanged**, because the encoding scheme for *one state's own
content* is orthogonal to *how states relate to each other* — §3.2 through §3.7 below are the
research's H2 design, unmodified, just applied to a chain instead of a tree.

One direct, small benefit of this decision: whatever considerations attach to tree-structured undo
in general are now **structurally inapplicable** — there is no tree in the real implementation at
all. See §4.

### 3.2 HIST commit frames

A `HIST` chunk closes each undo state, carrying `{state, parent, op, params, dirty_keys,
manifest_snapshot}`. In the **recovery journal** it is a **linked** frame (§2.2, §2.6) written
immediately after that state's own dirty tile/vector frames — a state "exists," as far as any
reader is concerned, only once its `HIST` frame validates. This gives every undo state
transactional semantics for free: a torn autosave loses at most the one half-written state, never
leaves a half-applied one — and it is also what makes honest salvage possible (§2.8: the dirty
list is the authority on which keys a lost state touched). In a **checkpoint file**, `HIST`
chunks are ordinary unlinked chunks — the atomic rename (§2.6) is the transaction there, so no
frame-level chaining is needed or written.

With linear undo, `parent` is always literally `state − 1` — a real simplification versus the tree
design (where `parent` could be any earlier ancestor on any branch). **Tested, not just claimed:**
this invariant was checked directly, including across a "Flatten History" event specifically (the
one case where it wasn't obviously true — does the state right after a flatten still satisfy
`parent == state − 1`, given the flatten discarded everything before it?) — confirmed yes, because
state ids are monotonic and never reset (this project's own established convention, matching
`LayerId`'s), so the flattened floor state's own id is still exactly one less than the next live
edit's. A from-scratch rebuild of every `.parent` consumer (`path_to`, and the interactive undo/redo
model's `undo`/`redo`) using a **derived** `state − 1` instead of the **stored** field produced
identical results at every single state, both before and after a flatten. **Decision: keep the field
anyway** — it costs a few bytes, and buys a free redundant consistency check (a reader can flag a
corrupted-but-checksum-passing state whose stored `parent` disagrees with `state − 1`) at no
downside, even though it's provably safe to derive instead. Separately confirmed: "no history"
(§3.7's on/off question) is represented by the **absence** of the history table / any `HIST` chunks
at all, never by a sentinel value on `parent` — a document that's never had a single edit, or one
whose last checkpoint discarded everything, simply has zero retained states, which is exactly what a
reader already treats as "nothing to walk."

The **manifest snapshot rides inside every `HIST` frame**, not in a separately versioned structure —
so layer rename/add/reorder is undoable for free, without inventing a second versioning mechanism
just for document structure. (A real bug the research found and fixed: a full-scan disaster-recovery
path that only ever looked at `MFST` chunks — written once, at checkpoint time — silently reverted a
rename committed only through a live `HIST` frame. The fix, extended here too: full-scan recovery
must consider both `MFST` chunks and `HIST` frames' carried manifest snapshots, taking the
highest-generation one.)

### 3.3 H2 default encoding

Each state stores only its **dirty tiles' after-images** (one `TILE`/`VECT` chunk per key that
state touched, generation = that state's id) plus the `HIST` commit frame. This is exactly the
content the recovery journal (§2.6) already writes at autosave time — "keeping history" is simply
**carrying those states into the checkpoint** instead of discarding them at Save time, rather
than a separate mechanism. (Under the sidecar design the bytes are no longer literally shared
with the document file — the journal's `fast`-profile frames are re-encoded once, to `balanced`,
at the commit-append Save that first writes them into the file; the copy-through rules below.
The economics are unchanged: autosave already paid the expensive part, capturing and encoding
the dirty content.) The newest retained version of any given key is never re-stored — it *is*
the current checkpoint or committed chunk, referenced rather than duplicated.

Measured storage overhead in the research (over an equivalent build with history entirely
discarded): **+41% at 10 retained states, +149% at 40** — real, but bounded and directly tunable via
the retention budget below.

**Encode-once + copy-through (review round; updated for commit-append Save).** "Re-encode the
retained history at every full write" would make it cost grow without bound over a document's
life — recompressing tens of MB of accumulated history violates the spirit of goal 2 for no
benefit, since history chunks are immutable content with their compression profile recorded
per-chunk. The rule instead, across the two write paths (§2.6):

- **Each state's content is compressed at `balanced` exactly once, at the commit-append Save
  that first writes it into the file** (the journal's LZ4 copy was crash insurance, not the
  archival encoding) — so retained history stays zstd-shaped rather than silently degrading to
  LZ4 ratios, at a cost amortized to O(states new since the last Save) per Save.
- **The full write (Save As / compaction) re-encodes nothing:** checkpoint history chunks *and*
  committed batches' chunks are **verified, then copied byte-verbatim** into the new file.
  Verify-then-copy, not blind copy — silent corruption must route into the §2.8 recovery/parity
  path at Save time, never propagate into a fresh file.
- The **only** whole-history re-encode is an H2↔H4 mode switch (§3.9), which §3.9 already treats
  as the rare, hysteresis-guarded case.

Net cost: an ordinary Save is O(changed since last Save); a full write is O(current content)
compression plus sequential verify-copy I/O for the history — never O(session length) CPU
anywhere.

### 3.4 Checkpoint retention and budget

At full-write time — "checkpoint" throughout §3 means the full write of §2.6 (Save As or a
threshold-compaction Save), since eviction physically happens by *not copying* chunks into the
new file — decide which states to keep:

- **`budget = None` (default), backstopped by a generous safety-net cap** (several hundred MB) —
  **measured, not guessed.** A real, correctness-gated sweep (40-state sessions, two document sizes)
  found retention cost is essentially **document-size-independent**: it's set by how much content
  one edit actually touches, not by overall canvas size, which only affects the *floor* (no-history
  baseline) cost. At the 256px tiles the sweep itself used, a single-tile-edit state cost ~140–145KB
  consistently at both a 1024×768 and a 2048×1536 document. Rescaled to this format's actual 64px
  tile size using the measured per-tile compressed-size ratio between the two sizes (§2.1/§2.4),
  that's roughly **7–10KB per single-tile-edit retained state** — meaning even a 500-state retained
  history costs on the order of 4–5MB, and a genuinely long, heavily-edited session in the low
  thousands of states is still tens of MB. Unlimited retention isn't risky for any realistic
  session; the safety-net cap exists purely as cheap insurance against the pathological case of a
  document left open, continuously edited, for an extremely long, uninterrupted stretch.
  **One honest caveat (review round): the per-state cost is bimodal, and the numbers above are
  the localized-edit mode.** A whole-canvas operation — levels, a filter over the full layer, a
  resize — dirties *every* tile, so that one state's after-image costs roughly the full
  compressed document (tens of MB on a 36MP photo), not 7–10KB. The eviction behavior stays
  correct, but a session built around global adjustments reaches the undo floor after a handful
  of such states under the safety-net cap, not hundreds — and in Build 1 neither H4 (which needs
  content *reuse*, §3.9) nor the dropped H3 changes that. Read the backstop as "hundreds of MB of
  history," never "hundreds of undos guaranteed."
- **`budget = <bytes>`:** evict the oldest states first, walking backward from head until the byte
  budget is exhausted. Because undo is now strictly linear (§3.1), "the newest states that fit the
  budget" has **no branch-ambiguity question at all** — the exact class of bug the tree design needed
  a dedicated fix for (Bug 3 in the research) cannot occur here by construction, since there is only
  ever one chain to walk.
- The oldest kept state's content becomes the undo **floor** — older states are gone, and a
  further-back undo simply isn't offered.
- **Worth considering, not built or tested here:** a *state-count* cap (e.g. "keep the last 500
  undo states") as an alternative or companion to the byte budget — it maps more directly to a
  user's own mental model ("how many undos do I get") than a raw byte number, and given the measured
  per-state cost is fairly stable for a given tile size, converting between the two is
  straightforward. The underlying retention planner would need only a small addition (a state-count
  ceiling alongside the existing byte-budget one) — cheap, but a product-surface decision more than
  a format one, so it's flagged here rather than resolved.

### 3.5 The interactive undo/redo hot path

A whole-document resolve (`load_state`/`load_current`-style: re-walk every key in the document to
answer "what does state N look like") is the right tool for a **cold, reopen-time** operation — "I
just opened this file and want to jump to an old state" — and is *correct* there (measured:
proportional to document size, e.g. ~5ms for a tiny document vs. ~1 second for a 216-tile one, for
the identical single-state query). It is the **wrong** tool for the interactive path: a live
session's repeated undo/redo clicks should cost O(one edit), not O(document size).

The interactive model instead: seed once from the current state (the one unavoidable O(document)
cost, identical to what opening the file already pays), then step **one state at a time**, touching
only that one state's own dirtied keys.

- **Redo is the easy direction.** Moving forward into child state M applies exactly M's own dirty
  set, resolved via that state's own locator — M *is*, by definition, the state that produced that
  content.
- **Undo is where a real, non-obvious bug was found and fixed.** The naive approach — resolve "the
  value before state M" via M's *parent* state's own locator for that key — is wrong: the parent
  state almost never touched the *same* key M did (that's a different, unrelated edit), so the
  lookup is simply absent for the common case, and a plausible-looking fallback ("not tracked here →
  keep whatever's already materialized") silently turns every undo into a no-op. This was completely
  invisible when first tested against the fullest history variant (which coincidentally stores every
  key at every state, masking the bug entirely) and immediately broke every single step (26 for 26)
  the moment it was tested against a leaner variant instead.
- **The fix**: for each key, keep a small sorted list of exactly the states that touched *that* key
  (built once, from the file's own locator table), and binary-search it for the entry immediately
  before the state being undone — "what this key's own history says it held right before this edit,"
  not "whatever the parent state happens to have." Cost is O(log k) for that one key, never
  O(document).

Measured: this undo/redo step cost is **flat** across an 18x document-size range (~1.01x ratio)
versus the whole-document resolve's ~16.87x scaling for the identical range. **Build the real
implementation's interactive undo/redo on this shape from the start** — do not route ordinary
undo/redo clicks through a whole-document resolve, even though that resolve is the right, simple
tool for "jump to an arbitrary old state directly after reopening a file."

### 3.6 "Flatten History"

An on-demand action: discard every retained state, keep only current content, and relabel the one
surviving state (e.g. "Original," or a synthetic "Flattened" marker) so a history panel doesn't read
as empty or broken. This needs **no new format capability** — it is byte-for-byte what "retain no
history" already produces. Ship it as the storage-reclaim escape hatch alongside history-on-by-
default (§3.7).

(Naming note, for context only: this action deliberately avoids the more obvious name "Coalesce" —
an unrelated company holds an active trademark on that word in a different product category, and
"Flatten History" has the side benefit of reusing vocabulary users already know from "Flatten Image"
in Photoshop/GIMP/Krita. Included here only so the name doesn't look arbitrary later.)

### 3.7 History is on by default

No settings toggle. H2 (§3.3) makes retaining history free at the exact moment it would otherwise
cost something (autosave); the ongoing storage cost is bounded and tunable (§3.4) without the user
ever having to think about it unless they explicitly choose to via a retention setting. There is no
genuine trade-off here for a toggle to arbitrate.

### 3.8 Researched, but not in the build plan: H3 and history-region parity

- **H3, xor-delta compaction.** At checkpoint time, superseded versions get re-encoded as reverse
  XOR deltas against their successor, with a periodic full keyframe bounding any decode chain.
  Real and tested: **1–4% storage overhead vs. H2's 41–149%** — the best compression ratio of any
  variant researched. **Dropped from the build plan** (not merely deferred): H4 + adaptive switching
  (§3.9) already covers "long retained history costs too much" a different way, without a third
  encoding path, its own keyframe-interval tuning, or the standing implementation constraint that
  would apply if it were ever built (§4 — checkpoint-time, whole-tile only, never live/per-stroke/
  grid-subdivided). The design is fully validated and documented here for the record in case its specific
  trade-off (best-in-class ratio, at the cost of a quantified, bounded blast radius) is ever wanted —
  it is not scheduled.
- **History-region Reed-Solomon parity.** Extends §2.7's parity to history-region chunks too (not
  just current-content) — measured to close H4's shared-blob blast radius (a corrupted content-
  addressed blob takes out every logical reference to it, §3.9) entirely in the research's own tests.
  Real, non-trivial storage cost (~18% over an H4 baseline in the research's synthetic session). A
  real shard-size-heterogeneity padding-waste inefficiency was found but not fixed in the research
  (mixing a large full-tile entry with several small deltas in one Reed-Solomon stripe pads every
  small entry up to the large one's size). Not part of the build plan — worth reconsidering as
  opt-in once real documents' history-region content is measured, not before.
- **Branch/redo-abandonment retention policy.** Moot — no branches exist under linear undo (§3.1).

### 3.9 H4 content-addressing + adaptive H2↔H4 switching — in the build plan (Build 2)

Unlike H3, this is **not gated on data Mosaic doesn't have.** The whole point of the design is that
it never needs aggregate usage telemetry — it measures each document's *own* retained history,
locally, at checkpoint time, and decides per-document. No population-level statistic, and no
instrumentation of real users, is required at any point.

- **H4, content-addressed history.** Tiles/vectors stored once per unique content hash (BLAKE3);
  states reference hashes instead of offsets. Autosave cost is the same shape as H2's — O(edit
  size): a new hash is compared against the set already known this session, and only genuinely
  novel content gets appended. Measurably beats H2 on storage once a document's own editing churn
  (repeated undo/redo, reused content) crosses roughly 30% content-reuse in the research's synthetic
  sessions — reaching -59% vs. H2 at 90% reuse, and still ahead even under a tight retention budget.
- **Adaptive switching.** At each checkpoint, measure how much of *this document's own* retained
  history is duplicate content (a content-hash-reuse fraction) and pick whichever of H2/H4 is
  smaller for *this* file — with hysteresis (`switch_up=0.35` / `switch_down=0.15` in the research,
  deliberately asymmetric) so churn hovering near one cutoff doesn't thrash formats back and forth
  every checkpoint. Architecturally cheap: every checkpoint already rebuilds the entire retained
  history from an in-memory model regardless of which variant is active, so "switch formats" is just
  calling a different encoder on the next save — not a migration, and no format-version bump is
  needed (the root's `mode` field, §2.3, already records which encoding wrote a given checkpoint).
- **The one real design rule this needs** (found the hard way in the research): the churn-
  measurement window must match the retention horizon. Unbounded retention (§3.4, `budget=None`)
  needs a *whole-history* churn signal — "everything currently retained" and "the whole history" are
  the same set by construction, so a short recent window can miss that a long earlier high-churn
  stretch is still fully retained and still benefiting from H4's dedup (measured: picking the wrong
  format this way cost 17.2MB instead of 12.0MB in one test session). A *bounded* retention budget
  needs a window sized to roughly match that budget instead, since old high-churn content genuinely
  ages out under a budget and the signal should track that. Getting this wrong never corrupts
  anything — every checkpoint round-trips correctly regardless of mode — it just picks a needlessly
  bigger file.

---

## 4. Licensing posture and standing implementation constraints

**Licences.** zstd is BSD-3 / GPLv2 (it dropped its ambiguous "BSD+Patents" grant template in
2017); LZ4 is BSD-2; xxHash is BSD-2; BLAKE3 is CC0 / Apache-2.0 (taken under CC0). All are
GPLv3-compatible, and every vendored one has a row in `docs/third-party-licenses.md`. The
Reed-Solomon coder is written from scratch here, so nothing is vendored for it.

**Standing implementation constraints.** Each of these is a deliberate narrowing of the general
technique. They cost something, they are not oversights, and none of them may be relaxed casually:

- **⚠ Reed-Solomon is known-erasure-position decode ONLY** (§2.7) — never blind error correction.
  **Rateless / fountain codes (Raptor, RaptorQ) are excluded**, and nothing here uses them.
- **⚠ Undo history is LINEAR** (§3.1). There is no tree in the shipped design, and introducing one
  is a design change rather than an extension.
- **⚠ If H3 (XOR-delta compaction, §3.8) is ever built:** stay **checkpoint-time, whole-tile
  only**. Never add finer-than-tile, stroke-bounding-box-derived, grid-subdivided granularity to a
  live edit path. H3 is not in the build plan; this is recorded for whoever revisits it. (Its
  general shape — reverse-delta versioning with periodic keyframes — is RCS's, Tichy 1982, still in
  unmodified production use in Git packfiles, Mercurial revlogs, Perforce and ClearCase.)
- **⚠ H4 dedup chunk boundaries are FIXED and grid-determined** (§3.9) — whole, fixed-size tiles,
  with boundaries settled in advance by the document's own tiling grid and entirely independent of
  content. **Content-defined, variable-length chunking — a rolling-hash fingerprint over a sliding
  window picking boundary "landmarks" — is deliberately not used, for any reason.** The "where do
  we cut the stream" problem simply does not arise in H4's design, and that is the point. If a
  future revision ever wants variable-length chunk boundaries, it is a new design, not a tweak.

---

## 5. Reconciling with existing repo assumptions

PLAN.md §3.16 and the S48 entry (PLAN.md:2161) currently sketch a ZIP-based container ("like
ORA/KRA": mimetype-first-entry, `manifest.json`, tiled lossless blobs, forward-compatible
unknown-chunk preservation). **This document's container design (§2) supersedes that sketch on the
physical envelope specifically** — the dedicated research's core finding is that ZIP's central
directory (written last, the first thing lost on truncation, with zero built-in redundancy) is
exactly the wrong shape for a "no single point of failure" requirement, backed by both a real
Mozilla CI incident and Krita's own (also ZIP-based) `.kra` format's real corrupted-file bug
reports.

Everything else survives unchanged:

- **JSON stays.** Only the outer envelope moves from a ZIP wrapper to the chunk stream in §2 — the
  manifest itself is still JSON, `documentType` is still a manifest-level field exactly as
  PLAN.md:1555/1838 already plan, and every doc assuming a JSON manifest needs no revision.
- **Tiled storage, forward-compatible unknown-chunk preservation, embedded undo history, a small
  preview** — all explicit PLAN.md goals, all delivered directly by §2/§3, arguably more robustly
  than the ZIP sketch could have delivered them.
- **S18-d's dirty tracking gets a precise meaning, and stays truthful.** The just-shipped
  unsaved-state title (`CommandStack::isSaved`/`markSaved`, the saved-position marker) means
  "differs from the last explicit Save" — and under the recovery-journal rule (§0, §2.6) that is
  *literally* a statement about the user's file on disk, since nothing else ever writes it. A
  real `.mosaic` Save is what finally calls `markSaved()` (deferred from S18-d exactly for this).
  Crash recovery composes cleanly: restored journal work arrives in memory with the dirty marker
  set, title showing unsaved, file untouched until the user says Save.
- **`docs/le-d2-image-patterns.md` §4's `TilePool`** (a document-scoped `contentHash →
  shared_ptr<const Image>` map, deduplicated on save) is conceptually compatible — it's close to an
  in-memory precursor of H4's content-addressed design (§3.9). Its specific physical detail
  ("written once... as `tiles/<hash>.png`" inside a zip) is superseded the same way §2 supersedes
  the rest of the ZIP sketch: a deduplicated, content-hash-addressed tile fits this design fine, as
  a `BLOB` chunk keyed by hash rather than a zip entry path. `TilePool`'s in-memory dedup can ship
  against plain H2 tile chunks in Build 1 and adopt on-disk content-addressing when Build 2 (§3.9,
  §6) lands.

---

## 6. Build plan

A naming note before the breakdown: the research names its six *container methodologies*
`M1`–`M6` (§2's design is what it calls `M6` — `M1` there is the naive-ZIP baseline, not a build
step). To avoid colliding with that, the *build* breakdown below uses **Build 1 / Build 2**, not
`M1`/`M2`. Each build names a shippable, testable chunk of work — like this repo's own `S`-numbers —
not a single working session; split either across as many sessions as it takes.

**Build 1** — "Save works, reliably, with undo history that survives a reopen":

- The container (§2): preamble, chunk framing with structured keys and header-covering checksums
  (§2.2), replicated root + directory with slot overflow (§2.3), full-scan fallback, the write
  paths — **commit-append File→Save** (batch + `CMIT`, atomic, encode-once §3.3), the full write
  (Save As / first save / threshold compaction, with verify-then-copy-through), and the recovery
  journal with explicit-link frames and binding (§2.6) — compression profiles, Paeth filtering
  with a real SIMD decoder.
- Recovery flow (§2.8): journal restoration offer on reopen-after-crash (ask/tell dialog),
  conservative replay, and the salvage mode with its honesty rules; the advisory per-document
  lock (§2.10).
- Linear H2 history (§3.2–3.4, §3.6–3.7): `HIST` commit frames, retained-journal encoding, a
  retention budget, "Flatten History."
- The `LiveUndoModel`-style interactive undo/redo hot path (§3.5) — built from day one, not bolted
  on after shipping a whole-document-resolve version.

**Build 2** — H4 content-addressing + adaptive H2↔H4 switching (§3.9). A real, fully tested design,
not gated on data Mosaic doesn't collect — §3.9's churn signal is local and per-document, no
telemetry involved. Sequenced after Build 1 purely to keep the first real end-to-end slice small and
to let the container/H2 basics get a real shakeout before adding a second encoding path — a schedule
call, not a data-availability one.

**Not in the build plan** (real, tested, available if a concrete need arises — §3.8):

- H3 xor-delta compaction.
- History-region Reed-Solomon parity.
- Size-bucketed Reed-Solomon striping (closing the shard-heterogeneity padding waste flagged but not
  fixed in the research).
- The vector-only `documentType` exercised end-to-end (depends on S30-b, the vector document type
  itself, not on anything in this format).

Build 1 matches this repo's own convention (`docs/export-system-plan.md` §10) of a first milestone
sized to the smallest real, end-to-end, user-verifiable slice.

---

## 7. Verification plan (headless)

- **Chunk framing round-trip**: pack/scan, magic-resync after corruption at every offset, and
  specifically a test that a corrupted `KEY`/`GENERATION` field (not just payload) is caught —
  this is precisely the class of bug (§2.2) that a payload-only checksum test would miss entirely.
- **Corruption battery**: bit flips, large corrupted regions, torn writes, crash-mid-write,
  destroyed header/root (including all replicas at once), truncation at every N% of file length —
  assert a recovery-percentage floor and, more importantly, zero crashes and zero silent-wrong-data
  results, not just "some data recovered."
- **Journal chain**: a many-sequential-autosaves stress test specifically, not just one
  autosave — the research's own chain-reset bug (§2.6) is invisible to a single-autosave test by
  construction.
- **Journal binding & staleness** (Round 11's A-battery, re-run against the real implementation):
  round-trip over a checkpoint; torn tail yields exactly the last complete state; a journal bound
  to a *different* save of the file is rejected **both** structurally (link seed) and by the
  `JHDR` policy fields — test the two layers separately, they defend different failure modes;
  parity-repaired main file + journal replay compose. Plus an untitled-document journal
  (crash before the document ever had a path) restoring correctly.
- **Salvage honesty** (Round 11's B-battery): the naive apply-what-parses hazard must be a
  *regression test* — a suite that can't detect silently-wrong salvage output can't protect the
  honesty rules; payload-level vs structural frame damage; destroyed `HIST` forcing
  declared-imprecise mode; flagged-keys == mismatching-keys exactly in every precise case; torn
  tails and reordered frames still detected (no regression vs the cumulative chain).
- **Commit-append Save** (Round 12's A/B batteries, re-run against the real implementation):
  multi-Save round-trip; a torn Save opens at the *previous* commit (batch atomicity — never a
  half-saved state); torn Save + intact journal composes to zero loss (the reset-after-durable
  ordering); a pre-Save journal is structurally stale against the new commit; salvage works in
  the committed region with exact per-key flags; full-scan reconstructs appended files with no
  index — including a regression test for the generation-tie failure A5 caught (§2.2);
  threshold compaction restores parity coverage and rebinds the journal, and never fires
  outside a Save.
- **Encode-once / copy-through identity** (§3.3): a state's chunks are compressed at `balanced`
  exactly once (at the Save that commits them) and copied byte-verbatim by every later full
  write; consecutive full writes with no edits between them produce byte-identical history
  chunks; a corrupted history chunk is *caught at Save time* (verify-then-copy), not propagated.
- **Locking** (§2.10, OS-integration tests): second-instance read-only + takeover; stale-lock
  detection routing into crash recovery; lock never touches the user's file.
- **Dual-writer battery** (Round 13's D-checks, re-run against the real implementation):
  sequential second writer → deterministic winner, loser's batch inert; the pre-Save tail check
  fires on foreign append, truncation, and inode replacement (stale handle after a foreign
  compaction); frame-interleaved race → conservative replay at the last pre-race commit;
  lineage salvage recovers both sides separately; and the salvage-blending hazard (D2) as a
  regression test — a salvager that merges lineages must fail the suite.
- **Save durability**: the rename-then-parent-directory-fsync sequence (§2.6) — an integration
  check, hard to unit test, cheap to get wrong silently.
- **History**: a full multi-state session round-trip (checkpoint + autosave mix), undo-walk
  correctness checked at **every single state**, not just endpoints, against two independent
  resolution paths (the `LiveUndoModel` step-by-step path vs. a whole-document resolve) — the
  research's own live-undo bug was only caught because it was checked against a second, independent
  source of truth at every step. Budget-eviction behavior; "Flatten History" behavior.
- **Reed-Solomon**: excess-erasure (more damage than parity can fix) degrades honestly — reported as
  unrecoverable, never silently wrong; every reconstruction is independently re-verified (checksum +
  decompress) before being trusted, never accepted on faith.
- **General discipline, carried over directly**: test harder than the obvious happy path before
  trusting any design decision. Every real bug the source research found (six in the container, four
  more in history) came from adversarial/edge-case testing specifically — never from a first
  hand-written self-test alone. A production test suite for this format should budget real time for
  the same kind of adversarial pass, not just golden round-trip tests.

---

## 8. Notes for implementers — do not port these details verbatim from the prototype

The empirical research was validated in a Python prototype (`scripts/` in the standalone research
project, not part of this repo). Several of its concrete choices are prototype-scale conveniences,
not part of the design itself:

- **Chunk-id derivation — superseded, do not port.** The prototype derives a tile's `chunk_id` as
  `1000 + layer_id·100000 + ty·1000 + tx` (and a vector layer's as `900000 + layer_id`) — workable
  for a small Python test harness, silently colliding once a canvas has ≥1000 tiles along either
  axis (only 64,000px at the shipping 64px tile size) or `layer_id` outgrows its assumed digit
  range. The real format doesn't fix this formula, it **removes it**: the structured 16-byte KEY
  (§2.2) carries `layer_id`/`tx`/`ty` as literal fields sized to Mosaic's actual 64-bit `LayerId`
  and to arbitrarily large canvases, no arithmetic encoding anywhere. The prototype's forward
  formula *and* its inverses (`formats._store_payload`, Round 11's `invert_tile_cid`) are
  prototype-only artifacts — nothing in them should survive into C++.
- **The Paeth "wavefront" decoder** (§2.5) is a Python/numpy-specific vectorization trick for a
  language with no other way to parallelize a 2D sequential dependency. A C++ implementation should
  use an ordinary SIMD-vectorized Paeth decoder (libpng's own is the reference); do not attempt to
  literally port the anti-diagonal-diagonal-batch numpy code.

---

## 9. Open questions

Tile size, root-slot sizing, the retention-budget default, LZ4-vs-zstd for the fast tier, the
`HIST` `parent` field, and container-version-vs-manifest-schema independence were resolved by
Round 10; the review round resolved the last format-level one — **chunk-id derivation** is gone
as a question, replaced by the structured KEY design (§2.2, and §8 for why the prototype formula
must not survive). The save-semantics question the review surfaced was resolved by user decision
(§0: recovery journal; the user's file is written only by explicit Save).

**No format-level questions remain open.** Two product-surface flags ride along for whoever
builds the UI around Build 1 — neither blocks, nor changes, anything on disk:

- **State-count retention cap** (§3.4): "keep the last N undo states" alongside/instead of the
  byte budget — maps better to a user's mental model, trivially supported by the retention
  planner, purely a settings-surface decision.
- **Recovery and salvage prompt copy** (§2.6, §2.8): the ask/tell dialog
  (`docs/askortell-dialog.md`) was built with corruption recovery as an intended consumer; the
  flows exist in this spec (restore-journal offer, salvage-with-a-gap report, lock takeover), and
  what remains is wording and flow polish at build time.
