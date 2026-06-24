#include "statusview.h"
#include "test_framework.h"

using p4gw::StatusInfo;

namespace {

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// A typical "on a feature branch, baseline created" starting point that the
// individual tests then tweak.
StatusInfo featureBranch() {
    StatusInfo info;
    info.hasCommits = true;
    info.branch = "fix-anim-blend";
    info.baselineBranch = "main";
    info.baselineExists = true;
    info.lastImportedCl = "48213";
    info.p4Reachable = true;
    return info;
}

}  // namespace

TEST(parse_imported_cl_extracts_number) {
    CHECK(p4gw::parseImportedCl("Import depot state at CL 48213") == "48213");
}

TEST(parse_imported_cl_handles_no_cl) {
    CHECK(p4gw::parseImportedCl("Import depot state").empty());
    CHECK(p4gw::parseImportedCl("some unrelated commit").empty());
}

TEST(parse_imported_cl_stops_at_non_digit) {
    CHECK(p4gw::parseImportedCl("Import depot state at CL 99 (note)") == "99");
}

TEST(next_step_fresh_repo_points_at_import) {
    StatusInfo info;
    info.hasCommits = false;
    info.baselineBranch = "main";
    CHECK(contains(p4gw::nextStep(info), "gw import"));
}

TEST(next_step_on_baseline_starts_a_branch) {
    StatusInfo info = featureBranch();
    info.branch = "main";
    info.onBaseline = true;
    CHECK(contains(p4gw::nextStep(info), "git switch -c"));
}

TEST(next_step_pending_cl_wins) {
    StatusInfo info = featureBranch();
    info.ahead = 3;
    info.hasPending = true;
    info.pendingCount = 2;
    // Even with shippable commits, an open CL is the active thing to finish.
    CHECK(contains(p4gw::nextStep(info), "P4V"));
}

TEST(next_step_behind_suggests_rebase) {
    StatusInfo info = featureBranch();
    info.ahead = 1;
    info.behind = 2;
    CHECK(contains(p4gw::nextStep(info), "gw import --rebase"));
}

TEST(next_step_ahead_and_clean_suggests_prepare) {
    StatusInfo info = featureBranch();
    info.ahead = 3;
    CHECK(contains(p4gw::nextStep(info), "gw prepare"));
}

TEST(next_step_ahead_and_dirty_says_commit_first) {
    StatusInfo info = featureBranch();
    info.ahead = 3;
    info.dirty = true;
    info.changeCount = 2;
    const std::string step = p4gw::nextStep(info);
    CHECK(contains(step, "Commit"));
    CHECK(contains(step, "gw prepare"));
}

TEST(next_step_nothing_to_ship_suggests_import) {
    StatusInfo info = featureBranch();  // ahead == 0, clean, no pending
    CHECK(contains(p4gw::nextStep(info), "gw import --rebase"));
}

TEST(render_shows_core_rows) {
    StatusInfo info = featureBranch();
    info.ahead = 3;
    const std::string out = p4gw::renderStatus(info);
    CHECK(contains(out, "Branch"));
    CHECK(contains(out, "fix-anim-blend"));
    CHECK(contains(out, "main - 3 ahead"));
    CHECK(contains(out, "Working tree"));
    CHECK(contains(out, "clean"));
    CHECK(contains(out, "CL 48213"));
    CHECK(contains(out, "Pending CL"));
    CHECK(contains(out, "none"));
    CHECK(contains(out, "Next:"));
}

TEST(render_reports_p4_unreachable) {
    StatusInfo info = featureBranch();
    info.p4Reachable = false;
    CHECK(contains(p4gw::renderStatus(info), "P4 not reachable"));
}

TEST(render_dirty_count_is_pluralized) {
    StatusInfo info = featureBranch();
    info.dirty = true;
    info.changeCount = 1;
    CHECK(contains(p4gw::renderStatus(info), "1 uncommitted change"));

    info.changeCount = 4;
    CHECK(contains(p4gw::renderStatus(info), "4 uncommitted changes"));
}

TEST(render_fresh_repo_has_no_rows) {
    StatusInfo info;
    info.hasCommits = false;
    info.baselineBranch = "main";
    const std::string out = p4gw::renderStatus(info);
    CHECK(contains(out, "No commits yet"));
    CHECK(contains(out, "Next:"));
}
