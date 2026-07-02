# What working with gw feels like

A walkthrough of a typical couple of days using `gw`, as terminal
transcripts. The shape of the output matches the implemented commands; the
commit contents are deliberately trivial. `C:\work\project\src>` is the
source subtree inside a much larger P4 workspace, and
`C:\work\project\src\.p4gw` is the mirror directory (a gitignored
subdirectory of the repo) the client view routes that subtree into.

## One-time setup

Two halves: `gw setup` writes the config (offline — works on the train),
then `gw init` verifies your client view actually matches it.

```
C:\work\project\src> gw setup --depot-path //depot/project/main/src/... --client aaron-dev
Wrote C:\work\project\src\p4gw.cfg

Next steps:
1. Add a remap line to your client view (p4 client) for each include:

     //depot/project/main/src/... //aaron-dev/<workspace-relative path of .p4gw>/...

   so the depot subtree syncs into the mirror instead of this
   directory. Later view lines win, so each remap must come after
   any broader line it overlaps.
2. Run 'gw init' to verify the include(s) and set up the Git repo.
```

You add `//depot/project/main/src/... //aaron-dev/src/.p4gw/...` to the view
and run init, which checks it against the live spec before touching anything:

```
C:\work\project\src> gw init
ok    client view maps all 1 include(s) into the mirror
Initialized empty Git repository in C:\work\project\src
Set the repo's Git identity to 'aaron' (local account - these commits stay local; P4 only sees filesystem state)
Enabled core.untrackedCache (speeds up 'gw import' file scans; per-repo, no background process)
Wrote starter .gitignore
Wrote starter .gitattributes (pins line endings so imports and rebases never fight over CRLF/LF)
Mirror directory C:\work\project\src\.p4gw does not exist yet - it appears on the first sync.

All set. Sync (any tool you like), then run 'gw import' to build the 'main' baseline.
```

(Had you forgotten the view line, init would have refused with the exact
line to add — that's its job.) You sync with the team's sync tool like
always — and the subtree lands in the mirror. (If `src/` was previously
synced the old way, that sync also removes the old copies from it: they
live in the mirror now.) Then:

```
C:\work\project\src> gw import
[+  0.0s] Reading P4 state...
[+  0.4s] Listing mirror files under C:\work\project\src\.p4gw...
[+  1.1s] Querying p4 have for //depot/project/main/src/......
[+  3.9s] Importing //depot/project/main/src/... (38114 mirror file(s) to scan, 0 to delete)...
[+ 41.3s] Staging snapshot in Git...
[+ 88.6s] Snapshot staged. Updating branch...
Imported depot state to 'refs/p4gw/main' (38114 file(s) updated, 0 deleted)
You are on 'main'. Start work with: git switch -c <branch>
```

(The pristine depot state lives on the hidden ref `refs/p4gw/main`; the local
`main` branch is a convenience pointer kept fast-forwarded to it.) From here
on, you never run `p4 edit` and Perforce never writes into your working
tree. You just use Git.

## Day 1, morning: pick up the team's changes

Sync with whatever you like — P4V, `p4 sync`, the team tool syncing each
directory to its own known-good CL. Whenever you feel like absorbing it:

```
C:\work\project\src> gw import
  ...progress lines...
Imported depot state to 'refs/p4gw/main' (214 file(s) updated, 3 deleted)
You are on 'main'. Start work with: git switch -c <branch>
```

It's `git fetch` (and later, with `--rebase`, `git pull --rebase`) with the
depot as the remote. Sync timing doesn't matter: a sync can land while
you're mid-edit on any branch, because it only ever writes to the mirror.
(Re-imports are far quicker than the first one: unchanged files are skipped
by a stat, not recopied.)

## Working: just Git

```
C:\work\project\src> git switch -c fix-anim-blend
Switched to a new branch 'fix-anim-blend'

C:\work\project\src> git commit -am "Clamp blend weights before normalization"
[fix-anim-blend 3f1c2aa] Clamp blend weights before normalization

C:\work\project\src> git commit -am "Add regression test for zero-weight blend"
[fix-anim-blend 91d04be] Add regression test for zero-weight blend
```

Lost track of where things stand? `gw status` is the one-screen answer:

```
C:\work\project\src> gw status
gw status

  Branch       fix-anim-blend
  Baseline     main - 2 ahead
  Working tree clean
  Last import  CL 481467
  Pending CL   none

Next: Ship your work - open a Perforce changelist with:  gw prepare
```

Mid-afternoon, a teammate pings you: "did you break the cooker?" You check
in seconds, because it's just Git:

```
C:\work\project\src> git stash && git switch main   # pristine depot state
C:\work\project\src> ...build, reproduce: nope, broken in the depot too...
C:\work\project\src> git switch fix-anim-blend && git stash pop
```

That's the overlay's quiet superpower: a known-pristine copy of the depot
state is always one `git switch` away, without touching the server.

## Day 2: ship it

Absorb the latest depot state first so the changelist is built against it:

```
C:\work\project\src> gw import --rebase
  ...progress lines...
Imported depot state to 'refs/p4gw/main' (89 file(s) updated, 0 deleted)
Rebased 'fix-anim-blend' onto the new depot state.
```

(If the rebase had conflicted, gw would stop and leave you in a normal
`git rebase` conflict — fix, `git rebase --continue`, carry on. No P4 state
is involved yet, so nothing can be half-shipped.)

Now turn the branch into a pending changelist:

```
C:\work\project\src> gw prepare
Created pending changelist 481469
  edit    anim/Blend.cpp
  edit    anim/Blend.h
  add     anim/tests/BlendZeroWeight.cpp
Verified the changed files match the mirror.
(For a full check that also catches unexpected changes elsewhere in the subtree, rerun with --verify.)

Changelist 481469 is ready - review and submit it from P4V (or: p4 submit -c 481469).
After it is submitted, run 'gw import' to absorb the new depot state.
```

Because the diff comes from Git, gw runs the exact `p4 edit/add/delete/move`
calls — no reconcile guessing. The "Verified" line is the safety net anyway:
a scoped `p4 reconcile -n` confirming the files gw touched hold precisely
the branch state (add `--verify` to scan the whole subtree for strays too).
If a stray build artifact or hand edit had leaked into the mirror, gw
would print a loud warning here instead of letting a 4,000-file surprise
into the depot. Not sure what a prepare will do? `gw prepare --dry-run`
prints exactly these operations and stops without touching P4 or the
mirror.

You review the CL in P4V — it looks exactly like a changelist a tidy
colleague would make, description stitched from your commit messages — and
submit it there. Then:

```
C:\work\project\src> gw import --rebase
  ...progress lines...
Imported depot state to 'refs/p4gw/main' (3 file(s) updated, 0 deleted)
Rebased 'fix-anim-blend' onto the new depot state.
```

The branch's commits melt away during that rebase — the depot already has
them — leaving `fix-anim-blend` sitting on the new baseline, ready to
delete. Your teammates neither know nor care that Git was involved.

## Handing work off: shelves

Want a teammate's eyes on it before submitting? Build a shelf instead of a
pending changelist — same staging, but gw shelves it and reverts the opens,
so nothing is left cluttering your workspace:

```
C:\work\project\src> gw prepare --shelf
Created pending changelist 481502
  edit    anim/Blend.cpp
Verified the changed files match the mirror.
(For a full check that also catches unexpected changes elsewhere in the subtree, rerun with --verify.)

Shelved changelist 481502 is ready - unshelve or review it from P4V (or: p4 unshelve -s 481502).
The mirror and working tree are unchanged. Reconstruct it in Git any time with 'gw shelf import 481502'.
```

On the other side (or on your other machine), a shelf comes back into Git as
a branch — `gw shelf list` to find the CL, then:

```
C:\work\project\src> gw shelf import 481502
  edit    anim/Blend.cpp

Imported shelved CL 481502 onto new branch 'shelf-481502' (1 file(s)).
Work on it, then 'gw prepare' to build a fresh pending changelist.
```

If the depot moved on since the shelf was made, the replay is a git 3-way
merge, so any conflict surfaces as normal git markers to resolve once — in
Git, not in P4.

## When something's off: doctor

A month later, a sync mysteriously writes files into `src/` again. Instead
of spelunking, you run:

```
C:\work\project\src> gw doctor
ok    git found: git version 2.45.1
ok    p4 found
ok    p4gw.cfg found at C:\work\project\src (1 include(s))
      //depot/project/main/src/... -> .p4gw
ok    mirror directory exists: C:\work\project\src\.p4gw
ok    repo directory is owned by the current user
ok    .gitattributes pins line endings for all paths
ok    p4 connection works
FAIL  the effective mapping for //depot/project/main/src/... is
      '//depot/project/main/... //aaron-dev/...'; expected
      '//depot/project/main/src/... //aaron-dev/src/.p4gw/...'
      (place it after any view line it overlaps - later lines win)
ok    line endings pinned by .gitattributes (core.autocrlf=false is not consulted)
ok    no files opened under the configured mappings

1 check(s) failed, 0 warning(s).
```

Someone "helpfully" reset your client spec and dropped the remap line. Two
minutes, fixed. (Unrelated custom view lines are ignored — doctor only cares
about mappings that involve the subtree or point into the Git repo.)

## The feel, in one paragraph

Git is your editor-adjacent daily driver; Perforce is a mirror directory
that fills up when you sync and empties your branch into a changelist when
you ship. `import` whenever you've synced, `prepare` when a branch is ready,
submit in P4V, `doctor` when suspicious. Everything in between — branching,
stashing, bisecting, rewriting history until the commits tell a clean
story — is stock Git, and none of it touches the server, the mirror, or
anyone else's workflow.
