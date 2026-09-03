// SPDX-License-Identifier: MIT

#include "restack.h"
#include "test_framework.h"

using namespace p4gw::restack;

namespace {

// Four depot snapshots on the anchor's chain, taken at t=100, 200, 300, 400.
// A stack authored at t=350 has one snapshot past it; one authored at t=50 has
// all four.
const std::vector<long long> kBaselines = {400, 300, 200, 100};

StackCandidate stack(const std::string& root, long long authoredAt) {
    StackCandidate candidate;
    candidate.root = root;
    candidate.label = root + " subject";
    candidate.lastAuthoredAt = authoredAt;
    return candidate;
}

bool carries(const RestackPlan& plan, const std::string& root) {
    for (const auto& carried : plan.carryRoots) {
        if (carried == root) return true;
    }
    return false;
}

}  // namespace

TEST(restack_carries_stacks_within_the_depth_limit) {
    auto plan = planRestack({stack("fresh", 450),    // 0 snapshots past it
                             stack("recent", 350),   // 1
                             stack("old", 250)},     // 2
                            kBaselines, 1, false);
    CHECK(plan.carryRoots.size() == 2);
    CHECK(carries(plan, "fresh"));
    CHECK(carries(plan, "recent"));
    CHECK(plan.parkedLabels.size() == 1);
    if (plan.parkedLabels.empty()) return;
    CHECK(plan.parkedLabels[0] == "old subject");
}

TEST(restack_depth_zero_keeps_only_work_no_restack_has_passed) {
    auto plan = planRestack({stack("fresh", 450), stack("recent", 350)},
                            kBaselines, 0, false);
    CHECK(plan.carryRoots.size() == 1);
    CHECK(carries(plan, "fresh"));
}

TEST(restack_counts_a_same_second_snapshot_as_not_past_you) {
    // A snapshot taken in the same second you wrote the commit has not left
    // you behind; only a strictly newer one has.
    auto plan = planRestack({stack("tied", 400)}, kBaselines, 0, false);
    CHECK(carries(plan, "tied"));
}

TEST(restack_always_carries_the_stack_holding_head) {
    // Standing on parked work and importing must bring it with you - that is
    // the whole "come back to it later" gesture, and leaving HEAD behind is
    // what import otherwise treats as a bug.
    auto ancient = stack("ancient", 1);
    ancient.holdsHead = true;
    auto plan = planRestack({ancient, stack("old", 1)}, kBaselines, 1, false);
    CHECK(carries(plan, "ancient"));
    CHECK(plan.parkedLabels.size() == 1);
}

TEST(restack_all_overrides_the_limit) {
    auto plan = planRestack({stack("old", 1), stack("older", 0)}, kBaselines, 1,
                            true);
    CHECK(plan.carryRoots.size() == 2);
    CHECK(plan.parkedLabels.empty());
}

TEST(restack_with_no_baselines_on_record_parks_nothing) {
    // An anchor with no history behind it (or a repo where the anchor was just
    // created) cannot have gone past anything, so nothing has aged out.
    auto plan = planRestack({stack("old", 1)}, {}, 0, false);
    CHECK(plan.carryRoots.size() == 1);
}

TEST(restack_with_no_candidates_carries_nothing) {
    // Load-bearing: an empty carry list must never reach `git branchless sync`,
    // where no arguments means "sync every draft stack" - the opposite.
    auto plan = planRestack({}, kBaselines, 1, false);
    CHECK(plan.carryRoots.empty());
    CHECK(plan.parkedLabels.empty());
}
