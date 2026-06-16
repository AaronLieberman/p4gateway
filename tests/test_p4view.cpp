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
    "\t//depot/game/main/src/... //aaron-dev/src/.p4gw/...\n";

constexpr const char* kDepotPath = "//depot/game/main/src/...";
// The recommended mirror `.p4gw` lives inside the repo, so its client path is
// itself under the repo prefix - the relaxed repo-mapping check must allow it.
constexpr const char* kMirrorPath = "//aaron-dev/src/.p4gw/...";
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
        CHECK(view[4].client == "//aaron-dev/src/.p4gw/...");
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
                    "//aaron-dev/src/.p4gw/generated/...", true, false});
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
                                   "/work/game/.p4gw", "/...") ==
          "//aaron-dev/.p4gw/...");
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

TEST(check_spec_mapping_allows_mirror_inside_repo) {
    // The recommended layout: mirror `.p4gw` is a child of the repo, so the
    // remap line's client path sits under the repo prefix. It must pass.
    const std::string spec =
        "Client:\tc\n"
        "Root:\t/work\n"
        "View:\n"
        "\t//depot/game/... //c/game/...\n"
        "\t//depot/game/src/... //c/game/src/.p4gw/...\n";
    const auto good = p4gw::p4::checkSpecMapping(
        spec, "//depot/game/src/...", "/work/game/src", "/work/game/src/.p4gw");
    for (const auto& problem : good) {
        std::printf("  unexpected problem: %s\n", problem.c_str());
    }
    CHECK(good.empty());

    // A different line mapping into the repo (but not the mirror) still fails.
    const std::string leaks =
        "Client:\tc\n"
        "Root:\t/work\n"
        "View:\n"
        "\t//depot/game/... //c/game/...\n"
        "\t//depot/game/src/... //c/game/src/.p4gw/...\n"
        "\t//depot/tools/... //c/game/src/tools/...\n";
    const auto problems = p4gw::p4::checkSpecMapping(
        leaks, "//depot/game/src/...", "/work/game/src", "/work/game/src/.p4gw");
    CHECK(problems.size() == 1);
}

TEST(view_check_fails_when_correct_remap_is_shadowed) {
    // Two lines both cover depot_path; the later one maps to the wrong place,
    // shadowing the correct remap. The "later lines win" rule makes this fail.
    const std::vector<p4gw::p4::ViewLine> view = {
        {"//depot/game/main/...", "//aaron-dev/...", false, false},
        {"//depot/game/main/src/...", "//aaron-dev/src/.p4gw/...", false, false},
        {"//depot/game/main/src/...", "//aaron-dev/wrong-mirror/...", false, false},
    };
    const auto problems =
        p4gw::p4::checkViewMapping(view, kDepotPath, kMirrorPath, kRepoPrefix);
    CHECK(!problems.empty());
}

TEST(view_check_passes_when_correct_remap_wins_over_earlier_wrong_line) {
    // Two lines both cover depot_path; the correct remap is last and wins.
    const std::vector<p4gw::p4::ViewLine> view = {
        {"//depot/game/main/...", "//aaron-dev/...", false, false},
        {"//depot/game/main/src/...", "//aaron-dev/wrong-mirror/...", false, false},
        {"//depot/game/main/src/...", "//aaron-dev/src/.p4gw/...", false, false},
    };
    const auto problems =
        p4gw::p4::checkViewMapping(view, kDepotPath, kMirrorPath, kRepoPrefix);
    for (const auto& problem : problems) {
        std::printf("  unexpected problem: %s\n", problem.c_str());
    }
    CHECK(problems.empty());
}

TEST(view_check_passes_when_remap_is_not_last_but_unshadowed) {
    // The src remap is followed by an unrelated config remap. Nothing after it
    // overlaps the src depot path, so it need not be the last line overall -
    // only the last line that overlaps it.
    const std::vector<p4gw::p4::ViewLine> view = {
        {"//depot/game/main/...", "//aaron-dev/...", false, false},
        {"//depot/game/main/src/...", "//aaron-dev/src/.p4gw/...", false, false},
        {"//depot/game/main/config/...", "//aaron-dev/config/.p4gw/...", false,
         false},
    };
    const auto problems =
        p4gw::p4::checkViewMapping(view, kDepotPath, kMirrorPath, kRepoPrefix);
    for (const auto& problem : problems) {
        std::printf("  unexpected problem: %s\n", problem.c_str());
    }
    CHECK(problems.empty());
}

TEST(check_spec_mapping_requires_client_and_root) {
    const auto problems = p4gw::p4::checkSpecMapping(
        "View:\n\t//a/... //c/a/...\n", "//a/...", "/r", "/m");
    CHECK(problems.size() == 1);
}

TEST(server_id_from_info) {
    // p4 info prints the field as "ServerID:" in recent releases ...
    const char* infoNoSpace =
        "User name: integtest\n"
        "Server address: 127.0.0.1:1666\n"
        "ServerID: p4gw-integtest-throwaway\n"
        "Server license: none\n";
    CHECK(p4gw::p4::serverIdFromInfo(infoNoSpace) ==
          "p4gw-integtest-throwaway");

    // ... and as "Server ID:" in others; both must parse.
    const char* infoSpaced =
        "User name: integtest\n"
        "Server ID: master.1\n";
    CHECK(p4gw::p4::serverIdFromInfo(infoSpaced) == "master.1");

    // No server.id set -> empty.
    CHECK(p4gw::p4::serverIdFromInfo("User name: integtest\n").empty());
}

TEST(security_level_from_show) {
    CHECK(p4gw::p4::securityLevelFromShow("security=0\n") == 0);
    CHECK(p4gw::p4::securityLevelFromShow("security=3\n") == 3);
    // p4 often annotates the source of the value.
    CHECK(p4gw::p4::securityLevelFromShow("security=1 (configure)\n") == 1);
    // Unset configurable: no security= line means level 0.
    CHECK(p4gw::p4::securityLevelFromShow(
              "No configurables have been set.\n") == 0);
    CHECK(p4gw::p4::securityLevelFromShow("") == 0);
}
