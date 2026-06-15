# TODO: mirror opened-files preflight for `import` and `prepare`

> Status: not started. This is a scoped work item for a follow-up change,
> tracked separately from the `gw shelf import` feature being developed on
> `claude/p4-shelves-integration-dnzq38`. **See "Coordination with shelf
> import" at the bottom — the two changes must be verified together.**

## Problem

Neither `gw import` nor `gw prepare` accounts for files that are already
*opened* in P4 inside the mirror (a half-finished prepare's pending CL, a
stray `p4 edit`, or any other workspace open under `depot_path`). Today:

- **`import`** (`src/commands/import.cpp`) only checks the **Git** working
  tree with `git::isDirty()`. It never inspects P4 state. It then copies the
  mirror's *working-copy* bytes into the Git tree and commits them to
  `p4-main`. If a mirror file is open for edit/add/delete, its working copy
  reflects the **un-submitted** pending change, so `p4-main` silently absorbs
  edits that were never submitted. `p4-main` is supposed to be pristine
  submitted depot state; this breaks that invariant.

- **`prepare`** (`src/commands/prepare.cpp`) creates a *new* numbered CL and
  runs `p4 edit/add/delete/move -c <newCL>` on the mirror files. If those
  files are **already** opened (e.g. in a prior prepare's still-pending CL),
  the fresh open either errors or silently moves the file between
  changelists. The end-of-run `p4 reconcile -n` does not catch this: a
  pre-opened file already matches the staged content, so reconcile sees
  nothing to report.

`gw doctor` and `gw status` already surface opened files (both call
`p4::openedFiles`, scoped to `depot_path`), but only as an informational
warning — neither `import` nor `prepare` changes its behavior.

## Decided approach: read the depot version, ignore the opened copy

The guiding principle (decided 2026-06-15): when gw needs the content of a
file that may be open in the mirror, it should read the **current depot
(head) revision via `p4 print`** rather than trusting the mirror's
working-copy bytes. The opened/edited copy is effectively ignored.

### `import`

1. Up front, call `p4::openedFiles(config)` (scoped to `depot_path`) to learn
   which files are open and their action.
2. When syncing the mirror into the Git tree, for each **opened** file:
   - `edit` / `move/add` / `move/delete` target: do **not** copy the mirror's
     working copy. Instead `p4 print` the current **head** depot revision of
     that file and write *that* into the Git tree, so `p4-main` reflects
     submitted state.
   - `add` (not yet in the depot): there is no depot revision — **omit** the
     file from the import entirely (it must not appear in `p4-main`).
   - `delete` (deletion not yet submitted): the file still exists in the
     depot — `p4 print` the head revision and include it.
3. Files that are **not** open keep the existing fast path (plain copy from
   the mirror).

Net effect: `import` always commits true submitted depot state to `p4-main`,
even when a prepare is mid-flight or someone left files checked out.

### `prepare`

`prepare` writes Git blobs *into* the mirror, so the depot-read principle
applies less directly here; the real hazard is the double-open collision.
Detect pre-existing opens up front (`p4::openedFiles`) and refuse to silently
double-open. The precise resolution most likely ties into the planned
`gw prepare --update <CL>` / `--abandon <CL>` flows (PLAN.md M3) — pick the
mechanism when implementing, but the invariant is: **`prepare` must never
move a file between changelists or open it twice without the user opting in.**

## Reuse / implementation notes

- `p4::openedFiles(const Config&)` already exists (`src/p4.h`) and returns the
  scoped `p4 opened <depot_path>` output. Prefer parsing the structured
  `-ztag` form (`p4 -ztag opened`) to get per-file `action` reliably.
- A `p4 print -q -o <dest> <depotPath>#head` wrapper is needed. **The
  `gw shelf import` change introduces the same `p4 print -o` wrapper** — share
  one wrapper in `src/p4.{h,cpp}` rather than adding two (see below).
- Keep every call scoped to `config.depotPath` or an explicit file list — an
  unscoped `p4 opened`/`p4 print` against a game depot is a bug.
- Follow the existing layering: new P4 invocations get a typed wrapper in
  `src/p4.{h,cpp}`; commands never call `process::run` directly.

## Coordination with `gw shelf import`

`gw shelf import` (developed on `claude/p4-shelves-integration-dnzq38`) is
built on the **same** read-the-depot-via-`p4 print` philosophy: it never
reads the mirror and merges shelf content into Git with `git merge-file`. It
adds a `p4 print -o` wrapper that this preflight work also needs.

**Once both changes have landed, verify they work together:**

- The shared `p4 print` wrapper is consistent (one implementation, scoped,
  binary-safe via `-o`/`-q`).
- `gw import` with files opened by an in-progress prepare commits pristine
  depot state to `p4-main` (no un-submitted edits leak in).
- `gw shelf import` followed by `gw prepare` / `gw import` behaves correctly
  given the new preflight checks.

This needs a Windows machine with a live P4 workspace (CI has no `p4`).
Prefer extending `gw integtest run` (see PLAN-integrationtests.md) with an
opened-files scenario so the check is automated on such a machine.
