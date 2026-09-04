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

}  // namespace p4gw::restack
