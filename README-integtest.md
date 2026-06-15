# Running the integration tests

`gw integtest` drives the full workflow — `init`, `import`, `prepare`,
`doctor`, submits, rebases — against a real Perforce server. It can't run in
CI (it needs a live server and a configured workspace), so it lives here as a
manual step. The setup below runs a throwaway p4d server in WSL2 and runs the
tests natively on Windows against it.

The P4 client root is the **`p4gw-test` directory under this repo root**
(`p4gateway/p4gw-test`) — it's already gitignored, and `integtest init`
deletes everything under it, so the whole P4 workspace stays self-contained
there and never collides with the Git repo.

## Part 1 — Start a p4d server in WSL2

### Install p4d (one time)

```bash
mkdir -p ~/p4root
cd ~/p4root
curl -O https://cdist2.perforce.com/perforce/r25.1/bin.linux26x86_64/p4d
chmod +x p4d
echo "integtest" > server.id
```

Create a startup script:

```bash
cat > ~/p4root/start-p4d.sh << 'EOF'
#!/bin/sh
cd ~/p4root
exec ./p4d -r ~/p4root -p 1666
EOF
chmod +x ~/p4root/start-p4d.sh
```

### Start the server

```bash
~/p4root/start-p4d.sh
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
`p4.exe` anywhere that's on your `PATH`. Verify with:

```
p4 --version
```

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
   //depot/src/...   //integtest-ws/.p4gw/mirror/src/...
   ```

Save and close. This remaps the `src` subtree into the mirror, which is the
mapping `gw init` verifies. If you get the depot path wrong, `gw integtest
init` prints the exact line to use when its mapping check fails — fix it with
`p4 client` and re-run.

## Part 5 — Build gw

From the repo root:

```
cmake -S . -B build
cmake --build build --config Release
```

The binary lands at `build\Release\gw.exe`. Add it to your `PATH`, or pass
`--gw <path>` to `gw integtest` to point at it explicitly.

## Part 6 — Run the tests

From inside `p4gw-test`:

```
cd p4gw-test
gw integtest init
gw integtest run
```

`integtest init` builds the depot-side fixture and writes the config — it
**deletes everything under the current directory** except `p4.ini`, so never
keep anything of value there. Re-run it any time you want a clean slate.

`integtest run` drives the full workflow. On failure it prints the step that
failed and the error; add `--verbose` to echo every command and its output:

```
gw integtest run --verbose
```

## Resetting

- **Local state and depot fixture:** re-run `gw integtest init`.
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
