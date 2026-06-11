#include "p4.h"
#include "test_framework.h"

namespace {

const char* kSpec =
    "# A Perforce Client Specification.\n"
    "\n"
    "Client:\taaron-dev\n"
    "\n"
    "Root:\tC:\\work\\game\n"
    "\n"
    "Options:\tnoallwrite noclobber nocompress unlocked nomodtime rmdir\n"
    "\n"
    "LineEnd:\tunix\n"
    "\n"
    "View:\n"
    "\t//depot/game/main/... //aaron-dev/...\n"
    "\t-//depot/game/main/art/... //aaron-dev/art/...\n"
    "\t\"//depot/game/main/My Docs/...\" \"//aaron-dev/My Docs/...\"\n"
    "\t//depot/tools/bin/... //aaron-dev/tools/bin/...\n"
    "\t//depot/game/main/src/... //aaron-dev/p4gw-mirror/...\n";

constexpr const char* kDepotPath = "//depot/game/main/src/...";
constexpr const char* kMirrorPath = "//aaron-dev/p4gw-mirror/...";
constexpr const char* kRepoPrefix = "//aaron-dev/src/";

}  // namespace

TEST(spec_field_values) {
    CHECK(p4gw::p4::specField(kSpec, "Client") == "aaron-dev");
    CHECK(p4gw::p4::specField(kSpec, "Root") == "C:\\work\\game");
    CHECK(p4gw::p4::specField(kSpec, "LineEnd") == "unix");
    CHECK(p4gw::p4::specField(kSpec, "Missing").empty());
}

TEST(view_parses_lines_quotes_and_exclusions) {
    const auto view = p4gw::p4::parseClientView(kSpec);
    CHECK(view.size() == 5);
    if (view.size() == 5) {
        CHECK(view[0].depot == "//depot/game/main/...");
        CHECK(view[0].client == "//aaron-dev/...");
        CHECK(!view[0].exclude);
        CHECK(view[1].exclude);
        CHECK(view[1].depot == "//depot/game/main/art/...");
        CHECK(view[2].depot == "//depot/game/main/My Docs/...");
        CHECK(view[2].client == "//aaron-dev/My Docs/...");
        CHECK(view[4].client == "//aaron-dev/p4gw-mirror/...");
    }
}

TEST(view_check_passes_with_remap_line) {
    const auto view = p4gw::p4::parseClientView(kSpec);
    const auto problems =
        p4gw::p4::checkViewMapping(view, kDepotPath, kMirrorPath, kRepoPrefix);
    for (const auto& problem : problems) {
        std::printf("  unexpected problem: %s\n", problem.c_str());
    }
    CHECK(problems.empty());
}

TEST(view_check_fails_without_remap_line) {
    // Drop the last (remap) line: the broad mapping becomes effective and
    // would sync depot files into the repo directory.
    auto view = p4gw::p4::parseClientView(kSpec);
    view.pop_back();
    const auto problems =
        p4gw::p4::checkViewMapping(view, kDepotPath, kMirrorPath, kRepoPrefix);
    CHECK(!problems.empty());
}

TEST(view_check_fails_when_depot_path_unmapped) {
    const auto problems = p4gw::p4::checkViewMapping(
        {}, kDepotPath, kMirrorPath, kRepoPrefix);
    CHECK(problems.size() == 1);
}

TEST(view_check_fails_on_late_exclusion) {
    auto view = p4gw::p4::parseClientView(kSpec);
    view.push_back({"//depot/game/main/src/generated/...",
                    "//aaron-dev/p4gw-mirror/generated/...", true, false});
    const auto problems =
        p4gw::p4::checkViewMapping(view, kDepotPath, kMirrorPath, kRepoPrefix);
    CHECK(!problems.empty());
}

TEST(view_check_flags_lines_mapping_into_repo) {
    auto view = p4gw::p4::parseClientView(kSpec);
    view.push_back({"//depot/tools/extra/...", "//aaron-dev/src/extra/...",
                    false, false});
    const auto problems =
        p4gw::p4::checkViewMapping(view, kDepotPath, kMirrorPath, kRepoPrefix);
    CHECK(problems.size() == 1);
}

TEST(view_check_ignores_unrelated_mappings) {
    // The art exclusion, "My Docs", and tools/bin lines don't overlap the
    // src depot path and must not produce problems.
    auto view = p4gw::p4::parseClientView(kSpec);
    view.push_back({"//depot/other/...", "//aaron-dev/other/...", false,
                    false});
    const auto problems =
        p4gw::p4::checkViewMapping(view, kDepotPath, kMirrorPath, kRepoPrefix);
    CHECK(problems.empty());
}
