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

namespace {

// A commit on the line: `touched` is cumulative from the base, `differs` the
// paths where it disagrees with the snapshot.
LineCommit lineCommit(std::string oid, std::string parent,
                      std::vector<std::string> touched,
                      std::vector<std::string> differs, bool empty = false) {
    LineCommit commit;
    commit.oid = std::move(oid);
    commit.parents = {std::move(parent)};
    commit.touched = std::move(touched);
    commit.differsFromDepot = std::move(differs);
    commit.empty = empty;
    return commit;
}

}  // namespace

TEST(absorb_two_commits_submitted_in_one_import) {
    // main -> A -> B, both touching f, submitted as two CLs and imported once:
    // the snapshot holds A+B, so A reads differently from it but B does not -
    // and B being carried means everything under it is too.
    auto plan = planAbsorption({lineCommit("A", "base", {"f"}, {"f"}),
                                lineCommit("B", "A", {"f"}, {})});
    CHECK(plan.absorbed == std::vector<std::string>({"A", "B"}));
    CHECK(plan.frontier.empty());
}

TEST(absorb_keeps_the_unsubmitted_top_of_a_line) {
    // A and B submitted, C still local: C is moved straight onto the snapshot.
    auto plan = planAbsorption({lineCommit("A", "base", {"f"}, {"f"}),
                                lineCommit("B", "A", {"f"}, {}),
                                lineCommit("C", "B", {"f", "g"}, {"g"})});
    CHECK(plan.absorbed == std::vector<std::string>({"A", "B"}));
    CHECK(plan.frontier == std::vector<std::string>({"C"}));
}

TEST(absorb_nothing_when_only_the_top_was_submitted) {
    // main -> C -> D with just D submitted: D's line still carries C's change,
    // which the depot lacks, so content says nothing is absorbed. (The patch-id
    // skip in the restack itself is what drops D there.)
    auto plan = planAbsorption({lineCommit("C", "base", {"g"}, {"g"}),
                                lineCommit("D", "C", {"g", "f"}, {"g"})});
    CHECK(plan.absorbed.empty());
    CHECK(plan.frontier.empty());
}

TEST(absorb_ignores_paths_the_depot_changed_elsewhere) {
    // The snapshot also moved files the line never touched (a teammate's CL).
    auto plan = planAbsorption({lineCommit("A", "base", {"f"}, {"other.cpp"})});
    CHECK(plan.absorbed == std::vector<std::string>({"A"}));
}

TEST(absorb_nothing_when_someone_else_edited_the_same_file_since) {
    // The depot's copy of f moved on past the line's: not provably carried.
    auto plan = planAbsorption({lineCommit("A", "base", {"f"}, {"f"}),
                                lineCommit("B", "A", {"f"}, {"f"})});
    CHECK(plan.absorbed.empty());
}

TEST(absorb_keeps_an_empty_commit_on_top) {
    // An empty placeholder over submitted work changes nothing itself, so it
    // survives - moved onto the snapshot rather than dropped with its parent.
    auto plan = planAbsorption({lineCommit("A", "base", {"f"}, {}),
                                lineCommit("E", "A", {"f"}, {}, true)});
    CHECK(plan.absorbed == std::vector<std::string>({"A"}));
    CHECK(plan.frontier == std::vector<std::string>({"E"}));
}

TEST(absorb_nothing_for_a_line_that_nets_to_no_change) {
    // A and its revert: the line touches nothing overall, which says nothing
    // about whether it was ever submitted.
    auto plan = planAbsorption({lineCommit("A", "base", {"f"}, {"f"}),
                                lineCommit("R", "A", {}, {})});
    CHECK(plan.absorbed.empty());
}

TEST(absorb_keeps_a_fork_point_a_surviving_branch_needs) {
    // A forks into B (submitted, carried) and X (local, not carried). A itself
    // is not carried, and X still builds on it, so A stays; B goes.
    auto plan = planAbsorption({lineCommit("A", "base", {"f"}, {"f"}),
                                lineCommit("B", "A", {"f"}, {}),
                                lineCommit("X", "A", {"f", "g"}, {"f", "g"})});
    CHECK(plan.absorbed == std::vector<std::string>({"B"}));
    CHECK(plan.frontier.empty());
}

TEST(absorb_a_fork_point_when_every_branch_is_carried) {
    auto plan = planAbsorption({lineCommit("A", "base", {"f"}, {"f"}),
                                lineCommit("B", "A", {"f"}, {}),
                                lineCommit("C", "A", {"f", "g"}, {})});
    CHECK(plan.absorbed == std::vector<std::string>({"A", "B", "C"}));
}

TEST(absorb_leaves_merges_alone) {
    auto merge = lineCommit("M", "A", {"f"}, {});
    merge.parents.push_back("other");
    auto plan = planAbsorption({lineCommit("A", "base", {"f"}, {}), merge});
    CHECK(plan.absorbed.empty());
    CHECK(plan.frontier.empty());
}
