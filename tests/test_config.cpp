#include <cstdio>
#include <filesystem>
#include <fstream>

#include "config.h"
#include "test_framework.h"

namespace fs = std::filesystem;

namespace {

// Writes `content` to a temp file and loads it as a config.
std::expected<p4gw::Config, std::string> loadFromString(const std::string& content) {
    const fs::path path = fs::temp_directory_path() / "p4gw_test_config";
    {
        std::ofstream file(path);
        file << content;
    }
    auto result = p4gw::loadConfig(path.string());
    fs::remove(path);
    return result;
}

}  // namespace

TEST(config_parses_a_single_mapping) {
    auto config = loadFromString(
        "# overlay for the engine source tree\n"
        "mapping = //depot/yourgame/src/... .p4gw\n"
        "client = aaron-dev\n"
        "baseline_branch = p4-base\n");
    CHECK(config.has_value());
    if (config) {
        CHECK(config->mappings.size() == 1);
        CHECK(config->mappings[0].depotPath == "//depot/yourgame/src/...");
        CHECK(config->mappings[0].mirrorPath == ".p4gw");
        CHECK(config->mappings[0].repoSubtree.empty());
        CHECK(config->client == "aaron-dev");
        CHECK(config->baselineBranch == "p4-base");
    }
}

TEST(config_parses_multiple_mappings_in_order) {
    auto config = loadFromString(
        "mapping = //depot/develop/src/...    .p4gw/src\n"
        "mapping = //depot/develop/config/... .p4gw/config\n");
    CHECK(config.has_value());
    if (config) {
        CHECK(config->mappings.size() == 2);
        CHECK(config->mappings[0].depotPath == "//depot/develop/src/...");
        CHECK(config->mappings[0].mirrorPath == ".p4gw/src");
        CHECK(config->mappings[0].repoSubtree == "src");
        CHECK(config->mappings[1].depotPath == "//depot/develop/config/...");
        CHECK(config->mappings[1].mirrorPath == ".p4gw/config");
        CHECK(config->mappings[1].repoSubtree == "config");
    }
}

TEST(config_derives_repo_subtree_from_mirror) {
    // The leading `.p4gw` container is dropped; the rest is the working-tree
    // directory the subtree occupies.
    CHECK(p4gw::mirrorRepoSubtree(".p4gw") == "");
    CHECK(p4gw::mirrorRepoSubtree(".p4gw/src") == "src");
    CHECK(p4gw::mirrorRepoSubtree(".p4gw/a/b") == "a/b");
    CHECK(p4gw::mirrorRepoSubtree("./.p4gw/src") == "src");
}

TEST(config_resolves_relative_mirror_path) {
    const fs::path root = fs::path("work") / "game" / "src";
    CHECK(fs::path(p4gw::resolveMirrorPath(".p4gw/src", root.string())) ==
          root / ".p4gw" / "src");

    // A parent-relative mirror still normalizes correctly.
    CHECK(fs::path(p4gw::resolveMirrorPath("../sibling", root.string())) ==
          fs::path("work") / "game" / "sibling");
}

TEST(config_keeps_absolute_mirror_path) {
    const fs::path absolute = fs::temp_directory_path() / "mirror";
    CHECK(fs::path(p4gw::resolveMirrorPath(absolute.string(), "elsewhere")) ==
          absolute);
}

TEST(config_defaults_baseline_branch) {
    auto config = loadFromString("mapping = //depot/yourgame/src/... .p4gw\n");
    CHECK(config.has_value());
    if (config) {
        CHECK(config->client.empty());
        CHECK(config->baselineBranch == "p4-main");
    }
}

TEST(config_requires_at_least_one_mapping) {
    auto config = loadFromString("client = aaron-dev\n");
    CHECK(!config.has_value());
}

TEST(config_rejects_depot_path_without_wildcard) {
    auto config = loadFromString("mapping = //depot/x .p4gw\n");
    CHECK(!config.has_value());
}

TEST(config_rejects_mapping_with_wrong_arity) {
    CHECK(!loadFromString("mapping = //depot/x/...\n").has_value());
    CHECK(!loadFromString("mapping = //depot/x/... a b\n").has_value());
}

TEST(config_rejects_duplicate_depot_or_mirror) {
    CHECK(!loadFromString(
              "mapping = //depot/x/... .p4gw/a\n"
              "mapping = //depot/x/... .p4gw/b\n")
               .has_value());
    CHECK(!loadFromString(
              "mapping = //depot/x/... .p4gw/a\n"
              "mapping = //depot/y/... .p4gw/a\n")
               .has_value());
}

TEST(config_rejects_unknown_keys) {
    auto config = loadFromString(
        "mapping = //depot/x/... .p4gw\n"
        "mapping_paht = typo\n");
    CHECK(!config.has_value());
}

TEST(config_rejects_malformed_lines) {
    auto config = loadFromString("mapping //depot/x/... .p4gw\n");
    CHECK(!config.has_value());
}

namespace {

p4gw::Mapping mappingWithSubtree(const std::string& subtree) {
    p4gw::Mapping m;
    m.depotPath = "//depot/x/...";
    m.mirrorPath = subtree.empty() ? ".p4gw" : ".p4gw/" + subtree;
    m.repoSubtree = subtree;
    return m;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST(gitignore_allowlists_a_top_level_subtree) {
    const std::string out =
        p4gw::buildGitignore({mappingWithSubtree("src")});
    // Ignore everything, then re-include just the mapped subtree and the
    // .gitignore itself. Unmapped content and the mirror are not negated.
    CHECK(contains(out, "/*\n"));
    CHECK(contains(out, "!/.gitignore\n"));
    CHECK(contains(out, "!/src/\n"));
    CHECK(!contains(out, "!/bin/"));
    CHECK(!contains(out, ".p4gw/\n"));  // covered by the blanket /* ignore
}

TEST(gitignore_allowlists_multiple_subtrees) {
    const std::string out = p4gw::buildGitignore(
        {mappingWithSubtree("src"), mappingWithSubtree("config")});
    CHECK(contains(out, "!/src/\n"));
    CHECK(contains(out, "!/config/\n"));
}

TEST(gitignore_reincludes_ancestors_for_nested_subtree) {
    const std::string out =
        p4gw::buildGitignore({mappingWithSubtree("a/b")});
    // The blanket /* hides `a`, so re-include `a/`, re-exclude its other
    // children, then re-include `a/b/` so only that subtree shows through.
    CHECK(contains(out, "!/a/\n"));
    CHECK(contains(out, "/a/*\n"));
    CHECK(contains(out, "!/a/b/\n"));
    // Ordering: the ancestor re-include and child re-exclude precede the
    // deeper re-include, or the leaf would be hidden again.
    CHECK(out.find("/a/*\n") < out.find("!/a/b/\n"));
}

TEST(gitignore_drops_subtree_nested_under_another_mapping) {
    const std::string out = p4gw::buildGitignore(
        {mappingWithSubtree("a"), mappingWithSubtree("a/b")});
    // `a` is tracked whole, so `a/b` is redundant and must not trigger a
    // `/a/*` that would re-exclude the rest of `a`.
    CHECK(contains(out, "!/a/\n"));
    CHECK(!contains(out, "/a/*\n"));
    CHECK(!contains(out, "!/a/b/\n"));
}

TEST(gitignore_falls_back_to_denylist_for_whole_repo_mapping) {
    const std::string out = p4gw::buildGitignore({mappingWithSubtree("")});
    // Nothing is unmapped, so allowlisting would only hide the repo's own
    // content; ignore just the gw-managed paths instead.
    CHECK(!contains(out, "/*\n"));
    CHECK(contains(out, "p4gw.cfg\n"));
    CHECK(contains(out, ".p4gw/\n"));
}
