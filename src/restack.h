// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <vector>

namespace p4gw::restack {

// Which stacks `gw import --rebase[-all]` should hand to `git branchless sync`,
// and which it deliberately leaves alone.
//
// Stacks are named by their root commit, because that is what `sync` takes as
// an argument and what identifies a stack across a run. Parked stacks are
// recorded as refs (see parkedRefPrefix in config.h): a stack that could not be
// rebased once will conflict again on every future import, so it is remembered
// and skipped rather than retried forever.
struct RestackSelection {
    std::vector<std::string> carryRoots;     // hand these to `sync`
    std::vector<std::string> skippedParked;  // roots left alone, for the report
};

// Picks the stacks to carry.
//
// `--rebase` (allStacks false) carries only the stack HEAD is on, and ignores
// the parked list entirely: checking a parked stack out and asking for it is an
// explicit request, so it must not need a second flag. `--rebase-all` carries
// every visible stack except the parked ones, and `force` overrides that.
//
// Pure; unit-tested.
RestackSelection selectStacks(const std::vector<std::string>& allRoots,
                              const std::vector<std::string>& currentStackRoots,
                              const std::vector<std::string>& parkedRoots,
                              bool allStacks, bool force);

// Parked entries that no longer name a visible stack root - the stack was
// rebased by hand (its commits were rewritten, so the root oid changed), hidden,
// or absorbed. Their refs are swept, so the namespace tracks reality instead of
// growing forever.
//
// Pure; unit-tested.
std::vector<std::string> staleParkedRoots(
    const std::vector<std::string>& parkedRoots,
    const std::vector<std::string>& visibleRoots);

// One commit of a line of local work (a branchless stack, or the commits a
// branch or detached HEAD carries past the depot baseline), as absorption sees
// it. The facts are gathered by the caller from git; the decision is pure.
struct LineCommit {
    std::string oid;
    std::vector<std::string> parents;
    // The commit's tree equals its parent's: it changes nothing on its own.
    bool empty = false;
    // Every path the line changed from its base up to and including this
    // commit (`git diff --name-only <base> <oid>`).
    std::vector<std::string> touched;
    // Every path whose content differs between this commit and the new depot
    // snapshot (`git diff --name-only <oid> <snapshot>`).
    std::vector<std::string> differsFromDepot;
};

// What `gw import --rebase` can drop from a line instead of replaying it.
struct Absorption {
    // Commits whose content the depot snapshot already carries - hide/drop
    // them. Parents before children.
    std::vector<std::string> absorbed;
    // Commits that are not absorbed but whose parent is: each is moved (with
    // its descendants) straight onto the snapshot.
    std::vector<std::string> frontier;
};

// Decides which commits of a line the new depot snapshot already carries, so
// the restack drops them instead of replaying them.
//
// Git and git-branchless only skip a commit whose *patch* matches an upstream
// commit. One import commits everything submitted since the last one, so
// preparing and submitting A and then B, then importing once, yields a single
// snapshot holding A+B: neither patch matches, and replaying A onto a file
// that already has B's lines on top conflicts. Content catches that: a commit
// is absorbed when the line has changed something by then and every path it
// changed reads the same at that commit as in the snapshot - so the line's
// state up to there is already in the depot, whoever submitted it.
//
// A commit that changes nothing itself is never absorbed on its own (an empty
// placeholder on top of submitted work survives); an ancestor is absorbed when
// every child is, so a line that forks keeps any shared commit a surviving
// branch still needs. A line with merge commits (or any commit whose parent
// count is not one) is left alone: the plan is empty.
//
// `commits` must list parents before children. Pure; unit-tested.
Absorption planAbsorption(const std::vector<LineCommit>& commits);

}  // namespace p4gw::restack
