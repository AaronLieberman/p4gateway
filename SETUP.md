# Setting up gw

Build the tool, point it at your depot, run the first import. Once the
binary is on your `PATH`, the setup itself is one config file and one
client-view line.

## Build

Requires CMake ≥ 3.25 and a C++23 compiler (Visual Studio 2022 on Windows;
GCC 13+ / Clang 17+ also work and are used for development and CI on Linux).
No external dependencies.

```
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The binary lands at `build\Release\gw.exe` (MSVC) or `build/gw`. Put it on
your `PATH`.

At runtime, `gw` needs `git` and `p4` on your `PATH` and a working P4
connection (`P4PORT`/`P4USER`/`P4CLIENT` or a `.p4config`). `gw doctor`
checks all of this.

## 1. Write the config

```
cd C:\work\project\src              # the subtree you want to work on in Git
gw setup --depot-path //depot/project/main/src/... --client aaron-dev
```

This writes `p4gw.cfg`; anything not given as a flag is left as a commented
placeholder to edit. See the
[configuration reference](INSTRUCTIONS.md#configuration) for the full file
format — including mapping several subtrees into one repo and carving
directories out of a subtree.

## 2. Add the client-view line

For each `include` in the config, add a line like

```
//depot/project/main/src/...   //aaron-dev/src/.p4gw/...
```

to your client view (`p4 client`). This is the remap that sends the subtree
to the `.p4gw` mirror instead of `src/` itself. Later view lines win, so the
remap must come **after** any broader line it overlaps — but it does **not**
have to be the last line in the view; several remaps and other custom
mappings coexist fine as long as none of them overlaps your subtrees.

## 3. Initialize

```
gw init
```

This verifies the mapping against the live client spec — failing loudly if
the view line is missing or wrong (it never edits your client spec) — then
creates the Git repo and commits a starter `.gitignore` and `.gitattributes`.

## 4. Sync and import

Sync however you like, then:

```
gw import
```

That creates the `main` baseline from the mirror. You're set — branch off
`main` and work. See [INSTRUCTIONS.md](INSTRUCTIONS.md) for the day-to-day
commands.

**Note for an existing synced workspace:** after the view line is added, the
next sync removes the old copies from `src/` (they now belong at the mirror)
— that's expected; `gw import` then populates the Git repo from the mirror.

Run `gw doctor` whenever something smells off; it re-checks everything above.
