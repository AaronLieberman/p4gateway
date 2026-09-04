// SPDX-License-Identifier: MIT

#include "restack.h"
#include "test_framework.h"

using namespace p4gw::restack;

namespace {

const std::vector<std::string> kAllRoots = {"alpha", "beta", "gamma"};

bool carries(const RestackSelection& selection, const std::string& root) {
    for (const auto& carried : selection.carryRoots) {
        if (carried == root) return true;
    }
    return false;
}

}  // namespace

TEST(restack_default_carries_only_the_current_stack) {
    auto selection = selectStacks(kAllRoots, {"beta"}, {}, false, false);
    CHECK(selection.carryRoots.size() == 1);
    CHECK(carries(selection, "beta"));
    CHECK(selection.skippedParked.empty());
}

TEST(restack_current_stack_ignores_parking) {
    // Checking a parked stack out and asking for it is an explicit request;
    // needing --force there would defeat the whole come-back-to-it gesture.
    auto selection = selectStacks(kAllRoots, {"beta"}, {"beta"}, false, false);
    CHECK(carries(selection, "beta"));
    CHECK(selection.skippedParked.empty());
}

TEST(restack_all_carries_every_stack_but_the_parked_ones) {
    auto selection = selectStacks(kAllRoots, {"alpha"}, {"gamma"}, true, false);
    CHECK(selection.carryRoots.size() == 2);
    CHECK(carries(selection, "alpha"));
    CHECK(carries(selection, "beta"));
    CHECK(selection.skippedParked.size() == 1);
    if (selection.skippedParked.empty()) return;
    CHECK(selection.skippedParked[0] == "gamma");
}

TEST(restack_force_retries_the_parked_ones) {
    auto selection = selectStacks(kAllRoots, {"alpha"}, {"gamma"}, true, true);
    CHECK(selection.carryRoots.size() == 3);
    CHECK(selection.skippedParked.empty());
}

TEST(restack_all_with_everything_parked_carries_nothing) {
    // Load-bearing: an empty carry list must never reach `git branchless sync`,
    // where no arguments means "sync every draft stack" - the opposite.
    auto selection = selectStacks(kAllRoots, {}, kAllRoots, true, false);
    CHECK(selection.carryRoots.empty());
    CHECK(selection.skippedParked.size() == 3);
}

TEST(restack_current_stack_empty_when_head_is_on_no_stack) {
    // Detached on the baseline itself: nothing of yours to move.
    auto selection = selectStacks(kAllRoots, {}, {}, false, false);
    CHECK(selection.carryRoots.empty());
}

TEST(restack_sweeps_parked_entries_that_no_longer_name_a_stack) {
    // A stack rebased by hand comes back with a new root oid, so the old entry
    // stops naming anything visible and the ref is swept.
    auto stale = staleParkedRoots({"gamma", "rewritten-away"}, kAllRoots);
    CHECK(stale.size() == 1);
    if (stale.empty()) return;
    CHECK(stale[0] == "rewritten-away");
}

TEST(restack_keeps_parked_entries_that_still_name_a_stack) {
    auto stale = staleParkedRoots({"gamma"}, kAllRoots);
    CHECK(stale.empty());
}
