# Nine rounds of trying to break Mosaic's future file format

This document tells the story of a research project. It is not the spec.
The byte-level implementation plan — chunk layouts, offsets, milestones —
lives in a companion document, `docs/mosaic-native-format.md`, written in
parallel to this one. Think of the two as playing different roles: that one
is the RFC; this one is the report of what we actually did, what broke, and
why we ended up trusting the design we did.

The underlying work happened in a standalone research project outside the
Mosaic repository, over nine rounds, against a real working prototype — not
a whiteboard sketch. It set out to answer two questions PLAN.md's S48 item
asks of Mosaic's eventual native `.mosaic` format: how do we store a
document so that corruption, crashes, and bad luck can't quietly eat an
artist's work, and how do we make undo/redo survive a close-and-reopen
cycle, which — as we'll get into — almost nobody in this space actually
does.

## What a save file actually has to survive

The brief, in priority order: detect any corruption and recover from a lot
of it, with no single point of failure; save fast, including autosave, at a
cost that scales with how much changed rather than how big the document is;
and compress well, without making autosave pay a heavy CPU tax to do it.
Underneath those three: no licensing risk, tolerance for
case-insensitive filesystems, tolerance for cloud-sync clients that upload a
file mid-write, a `documentType` field so a future vector-only or
raster+vector document can be told apart at a glance, a small embedded
preview, and backward compatibility.

That list reads like a wish list until you look at what happens when
software doesn't take it seriously.

## Krita already shows you what happens when you don't

We didn't start from a blank page. Krita's `.kra` format was researched
directly from Krita 6.0.2.1's own source on disk, not from secondhand
writeups, and it's a genuinely useful cautionary tale.

A `.kra` file is a ZIP archive. It borrows the same trick ODF and EPUB use:
the very first entry is literally named `mimetype`, stored with zero
compression, so a file manager can identify the format by sniffing a fixed
offset without ever parsing the ZIP directory. Tiles are a fixed 64×64
pixels, each layer's pixel stream is a plain-text header followed by
self-delimited tile records, and each tile is compressed independently with
LZF — fast, no cross-tile dependency, with a raw fallback if compression
didn't actually help. It's a sensible design in a lot of ways.

But look at what it does for corruption: nothing, beyond what ZIP and zlib
already give it for free. A tile's decompressed length is checked against
its expected size — that's a length check, not a content check, so a bit
flip that doesn't change size sails through undetected. There's no resync
marker anywhere, so a single corrupted `compressedSize` field
desynchronizes every tile after it in that layer's stream. There is no
incremental save at all — every save rewrites the entire ZIP from scratch.
And Krita's own FAQ documents "the last few bytes missing" as a known
truncation failure mode, with a small pile of KDE bug reports
("File corrupted: not a valid krita file," "commission work corrupted") as
the receipts.

OpenRaster, Krita's deliberately simpler ZIP-based sibling, makes the same
foundational choice — one PNG per layer, no custom compression, maximum
interoperability, and the identical fragility.

Krita's one real strength, actually, is small and worth keeping regardless
of container choice: if one layer's data is missing or unreadable, Krita
degrades that layer to blank and keeps loading everything else, rather than
aborting the whole document. That's a good instinct. It's just not enough
on its own.

## Why ZIP itself was the wrong foundation to build on

The Krita/OpenRaster failure mode isn't really Krita's fault — it's baked
into ZIP itself. A ZIP file's authoritative index, the central directory, is
written *last*, at the very end of the archive. A conforming reader is
specified to find it by seeking backward from EOF, not by scanning forward
from the start. That means the one part of the file that tells you what's
in it is also the first thing a truncated write destroys — even when every
byte of actual entry data earlier in the file is completely intact.

This isn't hypothetical. Mozilla's own CI hit exactly this failure via a
partial S3 transfer (Bugzilla #1306189): engineers confirmed the truncated
file's leading bytes were byte-for-byte identical to the good file's, and it
still wouldn't open, because the one thing missing was the directory at the
tail. The reference Python `zipfile` implementation has no forward-scanning
fallback at all for a damaged central directory — it just raises
`BadZipFile`. Repair is left to a separate, lossy, opt-in tool (`zip -FF`),
which itself warns it can't fix entries with a bad checksum. And there is
exactly one central directory, exactly one end-of-central-directory record,
with no redundant copy specified anywhere.

The mimetype-first-entry trick is genuinely worth keeping — it's cheap,
proven, and orthogonal to everything else. But it only solves "what kind of
file is this." It does nothing for "can I get my work back."
**Recommendation, stated flatly: do not build `.mosaic` on ZIP.**

## Borrowing from people who've already solved pieces of this

Four other systems had already solved individual pieces of this problem,
each in a different domain, and each contributed one specific, transferable
idea.

**ZFS** checksums every block, but — critically — stores that checksum in
the *parent* block pointer, never alongside the data it protects. That
separation is what lets it build a self-validating tree instead of a single
point of failure: a corruption event can't destroy both the payload and the
check that would catch it in the same stroke. Sun found 152 silent
corruption events across 3,000 servers running hardware RAID over three
weeks — corruption disk-level checksums never caught, because they only
guard the medium, not phantom writes or driver bugs. ZFS never overwrites a
live block in place either; a write allocates new blocks and only commits a
new root ("uberblock") once everything beneath it is durable, so a crash
mid-write just orphans some unreferenced new bytes and leaves the old,
still-valid tree untouched. And it keeps a 128-slot ring of prior uberblock
generations, plus 2-3 replicated copies of metadata weighted by how
important it is. That last idea — a small ring of root generations, not
just current-and-previous, importance-weighted — turned out to be the single
most directly transferable piece of the whole ZFS design.

**SQLite** does *not* checksum its own pages by default (an opt-in shim,
`cksumvfs`, exists specifically to patch that gap) — a useful, humbling data
point that even a hugely mature, widely-deployed format can ship without
this. What we actually wanted from SQLite was its WAL (write-ahead log)
mode: instead of a rollback journal that writes every changed page twice,
WAL appends only the changed pages to a separate log, and — this is the
important part — each 24-byte frame header carries a checksum computed as a
running sum chained from the *previous* frame's checksum. A reader replaying
the log just stops at the first frame that doesn't chain correctly. There's
no separate "was this torn?" flag anywhere; the chain itself is the
detector. We took the chaining idea and explicitly rejected the other half
of WAL's design — its shared-memory `wal-index` coordination between
readers and writers, which SQLite's own documentation says is unsupported
over network filesystems. Mosaic doesn't have a multi-writer problem to
justify that cost; a strictly append-only log, no shared index, is the
safer analog for a naive cloud-sync client watching the file.

**MKV/EBML** (the Matroska video container) solves the "resync after
corruption" problem elegantly: its element IDs are variable-length integers
whose own leading bits declare their width, with degenerate all-0/all-1
patterns banned at every length. That makes a legal ID a narrow,
recognizable subset of the byte space, so a parser hitting garbage can walk
forward testing "does this look like a real ID" with a low false-positive
rate — and the spec calls this out by name as useful "for resynchronizing to
major structures in the event of data corruption or loss." MKV also treats
an incomplete trailing recording as a designed-for case rather than an
error, which is why OBS Studio users report a crash-mid-recording MKV file
can usually just be remuxed back into something playable, in explicit
contrast to MP4, where an interrupted recording's end-of-file index makes it
"unusable, irrecoverable garbage" (a real quote from OBS's own forum). And
its Cues (seek index) element is explicitly never load-bearing — a reader
can always fall back to a full linear scan of the content, and a tool like
`mkclean` can rebuild Cues from scratch without touching the actual video
data. That's exactly the shape we wanted for our own directory chunk: an
accelerator, never a dependency.

**PNG** contributed two ideas and one honest warning. The two ideas: a
chunk's 4-byte type code uses letter case itself to signal
critical-vs-ancillary, so a decoder that has never seen a given chunk type
before can still decide whether it's safe to skip purely from
capitalization, no registry lookup required — and each chunk's own CRC-32
covers only that chunk's own data, so a corrupted comment chunk leaves
`IHDR`/`IDAT`/`IEND` independently verifiable, instead of one bad byte
invalidating a whole-file checksum. The warning: PNG's chunk length field has
**no independent way to verify itself** before a reader trusts it — flip
that one 4-byte field and a reader lands mid-chunk and cascades into garbage
for the rest of the file. Real PNG recovery tools work around this by
brute-forcing plausible resyncs against the CRC rather than trusting the
length field outright. That's precisely the gap EBML's self-synchronizing
IDs close, and it's why our own chunk framing leans on both ideas at once.

**PAR2** (the classic `.par2` parity-file format) gave us the actual
recovery mechanism, not just detection. It splits data into blocks and
computes Reed-Solomon parity blocks over them; any *k* of the total *n*
blocks reconstruct everything. The useful detail: this is a *systematic,
known-erasure-position* code — because Mosaic's own chunk directory already
tracks which chunk lives where, a damaged region is always a known-position
erasure, not a mystery to be located, and a code that only has to fix known
erasures is meaningfully cheaper than one that also has to find unknown
ones. The foundational 1960 Reed-Solomon algorithm itself is public domain,
with free implementations everywhere (the Linux kernel's own RS library,
QR-code error correction). The newer rateless "fountain" codes in the same
family — Raptor, RaptorQ — carry a licensing regime this project wants no
part of. We hand-rolled a from-scratch GF(256) Reed-Solomon coder
specifically to stay in the classical half of that landscape and nowhere
near the other half.

One more cross-cutting thread, less about file formats and more about
where files actually live: consumer cloud-sync clients (Dropbox, OneDrive)
watch for filesystem change notifications and start uploading the instant
they see a write begin, with no way to know an app intends to write more
bytes — Dropbox's own documentation acknowledges "conflicted copy" as a
known consequence specifically for apps with autosave. `rename()` is a
single atomic filesystem metadata operation, so the old, complete file stays
visible right up until the instant it's replaced — a sync client watching
for changes never sees a half-written intermediate state. That's the
justification for write-temp-then-rename as the only sync-safe way to
publish a full rewrite. Plain appending doesn't fully dodge this either —
appending still touches mtime and size, so a sync client can still catch and
upload a torn trailing record; only a self-checksummed, self-delimited
append format (the WAL idea again) makes that recoverable. And
case-insensitive filesystems are a genuinely separate hazard: APFS is
case-insensitive-but-preserving by default, Win32 defaults to
case-insensitive too, and name-collision bugs in ZIP-like archives on such
filesystems are a real, documented vulnerability class (USENIX FAST'23 cites
Git's own CVE-2021-21300 RCE as an instance). The clean fix is structural,
not defensive: address every chunk by a numeric ID and file offset, never by
a string name, and the whole bug class simply doesn't apply.

## Six ways to build the container, forced to fight each other

All of that converged into one design — call it M6 — and five other points
in the design space, built as an ablation so we could measure what each
property actually buys rather than just asserting the fancy one wins.

| | Chunking | Replicated root | WAL chaining | Parity | Full-save write |
|---|---|---|---|---|---|
| **M1** naive-zip | no | no | no | no | in-place (à la `.kra`) |
| **M2** monolithic-blob | no | no | no | no | in-place ("what not to do") |
| **M3** chunk-stream | yes | no | no | no | temp+rename |
| **M4** replicated-root | yes | yes | no | no | temp+rename |
| **M5** wal-append | yes | no | yes | no | temp+rename |
| **M6** hybrid | yes | yes | yes | yes (checkpoint-only) | temp+rename |

M6, concretely: a fixed 16-byte preamble at byte 0 (magic bytes, format
version, document-type code) for instant sniffing, purely a convenience —
losing it never blocks recovery. Every chunk after that is self-describing:
an 8-byte magic sentinel (`MoCk1223` in the prototype), then type, flags,
compression profile, a numeric chunk ID, a generation number, and two length
fields, followed by the payload and a checksum — 30 bytes of header, plus an
8-byte fast checksum (xxh3-64) for ordinary chunks or a 32-byte strong one
(BLAKE3) for the root/directory structure specifically, the same
importance-weighting idea ZFS uses. Root/directory chunks are written
redundantly — once near the start of the file, and twice more at the end,
so even the "here's the last commit" marker itself has its own redundancy —
and if every single replica is destroyed at once, a full linear scan
finding every chunk by its magic bytes reconstructs the same information,
just slower. Two save paths exist side by side: a full/checkpoint save
writes a complete new file to a temp path and atomically renames it over
the target, and an incremental autosave appends only the changed tiles as a
chained-checksum WAL frame, so a reader just stops at the first
invalid-or-incomplete frame. Reed-Solomon parity (in the prototype, 8 data
shards to 2 parity shards, roughly 25% redundancy) is computed only at
checkpoint time, never autosave, because it's real CPU cost. Chunks carry
one of four compression profiles — none, fast (lz4), balanced (zstd-3), or
max (zstd-19) — recorded per chunk, so a file can freely mix a fast autosave
tile next to a max-compressed one from an earlier explicit save.

Everything is addressed by numeric chunk ID and byte offset. There is no
string filename anywhere inside the container, which is what makes the
whole case-insensitive-filesystem hazard structurally inapplicable rather
than merely avoided.

## Two real bugs, found by testing harder than the obvious happy path

The brief said to test for everything, "no matter how insignificant." Taken
literally — rerunning the same six named corruption scenarios with more
trials, rather than inventing new ones — that discipline surfaced two real
design bugs neither Round 1's single-corruption-event testing, nor a
casual read of the design, would ever have caught.

**The first was serious, and it's the kind of bug this whole project cared
most about avoiding: silent misattribution.** Every chunk's checksum
covered its payload, but not its own header fields — `chunk_id`,
`generation`, `profile`, `chunk_type`. A bit flip isolated to just one of
those fields left the payload, and the payload's own checksum, completely
untouched — so the chunk verified as perfectly valid, while the correct,
checksum-passing pixel data inside it got filed under the wrong logical
tile entirely. We didn't just reason about this — we reproduced it. A
single flipped bit in one autosave frame's `chunk_id` field caused that
frame's correct pixel edit to silently overwrite a **neighboring tile**
instead of the one the artist had actually painted. No error, anywhere.
The fix was to make the checksum cover the header fields too, not just the
payload. Re-running the identical attack afterward, the intended edit is
honestly lost — matching plain WAL semantics, an invalid frame just doesn't
apply — but the neighboring tile is untouched and keeps its own original
content. Losing your own unsaved edit to bad luck is the correct, expected
outcome. Corrupting someone else's tile instead is not, and that's the
difference the fix actually buys.

**The second was a data-loss bug hiding behind a test that was too
gentle.** The WAL chain-building code started its running checksum from a
fixed salt value on *every call*, instead of continuing from wherever the
previous autosave's chain had left off. A test that only autosaves once —
which is all Round 1 ever did — can never expose this; it only shows up
once a document has been autosaved more than once since its last
checkpoint. We built a 40-sequential-autosave stress test specifically to
push past that blind spot, and it failed cleanly: only the very first
autosave's edits validated. Every one of the other 39 looked "torn" to a
reader, because each one's first frame was supposed to chain from the
previous batch's last checksum but instead restarted from the salt — and
was silently discarded. That's 39 out of 40 real, recoverable edits lost to
a bug that a single-autosave test would never see. The fix threads the
chain state across calls explicitly, so a real editing session carries it
through memory at a cost proportional to changed data, not document size —
deriving it fresh by scanning the file is only ever paid once, the first
time a file is opened for editing that session.

A third, smaller bug is worth naming for what it says about test hygiene
rather than the format itself: the corruption-scenario random number
generator was seeded from Python's built-in `hash()` on strings, which is
randomized per process as a security feature. Every run of the test suite
was silently drawing a *different* random corruption severity for
"the same" scenario, so headline recovery percentages wobbled between runs
of a test suite that was supposed to be deterministic. A plain CRC-32-based
seed fixed it — confirmed by running the whole battery twice and diffing;
recovered-data percentages are now byte-identical across runs.

After all three fixes, a much broader adversarial sweep — 129 checks in
total, everything from empty and truncated files at every 3% cut point, to
compound bit-flip-plus-truncation fuzzing, to destroying all three root
replicas simultaneously, to a 40-autosave stress test with the chain
actually threaded through real disk writes — passed clean across all six
methodologies. The one hard rule every check enforced: nothing may ever
crash the recovery path. Corrupted or nonsensical input has to degrade
gracefully to "recovered less," never an exception.

The final numbers, once the fixes were in: M1 (naive ZIP) and M2 (a single
compressed blob) recover an average of 17-18% of data across the six
corruption scenarios. M3 through M5 — bare chunking, plus replication, plus
WAL chaining, each added on its own — all land around 84.5%, a useful result
in itself: simple chunking alone already captures most of the achievable
resiliency, and the fancier properties each add a real but modest increment
on top. M6, everything combined, reaches 88.3%, with parity giving it a
clear edge specifically on the scenarios that damage but don't fully destroy
data (bit flips, large corrupted regions, truncation).

## The compression saga

Here's the part we could have quietly skipped past and didn't: the brief
asked for compression that would "blow away everything," and the first
honest measurement said the opposite. **Fired directly at raw RGBA tile
bytes, our zstd-based approach came out 33.6% *larger* than the simplest
possible baseline — one plain, ordinary PNG per layer.** Zstd alone doesn't
exploit the spatial correlation between neighboring pixels the way PNG's own
per-scanline filtering does; you're compressing noise-shaped data with a
general-purpose compressor and getting a general-purpose result.

The fix is one of PNG's own oldest tricks: apply a spatial predictor before
compression, so you're compressing small, mostly-flat *differences* between
neighboring pixels instead of the raw values. We tried this properly, with
every transform verified byte-exact round-trip before trusting any size
number. "Sub" (subtract the pixel to the left) got us to roughly 1.93-2.10x
compression depending on compressor level, up from 1.61-1.75x with no
filter at all. "Paeth" (pick whichever of left/up/upper-left the linear
predictor points closest to, PNG's own best all-around filter) did better
still — about 2.05-2.28x. A true PNG-style *adaptive* filter, choosing the
best of five options per scanline, barely improved on fixed Paeth for this
photographic content (+0.2%) — not worth its complexity here, though it
might matter more for a document mixing flat vector fills with photo tiles.

So why didn't we just ship Paeth immediately? Because Paeth's *decode*
direction has a real, structural problem Sub doesn't: reconstructing pixel
`(y, x)` needs its left, up, and upper-left neighbors *already
reconstructed* — a genuine two-dimensional sequential dependency. Sub's
decode is just a cumulative sum, trivially vectorized in one numpy call.
Paeth's naive decode is a nested Python loop over every single pixel, and
that's slow enough to meaningfully hurt every recovery-path benchmark in the
whole project. So the first, honest engineering call was: ship Sub, not
Paeth, in the actual container — Paeth compresses better, but Sub decodes
fast, and decode speed is what the recovery path actually needs. Wiring Sub
in dropped the shared "large" benchmark document's full-save size by about
29% versus no filtering at all — a real, substantial, verified-lossless
win, even while conceding the extra 5-6% Paeth would have bought.

We came back for that 5-6% later, and the way we got it is worth telling on
its own. The insight that unlocks Paeth's decode: look at *which* diagonal
each of those three needed neighbors lives on. If you define a diagonal as
every pixel where `y + x` equals some constant `d`, then the left and up
neighbors both live on diagonal `d - 1`, and the upper-left neighbor lives
on `d - 2` — strictly earlier than `d`, always. Every pixel on the *same*
diagonal is therefore mutually independent of every other pixel on that
diagonal, and can all be reconstructed in one vectorized step. The only
thing that has to stay a sequential, one-at-a-time loop is the *count* of
diagonals — for a 256×256 tile, that's 511 steps, not 65,536 pixels. That
one reframing turned an roughly-920-millisecond-per-tile naive Python
decoder into about 30 milliseconds — later measured more precisely at
1.9 seconds for 48 real photo tiles versus an extrapolated 65.5 seconds for
the naive version, a 34.5x speedup — all verified byte-exact against the
naive reference first, at the real tile size and at a battery of degenerate
and non-square shapes no real tile in this format will ever actually be, on
the theory that a shape bug shouldn't get to hide behind the one dimension
that's always tested. With that decoder in hand, Paeth replaced Sub
everywhere — every one of the six container methodologies, every one of the
four history variants — and both correctness gates (the 129-check
adversarial sweep, and the history suite described later) were rerun in
full and stayed green. The swap changes speed and compressed size. It never
changes correctness.

Here's the part that's easy to leave out and shouldn't be: **making Paeth's
decode fast didn't make Paeth free.** Its *encode* direction — which runs on
every single save, not just recovery — also costs more than Sub's, because
it has three neighbor lookups and a per-pixel nearest-of-three-distances
choice instead of one subtraction. Re-measured cleanly, this Python
prototype's full-save time went up 56-75% across the container
methodologies, and incremental-autosave time went up roughly 70%, purely
from the switch. That's a real cost, honestly reported, not glossed over —
and it's still the right call, for three reasons. First, it changes nothing
about which methodology wins or by how much. Second, the absolute numbers
are still small in real terms: tens of milliseconds for an incremental
autosave, roughly a second and a half for a full checkpoint of a large,
multi-layer document. Third, and most importantly, this slowdown is a
Python-interpreter-overhead artifact of this specific prototype, not a
property of Paeth versus Sub that a real C++ implementation would inherit —
Paeth's encode direction has no sequential dependency at all (every pixel's
neighbors are already-known raw values), so a compiled implementation
vectorizes it exactly as trivially as Sub, with three neighbor reads
instead of one. As a further, deliberately-not-shipped data point: a
JIT-compiled true per-pixel decoder (built in a fully separate, throwaway
Python environment specifically so it would never disturb this project's
own pinned dependencies) hit about 1.35 milliseconds per tile after warmup
— a further ~25x faster than the vectorized numpy version, and reassuring
confirmation that a real compiled decoder has plenty of headroom, not a
looming risk.

Where this landed, honestly: this prototype does not yet "blow away" a
30-year-old lossless image format on compression ratio alone at its
default, everyday profile — it's competitive-to-slightly-behind plain PNG
at the balanced setting, and would actually beat it at the max setting with
a properly fast Paeth decoder, which is exactly the follow-up recommendation
for the real implementation. Where the design decisively wins over
PNG-shaped formats like `.kra` or `.ora` is everything the container section
above already established: real corruption resilience, and autosave that
costs the same whether your document is 10 megabytes or 10 gigabytes.

## Where the container landed

M6 won, and kept winning after every fix. Weighting most heavily on data
recovered, then recovery speed, then save speed, then compression size, the
final composite ranking (with Paeth wired in) puts M6 clearly ahead of the
tight M3/M4/M5 cluster, which in turn is worlds ahead of the M1/M2
baselines: M6 dominates the highest-weighted dimension (most data
recovered across the corruption battery) while paying a real but
moderate cost on the two lower-weighted ones — its ~25% Reed-Solomon
overhead visibly costs it the worst compression score of any methodology,
and that's an accepted, deliberate trade for a mechanism that can actually
*reconstruct* fully destroyed bytes rather than merely flag them as gone.

## Does anyone actually keep undo history around?

The container design answers *how to store the document*. It says nothing
about the other half of PLAN.md's S48 requirement: round-tripping *history*
itself, not just current content. Before designing anything there, the
right first move was empirical, not architectural: does any shipping tool
actually persist a real undo/redo stack across a close-and-reopen cycle, or
is this uncharted territory for a good reason?

**The raster-editor world's answer is a clean, direct no — and its
maintainers say exactly why.** A Krita feature request asking for precisely
this draws a direct answer from the project itself: undo operations "are
made to the images permanently, they are not saved in terms of textual
instructions" — undo is pixel-snapshot-based, so persisting the stack would
mean the file balloons by the full cost of every retained snapshot.
Photoshop, Procreate, and Affinity Photo follow the identical pattern,
industry-wide, for the same reason: undo state lives in memory for the
session and gets thrown away on close.

**Vim's persistent undo is the one real, mainstream counter-example, and
it's directly relevant, feature by feature.** Turn on `'undofile'` and Vim
writes a per-file undo tree to disk on every write, transparently restoring
it the next time that file opens. It hashes the live file's contents and
compares that hash to one stored in the undo file — a mismatch (someone
edited the file with a different tool) makes Vim quietly ignore the stale
undo file rather than apply history to content it no longer describes. It
uses a magic number and version field, and — this detail matters a lot for
what follows — it stores history as a **tree**, with sequence numbers, not a
flat list, specifically so a redo branch survives an edit made after an
undo. But there's one structural choice we deliberately didn't copy: Vim's
undo file is a *separate sidecar*, mapped to its source file by path — which
is exactly the fragility class the cloud-sync and case-insensitive
filesystem research above already rules out. A sidecar can drift out of
sync with, or simply get lost separately from, the file it describes
(that's *why* Vim's content-hash check exists at all — this genuinely
happens in practice). Keeping history inside the very same container this
document already settled on eliminates that whole failure class by
construction: there's no second file to go stale or be silently skipped by
a backup tool that only knows about the primary one.

**macOS document versioning is real, and worth explicitly rejecting as a
substitute, not adopting.** Every APFS/HFS+ volume keeps prior document
versions in a hidden folder, indexed by a small local SQLite database, with
no data actually copied for unchanged regions (APFS's copy-on-write means a
version record is just a reference to changed blocks). But it's
macOS/APFS-only — no help on Linux or Windows — it doesn't travel with the
file (copy it to another machine, or upload it to cloud storage, and the
version history stays behind on the source volume), and it has no
visibility into *why* something changed, only that bytes changed. Mosaic
gets whatever the OS happens to provide either way; it just shouldn't be
relied on to provide the feature.

**SQLite's session/changeset extension validates the whole idea as sound,
shipped engineering — and sharpens one specific design choice.** It records
row-level changes as invertible changesets; inverting a changeset undoes the
original edit. The detail worth noticing: an UPDATE's changeset stores
*both* the old and new values, so inversion needs nothing beyond that one
changeset — no chain of prior states to consult. That's a genuinely
different design point from what we ultimately built (a chain of
after-image snapshots per key, where "the value before this edit" just
means "the prior entry in this key's own chain") — cheaper to write, at the
cost of needing the whole chain, unless it gets re-encoded later. That
lazy, later re-encoding is exactly what our own xor-compaction variant
(described below) does — arriving at something resembling SQLite's
diff-based model, just computed at compaction time instead of eagerly on
every edit.

**Git's reflog gave us a retention idea we didn't fully build, but flagged
clearly for later.** Git keeps two different expiry clocks: reflog entries
still reachable from a live branch get a generous 90-day default grace
period, while entries that have become unreachable — the tip of a deleted
branch, commits orphaned by a rebase — expire in 30. The asymmetry is the
interesting part: Git treats "reachable from where you are now" and
"reachable from somewhere you used to be" very differently.

**Blender's Global Undo is real-world validation that "just keep full
copies" is a legitimate choice — with an important asterisk.** Blender's
default undo system keeps a full copy of scene data per undo step in
memory, bounded by a step count and a memory limit the user can tune
directly. That's a real, shipped, widely-used piece of software making
exactly the "snapshot everything" choice our own naive baseline models. The
asterisk: Blender's memfile undo never has to leave RAM. It's discarded on
file close and never serialized to disk on every step. The moment
persistence *across sessions* is required — which is Mosaic's actual
requirement — "just keep full copies" stops being nearly free and starts
costing what we go on to measure directly below.

**A wider competitive survey — Clip Studio Paint, Corel Painter, Rebelle,
ArtRage — pressure-tested both claims above rather than just repeating
them**, researched by document and community search since none of these
have the kind of source access Krita's local checkout gave us (a real,
honestly-carried caveat: this is a search-limited "nothing found," not
source-level certainty, the way the Krita findings are).

Clip Studio Paint's `.clip` format is the best-documented of the four via
community reverse-engineering: a custom chunk wrapper around a genuine
embedded SQLite database — queryable directly, with tables like `Canvas`
and `CanvasPreview`. The Clip Studio community's own summary of this,
worth quoting directly, is that it's **"just a SQLite database with extra
steps."** No checksum field is documented anywhere in its chunk header.
CELSYS's own mitigation is entirely app-level — rotating backup folders,
plus a **paid professional file-repair service** — manual, human recovery,
not automated tooling. No evidence anywhere of undo persisting across
sessions.

Corel Painter's `.riff`-lineage format has thin public documentation, but
one specific feature was worth checking carefully rather than dismissing by
name: **Auto Playback**, a brushstroke/script recording feature that sounds,
on its name alone, like it could be persistent undo history. It isn't. It's
a separate recorded-macro system — it records a stroke or action sequence to
a reusable library object, replayable later, optionally on completely
different artwork — and Painter's actual undo is confirmed session-scoped
("the Undo control does not undo opening and closing files"). A genuine,
name-similarity red herring, checked and ruled out rather than assumed.

Rebelle turned out to be confirmed plain ZIP — `.reb` files rename directly
to `.zip`, one PNG per layer — the same central-directory fragility already
established for Krita and OpenRaster, with no checksum or redundancy
mechanism found anywhere. Its undo is a roughly 30-step linear stack with
no timeline view at all, and there's a live user forum request asking for
exactly that feature, which is itself evidence it doesn't exist yet.

ArtRage's `.ptg` format is fully opaque, no parser or spec found anywhere,
and its only mitigation is generational `.ptgback` backup rotation. Its own
support FAQ states plainly: it **"usually cannot fix corrupted or partial
files"** — a maintainer's own admission of essentially zero self-repair
capability, weaker even than Krita's.

Nothing in that survey beats or matches the container design above, and
nothing in it — outside Vim's sidecar-file precedent — persists undo across
a close-and-reopen cycle. Both claims held up under pressure rather than
collapsing.

## Building history persistence on top of the container

Every thread above converges on the same shape: keep history inside the
same self-describing, checksummed chunk stream the container already uses
— never a sidecar — as a tree of per-key after-image snapshots, closed off
by a small commit record per state, with a retention policy so an unbounded
editing session doesn't grow the file forever.

Concretely, a new `HIST` chunk closes each history state, carrying the
state's own ID, its parent's ID, what operation produced it, which keys it
touched, and — this detail matters — a full snapshot of the document's
*manifest* (the layer list, names, ordering) at that exact state. Riding the
manifest inside every `HIST` frame, rather than inventing a second
versioning mechanism just for document structure, means a layer rename, add,
or reorder becomes undoable for free.

The parent pointers are what make this a **tree**, deliberately, Vim-style:
undo to an earlier state, then make a new edit, and the states that used to
come after the point you undid to don't get destroyed just because you
stopped visiting them — they become an abandoned-but-recoverable branch,
hanging off the same parent.

Four variants were built and measured against each other, an ablation in
the same spirit as the container's own M1-through-M6:

| | Autosave cost | Checkpoint cost | Overhead at 10 states | Overhead at 40 states |
|---|---|---|---:|---:|
| **H1** snapshot-journal | whole document | whole document × every state | +747% | +3016% |
| **H2** retained-journal | one edit's worth | size of retained history | +41% | +149% |
| **H3** xor-compaction | same as H2 | history size + compaction CPU | +1.3% | +3.5% |
| **H4** content-addressed | one edit's worth | size of unique content | +41% | +109% |

H1 stores the entire document again at every kept state — Blender's Global
Undo idea, persisted to disk instead of kept in RAM — and the measured cost
confirms exactly why nobody does that once persistence across sessions
enters the picture: over 3000% storage overhead at 40 retained states, and
a 40-state checkpoint that takes over 20 *seconds* on a realistic document.
H2 stores only each state's dirty tiles as after-images, plus one small
commit frame — which is, byte for byte, already what the container's own
incremental autosave writes, so "keeping history" costs autosave nothing
new at all; the cost only shows up in what gets *kept* at checkpoint time.
H3 is identical to H2 at autosave time, but re-encodes superseded versions
as reverse XOR deltas against their successor at checkpoint time, bounded
by a periodic full keyframe (every 8 states, in the prototype) so no single
undo walk has to decode an unbounded chain — this is the SQLite-changeset
diff idea, applied lazily instead of eagerly. H4 hashes tiles by content
(BLAKE3) and stores each unique blob once, with states referencing hashes
rather than offsets — genuinely free for undo/redo churn that revisits
previously-seen content, though the synthetic session that produced the
table above painted fresh random content on every edit, which is close to
this variant's worst case.

## Four real bugs, three of them silently wrong data, one of them a crash reachable through ordinary use

A dedicated 188-check adversarial sweep, applying the exact same "test
harder than the obvious happy path" discipline the container testing had
already established, found and fixed four more real bugs — none of them
caught by the hand-written self-test already sitting in the history code's
own file.

**The most serious one needed no corruption at all.** The shared retention
planner, when a byte budget forced it to evict old states, picked which
ones to keep by walking the append-order list and keeping "the newest N" —
then derived the new floor state from whichever state happened to be first
in that kept set. That assumption — that the kept set forms one contiguous
chain — is only true for a linear history. But the whole point of the tree
design is that undoing to an earlier point and making a new edit creates a
*second* branch, and append order says nothing about which branch a state
is actually on. We demonstrated this directly: a chain of five states, undo
back to state 2, two new edits branching off it (states 6 and 7), and a
budget tight enough to keep only three states. The planner kept states
4, 5, 6, and 7 — but state 6 and 7's real ancestor, state 2, had been
evicted in favor of state 4, an unrelated node on a completely different
branch that just happened to be next in append order. Loading state 7 — the
document's own actual current head — raised a hard `KeyError`. This is
reachable through completely ordinary use: undo once, then keep editing.
No corruption, no bad luck, nothing adversarial about the input at all —
which makes it more serious than most of the corruption-battery findings,
not less. The fix draws retention candidates only from the head's own true
ancestor chain, not append order, and the necessary trade-off is now
explicit rather than accidental: a budget-limited checkpoint keeps a
bounded window of the head's own chain and drops every other branch
entirely, the instant any budget cap applies at all, even one with room to
spare.

**The second was a checkpoint numbering collision that silently reverted a
real edit.** A checkpoint stamped its own current-state chunks with
`generation = head_state + 1` — but the app's own existing convention is
that the very next live edit after a checkpoint at `head_state` is *itself*
numbered `head_state + 1`. Every single checkpoint-then-edit transition
therefore produced two different chunks sharing the identical ID and
generation number: the checkpoint's stale, pre-edit copy, and the WAL's
fresh, post-edit copy. Disaster recovery (no trustworthy directory, so it
breaks ties by "highest generation wins," then by scan order) picked the
checkpoint's copy every time, because it always appears earlier in the
file. The real edit wasn't lost or flagged unrecoverable — it was silently
*replaced* with the pre-edit content, on the very first edit after every
single checkpoint, whenever a reader ever had to fall back to a full scan.
The fix stamps checkpoint-authored chunks with `generation = head_state`
instead — a number the app's own convention guarantees a live edit can
never reuse.

**The third made undo show content from the future.** The lookup that
resolves "what was this key's value before we had a recorded floor entry
for it" fell back to the key's *current* checkpoint value, on the
reasoning that no floor entry means the key never changed. That reasoning
only holds for genuinely static content history never touched. For a key
created *partway through* the session — a new layer, say — there's no floor
entry for a completely different reason: it didn't exist yet. The same
fallback resurrected that layer's current, future content into every state
before the layer was ever created. We demonstrated it directly: undoing all
the way back to the document's original state showed a tile belonging to a
layer that wouldn't be created for two more edits. The fix tracks which
keys history actually dirties at all, and only applies the current-value
fallback to keys entirely outside that set.

**The fourth made a pure rename vanish under disaster recovery.** Full-scan
recovery, when reconstructing the manifest, only ever considered chunks
written at checkpoint time — but the whole design point of riding the
manifest inside `HIST` frames is that a layer rename should be undoable
*without* a checkpoint. A rename committed only through a live autosave
frame was therefore invisible to full-scan recovery, which silently fell
back to the stale, pre-rename manifest from the last checkpoint. We
demonstrated it directly: rename a layer, force full-scan recovery, and the
rename is gone with no error anywhere. The fix makes full-scan recovery also
scan `HIST` frames for their carried manifest snapshots, picking the
highest-generation manifest across both sources — the same "highest
generation wins" rule already used for tile content, just extended to the
one other place a newer manifest can legitimately live.

**A fifth bug, caught while building a completely different feature later
— the interactive undo/redo hot path — deserves its own telling, because
the way it hid is the actual lesson.** Every one of the four variants above
answers "give me the whole document at state N" correctly, but that's a
cold, whole-document operation — 5 milliseconds for a tiny document, nearly
a full second for a 216-tile one, for the *identical* logical request. A
live editing session's undo click should cost one edit's worth of work, not
scale with document size, so a new `LiveUndoModel` was built specifically
to prove that's achievable: seed once from the current document, then step
by exactly one state at a time, touching only the handful of keys that one
state actually dirtied. Redo turned out to be the easy direction. Undo's
first draft resolved "the value of this key right before state M" by
looking up `(key, M's parent)` — which sounds reasonable and is simply
wrong, because the parent state essentially never touched the same key M
did; a different edit typically touches different tiles. That lookup was
silently absent for the overwhelmingly common case, and the code's own
fallback — "not tracked at this exact state, so just keep whatever's
already there" — quietly turned every undo into a no-op. **This bug was
completely invisible when tested against H1 first**, because H1 stores a
full snapshot of every single key at every single kept state, so the wrong
lookup coincidentally succeeded anyway — not because the logic was right,
but because H1 duplicates everything everywhere and papers over exactly
this kind of key-resolution mistake. The moment the same code was tested
against H2, H3, or H4 — variants that don't store everything everywhere —
every single one of 26 undo steps in the test came back wrong. The fix
builds a small, per-key sorted list of exactly the states that ever
dirtied that one key, and binary-searches it for the entry immediately
before the state being undone. Verified afterward at every single step of
a 25-state undo-then-redo walk, against two independent sources of truth,
for all four variants — and measured to cost the same regardless of
document size: an 18x bigger document made this model's per-step cost move
by 1%, while the equivalent whole-document lookup moved by nearly 17x, the
exact O(one edit) property the whole exercise existed to prove. The
generalizable lesson, worth carrying forward: a data structure keyed by
"the state that changed this" is not the same thing as "the state whose
neighbor you happen to be asking about," and a test suite that only
exercises the one variant where the two coincidentally look the same will
never catch the difference.

Later rounds of this project also extended the design in two more ways
worth a brief mention. Reed-Solomon parity, which had only ever covered
current-state tiles, was extended — as a second, fully independent, opt-in
stripe pass — to history-region chunks too, and measured to drop the blast
radius of a corrupted xor-delta or a corrupted shared content-blob to zero
in both of the worst cases the earlier testing had found, at a storage cost
ranging from about 1.3% (for xor-compaction, whose history region is
already small deltas) up to the high teens or high twenties percent for the
other variants (whose history region mixes much larger full-tile entries).
And Git's own asymmetric reflog policy — a generous allowance for the
current chain, a smaller time-boxed allowance for abandoned branches,
rather than dropping every non-head branch the instant *any* budget cap
applies — was built as two new, off-by-default parameters on the retention
planner, tested across five deliberately adversarial scenarios (including
one where the abandoned branch is dropped precisely because its own
divergence point predates the current chain's own retained floor, a
documented scope limit rather than a hidden gap).

## One thing changed after this research concluded: linear, not branching, undo

Everything above was built and adversarially tested as a **tree**. That was
a deliberate, Vim-inspired choice at the time, and it's genuinely
interesting engineering — a real crash bug (the branch-eviction bug above)
was only reachable *because* the design supported branches at all, and
finding it required actually building that feature, not just reasoning
about it in the abstract.

After this research wrapped, in conversation with the user, the decision
was made that the real Mosaic implementation will use **linear** undo/redo
instead: undo, make a new edit, and whatever you undid past is gone for
good — exactly what Photoshop, GIMP, and Krita all do, and what most users
of a raster editor already expect from decades of muscle memory. This is an
ordinary product decision, made for ordinary product reasons — matching
what every mainstream competitor does — not a failure or a reversal of
anything the research found.

It does change what's actually in scope for the shipped format, though, and
it's worth being precise about which pieces that affects and which it
doesn't.

**Out of scope:** the branch-eviction fix's own tree-awareness, and the
whole asymmetric Git-reflog-style abandoned-branch allowance built on top of
it, both govern a situation — an abandoned redo branch surviving after a
new edit — that simply cannot occur once the product itself never creates
one. Under a strictly linear history, the set of retained states is always
one single chain by construction, which means append order and
ancestor-chain order are the same thing again — precisely the assumption
the tree-aware fix had to stop making. None of that work was wasted; it's
real, tested engineering, and the crash it caught is a legitimate one. It
just isn't what ships.

**Everything else carries over completely unchanged**, because none of it
actually depends on whether the graph of states is a tree or a straight
line. A linear history is really just the simplest possible tree — one
where every state has at most one child — so the `HIST` chunk and its
`parent` pointer still do exactly the same job, just with a guarantee the
tree design had to prove case-by-case instead of getting for free. That
means the H1-through-H4 encoding ablation and its numbers, the four
checkpoint/full-scan bugs described above, the `LiveUndoModel` hot path and
the lesson it taught, the XOR-compaction constraint (below),
the content-addressing dedup narrowing, the churn-driven case for H4,
and the H2-to-H4 adaptive switching design all stand exactly as tested. None
of that work assumed branching; all of it was really about how to encode
and retain the *content* of a state, a question the tree-vs-linear decision
never touched.

## The numbers that still apply: churn, and switching formats live

Two later rounds of this project are squarely in the part that still
applies, and both are worth walking through because they turned a hunch
into a measured, decision-ready number.

The storage table earlier in this document carried an honest caveat: its
test session painted fresh random content on every single edit, close to
content-addressed deduplication's actual worst case, since H4 only saves
anything when the *same* content shows up more than once. A follow-up round
built the opposite-extreme sibling: a session simulator that, with a tunable
probability, paints a tile with content already used earlier in the same
session — a rolling palette standing in for a brush reverting to an earlier
color, an undo-redo cycle re-creating earlier pixels, or just repainting
the same fill twice. At a 30% revisit rate — a fairly modest amount of
content reuse — H4 already overtakes H2 on storage size. At a 90% revisit
rate, H4 comes in 59% smaller than H2. And under a tight retention budget,
where every byte matters more rather than less, H4's advantage shows up even
sooner and wins by nearly 2x at high churn. One honestly-reported side note:
H3's size does *not* improve monotonically with churn the way H2 and H4 do
— because this particular simulator's revisits target a random tile rather
than the most recently dirtied one, churn actually increases how many
*distinct* per-key delta chains accumulate, which works against
xor-compaction's specific design even as the same bytes recur across keys.
That's a real, measured property of this specific revisit policy, not a
general claim that xor-compaction dislikes churn.

That finding led directly to a further, user-proposed idea: since H4 only
wins past a measurable threshold, why commit to one format for a file's
entire lifetime? Why not just measure the actual churn and switch formats
when it crosses that threshold? Both pieces this needs already existed —
every root chunk already carries a mode flag, and every checkpoint was
already re-emitting the *entire* retained history from scratch regardless
of which variant was in use — so "switch formats" turned out to mean
nothing more than calling a different variant's encoder on the same
in-memory history log at the next checkpoint, not a migration or a new
code path. What genuinely needed building: a way to reconstruct that
in-memory log from an already-saved file (so a switch can happen across an
app restart, not just within one continuous session), a repeatable measure
of how much of the retained history is actually duplicate content, and
hysteresis — asymmetric switch-up and switch-down thresholds — so churn
hovering right around a single cutoff doesn't thrash the format back and
forth, with each switch being a full rewrite.

Building the storage-comparison test for this caught one more real bug,
and it's a good one: the first version measured churn over a short recent
window — the last 20 states. Under *unbounded* retention (nothing ever gets
evicted, so a checkpoint re-emits everything, forever), that short window
reacts correctly to what's happening right now but completely "forgets"
that a long earlier high-churn stretch is still fully retained in the file
and still benefiting from H4's deduplication. In a test session built
specifically to catch this — 30 fresh states, then 60 high-churn states,
then 60 fresh states again — the 20-state window correctly switched to H4
during the churn spike, then switched straight back to H2 the moment recent
behavior looked fresh again, even though most of what the file actually
*contained* by that point was still the highly duplicative middle stretch.
The result was a needlessly bigger file — 17.2MB instead of 12.0MB, purely
from picking the worse format for content that was still sitting there. The
fix, now stated as a general design rule: **the churn-measurement window has
to match the retention horizon.** Unbounded retention needs a whole-history
signal, because "everything currently retained" and "the whole history" are
the same set by construction. A bounded retention budget needs a window
roughly the size of what that budget actually keeps, since old high-churn
content genuinely does age out under a budget and the signal should track
that. Both cases are demonstrated, not just argued for: with the fix, the
final file size matches the oracle-best choice — `min(always-H2,
always-H4)` — exactly, in both the unbounded and the budgeted scenario.

## The narrowings, and one naming story

The compression and erasure-coding landscape had one detail worth telling on
its own: Zstandard dropped its original, ambiguous "BSD+Patents" license
template in 2017, after the same community pushback that hit Facebook's
React license around the same time, and now ships under a plain
BSD-3-Clause-or-GPLv2 choice. Brotli carries a formal royalty-free
commitment Google made specifically for its use in the WOFF2 web-font
format. Classical Reed-Solomon is 1960s-vintage. The newer rateless
"fountain" codes in the same broad family — Raptor, RaptorQ — are governed
by a licensing regime this project has no interest in navigating, and were
avoided entirely rather than risked.

Three techniques in the history design ended up deliberately narrower than
their textbook forms, and each narrowing is worth recording as a standing
constraint rather than a footnote.

Reverse-delta versioning with periodic full keyframes — the shape of the
xor-compaction design — has genuinely ancient lineage: RCS's reverse-delta
storage from 1982, still used essentially unmodified in Git packfiles and
Mercurial revlogs more than forty years later.

Content-addressed, hash-keyed deduplication is the interesting one, because
"sounds similar" and "actually is the same mechanism" turned out to be
different questions. The enterprise deduplication literature is built almost
entirely on **variable-length, content-defined chunking** — a rolling
Rabin-fingerprint hash that finds chunk boundaries wherever they happen to
fall in an arbitrary byte stream, specifically so that shifted-but-similar
data still lines up and deduplicates. The design here does no such thing: it
hashes **whole, fixed-size tiles**, with chunk boundaries already fixed in
advance by the document's own tiling grid, completely independent of
content. The entire "where in this stream do we even decide to cut a chunk
boundary" problem simply doesn't arise here — the same kind of narrowing
this whole project already applied to Reed-Solomon (implement only the cheap
known-erasure decode path, never a blind error locator that would need
different, heavier machinery).

The XOR-compaction design needed an actual standing design constraint
attached to it, not just a note in a report. It operates at
checkpoint/compaction time over the whole retained history, never live and
never per-stroke; it XORs whole, fixed-size tiles, never a dynamically sized
stroke bounding box; and it never subdivides anything into a sub-grid to
select which pieces changed. That combination is deliberate, and the rule
that comes out of it is: **never add finer-than-tile,
stroke-bounding-box-derived, grid-subdivided XOR granularity to the live
edit path.** Whole-tile XOR, computed only at checkpoint time, isn't just
the tested design — it's the shape this design commits to, and any
temptation to make it more fine-grained later is a new design decision, not
a refinement.

One last, smaller story, about naming: the feature that discards retained
history down to just the current state needed a name, and "Coalesce" was the
first, most natural one. It turns out "COALESCE" is an active trademark held
by an unrelated data-integration company, in a different registered class
(downloadable data-transformation software) with no plausible marketplace
overlap with a single in-app action label — and "coalesce" would in any case
have just been ordinary descriptive English for what the action does, never
actually a blocker. But the feature shipped named **"Flatten History"**
instead anyway, sidestepping the question entirely, and picking up a real
bonus in the process: it reuses vocabulary every raster-editor user already
knows from "Flatten Image" in Photoshop, GIMP, and Krita — a different, but
conceptually adjacent, "collapse structure you no longer need down to one"
action. Lower cognitive load than a novel word would have carried, and a
small, honest example of how a real naming decision actually gets made.

## The part where reasoning wasn't enough, one more time

Everything above happened in the standalone research project. What follows
happened afterward, while turning that research into an actual buildable
spec for Mosaic (`docs/mosaic-native-format.md`) — and it's worth telling,
because it's the same lesson this whole project already learned twice,
learned a third time, this time about the act of writing the spec itself
rather than about the design.

The first pass at that spec answered five remaining questions by reasoning
from what was already known, instead of testing them: what tile size to
use, which codec should back the fast autosave tier, how big the root's
reserved slot should be, what the default retention budget should be, and
whether a small bookkeeping field on each undo state's commit record was
still needed. Every one of those answers sounded reasonable. Two of them
were wrong, in ways a real test caught immediately.

**Tile size** was the big one. The prototype had used 256×256 pixel tiles
throughout every one of the first nine rounds — copied from a single code
comment ("this amortizes per-chunk overhead better than Krita's 64×64"),
never actually tested against the alternative it was dismissing. When
asked directly why 256 and not something smaller, the honest answer was:
nobody had checked. So it got checked. Real photo content, three tile
sizes, the same correctness-gate-before-trusting-a-number discipline as
every round before it. The compression-ratio worry turned out to be almost
nothing — 128px tiles actually compressed slightly *better* than 256px,
and 64px tiles were only 1.4% worse, a rounding error. But a second effect,
one the original reasoning had never considered at all, turned out to be
decisive: this design has no way to save part of a tile. Touch one pixel,
and the *entire* containing tile gets re-filtered and re-compressed at the
next autosave. At 256px, a single small brush-sized correction costs
writing about 140 kilobytes. At 64px, the same correction costs about 10.
Thirteen times cheaper, for identical actual work — and this design's own
second-highest priority, right behind resiliency, is "autosave cost scales
with the size of the edit, not the size of the document." The 256px
default had been quietly working against its own stated goal the entire
time, and nine rounds of rigorous testing had never once looked at it,
because nobody had asked.

**The compression codec question went the other way — and this one's
worth including specifically because not every second look overturns the
first answer.** The original research's own compression table had actually
suggested the autosave path should just reuse zstd's fastest setting
instead of pulling in a second compression library (LZ4) at all — "zstd's
low levels already match it," the table said. Nobody had actually measured
that claim either; it was the *other* place a comment stood in for a test.
Measured now, directly: LZ4 turned out to be the faster choice by a
comfortable margin in both directions — roughly 1.5-1.8 times faster to
compress, and nearly three times faster to decompress, than zstd's own
fastest setting, in exchange for a meaningfully worse compression ratio.
Since the autosave path's whole job is to be fast, and reopening a file
means replaying that same autosave data back through the decompressor,
keeping LZ4 as a second, dedicated dependency turned out to be the right
call after all — this time, checking the assumption confirmed it instead
of breaking it.

Three more questions got the same treatment, each ending in a concrete,
tested answer instead of a plausible one. The reserved slot at the front of
the file that holds a fast-path copy of the root record had no real sizing
policy at all in the prototype — just a fixed 64-kilobyte allocation with
no plan for what happens if a document's root ever grew past it, which
would have silently corrupted every offset written after that point. Given
a target size (128 kilobytes) to build around, the fix turned out to be
almost elegant once it was actually attempted: if the real root fits, write
it directly, no different from before; if it doesn't, drop a tiny pointer
into the reserved slot instead and write the real thing as an ordinary
chunk right after it. And the pleasant surprise: the file's own disaster-
recovery reader, the one that falls back to scanning the whole file byte
by byte for anything that looks like a valid root record, turned out to
already be completely blind to *where* a root record sits — it was already
built to find one anywhere. The new pointer mechanism needed zero changes
to that reader to work correctly, including in the worst case tested: pointer
destroyed, overflow chunk destroyed, recovered anyway from the two backup
copies parked at the end of the file.

The retention-budget default got a real answer instead of a shrug, too. A
forty-state editing session, run against two documents of very different
sizes, showed that the cost of keeping one more undo state around is
essentially the same regardless of how big the overall document is — it's
set by how much a single edit actually touches, not by the canvas size,
which only affects the baseline cost of having no history at all. That's a
genuinely useful, non-obvious result: it means "keep everything, with a
generous safety net just in case" isn't a hand-wave, it's a number that can
actually be justified — retaining many hundreds of undo states, even at the
smaller 64px tiles the other test just settled on, costs only a few
megabytes.

And the smallest of the five, the one about a single field on the commit
record that closes each undo state, got the most satisfying kind of answer:
a direct test that the field really is provably safe to compute instead of
store, including in the one case that looked like it might not be (right
after a user discards their retained history and keeps working) — and then
a decision to keep storing it anyway, because a free correctness check that
costs a handful of bytes isn't worth giving up just because it's technically
redundant.

None of these five questions were dramatic on their own. What's worth
sitting with is that they were the exact same kind of mistake the earlier
nine rounds spent their whole existence hunting down in the format itself —
a plausible-sounding assumption, never actually run against the thing it
claimed to be true of — except this time it happened one level up, in the
act of writing the recommendation down. The fix was the same fix, too: stop
reasoning, go measure it.

## Where this leaves us

The product recommendations, stated plainly: build the container around
M6 — chunked, replicated root, WAL-chained autosave, checkpoint-only
Reed-Solomon parity. Turn history saving on by default, built around H2 —
the one variant where keeping history costs nothing extra at the moment it
actually matters, autosave, with a bounded, tunable cost showing up only at
checkpoint time. Offer xor-compaction (H3) as an opt-in, idle-time
recompaction pass for documents with a long retained history — its
overhead is compelling, but it should be computed lazily, not eagerly on
every save, matching this whole design's running "CPU is the currency,
don't spend it on autosave" principle. Keep content-addressing (H4) in
reserve for documents that show real undo/redo churn — texture work,
iterative color and composition experimentation, anything with heavy
revision-and-reuse — with the option of measuring a real Mosaic session's
actual revisit rate before deciding whether it or H2 should be the default
for a given document, or switching between them live, as the adaptive
design above already proves is cheap and correct to do. Build the
interactive undo/redo path on the `LiveUndoModel` hot-path design, never on
repeated whole-document loads. And ship it as **linear** undo/redo, not the
tree this research built and tested — an ordinary, deliberate product
decision made after the research concluded, for good reasons that have
nothing to do with anything the research found wrong. Use 64×64 pixel
tiles, not the 256×256 the prototype carried through all nine rounds
unexamined — the one place this whole project's own discipline had to be
turned back on itself before the design could actually be called finished.

The honest calibration this whole project earns, and the right note to end
on: this is a rigorously, adversarially-tested *design*, not shipped C++
code. Two real bugs were found and fixed in the container by a 129-check
adversarial sweep. Four more were found and fixed in history persistence
by a 188-check sweep, plus a fifth caught later while proving out the
interactive undo hot path, plus a calibration bug caught while building the
asymmetric-retention tests, plus a churn-window bug caught while building
the adaptive-switching tests. That is a real, meaningfully strong form of
validation — every one of those bugs was a genuine defect, found by
deliberately trying to break a working prototype rather than by reasoning
about it on paper, and every one is now fixed and covered by a regression
test. But "survived hostile testing in a Python prototype" is a different
claim from "proven in production." The real implementation will be C++,
with different performance characteristics than this prototype measured —
the Paeth encode-side slowdown in particular is not expected to survive the
move, but that's an expectation, not yet a measurement of the thing that
will actually ship. It will need its own adversarial testing pass once
built, on its own terms, rather than inheriting this prototype's green
checkmarks by assumption. What this research actually delivers is a design that has been through
a genuinely serious attempt to break it, with the scars and the fixes to
show for it — which is a good place to start building from, and an honest
distance short of "done."

## Postscript: Round 11 — the review round

After this write-up was finished, the spec (`docs/mosaic-native-format.md`)
went through a full review, and the review did what reviews are for: it
found the one question nobody had asked. The whole autosave design — the
crown jewel of the container work — appended WAL frames *to the user's own
file*. Technically elegant, empirically bulletproof, and quietly wrong as a
product: it meant closing without saving left the discarded edits sitting
recoverable in the file, the "unsaved" window title lied about the bytes on
disk, and cloud-sync clients re-uploaded the document all session long. The
user's ruling was immediate and total: **nothing writes to the user's file
without their permission — an explicit Save is the only thing that touches
it.** Autosave moved to a separate, app-owned recovery journal in the OS
state directory.

The move cost the design nothing — the journal is the same chunk stream,
relocated (which is, funnily enough, *closer* to SQLite's own sidecar-WAL
shape than the original append-in-place design was) — and the constraint
bought a small pile of unplanned wins: untitled documents get crash
protection (a UUID exists before a path does), read-only media get
autosave, the end-of-file root replicas are now genuinely at the end of the
file forever, and "unsaved" became a statement of literal fact.

Two design changes rode along, and both went through the same
test-before-trust discipline as Rounds 10's five (16 new checks, standing
gates re-run green). First, the journal binds to the exact checkpoint it
extends by seeding its frame chain from the bound root's checksum — a stale
journal now fails *mathematically*, at frame zero, before any policy field
gets a say. Second, and the genuinely new finding: the cumulative
WAL-checksum chain, inherited with pride from SQLite, turned out to have a
sharp edge the research had never probed. If a frame's *structure* is
destroyed (not just its payload), the chain-link value dies with it — and
every later, physically intact frame becomes unverifiable *by
construction*. Thirteen perfectly good frames, abandoned, in the test that
proved it. The fix is almost embarrassingly small: give each frame a
standalone checksum and carry the previous frame's checksum as an explicit
checked field instead of folding it into the checksum computation.
Detection power for torn tails and reordering: measured identical. Salvage
after mid-journal damage: from "impossible in the structural case" to
"12/12 states recovered, with the damaged state's keys flagged precisely."
The same round also proved why salvage must never be casual — a naive
scanner that applies whatever parses returns a *successful-looking*
document with silently stale pixels — so the spec's salvage mode is
conservative by default, explicit always, and honest about imprecision when
the commit frame itself is gone.

One measurement got back-filled too: the Reed-Solomon stripe padding waste
that Round 9 flagged for history parity but never quantified for the parity
that actually ships. At 256px tiles it would have been +30% over the parity
floor; at the 64px tiles Round 10 chose, it's +11.5% — the small-tile
decision quietly paying a second dividend nobody had claimed for it.

The calibration note above still stands, one round further along: still a
design, still not shipped C++ — but now a design whose save semantics a
user has actually ruled on, which is the kind of question no amount of
adversarial testing would ever have surfaced on its own.

And one more round after that. The user read the amended spec and asked the
question that inverts a career's worth of habit: if appending committed
states is safe enough for the journal, why does Ctrl+S — the operation
users hammer reflexively — pay for a full rewrite? Round 12 tested the
inversion: **File→Save appends one atomic committed batch** (a torn Save
opens at the previous commit, and since the journal only resets after the
Save's bytes are durable, even a crash mid-Save loses nothing), while Save
As and an occasional threshold-tripped Save do the full write. The
measurement that settles it: under retained history, the appended file came
out 0.9% *smaller* than the full rewrite it replaced — the full save had
been mostly re-writing chunks it was going to keep. Ctrl+S drops from
seconds to milliseconds on a large document, and every mainstream editor's
save (Krita, GIMP, Photoshop — all full rewrites) is now the slow way.
The same round measured the SSD-wear question to death (a heavy painting
hour journals ~60MB; the worst imaginable fsync cadence spends 0.19% of a
consumer drive's daily endurance budget) and produced one last
demonstrated-the-hard-way lesson: the first run of the full-scan check
briefly violated the "only states consume generation ids" rule, and
index-free recovery silently returned a stale tile — the kind of failure
you want a prototype to hand you once, loudly, before C++ makes it
somebody's lost afternoon.
