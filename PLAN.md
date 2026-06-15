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
//depot/yourgame/src/...   //client/src/.p4gw/...
```

Every sync — `p4 sync`, P4V, the studio's own sync tool, at any time, in any
state — lands in the mirror. The directory at the canonical `src/` path is
*purely* a Git repo that P4 never touches, while builds still see files in
the right place. gw moves state across the boundary explicitly:

- `gw import` commits the mirror's current state to the baseline branch
  (`p4-main`), like `git fetch`; `--rebase` rebases the current branch onto
  it, like `git pull --rebase`.
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
- [x] Process runner: stdin/stdout file redirection (for `p4 change -i` and
      binary-safe blob writes via `git cat-file blob`)
- [x] `gw setup`: write the `p4gw.cfg` template (placeholders + comments for
      anything not given as a flag; `--force` overwrites) — offline, no p4
      or git calls
- [x] `gw init`: require `p4gw.cfg` (point at `gw setup` if missing), verify
      the client view via p4 — hard failure on a wrong mapping or dead
      connection — then git init if needed (`--force-git-init` starts the
      repo over) + starter `.gitignore`; never edits the client spec
- [x] `gw import`: clean-tree check, switch to baseline (orphan-created on
      first import), copy mirror → working tree / delete vanished tracked
      files, commit (best-effort CL label), switch back, `--rebase`
- [x] `gw prepare`: ancestor preflight, CL from commit messages
      (`--message` override), explicit `p4 delete/edit/move/add` against the
      mirror in dependency order, `p4 reconcile -n` verification
      (`--no-verify` to skip), prints "submit from P4V" — no submit path
- [x] `gw doctor`: client-view consistency (depot path → mirror, nothing
      mapped into the repo dir, unrelated mappings ignored), LineEnd vs
      core.autocrlf, opened files under the depot path
- [x] Unit tests: view-spec parsing/checking, git-diff → p4-ops mapping,
      mirror copy/delete computation, config keys

**Needs real-workspace verification** (Windows + live P4 server): `p4 change
-i` spec round-trip, `p4 move` semantics, reconcile-preview output parsing,
the doctor checks against a real spec. Everything Git-side is exercised by a
scripted end-to-end run (init → import → branch → import --rebase →
conflict/abort) on Linux.

## M2 — Make it trustworthy

- [ ] Replace popen with `CreateProcessW` on Windows (and `posix_spawn` on
      POSIX): separate stdout/stderr, no shell quoting risks, real exit
      codes, native stdin/stdout plumbing
- [ ] `--dry-run` global flag: every mutating command prints the exact
      `p4`/`git` commands it would run
- [ ] `--verbose` echoing every spawned command line
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
- [ ] `gw prepare --update <CL>` to refresh an existing pending CL after a
      rebase instead of creating a new one
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
- [ ] `import` performance on big subtrees (skip copies by size/mtime
      instead of copying everything)
- [ ] CI on GitHub Actions: Linux + Windows build & unit tests

## Risks / open questions

- **The remap line disappearing from the client spec** (hand edits, spec
  regeneration) would make the next sync write into the Git repo again.
  `gw doctor`'s view check is the mitigation; run it when anything smells
  off. The studio sync tool is known not to rewrite the spec.
- **Line endings**: the mirror is written with the client `LineEnd` and
  committed by git on import, then written back from blobs on prepare. The
  doctor warning plus the reconcile-preview canary are the mitigations;
  revisit if a real workspace shows churn.
- **Mirror tampering** (builds writing into it, hand edits): import would
  absorb the damage into `p4-main` and prepare's reconcile preview would
  flag the residue. Treat the mirror as gw's — never point editors or
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
