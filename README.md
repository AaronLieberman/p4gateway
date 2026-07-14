# p4gateway

**Work in Git. Ship Perforce changelists.**

`gw` is a small Windows command-line tool for developers stuck on a huge
Perforce depot who would rather spend their day in Git.

Your P4 client view remaps your source code (say `src/`) into a *mirror*
directory off to the side. Your real `src/` becomes a normal Git repo that
Perforce never touches, with `gw` moving state across that boundary. Your
content stays in place, so all the rest of your tooling continues to work
just as before.

```
      you work here               gw's staging area              the team
┌─────────────────────┐  gw import  ┌──────────────┐  any p4 sync  ┌──────────┐
│  Git repo at src/   │ ◄────────── │    mirror    │ ◄──────────── │ Perforce │
│  (P4 never touches  │             │  directory   │               │  depot   │
│   this tree)        │ ──────────► │  (P4-mapped) │ ────────────► │          │
└─────────────────────┘  gw prepare └──────────────┘  p4 submit    └──────────┘
                                                      (you, in P4V)
```

Two commands do the work:

- **`gw import`** brings your p4 sync into the Git repo and rebases your
  work on top. It's just like `git pull --rebase`, with the p4 depot as the
  remote.
- **`gw prepare`** goes the other way: it turns your branch into a pending
  changelist — Git knows exactly what changed, and your commit messages
  become the description. You review and submit it from P4V, same as always.

Everything in between is plain Git — branch, commit, rebase, bisect — and
none of it is visible to Perforce. There's no history import and no
server-side anything: P4 only ever sees files changing on disk, which is why
this works. Every p4 command gw runs is scoped to your subtree, so the rest
of the multi-terabyte depot is never touched.

## Day to day

```
<sync however you like>          # P4V, p4 sync, your team's sync tool...

gw import --rebase               # absorb it: commit to main, rebase your branch

git switch -c fix-anim-blend     # work normally: branch, commit, rebase, bisect
...

gw prepare                       # ship it: builds the pending CL

<review and submit in P4V>

gw import                        # absorb your own submit into main, plus
                                 # anything else you synced
```

That's the whole loop. When you need more — handing work off as a shelf,
refreshing a pending CL after review feedback, a status screen, an
environment doctor — see [INSTRUCTIONS.md](INSTRUCTIONS.md).

## Better with git-branchless

`gw` pairs really well with
[git-branchless](https://github.com/arxanas/git-branchless), which replaces
branch juggling with a *smartlog* — one view of all your in-flight work as
stacks of commits:

<img src="https://raw.githubusercontent.com/arxanas/git-branchless/master/media/git-sl.png" alt="git-branchless smartlog showing stacked commits" width="600">

That's a natural fit here, where every stack is really pending work on top
of the depot baseline. `gw` detects branchless automatically, and
`gw import --rebase` restacks *all* of your stacks onto the fresh depot
state — not just the branch you happen to be on. Details in
[INSTRUCTIONS.md](INSTRUCTIONS.md#using-git-branchless).

## Getting started

[SETUP.md](SETUP.md) covers building `gw` and pointing it at your depot:
one config file, one client-view line, then `gw init` and your first import.

## Status

In daily use. All commands (`setup`, `init`, `import`, `prepare`, `status`,
`shelf`, `doctor`) are implemented, and `gw integtest run` exercises the
whole workflow end-to-end against a live P4 server
(see [README-integtest.md](README-integtest.md)). See
[docs/sample-session.md](docs/sample-session.md) for what using it feels
like and [PLAN.md](PLAN.md) for the roadmap.

## License

[MIT](LICENSE) © 2026 Aaron Lieberman
