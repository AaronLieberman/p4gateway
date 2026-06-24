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
        std::printf("  unexpected problem: %s\n", problem.message.c_str());
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

TEST(view_check_allows_narrower_exclusion_of_a_peer_dir) {
    // Carving a sub-directory out of the mirror with a `-` line is legitimate
    // (e.g. dropping a per-platform peer directory you don't build here). It
    // narrows what syncs but never sends depot files into the repo, so it is
    // allowed - the bulk remap of the subtree still wins.
    auto view = p4gw::p4::parseClientView(kSpec);
    view.push_back({"//depot/game/main/src/lib/linux/...",
                    "//aaron-dev/src/.p4gw/lib/linux/...", true, false});
    const auto problems =
        p4gw::p4::checkViewMapping(view, kDepotPath, kMirrorPath, kRepoPrefix);
    for (const auto& problem : problems) {
        std::printf("  unexpected problem: %s\n", problem.message.c_str());
    }
    CHECK(problems.empty());
}

TEST(view_check_allows_exclude_then_reinclude_into_mirror) {
    // The win64-yes-linux-no pattern: exclude a directory, then re-include one
    // peer back into the mirror. Both lines are narrower than the subtree and
    // neither maps into the repo, so the bulk remap still governs.
    auto view = p4gw::p4::parseClientView(kSpec);
    view.push_back({"//depot/game/main/src/lib/public/...",
                    "//aaron-dev/src/lib/public/...", true, false});
    view.push_back({"//depot/game/main/src/lib/public/win64/...",
                    "//aaron-dev/src/.p4gw/lib/public/win64/...", false, false});
    const auto problems =
        p4gw::p4::checkViewMapping(view, kDepotPath, kMirrorPath, kRepoPrefix);
    for (const auto& problem : problems) {
        std::printf("  unexpected problem: %s\n", problem.message.c_str());
    }
    CHECK(problems.empty());
}

TEST(view_check_allows_excluded_subtree_with_nested_client_carveouts) {
    // The "lib in place" form of the peer-exclusion trick: instead of putting
    // win64 in the mirror, lib/ is excluded from the mirror entirely (an
    // `exclude` line) and its internal shape - drop the linux peers, keep the
    // win64 ones - is expressed purely with client-view lines *under* lib. With
    // lib, thirdparty, and devtools all declared as excludes, every in-place
    // map and `-` sub-exclusion beneath them is intentional and must pass.
    // A non-empty repo prefix (the repo is a sub-directory of the client root)
    // makes the "maps into the repo" rule active so the exemption is exercised.
    const std::string depot = "//depot/project/main/src/...";
    const std::string mirror = "//client/game/.p4gw/src/...";
    const std::string repoPrefix = "//client/game/";
    const std::vector<p4gw::p4::ViewLine> view = {
        {depot, mirror, false, false},
        {"//depot/project/main/src/lib/...", "//client/game/src/lib/...", false,
         false},
        {"//depot/project/main/src/lib/public/...",
         "//client/game/src/lib/public/...", true, false},
        {"//depot/project/main/src/lib/public/win64/...",
         "//client/game/src/lib/public/win64/...", false, false},
        {"//depot/project/main/src/lib/common/...",
         "//client/game/src/lib/common/...", true, false},
        {"//depot/project/main/src/lib/common/win64*/...",
         "//client/game/src/lib/common/win64*/...", false, false},
        {"//depot/project/main/src/thirdparty/...",
         "//client/game/src/thirdparty/...", false, false},
        {"//depot/project/main/src/devtools/...", "//client/game/src/devtools/...",
         false, false},
    };
    const std::vector<std::string> excludes{
        "//depot/project/main/src/lib/...",
        "//depot/project/main/src/thirdparty/...",
        "//depot/project/main/src/devtools/...",
    };
    const auto problems =
        p4gw::p4::checkViewMapping(view, depot, mirror, repoPrefix, excludes);
    for (const auto& problem : problems) {
        std::printf("  unexpected problem: %s\n", problem.message.c_str());
    }
    CHECK(problems.empty());

    // Drop the thirdparty declaration: its in-place line is now an undeclared
    // mapping into the repo and is the only thing flagged - the lib lines and
    // its nested win64/linux carve-outs stay exempt under the lib exclude.
    const std::vector<std::string> missingThirdparty{
        "//depot/project/main/src/lib/...", "//depot/project/main/src/devtools/..."};
    const auto flagged = p4gw::p4::checkViewMapping(view, depot, mirror,
                                                    repoPrefix, missingThirdparty);
    CHECK(flagged.size() == 1);
}

TEST(view_check_flags_inplace_carveout_even_at_client_root) {
    // When the repo IS the client root, the repo prefix is empty and the
    // "maps into the repo" rule is off - yet an in-place line that diverts part
    // of the mapped subtree out of the mirror must still be caught, or gw would
    // gitignore-track p4-owned files. The detection keys off the depot subtree,
    // not the client prefix. Content outside the subtree (bin/, synced in place
    // like any unmapped directory) stays fine.
    const std::string depot = "//depot/project/main/src/...";
    const std::string mirror = "//client/.p4gw/src/...";
    const std::vector<p4gw::p4::ViewLine> view = {
        {depot, mirror, false, false},
        {"//depot/project/main/bin/...", "//client/bin/...", false, false},
        {"//depot/project/main/src/lib/...", "//client/src/lib/...", false, false},
    };
    // Empty repo prefix == the repo is the client root.
    const auto undeclared = p4gw::p4::checkViewMapping(view, depot, mirror, "");
    CHECK(undeclared.size() == 1);  // only src/lib, not bin/

    // Declaring the exclude resolves it.
    const auto declared = p4gw::p4::checkViewMapping(
        view, depot, mirror, "", {"//depot/project/main/src/lib/..."});
    for (const auto& problem : declared) {
        std::printf("  unexpected problem: %s\n", problem.message.c_str());
    }
    CHECK(declared.empty());
}

TEST(view_check_diversion_carries_exclude_suggestion) {
    // A diversion problem reports the depot path to add as an `exclude`, so the
    // command can group these into a single copy-pasteable hint.
    const std::string depot = "//depot/project/main/src/...";
    const std::string mirror = "//client/.p4gw/src/...";
    const std::vector<p4gw::p4::ViewLine> view = {
        {depot, mirror, false, false},
        {"//depot/project/main/src/lib/...", "//client/src/lib/...", false, false},
    };
    const auto problems = p4gw::p4::checkViewMapping(view, depot, mirror, "");
    CHECK(problems.size() == 1);
    if (problems.size() == 1) {
        CHECK(problems[0].excludePath == "//depot/project/main/src/lib/...");
        CHECK(!problems[0].message.empty());
    }
}

TEST(minimal_exclude_paths_drops_nested_under_an_ancestor) {
    // The user's view flags lib plus two win64 re-includes under it, and the
    // two leaf carve-outs. Excluding lib covers the win64 lines, so only three
    // `exclude` lines are suggested - in first-seen order.
    const auto out = p4gw::p4::minimalExcludePaths({
        "//depot/project/main/src/lib/...",
        "//depot/project/main/src/lib/public/win64/...",
        "//depot/project/main/src/lib/common/win64*/...",
        "//depot/project/main/src/thirdparty/...",
        "//depot/project/main/src/devtools/...",
    });
    CHECK(out.size() == 3);
    if (out.size() == 3) {
        CHECK(out[0] == "//depot/project/main/src/lib/...");
        CHECK(out[1] == "//depot/project/main/src/thirdparty/...");
        CHECK(out[2] == "//depot/project/main/src/devtools/...");
    }
}

TEST(minimal_exclude_paths_dedups_and_keeps_siblings) {
    const auto dups = p4gw::p4::minimalExcludePaths(
        {"//d/src/lib/...", "//d/src/lib/...", "//d/src/foo/..."});
    CHECK(dups.size() == 2);
    // A shared name prefix is not a parent: lib2 is not under lib.
    const auto siblings =
        p4gw::p4::minimalExcludePaths({"//d/src/lib/...", "//d/src/lib2/..."});
    CHECK(siblings.size() == 2);
}

TEST(view_check_flags_undeclared_inplace_carveout) {
    // A narrower line that syncs a sub-path in place into the repo (not the
    // mirror) is the dangerous case: p4 would write into a Git-tracked
    // directory. Without an `exclude` declaration it is flagged.
    auto view = p4gw::p4::parseClientView(kSpec);
    view.push_back({"//depot/game/main/src/thirdparty/...",
                    "//aaron-dev/src/thirdparty/...", false, false});
    const auto problems =
        p4gw::p4::checkViewMapping(view, kDepotPath, kMirrorPath, kRepoPrefix);
    CHECK(problems.size() == 1);
}

TEST(view_check_allows_declared_inplace_carveout) {
    // The same in-place line is fine once the config carves it out: the user
    // has told gw it should sync in place and be gitignored.
    auto view = p4gw::p4::parseClientView(kSpec);
    view.push_back({"//depot/game/main/src/thirdparty/...",
                    "//aaron-dev/src/thirdparty/...", false, false});
    const std::vector<std::string> excludes{
        "//depot/game/main/src/thirdparty/..."};
    const auto problems = p4gw::p4::checkViewMapping(
        view, kDepotPath, kMirrorPath, kRepoPrefix, excludes);
    for (const auto& problem : problems) {
        std::printf("  unexpected problem: %s\n", problem.message.c_str());
    }
    CHECK(problems.empty());
}

TEST(view_check_still_fails_when_whole_subtree_excluded) {
    // A `-` line at the subtree's own scope (not a narrower sub-path) empties
    // the mirror entirely, which is a misconfiguration worth catching.
    const std::vector<p4gw::p4::ViewLine> view = {
        {"//depot/game/main/...", "//aaron-dev/...", false, false},
        {"//depot/game/main/src/...", "//aaron-dev/src/.p4gw/...", false, false},
        {"//depot/game/main/src/...", "//aaron-dev/src/.p4gw/...", true, false},
    };
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
        std::printf("  unexpected problem: %s\n", problem.message.c_str());
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
        std::printf("  unexpected problem: %s\n", problem.message.c_str());
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

TEST(check_spec_mapping_exempts_declared_inplace_exclude) {
    // Repo is a sub-directory of the client root, so the repo prefix is real
    // and the "maps into the repo" rule is active. A line that syncs
    // src/thirdparty in place would normally trip it...
    const std::string spec =
        "Client:\tc\n"
        "Root:\t/work\n"
        "View:\n"
        "\t//depot/game/... //c/game/...\n"
        "\t//depot/game/src/... //c/game/src/.p4gw/...\n"
        "\t//depot/game/src/thirdparty/... //c/game/src/thirdparty/...\n";
    const auto undeclared = p4gw::p4::checkSpecMapping(
        spec, "//depot/game/src/...", "/work/game/src", "/work/game/src/.p4gw");
    CHECK(undeclared.size() == 1);

    // ... but is exempt once the config declares it as a carved-out subtree.
    const std::vector<std::string> excludes{"//depot/game/src/thirdparty/..."};
    const auto declared = p4gw::p4::checkSpecMapping(
        spec, "//depot/game/src/...", "/work/game/src", "/work/game/src/.p4gw",
        excludes);
    for (const auto& problem : declared) {
        std::printf("  unexpected problem: %s\n", problem.message.c_str());
    }
    CHECK(declared.empty());
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
        std::printf("  unexpected problem: %s\n", problem.message.c_str());
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
        std::printf("  unexpected problem: %s\n", problem.message.c_str());
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
