# p4gateway

**Work in Git. Ship Perforce changelists.**

`gw` is a small Windows command-line tool for developers stuck on a huge
Perforce depot who would rather spend their day in Git. One added line per
subtree in your client view remaps the depot subtrees you care about (say,
`src/` and `config/`) into a *mirror* directory off to the side; the directory
your builds actually use becomes a normal Git repo that Perforce never
touches. `gw` moves state
across that boundary: depot → Git when you sync, Git → a pending changelist
when you ship.

There is no import of P4 history into Git and no server-side anything. The
core insight is that Perforce doesn't care *how* files reached their current
state — so shipping a Git branch is just: write the branch's files into the
mirror, open them with the right `p4 edit/add/delete/move` (Git knows
exactly what changed), and put them in a changelist whose description is
built from your commit messages. You review and submit it from P4V.

## Status

In daily use. All commands (`setup`, `init`, `import`, `prepare`, `status`,
`shelf`, `doctor`) are implemented; `gw integtest run` exercises the whole
workflow end-to-end against a live P4 server (see
[README-integtest.md](README-integtest.md)), with a few edge paths still
awaiting real-workspace checks — see [PLAN.md](PLAN.md) for the roadmap and
[docs/sample-session.md](docs/sample-session.md) for what using it feels like.

## The workflow

```
      you work here               gw's staging area              the team
┌─────────────────────┐  gw import  ┌──────────────┐  any p4 sync  ┌──────────┐
│  Git repo at src/   │ ◄────────── │    mirror    │ ◄──────────── │ Perforce │
│  (P4 never touches  │             │  directory   │               │  depot   │
│   this tree)        │ ──────────► │  (P4-mapped) │ ────────────► │          │
└─────────────────────┘  gw prepare └──────────────┘  p4 submit    └──────────┘
                                                      (you, in P4V)
```

| Command | What it does |
|---|---|
| `gw setup` | Writes the `p4gw.cfg` config template in the current directory — flags prefill it, anything omitted is left as a commented placeholder to edit. Offline: no p4 or git calls. `--force` overwrites. |
| `gw init` | Verifies the client view against `p4gw.cfg` via p4 — failing loudly if the mapping line is missing or wrong — then sets up the Git side: creates the repo (if needed), defaults its local Git identity to your login account (these commits stay local — P4 only sees filesystem state), and commits a starter `.gitignore` and `.gitattributes` (which pins line endings so imports and rebases never fight over CRLF/LF). `--force-git-init` starts the repo over. Never edits your client spec. |
| `gw import` | Commits the mirror's current state — whatever you last synced, with any tool — to the hidden `refs/p4gw/main` ref that tracks pristine depot state (the `origin/main` analog). Your branch is fast-forwarded when it has no local commits; `--rebase` (`-r`) replays local commits on top, and without it divergent commits are left untouched — never stomped. Like `git fetch` / `git pull --rebase`. Imports are incremental: gw persists the `p4 have` state that produced each snapshot (`.git/p4gw/have-<baseline>`) and the next import copies only files whose have revision moved — no mirror walk, no per-file stat pass. The manifest is a pure cache: delete it, or pass `--full`, and import falls back to the full mirror walk and rewrites it. An import that fails partway restores your branch and working tree by itself, and the next run does that full walk instead of trusting the cache. With `import_mode = worktree` in `p4gw.cfg` the snapshot is built in a hidden git worktree under `.git/p4gw/worktree` instead of your own checkout, so import runs even with a dirty tree (it just skips the branch fast-forward until the tree is clean) and a crash can never leave your checkout detached. The worktree self-heals — a stale, deleted, or moved one is recreated and fully recopied on the next import; `git worktree remove --force .git/p4gw/worktree` clears it if you switch back to checkout mode. On Windows, `git config core.longpaths true` avoids MAX_PATH trouble from the deeper path. |
| `gw prepare` | Turns the current branch into a pending P4 changelist: stages the branch's files into the mirror with explicit `p4 edit/add/delete/move` and fills the CL description from your commit messages. You submit it from P4V. By default it ships the whole stack (everything since the import point). Name a slice to ship only part of it: `gw prepare <commit>` ships just that one commit's changes (`commit^..commit`), `gw prepare <base>..<target>` ships that range, and `gw prepare <commit> --stack` ships the whole stack up through it (`baseline..commit`) — none of which need the commit checked out. Slicing pairs with `--shelf` (one reviewable shelf per commit); when two slices touch the same file the shelves overlap, which you resolve in P4 at submit time. Files touched only by earlier, unshipped commits stay out of the slice, and a file an unshipped commit created but the slice edits ships as an add (P4 has no such file yet). `--message` (`-m`) overrides the description. A file that was changed earlier in the branch and then reverted back to the depot content is left out of the changelist, so reverted-within-the-branch files never ship as no-op edits. By default it runs a fast `p4 reconcile -n` over just the files it touched; `--verify` adds the full-subtree reconcile that also catches strays (slower on a big subtree). `--dry-run` (`-n`) prints the exact `p4` operations it would open and stops without touching P4 or the mirror. `--update <CL>` refreshes an existing pending changelist to match the branch instead of creating a new one — it reverts that CL's current opens to restore the mirror, then re-stages, keeping the CL's number and description (use it after a rebase or a review tweak, where a plain `gw prepare` would refuse because the CL's files are still open). `--shelf` builds a P4 shelf instead of a pending changelist — it stages, runs `p4 shelve`, then reverts the opens, so only the shelf is left behind and the mirror ends up untouched (reconstruct it later with `gw shelf import <cl>`). |
| `gw status` | One-screen view of where Git and P4 stand: current branch, commits ahead of / behind the baseline, working-tree cleanliness, the last imported changelist, and any pending changelist — plus the single most useful next step. Read-only; degrades gracefully when P4 isn't reachable. |
| `gw shelf list` | Lists your pending and shelved changelists under the subtree (newest first, shelved ones flagged), so you can pick a CL number to import. By default it shows only the current workspace; `--all` (`-a`) widens to every workspace you own, and `--user <name>` (`-u`) lists another user's changelists (implying `--all`). |
| `gw shelf import <cl>` | Brings a P4 shelf into Git as a new branch off `main`: replays the shelf's changes on top of the latest imported depot state with a git 3-way merge (conflicts surface as normal git markers to resolve). Reads everything with `p4 print` — it never touches the mirror or opens a P4 file. `--branch <name>` (`-b`) overrides the default `shelf-<cl>`. |
| `gw doctor` | Checks the environment, and above all the client view: the depot path must map into the mirror and nothing may map into the Git repo. Also flags an interrupted import. `--verify` additionally compares every mirror file byte-for-byte against its working-tree copy and reports files `gw import` would wrongly skip (stale size+mtime stamps); fix those with `gw import --full`. Run it whenever something smells off. |

Every command takes `--help` (or `-h`) for a description and its full list of options. The global `--verbose` flag (usable before or after the command, e.g. `gw --verbose prepare`) echoes every `git` and `p4` command to stderr as it runs — handy for seeing exactly what gw does against your depot.

Day to day:

```
<sync however you like>          # P4V, p4 sync, your team's sync tool...
gw import --rebase               # absorb it: commit to main, rebase your branch
git switch -c fix-anim-blend     # work normally: branch, commit, rebase, bisect
...
gw prepare                       # ship it: builds the pending CL
<review and submit in P4V>
gw import                        # absorb your own submit into main
```

When a reviewer wants one more tweak, or the depot moved under your still-pending
CL, refresh that same changelist in place instead of building a new one:

```
git commit / git rebase          # make the tweak; maybe gw import --rebase first
gw prepare --update 4821         # revert CL 4821's opens, re-stage the branch into it
<re-review and submit in P4V>
```

`gw prepare --update` keeps the changelist's number and description; a plain
`gw prepare` would refuse here because CL 4821's files are still open.

To hand work off for review without submitting, build a shelf instead:

```
gw prepare --shelf               # stage, shelve, then revert: only the shelf remains
<a teammate runs: gw shelf import <cl>>
```

`gw prepare --shelf` shelves the same content `gw prepare` would have opened,
then reverts the opens, so the mirror is left exactly as it was — there's no
pending changelist cluttering your workspace, just the shelf.

Starting from a shelf instead:

```
gw import                        # make sure main is up to date
gw shelf list                    # find the CL: your pending/shelved changelists
gw shelf import 4821             # branch 'shelf-4821' = the shelf, rebased onto main
<resolve any conflicts, then commit>
gw prepare                       # ship it back as a fresh pending CL
```

A shelf is just pending work on top of a submitted base — the same shape as a
Git feature branch. `gw shelf import` recreates it as a branch off `main`
and replays the shelf's changes with a git 3-way merge, so if the depot moved
on since the shelf was made you resolve it once, in Git, the way you resolve
any rebase. It reads the shelf with `p4 print` and never opens a P4 file or
writes to the mirror, so it's safe to run no matter what's synced or checked
out.

### Using git-branchless

If the repo is managed by [git-branchless](https://github.com/arxanas/git-branchless),
gw works with it instead of around it. It detects branchless automatically (its
`branchless.core.mainBranch` config key) and adapts:

- gw never modifies branchless's config — it just warns if branchless's main
  branch isn't your gw baseline (`main` by default), since that's the trunk
  `gw import --rebase` restacks onto.
- `gw import` accepts a **detached HEAD** — no need to mint a throwaway branch
  to absorb the depot (this works with or without branchless).
- `gw import --rebase` restacks **every visible stack** onto the new depot
  state via `git branchless sync`, not just the commit you happen to be on, and
  records the rewrites so the pre-import commits go obsolete (hidden from the
  smartlog). Without `--rebase`, your stacks are left untouched and you're
  pointed at `gw import --rebase` (or `git sync`).

The golden rules of the mirror workflow: **P4 never touches your working
tree, and you never touch the mirror.** Sync whenever and however you like —
it can't conflict with your Git state. Don't edit files in the mirror or
point builds at it; it belongs to `gw`. Every P4 operation gw runs is scoped
to your subtree (or to explicit file lists), so the rest of the
multi-terabyte depot is never touched.

## Setup

```
cd C:\work\project\src              # the subtree you want to work on in Git
gw setup --depot-path //depot/project/main/src/... --client aaron-dev
```

`setup` writes `p4gw.cfg` (anything not given as a flag is left as a commented
placeholder to edit) and prints the one manual step: for each include, add a
line like

```
//depot/project/main/src/...   //aaron-dev/src/.p4gw/...
```

to your client view (`p4 client`). Later view lines win, so each remap must
come **after** any broader line it overlaps — but it does **not** have to be
the last line in the view, so several remaps (e.g. `src` and `config`) and
other custom mappings coexist fine as long as none of them overlaps your
subtrees. Then:

```
gw init
```

verifies that mapping against the live client spec — it fails loudly if the
view line is missing or wrong — and creates the Git repo. Sync, and run
`gw import` to create the `main` baseline. `gw doctor` re-checks
everything whenever you like.

Note for an existing synced workspace: after the view line is added, the
next sync removes the old copies from `src/` (they now belong at the mirror)
— that's expected; `gw import` then populates the Git repo from the mirror.

## Building

Requires CMake ≥ 3.25 and a C++23 compiler (Visual Studio 2022 on Windows;
also builds with GCC 13+ / Clang 17+ on Linux for development and CI). No
external dependencies.

```
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The binary lands at `build\Release\gw.exe` (MSVC) or `build/gw`.

At runtime, `gw` needs `git` and `p4` on your `PATH` and a working P4
connection (`P4PORT`/`P4USER`/`P4CLIENT` or a `.p4config`). Run `gw doctor`
to check.

## Configuration

`gw setup` writes a `p4gw.cfg` file at the root of the overlay repo
(`key = value`, `#` comments):

```
# Each 'include' ties a depot subtree (scoping every p4 command) to the
# mirror directory the client view remaps it into. The mirror always lives
# under the repo's single '.p4gw' container; its path below the container is
# the working-tree directory the subtree occupies ('.p4gw/src' -> 'src/',
# '.p4gw' -> the whole repo). Add one 'include' line per subtree; the starter
# .gitignore tracks only the mapped subtrees, so unmapped directories (bin/,
# content/) stay out of Git. At least one include required.
#   include = <depot_path ending in /...>  <mirror_path>
include = //depot/yourproject/src/...     .p4gw/src
include = //depot/yourproject/config/...  .p4gw/config

# 'include' and 'exclude' lines form an ordered view, resolved later-wins per
# path - just like a p4 client view. An 'exclude' carves a depot subtree out of
# an earlier 'include': the client view drops it (a '-' line) or syncs it in
# place, and gw keeps it out of the mirror and gitignores it - exactly like
# unmapped depot content (bin/, content/), even though it lives under a mapped
# subtree. Intermix them freely, and a later 'include' deeper than an 'exclude'
# maps that part back into the mirror (the win64-yes-linux-no pattern). Each
# path ends in '/...'; each exclude must fall under a preceding include.
exclude = //depot/yourproject/src/thirdparty/...
exclude = //depot/yourproject/src/lib/...
include = //depot/yourproject/src/lib/public/win64/...  .p4gw/src/lib/public/win64

# Optional 'ignore' lines add extra .gitignore patterns (verbatim gitignore
# syntax), one per line. The allowlist tracks a whole mapped subtree, but P4
# ignores build output and IDE state that would otherwise land in Git - list
# those here so Git skips them too. Depot-specific, so share them with your team.
ignore = /src/.vs/
ignore = /src/**/*.vcxproj
ignore = /src/**/*.pdb

# P4 client name; omit to use the ambient P4CLIENT.
client = aaron-dev

# Name for the baseline that tracks pristine depot state. Default: main.
# gw keeps the canonical depot state on the hidden ref refs/p4gw/<name> and
# fast-forwards a like-named local branch to it for convenience.
baseline_branch = main

# How 'gw import' builds the depot snapshot. Default: checkout.
#   checkout  Stage it in your own working tree (needs a clean tree).
#   worktree  Stage it in a hidden git worktree instead, so import never
#             touches your checkout and works even with a dirty tree — it
#             just skips bringing your branch up until the tree is clean.
#             Costs roughly one extra copy of the source on disk.
#import_mode = worktree
```

`p4gw.cfg` is personal (it names your client); the starter `.gitignore` is an
allowlist — `/*` then a `!/src/` line per include — so Git tracks only the
mapped subtrees and everything else (the `.p4gw/` mirror, `p4gw.cfg` itself,
and any unmapped depot content that synced in place) stays out of Git. The
include/exclude rules are applied to the allowlist in order, later-wins: each
`exclude` adds a matching re-exclusion (e.g. `/src/thirdparty/`) so a carved-out
directory stays out of Git, and a deeper re-`include` opens part of it back up
(`!/src/lib/public/win64/`) — the same nesting Git itself understands. Each
`ignore`
line appends its pattern verbatim after the allowlist, so files P4 skips (build
output, IDE state) that would otherwise be tracked under a mapped subtree stay
out of Git. To keep a directory that is Git-only (never in P4), add a
`!/yourdir/` line. gw never opens `p4gw.cfg` or `.gitignore` in a changelist.

`gw init` also commits a starter `.gitattributes` with `* -text`. P4 is the
source of truth for file contents: gw copies the mirror's bytes into the working
tree exactly as P4 synced them (your client's `LineEnd` decides — CRLF on
Windows), and `-text` tells git to store those bytes **verbatim** — no
text-vs-binary guessing, no CRLF↔LF translation. The blob is byte-for-byte what
P4 has, so git and P4 never disagree about a file, and because every commit
stores the same bytes there are no CRLF/LF conflicts. Because `gw init` commits
`.gitattributes` before the first import, every depot snapshot carries it, so
the policy is always in effect when import stages — nothing depends on the
ambient `core.autocrlf`. If you already have a `.gitattributes`, gw keeps it;
`gw doctor` warns when nothing pins EOL for all paths.

This assumes everyone's P4 client uses the same `LineEnd` — the right choice for
an all-Windows (CRLF) team. A mixed CRLF/LF team would instead want
`* text=auto`, which normalizes every blob to LF regardless of client.

### Complex client views

A subtree need not sync as one solid block. Two patterns are supported:

- **Per-platform / peer carve-outs.** You can exclude directories from the
  client view and re-include their peers — keep `win64/` but drop the `linux/`
  and `osx/` peers, say. As long as the bulk of the subtree still remaps into
  the mirror and nothing re-includes into the repo outside it, `gw init`
  accepts the view; the absent peers simply never appear. No config needed.
- **Unmapped directories under a mapped subtree.** Some directories under
  `src/` (vendored `thirdparty/`, generated `devtools/`) may belong to P4 but
  not to your Git history. List them as `exclude` lines: gw leaves them out of
  the mirror, gitignores them, and ships nothing through them — they behave
  just like the top-level unmapped directories (`bin/`, `content/`), even
  though they live under `src/`. `gw init` requires the declaration: a view
  line that diverts part of a mapped subtree out of the mirror is flagged until
  you add the matching `exclude`, so gw never gitignore-tracks a p4-owned
  directory. (This holds even when the repo sits at the client root, where a
  generic "maps into the repo" check can't fire because everything syncs under
  the root.)
- **Re-including part of an excluded directory.** When you *do* want a slice of
  a carved-out subtree back — keep `src/lib/public/win64/` while the rest of
  `src/lib/` stays out — follow the `exclude` with a deeper `include` that maps
  it into a nested mirror (`.p4gw/src/lib/public/win64`). Because the rules are
  ordered and resolved later-wins, exactly like the p4 view they parallel, the
  deeper include overrides the exclude for just that subtree; gw tracks it,
  ships it, and the surrounding `src/lib/` stays carved out.
