# Async full write — design note (S48 Build 2, slice 4)

The ruling: the full write leaves the UI thread. The **snapshot stays synchronous** — the
document must be quiescent while it is serialized — and everything after it (compress, parity,
layout, the atomic write) runs on a worker with progress in the status bar, following the
proven inpaint async pattern (`runInpaint` + poll timer + `StatusBar::setProgress`).

Commit-append Saves are **not** touched: they are O(changed) and measured in fractions of a
millisecond — going async there would buy nothing and cost the simple linear flow every
Ctrl+S depends on. Journal autosaves likewise stay where they are.

## What moves off the UI thread

Every **full write**, all of which already funnel through two functions:

| path | today | after |
|---|---|---|
| plain checkpoint (`writeMosaicTo` fallback: first save, recovered/full-scan opens, fold declined) | `buildCheckpoint` + `writeFileAtomic` on the UI thread | worker |
| history-preserving fold (`foldedWriteTo`: compaction Save, Save As, proactive fold, Flatten History's write) | read + `openDocument` + `buildCompactedCheckpoint` + `writeFileAtomic` on the UI thread | worker (including the file read) |

The synchronous prologue, unchanged in spirit from today (the ruling's "snapshot"):
`compositeForPreview` + `buildDocumentCheckpoint` + `diffDocumentStates` against the anchor
baseline. Controls are disabled (`setMainControlsEnabled(false)`, the inpaint precedent) from
the moment the job starts until it finalizes, so the document cannot change under the worker —
the worker never touches `m_document` or any UI state; it owns copies of everything it needs.

## The state machine

One job at a time per window (`m_saveJob`), like inpaint's `m_inpaintJob`.

```
        Ctrl+S / Save As / fold trigger
                  |
                  v
  IDLE ---> SNAPSHOT (UI thread, sync: composite + serialize + diff; fast-fail tail check)
                  |
                  v
            WRITING (worker: [read source + openDocument + fold | buildCheckpoint],
                  |           late tail check, writeFileAtomic; publishes fraction+stage)
                  |   ... UI poll timer ~20 Hz -> StatusBar::setProgress ...
                  v
            FINALIZE (UI thread, on done: join; then exactly one of
                  |     ok        -> adoptWrittenFile (markSaved, journal rebind, anchor rearm,
                  |                  lock, recents) — all UI-thread state, per the ruling
                  |     tail lost -> surfaceSaveConflict dialog (Cancel / Save a copy...)
                  |     io error  -> tellError)
                  v
  IDLE  <--- controls re-enabled; a coalesced pending request replays here
```

### Coalescing (newest wins)

A Save or Save As requested while WRITING sets a **pending request** (`m_pendingSave`:
`None | Save | SaveAs`) and returns — the newest request simply overwrites an earlier pending
one. FINALIZE replays it through the ordinary entry points (`saveDocument()` /
`saveDocumentAs()`), which snapshot the **then-newest** document state. The running write is
never cancelled: `writeFileAtomic` is atomic-rename, so letting it complete is free and safe,
and the replayed request supersedes its bytes moments later. (With controls disabled no edit
can land in between, so in practice the replay is an empty diff — the mechanism exists so a
racing keypress is never silently dropped.)

### Close / quit during a save

Waits, with the progress strip live (`waitForBackgroundSave()`: pump `Fl::wait` until FINALIZE
clears the job — the same nested-wait shape the file-dialog guard already uses). Two entry
points are guarded: the WM close callback (before `confirmQuit`), and
`confirmDiscardActiveDocument`'s Save choice, which must wait for the async save it just
started before it can honestly answer "is the document clean now?". Re-entrancy is blocked by
the job pointer itself (a second close during the wait finds the same loop already pumping,
`m_waitingForSave`).

### The tail check happens twice, and the late one decides

* **Fast-fail** (UI thread, before the snapshot): the existing O(1) `verifyTail` in
  `compactionSave` / `writeMosaicTo` — no point building a fold that is already doomed.
* **Authoritative** (worker, immediately before `writeFileAtomic`): the file may have changed
  while the worker built. A late mismatch aborts the write (the target is untouched — the
  contract of `writeFileAtomic` never engages) and FINALIZE raises the conflict dialog on the
  UI thread. Check-then-rename is still not atomic; the §2.10 advisory lock remains the
  defense for the true race, exactly as in Build 1.

### Progress

`buildCheckpoint` (and the fold, which forwards to it) gains an optional progress callback,
reported per chunk/stripe from the worker thread into the job's mutex-guarded
`fraction`/`stage`; the UI poll timer forwards the newest values to
`StatusBar::setProgress(fraction, stage)`. Stages: *Saving* (build) → *Writing* (flush +
rename). No cancel affordance: a Save was an explicit command, its window of usefulness is
seconds, and a torn cancel would only discard finished work (the rename is the commit point).

## What FINALIZE must do on the UI thread (the ruling's list)

`adoptWrittenFile` runs there unchanged: `markSaved`, lock acquisition, journal reset/rebind
(only now that the bytes are durable — Round 12 A2 ordering is preserved because the worker
returns only after `writeFileAtomic` succeeded), commit-anchor rearm, recents. The anchor
rearm reuses the snapshot's own serialization as the baseline instead of re-serializing the
document a second time (`setCommitAnchor`'s baseline hint), halving the synchronous cost of a
full write.

## Failure modes

* Worker write fails → FINALIZE `tellError`; the previous file is untouched
  (`writeFileAtomic`'s contract). The document stays dirty; nothing was adopted.
* Late tail check fails → conflict dialog; the fold's work is discarded; the user chooses
  Cancel or Save a copy (Save As), which snapshots afresh.
* App killed mid-write → the temp file dies with it; the target never saw a byte. The
  recovery journal still holds the session (it is reset only in FINALIZE, after durability).
