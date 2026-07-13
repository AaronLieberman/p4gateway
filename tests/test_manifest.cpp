// SPDX-License-Identifier: MIT

#include <string>
#include <utility>
#include <vector>

#include "mirror.h"
#include "test_framework.h"

using p4gw::mirror::diffHaveState;
using p4gw::mirror::ManifestEntry;
using p4gw::mirror::parseHaveManifest;
using p4gw::mirror::renderHaveManifest;

TEST(have_manifest_path_flattens_branch_separators) {
    const std::string plain =
        p4gw::mirror::haveManifestPath("/repo/.git", "main");
    CHECK(plain.find("have-main") != std::string::npos);
    const std::string nested =
        p4gw::mirror::haveManifestPath("/repo/.git", "team/main");
    CHECK(nested.find("have-team_main") != std::string::npos);
    CHECK(nested.find("team/main") == std::string::npos);
}

TEST(have_manifest_round_trips) {
    const std::vector<ManifestEntry> entries = {
        {"//depot/src/main.cpp", "3"},
        {"//depot/src/docs/overview.md", "12"},
    };
    const std::string text = renderHaveManifest("abc123", entries);

    std::string snapshot;
    const auto parsed = parseHaveManifest(text, snapshot);
    CHECK(snapshot == "abc123");
    CHECK(parsed.size() == 2);
    if (parsed.size() == 2) {
        CHECK(parsed[0].depotFile == "//depot/src/main.cpp");
        CHECK(parsed[0].rev == "3");
        CHECK(parsed[1].depotFile == "//depot/src/docs/overview.md");
        CHECK(parsed[1].rev == "12");
    }
}

TEST(have_manifest_parse_skips_comments_and_malformed_lines) {
    std::string snapshot;
    const auto parsed = parseHaveManifest(
        "# a comment\n"
        "snapshot deadbeef\n"
        "//depot/a#1\r\n"      // CRLF tolerated
        "no-hash-here\n"       // malformed: skipped
        "#leading-hash\n"      // comment, not an entry
        "//depot/b#\n"         // empty rev: skipped
        "//depot/c#7\n",
        snapshot);
    CHECK(snapshot == "deadbeef");
    CHECK(parsed.size() == 2);
    if (parsed.size() == 2) {
        CHECK(parsed[0].depotFile == "//depot/a");
        CHECK(parsed[0].rev == "1");
        CHECK(parsed[1].depotFile == "//depot/c");
        CHECK(parsed[1].rev == "7");
    }
}

TEST(have_manifest_parse_without_snapshot_yields_empty_binding) {
    std::string snapshot = "stale";
    (void)parseHaveManifest("//depot/a#1\n", snapshot);
    CHECK(snapshot.empty());  // callers treat this as "no manifest"
}

TEST(diff_have_state_classifies_changes) {
    const std::vector<std::pair<std::string, std::string>> then = {
        {"unchanged.cpp", "2"},
        {"edited.cpp", "5"},
        {"deleted.cpp", "1"},
    };
    const std::vector<std::pair<std::string, std::string>> now = {
        {"unchanged.cpp", "2"},
        {"edited.cpp", "6"},
        {"added.cpp", "1"},
    };
    const auto actions = diffHaveState(then, now);
    CHECK(actions.copies.size() == 2);
    if (actions.copies.size() == 2) {
        CHECK(actions.copies[0] == "added.cpp");
        CHECK(actions.copies[1] == "edited.cpp");
    }
    CHECK(actions.deletes.size() == 1);
    if (actions.deletes.size() == 1) {
        CHECK(actions.deletes[0] == "deleted.cpp");
    }
}

TEST(diff_have_state_identical_states_are_a_noop) {
    const std::vector<std::pair<std::string, std::string>> state = {
        {"a.cpp", "1"},
        {"b.cpp", "9"},
    };
    const auto actions = diffHaveState(state, state);
    CHECK(actions.copies.empty());
    CHECK(actions.deletes.empty());
}

TEST(diff_have_state_empty_then_copies_everything) {
    const std::vector<std::pair<std::string, std::string>> now = {
        {"a.cpp", "1"},
    };
    const auto actions = diffHaveState({}, now);
    CHECK(actions.copies.size() == 1);
    CHECK(actions.deletes.empty());
}