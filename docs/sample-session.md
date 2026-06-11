# What working with gw feels like

A walkthrough of a typical couple of days using `gw`, as terminal
transcripts. The shape of the output matches the implemented commands
(`status` is still aspirational; see [PLAN.md](../PLAN.md)); the commit
contents are deliberately trivial. `C:\work\game\src>` is the source subtree
inside a much larger P4 workspace, and `C:\work\game\p4gw-mirror` is the
mirror directory the client view routes that subtree into.

## One-time setup

Two halves: `gw setup` writes the config (offline — works on the train),
then `gw init` verifies your client view actually matches it.

```
C:\work\game\src> gw setup --depot-path //depot/game/main/src/... --client aaron-dev
Wrote C:\work\game\src\.p4gw

Next steps:
1. Add this line at the END of your client view (p4 client):

     //depot/game/main/src/... //aaron-dev/<workspace-relative path of ../p4gw-mirror>/...

   so the depot subtree syncs into the mirror instead of this
   directory. Later view lines win, so keep it last.
2. Run 'gw init' to verify the mapping and set up the Git repo.
```

You add `//depot/game/main/src/... //aaron-dev/p4gw-mirror/...` to the view
and run init, which checks it against the live spec before touching anything:

```
C:\work\game\src> gw init
ok    client view maps //depot/game/main/src/... into the mirror
Initialized empty Git repository in C:\work\game\src
Wrote starter .gitignore
Mirror directory C:\work\game\p4gw-mirror does not exist yet — it appears on the first sync.

All set. Sync (any tool you like), then run 'gw import' to build the 'p4-main' baseline.
```

(Had you forgotten the view line, init would have refused with the exact
line to add — that's its job.) You sync with the studio's sync tool like
always — and the subtree lands in the mirror. (If `src/` was previously
synced the old way, that sync also removes the old copies from it: they
live in the mirror now.) Then:

```
C:\work\game\src> gw import
Committed depot state to 'p4-main' (38,114 files, 0 deleted)
You are on 'p4-main'. Start work with: git switch -c <branch>
```

From here on, you never run `p4 edit` and Perforce never writes into your
working tree. You just use Git.

## Day 1, morning: pick up the team's changes

Sync with whatever you like — P4V, `p4 sync`, the studio tool syncing each
directory to its own known-good CL. Whenever you feel like absorbing it:

```
C:\work\game\src> gw import
Committed depot state to 'p4-main' (214 files, 3 deleted)
You are on 'p4-main'. Start work with: git switch -c <branch>
```

It's `git fetch` (and later, with `--rebase`, `git pull --rebase`) with the
depot as the remote. Sync timing doesn't matter: a sync can land while
you're mid-edit on any branch, because it only ever writes to the mirror.

## Working: just Git

```
C:\work\game\src> git switch -c fix-anim-blend
Switched to a new branch 'fix-anim-blend'

C:\work\game\src> git commit -am "Clamp blend weights before normalization"
[fix-anim-blend 3f1c2aa] Clamp blend weights before normalization

C:\work\game\src> git commit -am "Add regression test for zero-weight blend"
[fix-anim-blend 91d04be] Add regression test for zero-weight blend
```

Mid-afternoon, a teammate pings you: "did you break the cooker?" You check
in seconds, because it's just Git:

```
C:\work\game\src> git stash && git switch p4-main   # pristine depot state
C:\work\game\src> ...build, reproduce: nope, broken in the depot too...
C:\work\game\src> git switch fix-anim-blend && git stash pop
```

That's the overlay's quiet superpower: a known-pristine copy of the depot
state is always one `git switch` away, without touching the server.

## Day 2: ship it

Absorb the latest depot state first so the changelist is built against it:

```
C:\work\game\src> gw import --rebase
Committed depot state to 'p4-main' (89 files, 0 deleted)
Rebased 'fix-anim-blend' onto 'p4-main'.
```

(If the rebase had conflicted, gw would stop and leave you in a normal
`git rebase` conflict — fix, `git rebase --continue`, carry on. No P4 state
is involved yet, so nothing can be half-shipped.)

Now turn the branch into a pending changelist:

```
C:\work\game\src> gw prepare
Created pending changelist 481469
  edit    anim/Blend.cpp
  edit    anim/Blend.h
  add     anim/tests/BlendZeroWeight.cpp
Verified: mirror matches the branch exactly.

Changelist 481469 is ready — review and submit it from P4V (or: p4 submit -c 481469).
After it is submitted, run 'gw import' to absorb the new depot state into 'p4-main'.
```

Because the diff comes from Git, gw runs the exact `p4 edit/add/delete/move`
calls — no reconcile guessing. The "Verified" line is the safety net anyway:
a scoped `p4 reconcile -n` confirming the mirror holds precisely the branch
state. If a stray build artifact or hand edit had leaked into the mirror, gw
would print a loud warning here instead of letting a 4,000-file surprise
into the depot.

You review the CL in P4V — it looks exactly like a changelist a tidy
colleague would make, description stitched from your commit messages — and
submit it there. Then:

```
C:\work\game\src> gw import --rebase
Committed depot state to 'p4-main' (3 files, 0 deleted)
Rebased 'fix-anim-blend' onto 'p4-main'.
```

The branch's commits melt away during that rebase — the depot already has
them — leaving `fix-anim-blend` sitting on the new baseline, ready to
delete. Your teammates neither know nor care that Git was involved.

## When something's off: doctor

A month later, a sync mysteriously writes files into `src/` again. Instead
of spelunking, you run:

```
C:\work\game\src> gw doctor
ok    git found: git version 2.45.1
ok    p4 found
ok    .p4gw found at C:\work\game\src (depot_path //depot/game/main/src/...)
ok    mirror directory exists: C:\work\game\p4gw-mirror
ok    p4 connection works
FAIL  the effective mapping for //depot/game/main/src/... is
      '//depot/game/main/... //aaron-dev/...'; expected
      '//depot/game/main/src/... //aaron-dev/p4gw-mirror/...'
      (add it as the LAST view line — later lines win)
ok    LineEnd 'unix' and core.autocrlf=false agree
ok    no files opened under //depot/game/main/src/...

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
