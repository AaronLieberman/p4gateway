# What working with gw feels like

A walkthrough of a typical couple of days using `gw`, as terminal
transcripts. Everything below is aspirational output for the finished tool
(milestones M1–M5 in [PLAN.md](../PLAN.md)); the commit contents are
deliberately trivial. `C:\work\game\src>` is the overlaid subtree inside a
much larger P4 workspace.

## One-time setup

You've already synced your P4 workspace the normal way. Now you put a Git
overlay on the `src` subtree:

```
C:\work\game\src> gw init --depot-path //depot/game/main/src/...
ok    p4 connection (perforce:1666, user aaron, client aaron-dev)
ok    //depot/game/main/src/... is mapped in client aaron-dev
ok    client has 'allwrite' set
ok    LineEnd=unix matches core.autocrlf=input
Initialized Git repository in C:\work\game\src\.git
Wrote .p4gw and starter .gitignore
Created branch 'p4-main' at synced changelist 481223 (38,114 files)

You're ready. Daily flow:
  gw sync       pick up the team's changes
  git switch -c   branch and work normally
  gw submit     ship a branch as a P4 changelist
```

From here on, you never run `p4 edit`. You just use Git.

## Day 1, morning: pick up the team's changes

```
C:\work\game\src> gw sync
Syncing //depot/game/main/src/... in client aaron-dev...
Synced 214 files to changelist 481390
Committed depot state to 'p4-main' (CL 481390)
Rebased 'main' onto p4-main: nothing to rebase (you have no local commits)
```

## Working: just Git

A bug report comes in about animation blending. You work exactly like you
would in any Git repo — branches, WIP commits, interactive rebase to clean
up. gw is not involved at all during this part.

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

## Day 1, end of day: where am I?

```
C:\work\game\src> gw status
branch        fix-anim-blend (2 commits ahead of p4-main)
working tree  clean
baseline      p4-main @ CL 481390 (synced this morning, 6h ago)
pending CLs   none created by gw
```

## Day 2: ship it

Sync first so you're submitting against the latest depot state:

```
C:\work\game\src> gw sync
Syncing //depot/game/main/src/... in client aaron-dev...
Synced 89 files to changelist 481467
Committed depot state to 'p4-main' (CL 481467)
Rebasing 'fix-anim-blend' onto p4-main... done (2 commits replayed)
```

(If the rebase had conflicted, gw would stop and leave you in a normal
`git rebase` conflict — fix, `git rebase --continue`, carry on. No P4 state
is involved yet, so nothing can be half-submitted.)

Now turn the branch into a changelist. You use `--shelve` because your team
reviews in Swarm:

```
C:\work\game\src> gw submit --shelve
Preflight: clean tree, p4-main is ancestor of HEAD, no files opened in subtree
Created pending changelist 481469:

    Clamp blend weights before normalization

    Add regression test for zero-weight blend

Reconciling //depot/game/main/src/... against CL 481469...
  edit   src/anim/Blend.cpp
  edit   src/anim/Blend.h
  add    src/anim/tests/BlendZeroWeight.cpp
Cross-check vs git diff p4-main..HEAD: 3 files, matches ✓
Shelved CL 481469.

Review:  https://swarm.yourco.com/changes/481469
Submit:  p4 submit -c 481469   (or rerun with --submit)
```

The cross-check line is the safety net: if reconcile had picked up files Git
didn't change (line endings, a stray build artifact), gw would warn loudly
instead of shelving a 4,000-file surprise.

Review comes back clean, so:

```
C:\work\game\src> gw submit --update 481469 --submit
Re-reconciled CL 481469: no changes since shelf
Submitted as changelist 481470.
Fast-forwarded p4-main; tagged 'fix-anim-blend' as shipped/481470
```

Done. Your teammates see one tidy changelist with a description stitched
from your commit messages. They neither know nor care that Git was involved.

## When something's off: doctor

A month later submit starts reconciling files you didn't touch. Instead of
spelunking, you run:

```
C:\work\game\src> gw doctor
ok    git found: git version 2.45.1
ok    p4 found: P4/NTX64/2024.1
ok    p4 connection (perforce:1666, user aaron, client aaron-dev)
ok    //depot/game/main/src/... is mapped in client aaron-dev
FAIL  client 'LineEnd: win' but Git 'core.autocrlf=input' — these will
      fight; set LineEnd to unix or autocrlf to true (see README)
WARN  3 files opened in the subtree outside gw changelists:
      //depot/game/main/src/render/Foo.cpp (someone ran 'p4 edit'?)

1 check(s) failed, 1 warning.
```

Someone "helpfully" changed your client spec. Two minutes, fixed.

## The feel, in one paragraph

Git is your editor-adjacent daily driver; Perforce is a thing that happens
twice a day at the boundaries. `sync` in the morning, `submit` when a branch
is ready, `status`/`doctor` when curious or suspicious. Everything in
between — branching, stashing, bisecting, rewriting history until the
commits tell a clean story — is stock Git, and none of it touches the
server or anyone else's workflow.
