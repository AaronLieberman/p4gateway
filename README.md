# p4gateway

**Work in Git. Submit to Perforce.**

`gw` is a small Windows command-line tool for developers stuck on a huge
Perforce depot who would rather spend their day in Git. It automates the
*overlay workflow*: a normal Git repo lives inside one subtree of your P4
workspace (say, `src/`), you do all your daily work with Git branches and
rebases, and when a change is ready you hand it back to Perforce as a clean
numbered changelist.

There is no import of P4 history into Git and no server-side anything. The
core insight is that Perforce doesn't care *how* files reached their current
state — so shipping a Git branch is just: make the working tree match the
branch, run `p4 reconcile` scoped to your subtree, and put the opened files
in a changelist whose description is built from your commit messages. `gw`
automates that, plus the bookkeeping around it.

## Status

Early development. The CLI skeleton, config handling, and environment checks
build and run; the core workflows are being implemented milestone by
milestone — see [PLAN.md](PLAN.md) for the roadmap and current state.

## The workflow

```
            you work here                        the rest of the team
   ┌────────────────────────────┐             ┌──────────────────────┐
   │  Git branches inside the   │   gw sync   │                      │
   │  src/ subtree of your P4   │ ◄────────── │   Perforce depot     │
   │  workspace                 │ ──────────► │                      │
   └────────────────────────────┘  gw submit  └──────────────────────┘
```

| Command | What it does |
|---|---|
| `gw init` | Sets up the Git overlay inside your synced P4 workspace subtree: writes the `.p4gw` config, creates the Git repo and a `p4-main` baseline branch from the current synced state. |
| `gw sync` | Runs `p4 sync` (scoped to your subtree), commits the result to `p4-main`, and offers to rebase your feature branch onto it. |
| `gw submit` | Turns the current branch into a pending P4 changelist: reconciles your subtree against the depot and fills the CL description from your commit messages. `--shelve` shelves it (for Swarm review); `--submit` submits it. |
| `gw status` | One-screen view of where Git and P4 stand: branch, commits ahead of baseline, dirty files, last synced CL, pending CLs. |
| `gw doctor` | Checks the environment for the classic overlay footguns: missing tools, P4 client not `allwrite`, line-ending mismatches between `core.autocrlf` and the client `LineEnd`. |

Day to day:

```
gw sync                      # morning: pick up the team's changes
git switch -c fix-anim-blend   # work normally: branch, commit, rebase, bisect
...
gw submit --shelve           # ship it: review the CL in P4/Swarm, then submit
```

The golden rule of the overlay workflow: **never `p4 edit` files you're
working on in Git.** Perforce only gets involved at the boundaries (`sync`
and `submit`), and every P4 operation is scoped to your subtree so the rest
of the multi-terabyte depot is never touched.

## Building

Requires CMake ≥ 3.25 and a C++23 compiler (Visual Studio 2022 on Windows;
also builds with GCC 13+ / Clang 17+ on Linux for development and CI). No
external dependencies.

```
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

The binary lands at `build\Release\gw.exe` (MSVC) or `build/gw`.

At runtime, `gw` needs `git` and `p4` on your `PATH` and a working P4
connection (`P4PORT`/`P4USER`/`P4CLIENT` or a `.p4config`). Run `gw doctor`
to check.

## Configuration

`gw init` writes a `.p4gw` file at the root of the overlay repo
(`key = value`, `#` comments):

```
# Which slice of the depot this Git repo overlays. Every p4 command is
# scoped to this path — required.
depot_path = //depot/yourgame/src/...

# P4 client name; omit to use the ambient P4CLIENT.
client = aaron-dev

# Git branch that tracks pristine depot state. Default: p4-main
baseline_branch = p4-main
```

Add `.p4gw` to the depot's ignore rules (or your client's exclusions) so it
never gets reconciled into a CL.
