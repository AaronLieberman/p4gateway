// SPDX-License-Identifier: MIT

#include "git.h"
#include "test_framework.h"

using namespace p4gw;

namespace {

// A real `git branchless sync` run (v0.11.1) that moved one stack and gave up
// on another. Note the zero exit status this came with: the conflict is only
// ever reported here, in the output.
const char* kPartial =
    "Attempting rebase in-memory...\n"
    "[1/1] Committed as: 8de7d7f X1\n"
    "branchless: processing 1 rewritten commit\n"
    "branchless: running command: git checkout gw-import-restack --\n"
    "In-memory rebase succeeded.\n"
    "Synced 9fa39e0 X1\n"
    "Merge conflict (1 file) for 70224ed C1\n";

// The absorbed-stack run: every commit dropped as now-empty, stack still
// reported as synced.
const char* kAbsorbed =
    "Attempting rebase in-memory...\n"
    "[1/3] Skipped now-empty commit: 88ccfc5 C1\n"
    "[2/3] Skipped now-empty commit: 7528619 C2\n"
    "[3/3] Skipped now-empty commit: dd644c3 C3\n"
    "In-memory rebase succeeded.\n"
    "Synced 5bfb553 C1\n";

}  // namespace

TEST(branchless_sync_reports_a_skipped_stack_alongside_a_moved_one) {
    auto outcome = git::parseBranchlessSync(kPartial);
    CHECK(outcome.synced.size() == 1);
    CHECK(outcome.conflicted.size() == 1);
    CHECK(outcome.upToDate.empty());
    if (outcome.synced.empty() || outcome.conflicted.empty()) return;
    CHECK(outcome.synced[0] == "9fa39e0 X1");
    CHECK(outcome.conflicted[0] == "70224ed C1");
}

TEST(branchless_sync_absorbed_stack_is_a_clean_run) {
    auto outcome = git::parseBranchlessSync(kAbsorbed);
    CHECK(outcome.conflicted.empty());
    CHECK(outcome.synced.size() == 1);
}

TEST(branchless_sync_up_to_date_stack_is_not_a_move) {
    auto outcome =
        git::parseBranchlessSync("Not moving up-to-date stack at 280f897 C1\n");
    CHECK(outcome.synced.empty());
    CHECK(outcome.conflicted.empty());
    CHECK(outcome.upToDate.size() == 1);
    if (outcome.upToDate.empty()) return;
    CHECK(outcome.upToDate[0] == "280f897 C1");
}

TEST(branchless_sync_survives_crlf_and_color) {
    // Windows pipes carry the CR; the escapes are what a colored run would add.
    auto outcome = git::parseBranchlessSync(
        "\x1b[1;31mMerge conflict (2 files) for 70224ed C1\x1b[0m\r\n"
        "Synced 9fa39e0 X1\r\n");
    CHECK(outcome.conflicted.size() == 1);
    CHECK(outcome.synced.size() == 1);
    if (outcome.conflicted.empty() || outcome.synced.empty()) return;
    CHECK(outcome.conflicted[0] == "70224ed C1");
    CHECK(outcome.synced[0] == "9fa39e0 X1");
}

TEST(branchless_sync_conflict_without_a_named_commit_still_counts) {
    // Keep the whole line when the wording carries no " for <commit>": a
    // conflict we cannot attribute is still a conflict, never a clean run.
    auto outcome = git::parseBranchlessSync("Merge conflict.\n");
    CHECK(outcome.conflicted.size() == 1);
    if (outcome.conflicted.empty()) return;
    CHECK(outcome.conflicted[0] == "Merge conflict.");
}

TEST(branchless_sync_ignores_progress_chatter) {
    // Only the three load-bearing lines count, so new chatter in a future
    // branchless release cannot turn a clean run into a reported failure.
    auto outcome = git::parseBranchlessSync(
        "branchless: processing 1 update: branch gw-import-restack\n"
        "branchless: creating working copy snapshot\n"
        "Attempting rebase in-memory...\n"
        "Switched to branch 'main'\n");
    CHECK(outcome.synced.empty());
    CHECK(outcome.conflicted.empty());
    CHECK(outcome.upToDate.empty());
}
