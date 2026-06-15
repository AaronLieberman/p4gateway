# p4gateway (gw)

The repo and project are named **p4gateway**; the command-line binary it
builds is **`gw`** (short, easy to type). The internal C++ namespace and
static library keep the `p4gw` prefix (`p4gw::`, `p4gw_core`), as does the
`.p4gw` config file — only the user-facing executable is `gw`.

A Windows command-line tool, written in C++23, that lets a developer work in
Git locally and ship Perforce changelists. The architecture is the **mirror
workflow**: one client view line remaps the depot subtree to a mirror
directory, so every p4 sync lands in the mirror and the canonical directory
is purely a Git repo. `gw import` commits mirror state to the `p4-main`
baseline branch (like `git fetch`/`git pull --rebase`); `gw prepare` stages
the current branch into the mirror with explicit `p4 edit/add/delete/move`
(driven by the git diff, verified by a scoped `p4 reconcile -n`) and builds
a pending CL that the user submits from P4V — gw itself never submits.
Getting started is split in two: `gw setup` writes the `.p4gw` template
offline; `gw init` verifies the client view against it via p4 (hard failure
on a wrong mapping) and creates the Git repo.
**There is no history import and no bidirectional bridge** — P4 only ever
sees filesystem state, which is the whole trick. Read README.md for the
user-facing workflow and PLAN.md for the implementation roadmap before
making significant changes.

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
  in `src/process.cpp`; everything else stays portable.
- No external dependencies. Standard library only (`std::expected`,
  `std::filesystem`). Don't introduce a dependency without asking.

## Architecture

```
src/main.cpp        CLI entry: parses <command> and dispatches; keep it dumb
src/commands.h      command signatures (one int cmdX(const Args&) each)
src/commands/*.cpp  one file per subcommand (setup, init, import, prepare, status, doctor, integtest)
src/process.{h,cpp} subprocess runner — the ONLY place that spawns processes
src/git.{h,cpp}     thin typed wrappers over the git CLI
src/p4.{h,cpp}      thin typed wrappers over the p4 CLI + pure client-view checks
src/p4ops.{h,cpp}   pure mapping: git diff -> ordered p4 operations (prepare)
src/mirror.{h,cpp}  mirror <-> working tree sync actions (import)
src/config.{h,cpp}  .p4gw config file (key = value), found by walking parents
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
  `p4 sync` against a game-studio depot can run for hours and open thousands
  of files — treat an unscoped p4 call as a bug even when it happens to work.
- P4 owns the mirror directory; gw owns moving state across the boundary.
  Never write code that lets p4 touch the Git repo directory, and never let
  gw absorb mirror state while it could be mid-sync without flagging it.
- Destructive operations (anything that reverts, deletes, or overwrites user
  work in Git or P4) must be opt-in via an explicit flag, never the default.

## Testing

- Unit-test pure logic (parsing, config, CL description building) with the
  harness in `tests/test_framework.h`; register with `TEST(name)` / `CHECK`.
- Code that talks to real `git`/`p4` binaries is kept thin precisely so it
  needs little testing; don't try to mock the p4 server. `p4` is usually NOT
  installed in the dev/CI environment — never write a test that requires it.
- A real end-to-end check needs a Windows machine with a P4 workspace; flag
  changes that need that in your summary rather than claiming they're verified.
  `gw integtest init|run` automates exactly that check on such a machine
  (see PLAN-integrationtests.md) — it is never run by ctest or CI.
