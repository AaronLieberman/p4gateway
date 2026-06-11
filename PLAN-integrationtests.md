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
  client spec — that's fine.)
- The user has **already** added a client view line remapping the `src`
  subtree of `p4gw-test` to a mirror directory at `.p4gw/mirror/src`. The
  tests verify this mapping is present and correct; they do not create it.

## `integtest init` — build the depot-side fixture

Prepares a known-good starting state in Perforce so `integtest run` has
something to import:

1. Verify the P4 mapping looks right, **including** the remap of the `src`
   subdirectory to `.p4gw/mirror/src` (reuses `p4::parseClientView` /
   `p4::checkViewMapping` from `src/p4.h`).
2. Sync the `p4gw-test` directory to head.
3. Delete all files currently present locally under `p4gw-test`.
4. Create a set of test files: some directly under `p4gw-test`, some under a
   `src/` subdir, and some under a `bin/` subdir.
5. `p4 reconcile` this layout and submit it (so the fixture is in the depot,
   not just local).
6. Write a local `.p4gw` config that maps the `src` directory but **not** the
   root or the `bin` directory (so the test exercises a partial-subtree
   overlay, with `bin/` and root files outside gw's scope).

**Open question (deferred):** whether `integtest init` also writes a
`.gitignore`, or whether that's left to `gw init`. Tied to the `gw init`
redesign below.

## `integtest run` — drive the real workflow

1. Use `gw init` to set everything up. If the P4 client mapping (added
   manually by the user beforehand) doesn't match what gw expects, `gw init`
   should fail. **(See "Open: gw init redesign" — this is the behavior under
   reconsideration.)**
2. Run a sequence of commands simulating real-world use:
   - actual `p4 sync`s
   - `gw import`s
   - changes to the git working tree + git commits
   - more syncs into the P4 workspace
   - `gw import --rebase`
   - `gw prepare`
   - submits to the P4 workspace
3. Assert expected state at each step (branch state, file contents, opened
   files, CL contents, reconcile-clean).

## Logging

- Default: minimal logging (one line per high-level step, pass/fail summary).
- `--verbose`: echo every spawned `git` / `p4` / `gw` command line and its
  output, so Claude Code can diagnose failures on the Windows machine.

## Implementation approach

- A new **C++ `integtest` executable** under `tests/integration/`, linking
  `p4gw_core`. This reuses the typed `git`/`p4` wrappers, the process runner,
  and the view-check logic for assertions, and keeps the no-external-deps
  policy (no Python/PowerShell dependency).
- Built by CMake everywhere so it can't rot, but **never registered with
  ctest** (it requires p4 + a live server).
- Subcommands `init` and `run` dispatched like the main binary
  (`src/main.cpp` is the pattern).

## Decisions locked in so far

- **Driver:** C++ binary (not PowerShell or Python).

## Open questions / in flux

- **`gw init` redesign:** the user wants to split `init` into two parts to
  make the whole flow easier to use. Details TBD — the user will explain next
  steps. This affects:
  - whether `gw init` itself performs the client-view verification (and fails
    on mismatch), vs. leaving that to `gw doctor` / a new subcommand;
  - who writes `.p4gw` (today `gw init` early-exits if one already exists, so
    `integtest init` writing it would make the `gw init` call in
    `integtest run` a no-op);
  - whether `.gitignore` creation stays in `gw init`.

  Revisit this whole section once the `gw init` split is defined.
