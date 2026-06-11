# PLAN.md — p4gateway implementation roadmap

The goal is a reliable, boring tool: thin typed wrappers around the `git` and
`p4` CLIs, a handful of commands, and loud failures instead of clever
recovery. Each milestone leaves the tool buildable, tested, and useful on its
own. Check items off as they land.

## M0 — Project scaffold ✅ (this commit)

- [x] CMake project, C++23, no external dependencies, builds with MSVC and GCC
- [x] CLI entry point with command dispatch, `--help`, `--version`
- [x] `process` subprocess runner (popen-based first cut)
- [x] `.p4gw` config: parser, parent-directory search, unit tests
- [x] Thin `git` wrappers: current branch, rev-parse, dirty check,
      `diff --name-status` parsing (with renames), commit-message collection
- [x] `p4` wrapper shape (run with `-c client`; higher-level ops stubbed)
- [x] Zero-dependency test harness + ctest wiring
- [x] CLAUDE.md, README.md, this plan

## M1 — Solid foundations

The plumbing the real commands will stand on. Do this before any workflow
command, because every later bug report will otherwise trace back here.

- [ ] Replace popen with `CreateProcessW` on Windows (and `posix_spawn` on
      POSIX): separate stdout/stderr, no shell quoting risks, real exit
      codes, support for feeding stdin (needed for `p4 change -i`)
- [ ] `p4` wrappers: `clientRoot()`, client-spec inspection via `-Ztag`
      output parsing (`p4 -Ztag client -o`), `p4 opened` scoped query
- [ ] Flesh out `doctor`: P4 connection works (`p4 info`), configured
      `depot_path` is mapped in the client view, client has `allwrite`,
      `LineEnd` vs Git `core.autocrlf` agreement, warn if files are already
      opened in the workspace subtree
- [ ] `--dry-run` global flag plumbing: every mutating command prints the
      exact `p4`/`git` commands it would run
- [ ] Decide on logging (`--verbose` echoing every spawned command line)

**Exit criteria:** `gw doctor` against a real Windows P4 workspace catches
each misconfiguration above when introduced deliberately.

## M2 — `gw init`

- [ ] Interactive-ish setup: take `--depot-path` (required), `--client`
      (default from `p4 set P4CLIENT`), validate against the client spec
- [ ] `git init` if no repo present; refuse to nest inside an existing repo
- [ ] Write starter `.gitignore` (build dirs, `.p4gw`) and `.p4gw` config
- [ ] Create baseline branch from current synced state; record the synced
      changelist number (`p4 changes -m1 -s submitted <depot_path>#have`) in
      the commit message
- [ ] Idempotent: rerunning on an initialized repo reports state, changes nothing

**Exit criteria:** fresh overlay created on a real workspace subtree in one
command; rerun is a no-op.

## M3 — `gw sync`

- [ ] Refuse to run on a dirty working tree (point user at stash/commit)
- [ ] Remember current branch, switch to baseline, `p4 sync <depot_path>`
- [ ] Commit resulting tree to baseline with CL number in the message;
      handle "already up to date" (empty diff) gracefully
- [ ] Switch back and rebase the feature branch onto baseline (`--no-rebase`
      to skip); on rebase conflict, stop with clear instructions — never
      auto-resolve
- [ ] Handle P4 file types that arrive read-only despite `allwrite` (warn)

**Exit criteria:** morning sync is one command; a deliberately conflicting
local change produces a normal Git rebase conflict, not corruption.

## M4 — `gw submit` (the payoff)

- [ ] Preflight: clean tree, on a feature branch, baseline is ancestor of
      HEAD (else tell user to `gw sync` first), no files already opened in
      the subtree outside gw changelists
- [ ] Build CL description from `git log baseline..HEAD` (oldest first),
      `--message` to override
- [ ] Create numbered pending CL via `p4 change -i`
- [ ] `p4 reconcile -c <CL> <depot_path>` — treat "no file(s) to reconcile"
      as a clean no-op, not an error
- [ ] Cross-check: compare reconciled file list against
      `git diff --name-status baseline..HEAD` and warn loudly on mismatch
      (the canary for ignore/line-ending/scoping bugs)
- [ ] Modes: default prints CL number and opened files and stops (user
      reviews and submits by hand); `--shelve` shelves for Swarm review;
      `--submit` submits directly
- [ ] On submit success, optionally fast-forward baseline and tag the branch
- [ ] Renames: pass 1 ships them as delete+add via reconcile (document the
      history-loss caveat); pass 2 upgrades R-status pairs to `p4 move`

**Exit criteria:** a 3-commit feature branch with an add, an edit, a delete,
and a rename becomes one correct pending CL; description matches the commit
messages; `--shelve` produces a reviewable shelf.

## M5 — `gw status` + quality of life

- [ ] `status`: branch, ahead-of-baseline count, dirty files, last synced CL,
      pending gw CLs with numbers
- [ ] Helpful error for the "someone else touched my files" case (depot
      changed under a pending CL → suggest sync + rebase + re-reconcile)
- [ ] `gw submit --update <CL>` to refresh an existing pending CL after
      rebase instead of creating a new one
- [ ] Windows polish: UTF-8 output, long-path awareness, exit codes audited

## M6 — Hardening (as needed, driven by real use)

- [ ] Binary/large file types: verify reconcile picks correct p4 filetype;
      add overrides in `.p4gw` if needed
- [ ] Multiple overlay roots in one client (second `.p4gw` elsewhere)
- [ ] CI on GitHub Actions: Linux + Windows build & unit tests
- [ ] Performance pass if reconcile on the subtree is slow (`p4 reconcile -m`
      modtime optimization)

## Risks / open questions

- **Line endings** are the most likely source of "reconcile opened 4,000
  files" disasters. Doctor's `LineEnd`/`autocrlf` check (M1) and the M4
  cross-check are the mitigations; resolve before M4 ships.
- **`allwrite` vs. teammates' expectations:** some studios' tooling assumes
  read-only-until-edit semantics. If `allwrite` is not acceptable on your
  client, gw would need a pre-submit `p4 edit` pass instead — decide in M1.
- **Renames:** delete+add loses P4 integration history. Acceptable for a
  personal workflow at first; `p4 move` upgrade is queued in M4.
- **Exclusive-lock filetypes (`+l`):** common for game assets/binaries.
  Reconcile of such a file can fail if a teammate holds the lock — needs a
  clear error message, test in M4 against typical engine filetypes.
- **Submit-time races:** depot changes between `sync` and `submit` are
  detected by P4 itself at submit; the M5 "re-reconcile" flow is the answer.
  No locking cleverness.
