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

// POSIX-style fake paths keep these portable: fs::relative is lexical for
// paths that don't exist.

TEST(client_view_path_inside_root) {
    CHECK(p4gw::p4::clientViewPath("aaron-dev", "/work/game",
                                   "/work/game/p4gw-mirror", "/...") ==
          "//aaron-dev/p4gw-mirror/...");
    CHECK(p4gw::p4::clientViewPath("aaron-dev", "/work/game",
                                   "/work/game/a/b", "/") ==
          "//aaron-dev/a/b/");
}

TEST(client_view_path_outside_root_is_empty) {
    CHECK(p4gw::p4::clientViewPath("c", "/work/game", "/elsewhere/mirror",
                                   "/...").empty());
    // The root itself can't be a view target for the mirror either.
    CHECK(p4gw::p4::clientViewPath("c", "/work/game", "/work/game",
                                   "/...").empty());
}

TEST(check_spec_mapping_end_to_end) {
    const std::string spec =
        "Client:\tc\n"
        "Root:\t/work\n"
        "View:\n"
        "\t//depot/game/... //c/game/...\n"
        "\t//depot/game/src/... //c/mirror/src/...\n";
    const auto good = p4gw::p4::checkSpecMapping(
        spec, "//depot/game/src/...", "/work/game/src", "/work/mirror/src");
    for (const auto& problem : good) {
        std::printf("  unexpected problem: %s\n", problem.c_str());
    }
    CHECK(good.empty());

    // Without the remap line, the broad mapping is effective and points at
    // the wrong client location.
    const std::string broken =
        "Client:\tc\n"
        "Root:\t/work\n"
        "View:\n"
        "\t//depot/game/... //c/game/...\n";
    const auto problems = p4gw::p4::checkSpecMapping(
        broken, "//depot/game/src/...", "/work/game/src", "/work/mirror/src");
    CHECK(!problems.empty());
}

TEST(check_spec_mapping_requires_client_and_root) {
    const auto problems = p4gw::p4::checkSpecMapping(
        "View:\n\t//a/... //c/a/...\n", "//a/...", "/r", "/m");
    CHECK(problems.size() == 1);
}
