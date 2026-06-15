# Running the integration tests

`gw integtest` drives the full workflow — `init`, `import`, `prepare`,
`doctor`, submits, rebases — against a real Perforce server. It can't run in
CI (it needs a live server and a configured workspace), so it lives here as a
manual step. Run it on Windows, against a p4d server in WSL2.

## Part 1 — Start a p4d server in WSL2

### Install p4d (one time)

```bash
mkdir -p ~/p4root
cd ~/p4root
curl -O https://cdist2.perforce.com/perforce/r25.1/bin.linux26x86_64/p4d
chmod +x p4d
```

### Start the server

```bash
~/p4root/p4d -r ~/p4root -p 1666
```

Leave this terminal open — p4d runs in the foreground. `Ctrl-C` stops it.
To reset the server state completely, stop it, `rm -rf ~/p4root/*` (keep the
binary), and restart.

### Find your WSL2 IP

```bash
hostname -I | awk '{print $1}'
```

You'll need this address for the Windows side.

## Part 2 — Configure the Windows p4 client (one time)

Make sure `p4.exe` is on your `PATH`. Then tell the p4 client where to find
`p4.ini` files:

```
p4 set P4CONFIG=p4.ini
```

Create a P4 user (only needed once per fresh server):

```
p4 -p <WSL2_IP>:1666 -u <your_username> user -f
```

Accept the defaults in the editor that opens (save and close it).

Create a P4 client whose root is the directory that will contain `p4gw-test`.
Pick a path you can write to, e.g. `C:\work\p4`:

```
p4 -p <WSL2_IP>:1666 -u <your_username> -c <client_name> client
```

In the editor set `Root` to your chosen path (e.g. `C:\work\p4`) and add a
view line that maps the depot into it:

```
//depot/...   //<client_name>/...
```

Save and close. Then add the remap line for the test's `src` subtree — this
is the critical line that `gw init` verifies. Add it at the **end** of the
view (later lines win):

```
//depot/p4gw-test/src/...   //<client_name>/p4gw-test/.p4gw/mirror/src/...
```

If you're not sure of the exact depot path yet, `gw integtest init` will
print the exact line to add when it fails the mapping check — run it once,
copy the line it shows, add it with `p4 client`, and re-run.

## Part 3 — Set up the test directory

Create `p4gw-test` under your client root and put a `p4.ini` in it:

```
mkdir C:\work\p4\p4gw-test
```

`C:\work\p4\p4gw-test\p4.ini`:

```ini
P4PORT=<WSL2_IP>:1666
P4USER=<your_username>
P4CLIENT=<client_name>
```

Verify the connection from inside the directory:

```
cd C:\work\p4\p4gw-test
p4 info
```

You should see your server address, user, and client.

## Part 4 — Build gw

From the repo root:

```
cmake -S . -B build
cmake --build build --config Release
```

The binary lands at `build\Release\gw.exe`. Add it to your `PATH`, or use
`--gw <path>` when calling `gw integtest` to point at it explicitly.

## Part 5 — Run the tests

From inside `p4gw-test`:

```
cd C:\work\p4\p4gw-test
gw integtest init
gw integtest run
```

`integtest init` resets the depot fixture and sets up the config — it
**deletes everything under the current directory** except `p4.ini`, so never
keep anything of value there. Re-run it any time you want a clean slate.

`integtest run` drives the full workflow. On failure it prints the step that
failed and the error. Add `--verbose` to see every command and its output:

```
gw integtest run --verbose
```

## Resetting

To reset just the local state and depot fixture, re-run `integtest init`.

To reset the entire server (all history, users, clients), stop p4d, wipe
the server root, and restart:

```bash
# in WSL2
^C                   # stop p4d
rm -rf ~/p4root/*
~/p4root/p4d -r ~/p4root -p 1666
```

Then redo Part 2 (create user and client) and Part 5.
