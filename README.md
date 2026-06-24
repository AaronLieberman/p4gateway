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

Early development. `init`, `import`, `prepare`, `status`, and `doctor` are implemented
and exercised end-to-end on the Git side; verification against a real P4
server is in progress — see [PLAN.md](PLAN.md) for the roadmap and
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
| `gw init` | Verifies the client view against `p4gw.cfg` via p4 — failing loudly if the mapping line is missing or wrong — then sets up the Git side: creates the repo (if needed) and commits a starter `.gitignore`. `--force-git-init` starts the repo over. Never edits your client spec. |
| `gw import` | Commits the mirror's current state — whatever you last synced, with any tool — to the hidden `refs/p4gw/p4-main` ref that tracks pristine depot state (the `origin/main` analog). Your branch is fast-forwarded when it has no local commits; `--rebase` replays local commits on top, and without it divergent commits are left untouched — never stomped. Like `git fetch` / `git pull --rebase`. |
| `gw prepare` | Turns the current branch into a pending P4 changelist: stages the branch's files into the mirror with explicit `p4 edit/add/delete/move` and fills the CL description from your commit messages. You submit it from P4V. `--no-verify` skips the reconcile-preview safety check; `--dry-run` prints the exact `p4` operations it would open and stops without touching P4 or the mirror. |
| `gw status` | One-screen view of where Git and P4 stand: current branch, commits ahead of / behind the baseline, working-tree cleanliness, the last imported changelist, and any pending changelist — plus the single most useful next step. Read-only; degrades gracefully when P4 isn't reachable. |
| `gw shelf list` | Lists your pending and shelved changelists under the subtree (newest first, shelved ones flagged), so you can pick a CL number to import. |
| `gw shelf import <cl>` | Brings a P4 shelf into Git as a new branch off `p4-main`: replays the shelf's changes on top of the latest imported depot state with a git 3-way merge (conflicts surface as normal git markers to resolve). Reads everything with `p4 print` — it never touches the mirror or opens a P4 file. `--branch <name>` overrides the default `shelf-<cl>`. |
| `gw doctor` | Checks the environment, and above all the client view: the depot path must map into the mirror and nothing may map into the Git repo. Run it whenever something smells off. |

The global `--verbose` flag (usable before or after the command, e.g. `gw --verbose prepare`) echoes every `git` and `p4` command to stderr as it runs — handy for seeing exactly what gw does against your depot.

Day to day:

```
<sync however you like>          # P4V, p4 sync, your studio's sync tool...
gw import --rebase               # absorb it: commit to p4-main, rebase your branch
git switch -c fix-anim-blend     # work normally: branch, commit, rebase, bisect
...
gw prepare                       # ship it: builds the pending CL
<review and submit in P4V>
gw import                        # absorb your own submit into p4-main
```

Starting from a shelf instead:

```
gw import                        # make sure p4-main is up to date
gw shelf list                    # find the CL: your pending/shelved changelists
gw shelf import 4821             # branch 'shelf-4821' = the shelf, rebased onto p4-main
<resolve any conflicts, then commit>
gw prepare                       # ship it back as a fresh pending CL
```

A shelf is just pending work on top of a submitted base — the same shape as a
Git feature branch. `gw shelf import` recreates it as a branch off `p4-main`
and replays the shelf's changes with a git 3-way merge, so if the depot moved
on since the shelf was made you resolve it once, in Git, the way you resolve
any rebase. It reads the shelf with `p4 print` and never opens a P4 file or
writes to the mirror, so it's safe to run no matter what's synced or checked
out.

### Using git-branchless

If the repo is managed by [git-branchless](https://github.com/arxanas/git-branchless),
gw works with it instead of around it. It detects branchless automatically (its
`branchless.core.mainBranch` config key) and adapts:

- `gw init` points branchless's main branch at the gw baseline (`p4-main`), so
  the depot state is your trunk and the smartlog hides import commits.
- `gw import` accepts a **detached HEAD** — no need to mint a throwaway branch
  to absorb the depot.
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
cd C:\work\game\src              # the subtree you want to work on in Git
gw setup --depot-path //depot/game/main/src/... --client aaron-dev
```

`setup` writes `p4gw.cfg` (anything not given as a flag is left as a commented
placeholder to edit) and prints the one manual step: for each include, add a
line like

```
//depot/game/main/src/...   //aaron-dev/src/.p4gw/...
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
`gw import` to create the `p4-main` baseline. `gw doctor` re-checks
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
include = //depot/yourgame/src/...     .p4gw/src
include = //depot/yourgame/config/...  .p4gw/config

# Optional 'exclude' lines carve a depot subtree out of the 'include' above
# them. The client view either drops it (a '-' line) or syncs it in place,
# and gw keeps it out of the mirror and gitignores it - exactly like unmapped
# depot content (bin/, content/), even though it lives under a mapped subtree.
# Each must end in '/...' and lie under its include's depot path.
exclude = //depot/yourgame/src/thirdparty/...
exclude = //depot/yourgame/src/devtools/...

# P4 client name; omit to use the ambient P4CLIENT.
client = aaron-dev

# Name for the baseline that tracks pristine depot state. Default: p4-main.
# gw keeps the canonical depot state on the hidden ref refs/p4gw/<name> and
# fast-forwards a like-named local branch to it for convenience.
baseline_branch = p4-main
```

`p4gw.cfg` is personal (it names your client); the starter `.gitignore` is an
allowlist — `/*` then a `!/src/` line per include — so Git tracks only the
mapped subtrees and everything else (the `.p4gw/` mirror, `p4gw.cfg` itself,
and any unmapped depot content that synced in place) stays out of Git. Each
`exclude` line adds a matching re-exclusion (e.g. `/src/thirdparty/`) so a
carved-out directory under a mapped subtree stays out of Git too. To keep a
directory that is Git-only (never in P4), add a `!/yourdir/` line. gw never
opens `p4gw.cfg` or `.gitignore` in a changelist.

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
