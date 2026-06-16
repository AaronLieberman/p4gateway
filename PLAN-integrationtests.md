# PLAN-integrationtests.md — automated integration tests for `gw`

## Purpose

The unit tests (`tests/test_*.cpp`) only exercise pure logic — view-spec
parsing, git-diff → p4-op mapping, mirror sync-action computation, config
parsing — with canned string inputs. They never spawn `p4` (it isn't
installed in dev/CI) and only run `gw --help` for the binary itself. That
leaves the entire process-touching half of the tool unverified by anything
automated: `p4 change -i` spec round-trip, `p4 move` semantics, reconcile
preview parsing, and the real `import` / `prepare` workflow against a live
server.

These integration tests close that gap. They **cannot run in CI** (they need
`p4.exe`, a live P4 server, and a configured workspace). They are meant to be
run **locally on a Windows machine** — either by the user directly, or by
Claude Code on that machine — to confirm the end-to-end workflow works and to
diagnose regressions. They build everywhere (so they can't bitrot silently)
but are never invoked by ctest.

## Environment assumptions (set up by the user ahead of time)

- A P4 workspace is configured on the local client, with connection details
  in a `p4.ini` (`P4CONFIG`) in the current directory.
- All test work happens under a **`p4gw-test`** directory, a subdirectory of
  the P4 workspace root. (It may be mapped to a different depot path in the
  client spec — that's fine; the depot path is discovered via `p4 where`.)
- The user has **already** added a client view line remapping the `src`
  subtree of `p4gw-test` into the repo's mirror container at
  `p4gw-test/.p4gw/src` (the repo root is `p4gw-test`; `bin/` and the root
  files stay unmapped). The tests verify this mapping is present and correct;
  they do not create it.
- `gw integtest run` is run from inside `p4gw-test`. It **deletes everything
  under it** (except `p4.ini`/`.p4config`), so nothing of value may live there.
- The server must be a dedicated **throwaway** p4d: `run` refuses unless its
  `Server ID` is `p4gw-integtest-throwaway` and its security level is 0, since
  it obliterates depot files and wipes the local tree.

`gw integtest run` is a single, self-resetting command. It does everything in
one shot — there is no separate `init` step. The phases below describe what it
does in order; `--clean` runs only the final cleanup phase (for recovery after
a failed run) and `--leave` skips it.

## Phase 1 — build the depot-side fixture

Prepares a known-good starting state in Perforce for the workflow phase to
import. Self-resetting: it reverts/force-syncs and (via reconcile) restores the
depot fixture to baseline, so a previous run's drift is undone every time.

1. Verify the P4 mapping looks right, **including** the remap of the `src`
   subdirectory into the repo's mirror container `.p4gw/src` (reuses
   `p4::checkSpecMapping` from `src/p4.h`). Fail with the exact line to add if
   it's missing.
2. Revert any files opened under the test depot path; sync `p4gw-test` to
   head.
3. Delete all files currently present locally under `p4gw-test` (including a
   previous run's `.git` and `p4gw.cfg`; the `p4.ini` survives).
4. Create the fixture files: a few directly under `p4gw-test`, a few under
   `bin/`, and the `src/` set **physically inside `.p4gw/src/`** — with the
   remap active, files placed at `p4gw-test/src/` would be unmapped and
   invisible to reconcile; the mirror is where the src depot paths live on
   disk.
5. `p4 reconcile` scoped to the test depot path and submit, so the fixture
   is in the depot. (If nothing changed, the empty CL is deleted and the
   submit skipped.)
6. Run `gw setup --depot-path <discovered>/src/... --mirror-path .p4gw/src` in
   `p4gw-test` (the repo root), as a child gw process — the mapping covers the
   `src` subtree but **not** the root or `bin/`, so the test exercises a
   partial-subtree overlay. (`.gitignore` is `gw init`'s job, exercised by
   `integtest run`.)

## Phase 2 — drive the real workflow

Each step asserts the expected state (branch state, file contents, opened
files, CL contents, reconcile-clean) and prints one summary line; failures
are loud and exit nonzero, leaving state in place for inspection.

1. `gw init` in `p4gw-test` — must succeed: it verifies the client
   mapping via p4 (this is the verifying-init behavior the split was for),
   creates the Git repo, commits `.gitignore`.
2. Sync; first `gw import` → `p4-main` exists and tracked files match the
   mirror exactly.
3. Feature branch: edit one file, add one, delete one, rename one, across
   two commits (so the CL description stitches multiple messages).
4. `gw prepare` → parse the CL number; `p4 opened -c <CL>` shows exactly the
   expected edit/add/delete/move set; output contains the reconcile-clean
   "Verified" line.
5. Submit the CL (via the `p4::submit` wrapper); sync; `gw import --rebase`
   → the branch's commits melt away (branch tip == `p4-main` tip) and the
   worktree matches the mirror.
6. Teammate simulation: `p4 edit` + modify + submit a different src file
   directly in the mirror; sync; new feature branch with a local commit;
   `gw import --rebase` → both changes present, rebase clean.
7. Opened-files preflight: open a src file in the mirror with un-submitted
   content; `gw prepare` must refuse (files already open), and `gw import`
   must commit the depot head — not the un-submitted working copy — to
   `p4-main`. Revert and clean up afterward.
8. Shelf import: build a shelved CL on the server (an edit plus an add),
   revert the workspace so only the shelf remains; `gw shelf list` must show
   that CL flagged `[shelved]`; then `gw shelf import <CL>` → a new
   `shelf-<CL>` branch off `p4-main` with one commit carrying the shelved
   edit and add. Validates the `#rev` base-revision assumption and the
   `p4 print`-only read path. Delete the shelf + CL afterward.
9. Final checks: `gw doctor` exits 0; nothing opened under the test path;
   the root and `bin/` fixture files were never touched.

## Phase 3 — cleanup

Runs last on success (skipped by `--leave`); also the whole job under
`--clean`. Leaves both sides empty: revert any opens, delete this user's
leftover pending/shelved changelists, `p4 obliterate -y` the explicitly-named
fixture files (no wildcards), then wipe the local tree (keeping only
`p4.ini`/`.p4config`). The throwaway-server check is re-run immediately before
the obliterate. Because Phase 1 self-resets, a subsequent `run` rebuilds
everything from scratch.

## Logging

- Default: minimal logging (one line per high-level step, pass/fail summary).
- `--verbose`: echo every spawned `git` / `p4` / `gw` command line and its
  output, so Claude Code can diagnose failures on the Windows machine.

## Implementation approach

- A **`gw integtest` subcommand of the main binary** (`src/commands/
  integtest.cpp`), not a separate executable: simpler build, and the p4
  helpers it needs (`where`, `sync`, `revert`, `reconcile`, `submit`) become
  regular typed wrappers in `src/p4.{h,cpp}` — the submit wrapper doubles as
  groundwork for a possible future submit workflow.
- The gw commands under test (`setup`, `init`, `import`, `prepare`,
  `shelf list`, `shelf import`, `doctor`) are spawned as child processes of
  the same executable (argv[0]), so the real CLI surface is exercised;
  `--gw <path>` overrides.
- Built always; **never registered with ctest** (it requires p4 + a live
  server). All p4 calls are scoped to the discovered test depot path or
  explicit file lists, per the standing rule.

## Decisions locked in

- Driver: C++, inside the `gw` binary itself (`gw integtest run`).
- `gw setup` / `gw init` split (see PLAN.md M1): Phase 1 writes the config via
  `gw setup`; Phase 2 exercises the verifying `gw init`, which owns
  `.gitignore` creation.
