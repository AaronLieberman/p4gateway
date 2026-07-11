# PLAN.md — p4gateway implementation roadmap

The goal is a reliable, boring tool: thin typed wrappers around the `git` and
`p4` CLIs, a handful of commands, and loud failures instead of clever
recovery. Each milestone leaves the tool buildable, tested, and useful on its
own. Check items off as they land. Testing gaps and planned test additions
are tracked separately in TESTING-TODO.md.

## The mirror architecture (the load-bearing idea)

A single client view line remaps the depot subtree to a **mirror directory**
elsewhere in the workspace (view lines are ordered and later-wins per depot
file, so one added line is enough):

```
//depot/yourproject/src/...   //client/src/.p4gw/...
```

Every sync — `p4 sync`, P4V, the team's own sync tool, at any time, in any
state — lands in the mirror. The directory at the canonical `src/` path is
*purely* a Git repo that P4 never touches, while builds still see files in
the right place. gw moves state across the boundary explicitly:

- `gw import` commits the mirror's current state to the hidden depot-tracking
  ref `refs/p4gw/main` (the `origin/main` analog), like `git fetch`. Your
  branch fast-forwards when it has no local commits; `--rebase` replays local
  commits onto the new depot state, like `git pull --rebase`. A like-named
  local branch is kept fast-forwarded to the ref for convenience.
- `gw prepare` opens the current branch's changes in a pending changelist by
  staging blobs into the mirror and running explicit `p4 edit/add/delete/
  move` — Git already knows exactly what changed, so no reconcile guessing.
  A scoped `p4 reconcile -n` afterwards is the canary for anything
  unexpected. gw never submits; that happens in P4V.

Because P4 only ever owns the mirror, the client does **not** need
`allwrite`, syncing can never corrupt or collide with Git state, and the
worst external failure (the remap line vanishing from the client spec) is a
`gw doctor` check away.

## M0 — Project scaffold ✅

- [x] CMake project, C++23, no external dependencies, builds with MSVC and GCC
- [x] CLI entry point with command dispatch, `--help`, `--version`
- [x] `process` subprocess runner (popen-based first cut)
- [x] `p4gw.cfg` config: parser, parent-directory search, unit tests
- [x] Thin `git` wrappers: current branch, rev-parse, dirty check,
      `diff --name-status` parsing (with renames), commit-message collection
- [x] Zero-dependency test harness + ctest wiring
- [x] CLAUDE.md, README.md, this plan

## M1 — Mirror architecture: `import` and `prepare` ✅

- [x] `p4gw.cfg` gains `mirror_path` (resolved against the config's directory)
- [x] `p4gw.cfg` becomes an **ordered `include`/`exclude` view, resolved
      later-wins per path** — the same model as a p4 client view (`ViewRule`
      list + `effectiveRuleFor*` in `config.cpp`). Rules intermix freely; an
      `exclude` carves a subtree out of an earlier `include`, and a deeper
      `include` after it re-includes that subtree into a nested mirror
      (win64-yes-linux-no). `buildGitignore`, prepare routing, the opened-file
      filter, and the view check all share the ordered resolvers
- [x] Process runner: stdin/stdout file redirection (for `p4 change -i` and
      binary-safe blob writes via `git cat-file blob`)
- [x] `gw setup`: write the `p4gw.cfg` template (placeholders + comments for
      anything not given as a flag; `--force` overwrites) — offline, no p4
      or git calls
- [x] `gw init`: require `p4gw.cfg` (point at `gw setup` if missing), verify
      the client view via p4 — hard failure on a wrong mapping or dead
      connection — then git init if needed (`--force-git-init` starts the
      repo over) + starter `.gitignore`; never edits the client spec
- [x] `gw import`: build the depot snapshot from the mirror, commit
      (best-effort CL label), advance the hidden ref `refs/p4gw/main`
      (orphan baseline on the very first import; legacy repos seed the ref
      from the baseline branch's last import commit), then fast-forward /
      `--rebase` the current branch and the convenience branch
- [x] `gw prepare`: ancestor preflight, CL from commit messages
      (`--message` override), explicit `p4 delete/edit/move/add` against the
      mirror in dependency order, scoped `p4 reconcile -n` verification by
      default (`--verify` for the full-subtree scan), prints "submit from
      P4V" — no submit path
- [x] `gw doctor`: client-view consistency (depot path → mirror, nothing
      mapped into the repo dir, unrelated mappings ignored), a `.gitattributes`
      pinning EOL for all paths (LineEnd vs core.autocrlf only when it does
      not), opened files under the depot path
- [x] Unit tests: view-spec parsing/checking, git-diff → p4-ops mapping,
      mirror copy/delete computation, config keys

Everything above that needs a live server (the `p4 change -i` round-trip,
`p4 move` semantics, reconcile-preview parsing, the doctor checks) is
exercised end-to-end by `gw integtest run`, which CI runs on a Windows
runner against a throwaway p4d (see README-integtest.md); the unit tests
cover the pure logic without p4.

## Next up — prioritized

The short list, in order — items promoted from the milestones below or added
by the 2026-07 design reviews.

1. [x] `gw prepare --update <CL>`: refresh an existing pending CL after a
       rebase or review feedback instead of creating a new one. Reverts the
       CL's current opens (restoring the mirror to the depot head), then runs
       the normal staging into that same CL — number and description kept.
       A post-revert opened-files re-check still blocks on files open in
       *other* changelists. `--dry-run` previews it. `gw integtest run`
       covers a refresh that adds a file and one that drops it back to the
       original CL.
2. [x] Unique temp-file names, so two concurrent gw runs (or two users on a
       shared /tmp) can't collide — `uniqueTempFile()` in `subprocess.{h,cpp}`
       splices the PID and a per-process counter into every scratch name; all
       call sites (prepare, p4 change spec, shelf, integtest) route through it.
3. [x] `gw import` builds its snapshot in a hidden git worktree pinned to
       `refs/p4gw/<baseline>` instead of detaching the user's checkout, so
       the user's checkout is never rewritten and a crash mid-import can't
       leave HEAD detached. The worktree lives at `.git/p4gw/worktree`,
       persists across imports (its mtime stamps ARE the size+mtime fast
       path), validates on reuse (`git rev-parse HEAD` == old baseline, clean)
       and self-heals (stale → reset+clean+full copy; broken/moved →
       prune+recreate+full copy); the pending marker forces a reset. A dirty
       tree is allowed — the branch fast-forward/rebase half is skipped with
       a note (the ref-only baseline convenience update still runs when the
       user isn't on that branch). First import ever falls back to
       checkout-mode staging (a worktree needs a commit to detach at).
       `gw doctor` reports worktree health and `--verify` compares against
       the worktree. The shared snapshot-build code (`buildSnapshot`) is
       exercised by both modes.
4. [x] Replace popen with `CreateProcessW` on Windows (and `posix_spawn` on
       POSIX): separate stdout/stderr, no shell quoting risks, real exit
       codes, native stdin/stdout plumbing. The runner spawns children
       directly (posix_spawnp with PATH lookup; CreateProcessW with CRT argv
       quoting), captures stdout and stderr on separate pipes drained
       concurrently so a flooded stream can't deadlock, and keeps
       RunResult::output as stdout-then-stderr. Follow-up: RunResult carries
       stdoutText/stderrText, parsers read stdout only, and the "no results"
       notices ("no file(s) to reconcile", "not opened", "not on client")
       are matched against stderr purely to excuse a non-zero exit — which
       also fixed a latent merged-stream bug where a reconcile chunk mixing
       clean and changed files had its real preview lines dropped.
5. [x] Trial `import_mode = worktree` on the real work repo — done (2026-07):
       a week of daily use on the real workspace with no issues, so worktree
       is now the default import mode; `import_mode = checkout` remains as
       the opt-out.
6. [x] Incremental `import` via a persisted `p4 have` manifest:
       `buildSnapshot` reads `.git/p4gw/have-<baseline>` and, when its
       `snapshot` line matches the old depot ref (and `--full`/marker are
       off), replaces the mirror walk + stat pass with a `diffHaveState` of
       fresh `p4 have` revs against the manifest (rev unchanged → skipped
       without a stat; deletes come from the manifest, so the mirror is never
       listed) and scopes the git staging to the touched paths
       (`git add --pathspec-from-file`, NUL-separated). The fresh have state
       is persisted after the ref advances via temp-file + rename, so a torn
       write can't leave a plausible manifest. Works in both import modes;
       opened-file depot-head reads and the gw-metadata delete guard carry
       over. Pure logic (render/parse/diff, path flattening,
       `parseTaggedHave`) is unit-tested; `gw integtest run` covers the fast
       path (exact changed/deleted counts, no mirror-walk line),
       deleted-manifest fallback + rewrite, corrupted-binding fallback, and
       the fast path returning. `gw doctor` reports the manifest's binding
       state.
7. [x] `gw prepare --abandon <CL>` — reverts the CL's opens (the scoped
       `p4 revert -w`, which restores the mirror to the depot head), drops
       any shelf, and deletes the changelist; a standalone cleanup mode that
       stages nothing. Covered by itPrepareAbandon (pending CL + shelved CL).
       (A "depot changed under your pending CL" warning was considered and
       dropped — the head is always moving, so import↔prepare drift is
       expected; real conflicts are resolved on the P4 side, e.g. `p4 sync`
       → resolve → `gw import` → `prepare --update`. Not gw's job.)
8. [ ] Cross-check `p4 opened -c <CL>` after prepare against the git diff
       file list — cheap belt-and-suspenders on top of the reconcile
       preview, plus an integtest assertion.

Deliberately deprioritized for now: Windows polish and the rest of M4
(filetypes, `+l` locks, rename chains, p4ignore derivation) stay
wait-until-it-bites.

Deferred idea — **isolated `prepare --shelf`** (build a shelf without touching
the mirror, working even with files already open in P4). Perforce has no
"reverse `p4 print` to a shelf": a shelf's only content source is opened files
in a client workspace, so today's `--shelf` stages into the mirror, shelves,
then reverts (net-zero on disk) and refuses when files under the mapping are
open. True isolation requires building the shelf in a *separate* p4 workspace —
a dedicated hidden shelving client rooted in a scratch dir: sync just the
changed files there, lay down the git content, open + `p4 shelve`, revert,
leave the shelf. Reuses `writeClientSpec`/`clientViewPath` and per-client `-c`
invocation. Motivating case is shelving a slice *alongside a gw pending CL*
where the files overlap — which only full isolation solves (the lighter
"relax the guard, isolate to gw's own CL" variant doesn't). Tradeoff that
parked it (2026-07): the extra hidden client isn't wanted yet, and the shelf
would be owned by it (submit means unshelving into the normal client from P4V).

## M2 — Make it trustworthy

- [x] `--dry-run`: `gw prepare --dry-run` does all its read-only planning
      (git diff → p4 ops, route check, opened-files guard) and prints the exact
      `p4` operations it would open, then stops before creating the changelist
      or touching the mirror.
- [x] `--verbose` (global, before or after the command) echoes every spawned
      `git`/`p4` command line to stderr from the process layer, so it covers
      every command uniformly
- [x] `gw doctor` catches each deliberate view misconfiguration — a permanent
      `gw integtest run` step rewrites the client view five ways (nothing
      mapped, remap removed, remap shadowed by a later broader line, subtree
      excluded, sub-path diverted out of the mirror) and asserts doctor fails
      with the matching message, restoring the spec around each probe. Known
      gap: the "maps into the Git repo" leak message can't fire when the repo
      is the client root (as in the fixture) — the diversion check is the one
      that covers that case.

## M3 — `gw status` + quality of life

- [x] `status`: branch, ahead/behind-of-baseline counts, dirty-file count,
      last imported CL (parsed from the baseline commit), pending CLs created
      by gw, and a single "next step" suggestion. Read-only; the Git side and
      last-import CL work offline, the pending-CL line degrades to a note when
      P4 is unreachable. Pure decision/format logic in `statusview` is unit
      tested. Not yet exercised by `gw integtest run` — see TESTING-TODO.md.
- [x] `gw shelf import <CL>`: recreate a shelved changelist as a branch off
      `main`, replaying the shelf's delta with a git 3-way merge (base =
      `p4 print //depot/f#rev`, theirs = `p4 print //depot/f@=CL`, ours =
      baseline tip). Binary types take the shelved content wholesale; conflicts
      leave git markers for the user to resolve and commit. Read-only on P4 —
      never opens a file or touches the mirror, so it's independent of mirror
      sync/opened state. Parsing (`-ztag describe -S`) and depot→repo mapping
      are pure and unit tested. `gw integtest run` covers the happy path
      (edit + add shelved against head, clean merge), which exercises the
      `#rev` base-revision assumption on a live server; the binary-file and
      conflict (shelf based on an older depot state) paths are not yet in the
      integtest — see TESTING-TODO.md. (The earlier LineEnd worry — that
      `p4 print` might skip the client LineEnd translation sync performs —
      is settled: the integtest print-fidelity step byte-matches print output
      against synced mirror copies for text and binary files.)
- [x] `gw shelf list`: the caller's pending and shelved CLs (`p4 -ztag
      changes -s pending|shelved -u <user>`, scoped to `depot_path`), newest
      first with shelved ones flagged, to pick one for `shelf import`.
      `parseChanges` is pure/unit-tested; `gw integtest run` asserts a freshly
      shelved CL shows up flagged.
- [x] `gw prepare` skips files that were changed earlier in the branch and
      reverted back to the depot content: the endpoint git diff still lists
      them (their blob differs from the baseline's), but the staged content
      matches the mirror, so opening them would ship a no-op edit. The target
      blob is compared byte-for-byte against the current mirror copy and equal
      ones are dropped before any file is opened.
- [x] `gw prepare --shelf`: build a P4 shelf instead of a pending changelist.
      Stages exactly as a normal prepare, runs `p4 shelve -c <CL>`, then
      `p4 revert -w -c <CL>` to drop the opens (the `-w` deletes added files so
      the mirror is fully restored), leaving only the shelf and an untouched
      mirror. `--update <CL>` refreshes an existing shelf in place
      (`p4 shelve -r`). `gw integtest run` covers both (shelf has the
      branch's files, no opens remain, the mirror is back at the depot head;
      an updated shelf replaces its files).
- [ ] Windows polish: UTF-8 output, long-path awareness, exit codes audited

## M4 — Hardening (as needed, driven by real use)

- [ ] Binary/large file types: verify `p4 add` picks the correct filetype;
      overrides in `p4gw.cfg` if needed
- [ ] Exclusive-lock filetypes (`+l`): clear error when a teammate holds the
      lock
- [ ] Rename chains (rename onto a freed path, rename + re-add of source):
      detect and fall back to delete+add with a note
- [ ] Maybe: `--dry-run` for `import` and the other mutating commands.
      Deprioritized (2026-07): worktree mode removed most of what an import
      dry-run guarded — staging no longer touches the checkout at all — so
      this waits for a concrete need.
- [ ] Auto-derive `.gitignore` from P4's own ignore rules (design): today the
      `ignore` key in `p4gw.cfg` carries per-depot patterns for files P4 skips
      (build output, IDE state) that the allowlist would otherwise track under a
      mapped subtree. A more hands-off option is to read P4's ignore rules
      (`P4IGNORE` / `.p4ignore`) and translate them into the allowlist, scoped
      per mapped subtree — most architecture-aligned ("Git tracks what P4
      tracks") and generic across depots. Traps: p4ignore files cascade
      per-directory and the syntax is close-to-but-not-identical to gitignore;
      it's p4-dependent so it's harder to unit-test in the p4-less CI; and some
      teams keep no `.p4ignore` at all (nothing to derive). Layers on top of the
      `ignore` key rather than replacing it.
- [x] `import` performance on big subtrees: `applySyncActions` skips a copy
      when the working-tree file already matches the mirror in size and mtime
      (rsync's default heuristic), and stamps the mirror's mtime onto each
      copy so unchanged files keep reading as current on the next run. Turns
      a full copy into a stat per file; `git add -A`'s own tree scan is the
      remaining lower bound.
- [x] Incremental `import` via a persisted `p4 have` manifest — built; see
      Next-up item 6 for the implementation summary. Design notes worth
      keeping: it is strictly a cache with a fallback (a missing or
      SHA-mismatched manifest, or `--full`, falls back to the full mirror
      walk — correctness never depends on the cache). Deliberate semantic
      shift: an in-place mirror edit whose rev did not change is no longer
      absorbed into the baseline (it surfaces at prepare's reconcile canary
      instead) — arguably more faithful, but worth stating. Writable-but-
      unopened (tampered) files need nothing extra: the default noclobber
      makes sync refuse to overwrite them, their have rev never advances, and
      the manifest diff skips them. Depot-head reads via `p4 print` are
      verified byte-faithful by the integtest print-fidelity step (text CRLF
      + binary, real Windows workspace, 2026-07). Untested corners: a client
      LineEnd that differs from the platform convention (e.g. 'unix' on
      Windows — not this shop's configuration) and `+k` keyword files, which
      the fixture doesn't carry.
- [ ] `import` from depot instead of mirror (design): read content via
      `p4 print` and fetch only the unique set of files changed in CLs since
      the baseline, rather than reading the mirror filesystem. Would harden
      against mirror tampering and skip the full-tree stat, but inverts the
      "P4 only ever sees filesystem state" principle and brings real traps:
      no single baseline CL exists (mixed-CL syncs) so a per-file have
      manifest must be persisted; `p4 print` content can differ from synced
      bytes (LineEnd translation, `+k` keyword expansion) which `prepare`'s
      diff would flag as spurious churn; and `prepare` still touches the
      mirror, so tampering isn't fully escaped. If the goal is the tampering
      risk specifically, a mirror-vs-`p4 have` verification check is a lower
      -risk alternative. The have-manifest variant above captures most of
      the performance win without the print-fidelity traps.
- [x] CI on GitHub Actions: Linux + Windows build & unit tests
      (.github/workflows/ci.yml)
- [x] Run `gw integtest run` in GitHub Actions
      (.github/workflows/integtest.yml). A windows-latest job fetches a
      pinned p4d/p4 (cached), writes the sentinel server.id, boots p4d on
      localhost in the background, scripts the user/client setup
      (`p4 user -o | p4 user -i`; `p4 client -o`, append the remap line,
      `p4 client -i`), and runs the suite — the Windows runner is the one
      that exercises the CRLF LineEnd path. It is a separate workflow from
      the unit-test CI so a Perforce download hiccup never blocks a normal
      merge.

## Risks / open questions

- **The remap line disappearing from the client spec** (hand edits, spec
  regeneration) would make the next sync write into the Git repo again.
  `gw doctor`'s view check is the mitigation; run it when anything smells
  off. The team sync tool is known not to rewrite the spec.
- **Line endings**: the mirror is written with the client `LineEnd` and
  committed by git on import, then written back from blobs on prepare. `gw
  init` commits a `.gitattributes` (`* -text`) so git stores every blob
  byte-for-byte as P4 synced it (verbatim, no text/binary guessing or CRLF<->LF
  translation) regardless of each machine's `core.autocrlf` - P4 is the source
  of truth, and identical bytes across commits is what keeps two imports (and a
  rebase across them) from disagreeing. init commits the metadata before the
  first import, so every depot snapshot carries `.gitattributes` and the policy
  is always in effect when import stages. `-text` assumes a single client
  LineEnd (an all-Windows CRLF shop); a mixed team wants `* text=auto`. The
  doctor check
  (a `.gitattributes` that pins EOL, else the LineEnd/autocrlf comparison)
  plus the reconcile-preview canary are the mitigations; revisit if a real
  workspace shows churn.
- **Mirror tampering** (builds writing into it, hand edits): `import`
  filters the mirror listing through `p4 have`, so stray files p4 never
  tracked (build output, leftovers from a botched sync) are ignored instead
  of absorbed into `main`. In-place edits to *tracked* files are still
  imported (they're in `have`); prepare's reconcile preview is the canary
  for that residue. Treat the mirror as gw's — never point editors or
  builds at it.
- **Disk cost**: the subtree exists twice (mirror + repo) plus Git objects,
  and worktree mode adds a third copy (the snapshot worktree). Acceptable
  for a source tree; not intended for art/binary subtrees.
- **First import on an existing Git repo** creates the baseline as an
  orphan branch, so `--rebase` of pre-existing unrelated history will
  replay everything. Migration of a pre-gw repo is a one-time manual
  operation; document rather than automate.
- **Renames** ship as `p4 move` (edit + move). Chained/overlapping renames
  in one branch may need the M4 fallback.
- **Submit-time races**: depot changes between `import` and submit are
  detected by P4 itself at submit; recover on the P4 side, then
  `gw import` → rebase → `gw prepare --update`. No locking cleverness.
- **Personal vs shareable config**: the `include`/`exclude` view and `ignore`
  patterns are depot knowledge a team would want to share, but `p4gw.cfg` is
  personal (it names the client) and different people may legitimately map
  different subtrees. Deliberately unsolved for now — needs more mileage on
  the system to see what teams actually end up sharing before picking a
  split (committed view file vs personal client file, or similar).
