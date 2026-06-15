# p4gateway

**Work in Git. Ship Perforce changelists.**

`gw` is a small Windows command-line tool for developers stuck on a huge
Perforce depot who would rather spend their day in Git. One added line in
your client view remaps the depot subtree you care about (say, `src/`) to a
*mirror* directory off to the side; the directory your builds actually use
becomes a normal Git repo that Perforce never touches. `gw` moves state
across that boundary: depot → Git when you sync, Git → a pending changelist
when you ship.

There is no import of P4 history into Git and no server-side anything. The
core insight is that Perforce doesn't care *how* files reached their current
state — so shipping a Git branch is just: write the branch's files into the
mirror, open them with the right `p4 edit/add/delete/move` (Git knows
exactly what changed), and put them in a changelist whose description is
built from your commit messages. You review and submit it from P4V.

## Status

Early development. `init`, `import`, `prepare`, and `doctor` are implemented
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
| `gw import` | Commits the mirror's current state — whatever you last synced, with any tool — to the `p4-main` baseline branch. `--rebase` then rebases your feature branch onto it. Like `git fetch` / `git pull --rebase`. |
| `gw prepare` | Turns the current branch into a pending P4 changelist: stages the branch's files into the mirror with explicit `p4 edit/add/delete/move` and fills the CL description from your commit messages. You submit it from P4V. `--no-verify` skips the reconcile-preview safety check. |
| `gw status` | One-screen view of where Git and P4 stand (not implemented yet). |
| `gw doctor` | Checks the environment, and above all the client view: the depot path must map into the mirror and nothing may map into the Git repo. Run it whenever something smells off. |

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
placeholder to edit) and prints the one manual step: add a line like

```
//depot/game/main/src/...   //aaron-dev/src/.p4gw/...
```

at the **end** of your client view (`p4 client`). Later view lines win, so
this routes the subtree into the mirror while the rest of your view stays
untouched; other custom mappings are fine as long as they don't involve the
subtree. Then:

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
# Which slice of the depot this Git repo overlays. Every p4 command is
# scoped to this path — required.
depot_path = //depot/yourgame/src/...

# Where the client view maps depot_path to — gw's staging area, synced by
# p4 and never edited by hand. Relative paths are resolved against the
# directory containing this file.
mirror_path = .p4gw

# P4 client name; omit to use the ambient P4CLIENT.
client = aaron-dev

# Git branch that tracks pristine depot state. Default: p4-main
baseline_branch = p4-main
```

`p4gw.cfg` is personal (it names your client); the starter `.gitignore` keeps
it — and the mirror directory (`.p4gw/`, p4's staging area) — out of Git, and
gw never opens it (or `.gitignore`) in a changelist.
