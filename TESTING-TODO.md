# Testing audit & todo (2026-07)

Audit of unit-test and integtest coverage after `import_mode = worktree`
became the default. No code changes yet — this is the actionable list.
Context: the integtest runs on a Windows GitHub Actions runner against a
throwaway p4d (see README-integtest.md), so "covered by integtest" means
covered on a real server in CI.

## Unit tests — the mode question

**Nothing to change.** The unit tests are mode-neutral by construction:
`ImportMode` is consumed only by the `import` and `doctor` *commands*, which
unit tests never run (they need real `git`/`p4`). No pure, unit-tested logic
(`mirror`, `p4ops`, `manifest`, `statusview`, …) branches on the mode — both
modes share `buildSnapshot` and differ only in the staging root. The config
tests cover the default (now asserts `kWorktree`), both explicit values, and
the bad-value load error. So the unit tests already "run in worktree mode"
in the only sense that exists for them.

Inventory (146 tests): config 45, p4view 30, preflight 18, status 14,
prepare 12, mirror 11, manifest 7, shelf 7, subprocess 2.

If mode-dependent *pure* logic ever gets extracted (e.g. the snapshot
worktree validate/self-heal decision), unit-test both modes there; today
there is none.

## Integtest — mode coverage after the default flip

Every step that doesn't set `import_mode` now runs **worktree** mode — that
includes first import, `--rebase` melts, teammate absorption, opened-file
handling, shelf import, have-manifest fast path, doctor probes, and all four
git-branchless restack scenarios. That matches the goal (most coverage in
the default mode). What still runs **checkout** staging:

- `itWorktreeImport` step (a): sets `import_mode = checkout` explicitly and
  asserts only the dirty-tree *refusal* (an error path — no staging runs).
- The very first import (`itFirstImport`): falls back to checkout staging by
  design (no baseline commit to pin a worktree to) — but that's the
  orphan-baseline variant, not a steady-state checkout import.

**Gap: checkout mode's steady-state import path (detach onto the old
baseline, overlay, commit, switch back, restore on failure) is no longer
exercised anywhere.** Before the flip it was the whole suite; now it's
nothing. Items 1–2 below close this.

## Todo list

### High

1. **Checkout-mode round trip.** New step late in the suite (after the
   worktree steps): write `import_mode = checkout`, submit a teammate-style
   depot change, sync, `gw import` on a clean tree — assert the branch
   fast-forwards, the change reaches the checkout, and HEAD ends back on the
   branch (the staging detach + switch-back actually ran). Then a local
   commit + another depot change + `gw import --rebase` in checkout mode.
   Restore the config after. This is the "make sure checkout still works"
   coverage and also proves **mode flipping on a live repo**: worktree →
   checkout (stale snapshot worktree left behind must not break anything)
   and back.
2. **Mode-flip hygiene.** As part of (or right after) item 1: with the stale
   snapshot worktree still on disk in checkout mode, assert `gw doctor` is
   still clean/informative and a subsequent worktree-mode import self-heals
   (reset or recreate + full copy) rather than trusting stale stamps. The
   self-heal code exists; nothing exercises the flip-back-and-forth sequence.

### Medium

3. **`gw status` step.** No integtest step runs `gw status` at all — the
   PLAN M3 note ("pending-CL count parsing against live `p4 opened`") is
   still open. Assert: on a clean baseline (branch/ahead-behind/no-dirty,
   last-import CL), with a dirty file, with local commits, and with a gw
   pending CL open (count + next-step suggestion).
4. **Shelf import: binary + conflict paths.** `itShelfImport` covers only
   the happy path (text edit + add against head, clean merge). Add: a binary
   file in the shelf (taken wholesale), and a shelf based on an older depot
   revision so the 3-way merge conflicts — assert git conflict markers and a
   usable branch state.
5. **Interrupted-import recovery.** Nothing exercises the pending-marker
   path. Plant the marker file (simulating a crash mid-import), then run
   `gw import` in worktree mode and assert the snapshot worktree is reset +
   fully recopied and the import succeeds; same for checkout mode's
   restore-on-failure if practical to fake.

### Low

6. **`gw prepare --abandon --dry-run`.** Assert the preview text and that
   the CL, its opens, and its shelf are all untouched afterwards.
7. **Doctor's unhealthy-worktree reports.** `doctor --verify` healthy is
   asserted; the WARN paths (worktree HEAD ≠ baseline, dirty worktree,
   deleted/broken registration → "will recreate") are not. Break the
   worktree deliberately, assert the matching WARN, then confirm the next
   import self-heals.
8. **First-import fallback note.** A repo whose config says
   `import_mode = worktree` gets checkout staging + a note on the very first
   import. Assert the note once (cheap; guards the fallback wiring).
9. **git-branchless under checkout mode.** The four restack scenarios now
   run only in worktree mode. Checkout mode shares the repositioning code
   (the `!worktreeMode` gates were removed), so one smoke scenario (e.g. the
   plain sync-restack) in checkout mode would guard the mode-specific
   switch-back interactions. Low priority — the shared-code argument is why
   it's not higher.

## Non-goals (deliberate)

- Mocking the p4 server for unit tests — the thin-wrapper layering makes
  the integtest the right place for p4 behavior (CLAUDE.md rule).
- A Linux integtest job — the Windows runner is the one that exercises the
  CRLF LineEnd path, which is where the fidelity risk lives.
- Exhaustive p4 filetype coverage (`+k`, `+l`, exotic LineEnds) — tracked as
  M4 hardening in PLAN.md, wait-until-it-bites.
