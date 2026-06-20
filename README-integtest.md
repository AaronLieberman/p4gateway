# Running the integration tests

`gw integtest run` drives the full workflow — `init`, `import`, `prepare`,
`doctor`, submits, rebases — against a real Perforce server. It can't run in
CI (it needs a live server and a configured workspace), so it lives here as a
manual step. The setup below runs a throwaway p4d server in WSL2 and runs the
tests natively on Windows against it.

> **⚠️ Destructive — throwaway servers only.** Every run **obliterates** the
> test depot files and **wipes** everything under the current directory (except
> `p4.ini`/`.p4config`). Only ever point it at a dedicated, disposable p4d you
> control — never a shared or studio server. `gw` enforces this: it refuses to
> run unless the server's `Server ID` is `p4gw-integtest-throwaway` **and** its
> security level is `0` (a fresh, unsecured server). There is no override.

The P4 client root is the **`p4gw-test` directory under this repo root**
(`p4gateway/p4gw-test`) — it's already gitignored, and `integtest run`
deletes everything under it, so the whole P4 workspace stays self-contained
there and never collides with the Git repo.

## Part 1 — Start a p4d server in WSL2

### Install p4d (one time)

```bash
mkdir -p ~/p4root
cd ~/p4root
curl -O https://cdist2.perforce.com/perforce/r25.1/bin.linux26x86_64/p4d
chmod +x p4d
echo "p4gw-integtest-throwaway" > server.id
```

The `server.id` value is **not** cosmetic: `gw integtest` reads it back as
`Server ID` and refuses to run unless it is exactly `p4gw-integtest-throwaway`.
This is the safety interlock that stops the destructive run from ever touching
a real server, so use this value verbatim.

Create a startup script that lives next to `p4d` in the server root. It uses
its own location as the root, so you can run it from any directory (and the
whole server root can move) without it assuming `~/p4root`:

```bash
cat > ~/p4root/start-p4d.sh << 'EOF'
#!/bin/sh
# Throwaway p4d launcher. Uses this script's own directory as the server
# root, so it runs from anywhere and travels with the root if it moves.
root="$(cd "$(dirname "$0")" && pwd)"
exec "$root/p4d" -r "$root" -p 1666
EOF
chmod +x ~/p4root/start-p4d.sh
```

### Start the server

```bash
~/p4root/start-p4d.sh         # from any directory
```

Leave this terminal open — p4d runs in the foreground; `Ctrl-C` stops it.
(See [Resetting](#resetting) to wipe server state and start fresh.)

### Find your WSL2 IP

```bash
hostname -I | awk '{print $1}'
```

You'll use this address as `P4PORT` on the Windows side. Note it doesn't
change while WSL is running, but can change across WSL restarts.

## Part 2 — Install p4.exe on Windows (one time)

Download the single-file Windows client from Perforce and drop it on your PATH:

```powershell
curl -o "$env:USERPROFILE\AppData\Local\Microsoft\WindowsApps\p4.exe" `
     https://cdist2.perforce.com/perforce/r25.1/bin.ntx64/p4.exe
```

`WindowsApps` is already on the PATH for most Windows installs; otherwise copy
`p4.exe` anywhere that's on your `PATH`. Verify it's on the PATH by running the
binary (no shell-specific syntax needed):

```
p4 -V
```

This prints the client version banner. If you instead get a "command not
found" error, `p4.exe` isn't on your `PATH` yet.

## Part 3 — Create the test directory and connection config

From the repo root, create the `p4gw-test` directory and a `p4.ini` inside it.
This is both the P4 client root and where you run the tests.

```
mkdir p4gw-test
```

`p4gw-test\p4.ini` (pick any user and client name — they get created next):

```ini
P4PORT=<WSL2_IP>:1666
P4USER=integtest
P4CLIENT=integtest-ws
```

Tell p4 to discover `p4.ini` by walking up from the current directory (one
time, global):

```
p4 set P4CONFIG=p4.ini
```

From here on, run every `p4` and `gw` command from **inside `p4gw-test`** so
the connection settings are picked up automatically:

```
cd p4gw-test
p4 info
```

`p4 info` should report your server address and the user/client names from
`p4.ini`. (It works even though that user and client don't exist yet — it's
just confirming the connection.)

## Part 4 — Create the P4 user and client (one time)

A fresh server has no security configured, so this needs no password. Create
the user from its own generated template (no editor needed):

```
p4 user -o | p4 user -i
```

Then create the client. Its `Root` defaults to the current directory
(`p4gw-test`), which is what we want, so just add the remap line:

```
p4 client
```

In the editor that opens:

1. Confirm `Root:` is your `...\p4gateway\p4gw-test` path.
2. Leave the default view line (`//depot/...  //integtest-ws/...`).
3. Add this remap line at the **end** of the `View:` section (later lines
   win, so it must come after the default line):

   ```
   //depot/src/...   //integtest-ws/.p4gw/src/...
   ```

Save and close. This remaps the `src` subtree into the mirror, which is the
mapping `gw init` verifies. If you get the depot path wrong, `gw integtest run`
prints the exact line to use when its mapping check fails — fix it with
`p4 client` and re-run.

## Part 5 — Build gw

From the repo root:

```
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The binary lands at `build\Release\gw.exe`. The tests below run from inside
`p4gw-test`, so invoke it by relative path as `..\build\Release\gw.exe` (or
add it to your `PATH` and just type `gw`).

## Part 6 — Run the tests

From inside `p4gw-test`:

```
cd p4gw-test
..\build\Release\gw.exe integtest run
```

That single command does everything: it confirms the server is a throwaway,
resets the fixture (locally **and** in the depot), drives the full workflow,
and — on success — cleans up after itself by obliterating the depot files and
wiping the local tree. Because it resets at the start, it is safe to run
repeatedly; there is no separate `init` step anymore.

`integtest run` **deletes everything under the current directory** except
`p4.ini`/`.p4config`, so never keep anything of value there. If it finds a file
it doesn't recognize it refuses and names it; pass `--force` to wipe anyway.

On failure it prints the step that failed and the error, and leaves the state
in place for inspection; add `--verbose` to echo every command and its output:

```
..\build\Release\gw.exe integtest run --verbose
```

To recover after a failed or interrupted run, use the `clean` subcommand — it
skips the tests and *only* cleans up: revert opens, drop leftover changelists,
obliterate the depot files, and wipe the local tree:

```
..\build\Release\gw.exe integtest clean
```

Running `gw integtest` with no subcommand prints usage and exits non-zero.

Flags (apply to `run` unless noted):

- `--leave` — keep the built repo/depot state instead of cleaning up at the
  end (handy for poking around after a successful run). `run` only.
- `--force` — proceed past the stray-file guard described above.
- `--verbose` — echo every command and its output.

## Resetting

- **Local state and depot fixture:** just re-run `gw integtest run` (it resets
  itself), or `gw integtest clean` to clean up without running the tests.
- **The entire server** (all history, users, clients): stop p4d, wipe the
  server data, and restart, keeping the binary, script, and `server.id`:

  ```bash
  # in WSL2
  ^C                   # stop p4d
  find ~/p4root -mindepth 1 \
    -not -name p4d -not -name start-p4d.sh -not -name server.id -delete
  ~/p4root/start-p4d.sh
  ```

  Then redo Part 4 (recreate the user and client) and Part 6.
