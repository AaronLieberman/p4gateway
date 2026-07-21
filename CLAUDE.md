# p4gateway (gw)

The repo and project are named **p4gateway**; the command-line binary it
builds is **`gw`** (short, easy to type). The internal C++ namespace and
static library keep the `p4gw` prefix (`p4gw::`, `p4gw_core`), as does the
`p4gw.cfg` config file — only the user-facing executable is `gw`.

A Windows command-line tool, written in C++23, that lets a developer work in
Git locally and ship Perforce changelists. The architecture is the **mirror
workflow**: one client view line per `include` remaps a depot subtree into the
repo's `.p4gw` mirror container, so every p4 sync lands in the mirror and the
canonical directory is purely a Git repo. The config is a list of
`include`/`exclude` lines — an **ordered view, resolved later-wins per path,
exactly like a p4 client view** (see `effectiveRuleFor*` in `config.cpp`). One
repo can ship several subtrees (e.g. `src/` and `config/`); the mirror path
below `.p4gw` is the working-tree subtree an `include` feeds. An `exclude =
<depot_subpath>` carves a directory back out of an earlier include (vendored
`thirdparty/`, generated `devtools/`): the client view drops it or syncs it in
place, and gw gitignores it and ships nothing through it — it behaves like the
top-level unmapped directories even though it lives under a mapped subtree.
Rules may be intermixed freely, and a later `include` **deeper** than an
`exclude` re-includes that subtree back into a nested mirror (the
win64-yes-linux-no pattern: `exclude src/lib`, then `include
src/lib/public/win64`). An `include` depot path ends in `/...` (recursive) or
`/*` (single-level — only the files directly in that directory, the p4 view
wildcard); a `/*` include pairs with a recursive `exclude` to keep a
directory's own files while dropping its sub-directories (`exclude src/build`,
then `include src/build/*`). The recursive-vs-single-level bit is carried on
`ViewRule::recursive` and threaded through `effectiveRuleFor*` (single-level
covers only direct children) and `buildGitignore` (a `/src/build/*/` line
re-excludes the child dirs). Excludes are always `/...`. The view check tolerates all of this plus per-platform
peer carve-outs done purely in the client view (keep `win64/`, drop its
`linux/` peer); its one hard rule is that nothing may map into the repo outside
a mirror unless an `exclude` declares it. The starter `.gitignore` `gw init`
writes is an allowlist (`/*` then `!/src/`…) built from the same ordered rules:
Git tracks only the mapped subtrees, each `exclude` adds a re-exclusion
(`/src/thirdparty/`), and a re-`include` opens its slice back up
(`!/src/lib/public/win64/`); unmapped depot content that syncs in place stays
out of Git unless the user re-includes a directory by hand (`!/dir/`). `gw import` commits mirror state to the
hidden depot-tracking ref `refs/p4gw/<baseline>` — the `origin/main` analog —
and keeps a like-named local branch fast-forwarded to it for convenience
(like `git fetch`/`git pull --rebase`); `gw prepare` stages
the current branch into the mirror with explicit `p4 edit/add/delete/move`
(driven by the git diff, verified by a scoped `p4 reconcile -n`) and builds
a pending CL that the user submits from P4V — gw itself never submits.
Getting started is split in two: `gw setup` writes the `p4gw.cfg` template
offline; `gw init` verifies the client view against it via p4 (hard failure
on a wrong mapping) and creates the Git repo.
**There is no history import and no bidirectional bridge** — P4 only ever
sees filesystem state, which is the whole trick. Read README.md for the
pitch, INSTRUCTIONS.md for the user-facing workflow and command/config
reference, SETUP.md for first-time setup, and PLAN.md for the implementation
roadmap before making significant changes.

## Build and test

```
cmake -S . -B build
cmake --build build            # add --config Debug/Release with MSVC
ctest --test-dir build -C Release --output-on-failure
build/gw --help                # build\gw.exe on Windows
```

- Primary target is Windows/MSVC (VS 2022), but the code must also build
  with g++/clang on Linux — that is how it is verified in CI and in remote
  Claude Code sessions. Keep platform-specific code behind `#ifdef _WIN32`
  in `src/subprocess.cpp`; everything else stays portable.
- No external dependencies. Standard library only (`std::expected`,
  `std::filesystem`). Don't introduce a dependency without asking.

## Architecture

```
src/main.cpp        CLI entry: parses <command> and dispatches; keep it dumb
src/commands.h      command signatures (one int cmdX(const Args&) each)
src/commands/*.cpp  one file per subcommand (setup, init, import, prepare, status, shelf, doctor, integtest)
src/subprocess.{h,cpp} subprocess runner — the ONLY place that spawns processes
                    (not named process.h: that shadows the CRT header MSVC's
                    <thread> needs)
src/git.{h,cpp}     thin typed wrappers over the git CLI
src/p4.{h,cpp}      thin typed wrappers over the p4 CLI + pure client-view checks
src/p4ops.{h,cpp}   pure mapping: git diff -> ordered p4 operations (prepare)
src/mirror.{h,cpp}  mirror <-> working tree sync actions (import)
src/shelf.{h,cpp}   pure parsing of p4 changes/describe output (shelf list/import)
src/statusview.{h,cpp}  pure status decision + formatting logic (status)
src/config.{h,cpp}  p4gw.cfg config file (key = value), found by walking parents
tests/              zero-dependency harness (test_framework.h), one file per unit
```

Layering rule: commands call git/p4/config; git and p4 call process; nothing
calls process directly from a command. New git or p4 invocations get a typed
wrapper function, not an inline `run("p4", ...)` call.

## Conventions

- Errors: `std::expected<T, std::string>` everywhere; no exceptions thrown by
  our code. Error strings include the failing command line and its output so
  the user can rerun by hand. Commands return process exit codes (0/1).
- Naming: `camelCase` functions/variables, `PascalCase` types, `kCamelCase`
  constants. Namespaces: `p4gw`, `p4gw::git`, `p4gw::p4`.
- Formatting: 4-space indent, ~90 columns, `{` on the same line.
- Every p4 operation that touches files MUST be scoped to the configured
  `depot_path` or to an explicit file list. An unscoped `p4 reconcile` or
  `p4 sync` against a large project depot can run for hours and open thousands
  of files — treat an unscoped p4 call as a bug even when it happens to work.
- P4 owns the mirror directory; gw owns moving state across the boundary.
  The recommended mirror is `.p4gw/` — a gitignored subdirectory *inside*
  the repo. This is the deliberate exception to "p4 must not write into the
  Git repo directory": the `.p4gw/` subtree is carved out by the client view
  remap line and gitignored, so p4 never touches tracked files. Never write
  code that lets p4 reach *outside* `.p4gw/` into the repo, and never let gw
  absorb mirror state while it could be mid-sync without flagging it.
- Destructive operations (anything that reverts, deletes, or overwrites user
  work in Git or P4) must be opt-in via an explicit flag, never the default.
- Git history: keep it linear — prefer rebasing onto the target branch and
  fast-forward merges over merge commits. When integrating a branch, rebase
  it on the latest target and `git merge --ff-only` (or fast-forward the
  target to it); don't create merge commits.

## Testing

- Unit-test pure logic (parsing, config, CL description building) with the
  harness in `tests/test_framework.h`; register with `TEST(name)` / `CHECK`.
- Code that talks to real `git`/`p4` binaries is kept thin precisely so it
  needs little testing; don't try to mock the p4 server. `p4` is usually NOT
  installed in the dev/CI environment — never write a test that requires it.
- A real end-to-end check needs a Windows machine with a P4 workspace; flag
  changes that need that in your summary rather than claiming they're verified.
  `gw integtest run` automates exactly that check on such a machine
  (see README-integtest.md) — it is never run by ctest or CI.
