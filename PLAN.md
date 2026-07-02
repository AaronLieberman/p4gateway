# PLAN.md — p4gateway implementation roadmap

The goal is a reliable, boring tool: thin typed wrappers around the `git` and
`p4` CLIs, a handful of commands, and loud failures instead of clever
recovery. Each milestone leaves the tool buildable, tested, and useful on its
own. Check items off as they land.

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
- [x] `gw import`: clean-tree check, build the depot snapshot off a detached
      checkout of the hidden ref `refs/p4gw/main` (orphan baseline on the
      very first import; legacy repos seed the ref from the baseline branch's
      last import commit), copy mirror → working tree / delete vanished
      tracked files, commit (best-effort CL label), advance the ref, then
      fast-forward / `--rebase` the current branch and the convenience branch
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

**Needs real-workspace verification** (Windows + live P4 server): `p4 change
-i` spec round-trip, `p4 move` semantics, reconcile-preview output parsing,
the doctor checks against a real spec. `gw integtest run` automates that
check on such a machine (see README-integtest.md); the unit tests cover the
pure logic without p4.

## Next up — prioritized

The short list, in order. Items promoted from the milestones below or added
by the 2026-07 design review.

1. [ ] `gw prepare --update <CL>`: refresh an existing pending CL after a
       rebase or review feedback instead of creating a new one. Today the
       answer to "reviewer wants one more tweak" or "depot moved under my
       pending CL" is revert-by-hand + re-prepare, and prepare's opened-files
       preflight (correctly) refuses to run until then — the sharpest edge
       left in daily use.
2. [ ] Unique temp-file names: `p4gw_prepare_cmp.tmp`, `p4gw_change_spec.txt`,
       and the `p4gw_shelf_*` files use fixed names in the shared temp
       directory, so two concurrent gw runs (or two users on a shared /tmp)
       can collide. Suffix with the PID (or use a properly unique name).
       Cheap.
3. [ ] `gw import` builds its snapshot in a hidden git worktree pinned to
       `refs/p4gw/<baseline>` instead of detaching the user's checkout.
       Today import rewrites the working tree twice (detach onto the old
       snapshot, then switch back), requires a clean tree even for the
       fetch-equivalent half, and a crash mid-import leaves HEAD detached.
       A worktree keeps full .gitignore/.gitattributes semantics (the
       `git add -A` flow is unchanged) while the user's checkout is never
       touched — only `--rebase`/fast-forward would need a clean tree.
       Pairs with the incremental-import-via-have-manifest design note in
       M4; together a no-change import is one p4 query and a no-op commit.
4. [ ] Replace popen with `CreateProcessW` on Windows (and `posix_spawn` on
       POSIX): separate stdout/stderr, no shell quoting risks (cmd.exe still
       expands `%` inside double quotes), real exit codes (POSIX `pclose`
       returns the raw wait status), native stdin/stdout plumbing. Separate
       streams also let several merged-output string heuristics ("no file(s)
       to reconcile", "not opened") get simpler or go away.

## M2 — Make it trustworthy

- [>] Replace popen with `CreateProcessW`/`posix_spawn` — moved to the
      prioritized list above
- [~] `--dry-run`: `gw prepare --dry-run` does all its read-only planning
      (git diff → p4 ops, route check, opened-files guard) and prints the exact
      `p4` operations it would open, then stops before creating the changelist
      or touching the mirror. Still to extend to `import` and the other
      mutating commands.
- [x] `--verbose` (global, before or after the command) echoes every spawned
      `git`/`p4` command line to stderr from the process layer, so it covers
      every command uniformly
- [ ] `gw doctor` against a real Windows P4 workspace catches each
      misconfiguration when introduced deliberately (exit criterion)
- [ ] Cross-check `p4 opened -c <CL>` after prepare against the git diff
      file list (belt to the reconcile-preview suspenders)

## M3 — `gw status` + quality of life

- [x] `status`: branch, ahead/behind-of-baseline counts, dirty-file count,
      last imported CL (parsed from the baseline commit), pending CLs created
      by gw, and a single "next step" suggestion. Read-only; the Git side and
      last-import CL work offline, the pending-CL line degrades to a note when
      P4 is unreachable. Pure decision/format logic in `statusview` is unit
      tested. **Needs real-workspace check**: pending-CL count parsing against
      live `p4 opened` output.
- [x] `gw shelf import <CL>`: recreate a shelved changelist as a branch off
      `main`, replaying the shelf's delta with a git 3-way merge (base =
      `p4 print //depot/f#rev`, theirs = `p4 print //depot/f@=CL`, ours =
      baseline tip). Binary types take the shelved content wholesale; conflicts
      leave git markers for the user to resolve and commit. Read-only on P4 —
      never opens a file or touches the mirror, so it's independent of mirror
      sync/opened state. Parsing (`-ztag describe -S`) and depot→repo mapping
      are pure and unit tested. `gw integtest run` covers the happy path
      (edit + add shelved against head, clean merge), which exercises the
      `#rev` base-revision assumption on a live server. **Still needs a
      real-workspace check** for binary files, the conflict (shelf based on
      an older depot state) path, and LineEnd effects on `p4 print` content
      (print skips the client LineEnd translation sync performs, so an LF
      base/theirs against a CRLF working tree could conflict on every line -
      the same translation caveat as the have-manifest note in M4).
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
      mirror. `gw integtest run` covers it (shelf has the branch's files, no
      opens remain, the mirror is back at the depot head). **Needs a
      real-workspace check** on a live server.
- [>] `gw prepare --update <CL>` — moved to the prioritized list above
- [ ] `gw prepare --abandon <CL>`: `p4 revert -c` + scoped `p4 sync -f` to
      restore the mirror to depot state
- [ ] Helpful error for the "depot changed under my pending CL" case
      (suggest import + rebase + re-prepare)
- [ ] Windows polish: UTF-8 output, long-path awareness, exit codes audited

## M4 — Hardening (as needed, driven by real use)

- [ ] Binary/large file types: verify `p4 add` picks the correct filetype;
      overrides in `p4gw.cfg` if needed
- [ ] Exclusive-lock filetypes (`+l`): clear error when a teammate holds the
      lock
- [ ] Rename chains (rename onto a freed path, rename + re-add of source):
      detect and fall back to delete+add with a note
- [ ] Multiple overlay roots in one client (second `p4gw.cfg` elsewhere)
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
- [ ] Incremental `import` via a persisted `p4 have` manifest (design):
      store the per-file `//depot/file#rev` manifest that produced each
      baseline snapshot (e.g. `.git/p4gw/have-<baseline>`, keyed by the
      snapshot's commit SHA, written only after the ref advances). The next
      import diffs a fresh `p4 have` (sub-second even at 40k files) against
      it: rev changed / file added -> copy that mirror file, gone -> delete,
      unchanged -> skip with no stat. Content still comes from the mirror,
      so LineEnd/`+k` byte fidelity is untouched (unlike the p4-print
      variant below), and the per-file manifest sidesteps that variant's
      no-single-baseline-CL trap outright. The touched-path list also lets
      the `git add` be scoped instead of `-A` (import requires a clean tree,
      so its own copies are the only changes). Strictly a cache with a
      fallback: a missing or SHA-mismatched manifest (first import, crash
      before the manifest write, hand-moved ref) falls back to today's full
      walk, and `--full` forces it - correctness never depends on the cache.
      Deliberate semantic shift: an in-place mirror edit whose rev did not
      change is no longer absorbed into the baseline (today it leaks into
      the "pristine" ref; with the manifest it surfaces at prepare's
      reconcile canary instead) - arguably more faithful, but worth stating.
      Client LineEnd/filetype changes without a rev bump are the `--full`
      cases. Opened files keep today's depot-head reads (planImport +
      printHeadToFile - import already never takes an opened file's mirror
      bytes), so local changes stay out of the baseline; writable-but-
      unopened (tampered) files need nothing extra, since the default
      noclobber makes sync refuse to overwrite them, their have rev never
      advances, and the manifest diff skips them - the baseline keeps the
      clean bytes of the earlier import. Caveat on the depot-head reads:
      `p4 print` does NOT apply the client LineEnd translation the way sync
      does (text files come back in the server's LF form, and `+k` keyword
      expansion differs), so printed text files likely need an explicit
      LF->CRLF translation by filetype (`p4 opened -ztag` carries the type,
      the spec carries LineEnd; isBinaryType exists). Verify on a real
      workspace first: `p4 print -q -o` a text file and byte-compare it to
      the synced mirror copy - identical means no translation step needed.
      Pairs with the hidden-worktree item in "Next up": together a
      no-change import is one p4 query, a text diff, and a no-op commit.
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
      -risk alternative. Needs a real workspace to verify byte fidelity.
      The have-manifest variant above captures most of the performance win
      without the print-fidelity traps.
- [x] CI on GitHub Actions: Linux + Windows build & unit tests
      (.github/workflows/ci.yml)
- [ ] Wishlist: scripted end-to-end run in CI (init → import → branch →
      import --rebase → prepare). Requires a disposable p4 server (p4d) in
      the CI container — even `gw init` needs a live client spec — which
      isn't worth setting up right now; parked until it is.

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
- **Disk cost**: the subtree exists twice (mirror + repo) plus Git objects.
  Acceptable for a source tree; not intended for art/binary subtrees.
- **First import on an existing Git repo** creates the baseline as an
  orphan branch, so `--rebase` of pre-existing unrelated history will
  replay everything. Migration of a pre-gw repo is a one-time manual
  operation; document rather than automate.
- **Renames** ship as `p4 move` (edit + move). Chained/overlapping renames
  in one branch may need the M4 fallback.
- **Submit-time races**: depot changes between `import` and submit are
  detected by P4 itself at submit; the M3 "re-prepare" flow is the answer.
  No locking cleverness.
- **Personal vs shareable config**: the `include`/`exclude` view and `ignore`
  patterns are depot knowledge a team would want to share, but `p4gw.cfg` is
  personal (it names the client) and different people may legitimately map
  different subtrees. Deliberately unsolved for now — needs more mileage on
  the system to see what teams actually end up sharing before picking a
  split (committed view file vs personal client file, or similar).
