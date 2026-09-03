// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <vector>

namespace p4gw::restack {

// One draft stack `git branchless sync` could move, as import sees it before
// deciding anything.
//
// Age is the stack's newest *author* time, because that is the only property of
// a stack that survives being restacked. A stack's distance from the depot
// baseline cannot serve: carrying a stack rebases it onto the new snapshot, so
// its base resets and it reads as brand new on the next run - a stack would
// never age out at all. Rebasing rewrites the committer date too, but leaves
// the author date alone, so "when did you last write anything here" is stable
// across any number of restacks.
struct StackCandidate {
    std::string root;   // commit oid of the stack's root, passed to sync
    std::string label;  // "<short> <subject>", for the report
    long long lastAuthoredAt = 0;  // newest author time in the stack, unix secs
    bool holdsHead = false;        // HEAD sits on this stack
};

// What import should do with the stacks it found.
struct RestackPlan {
    std::vector<std::string> carryRoots;    // revsets to hand `sync`
    std::vector<std::string> parkedLabels;  // left behind, for the report
};

// Chooses which stacks to carry onto the new depot state.
//
// `restackTimes` are when past restacks ran, read off the restack anchor's log.
// A stack's depth is how many of those happened after it was last authored:
// "how many restacks have gone by without you touching this work". Counting
// restacks and not depot snapshots is what keeps a bare `gw import` free - the
// basic loop imports after every submit, and shipping is not the same as
// letting work go stale.
//
// A stack is carried when its depth is within `depthLimit`, when it holds HEAD,
// or when `carryAll` is set. HEAD's stack is always carried: you are standing on
// it, so leaving it behind would strand your checkout on pre-import work - the
// very thing import otherwise treats as a bug - and it makes coming back to
// parked work need no flag at all (check it out, import, it comes with you).
//
// Pure; unit-tested.
RestackPlan planRestack(const std::vector<StackCandidate>& candidates,
                        const std::vector<long long>& restackTimes,
                        int depthLimit, bool carryAll);

}  // namespace p4gw::restack
