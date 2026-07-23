// SPDX-License-Identifier: MIT

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

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

TEST(config_parses_a_single_include) {
    auto config = loadFromString(
        "# overlay for the engine source tree\n"
        "include = //depot/yourproject/src/... .p4gw\n"
        "client = aaron-dev\n"
        "baseline_branch = p4-base\n");
    CHECK(config.has_value());
    if (config) {
        CHECK(config->rules.size() == 1);
        CHECK(!config->rules[0].exclude);
        CHECK(config->rules[0].depotPath == "//depot/yourproject/src/...");
        CHECK(config->rules[0].mirrorPath == ".p4gw");
        CHECK(config->rules[0].repoSubtree.empty());
        CHECK(config->client == "aaron-dev");
        CHECK(config->baselineBranch == "p4-base");
    }
}

TEST(config_parses_multiple_includes_in_order) {
    auto config = loadFromString(
        "include = //depot/develop/src/...    .p4gw/src\n"
        "include = //depot/develop/config/... .p4gw/config\n");
    CHECK(config.has_value());
    if (config) {
        CHECK(config->rules.size() == 2);
        CHECK(config->rules[0].depotPath == "//depot/develop/src/...");
        CHECK(config->rules[0].mirrorPath == ".p4gw/src");
        CHECK(config->rules[0].repoSubtree == "src");
        CHECK(config->rules[1].depotPath == "//depot/develop/config/...");
        CHECK(config->rules[1].mirrorPath == ".p4gw/config");
        CHECK(config->rules[1].repoSubtree == "config");
    }
}

TEST(config_parses_exclude_lines_under_an_include) {
    auto config = loadFromString(
        "include = //depot/project/main/src/... .p4gw/src\n"
        "exclude = //depot/project/main/src/lib/...\n"
        "exclude = //depot/project/main/src/thirdparty/...\n"
        "exclude = //depot/project/main/src/devtools/...\n");
    CHECK(config.has_value());
    if (config) {
        CHECK(config->rules.size() == 4);
        CHECK(!config->rules[0].exclude);
        CHECK(config->rules[1].exclude);
        CHECK(config->rules[1].depotPath == "//depot/project/main/src/lib/...");
        CHECK(config->rules[1].repoSubtree == "src/lib");
        CHECK(config->rules[2].repoSubtree == "src/thirdparty");
        CHECK(config->rules[3].repoSubtree == "src/devtools");
        // includeRules / excludeDepotPaths partition the ordered list.
        CHECK(p4gw::includeRules(config->rules).size() == 1);
        const auto excludes = p4gw::excludeDepotPaths(config->rules);
        CHECK(excludes.size() == 3);
        CHECK(excludes[0] == "//depot/project/main/src/lib/...");
    }
}

TEST(config_exclude_binds_to_enclosing_include_when_intermixed) {
    // An exclude may appear after an unrelated include and still bind to the
    // earlier include whose depot path contains it (the enclosing one).
    auto config = loadFromString(
        "include = //depot/develop/src/...    .p4gw/src\n"
        "include = //depot/develop/config/... .p4gw/config\n"
        "exclude = //depot/develop/src/lib/...\n"
        "exclude = //depot/develop/config/generated/...\n");
    CHECK(config.has_value());
    if (config) {
        CHECK(config->rules.size() == 4);
        CHECK(config->rules[2].exclude);
        CHECK(config->rules[2].repoSubtree == "src/lib");
        CHECK(config->rules[3].exclude);
        CHECK(config->rules[3].repoSubtree == "config/generated");
    }
}

TEST(config_parses_reinclude_deeper_than_an_exclude) {
    // The win64/linux pattern: map src, carve out src/lib, then re-include a
    // deeper directory back into the mirror. The re-include is just an include
    // whose depot path is deeper than the preceding exclude.
    auto config = loadFromString(
        "include = //depot/src/...                     .p4gw/src\n"
        "exclude = //depot/src/lib/...\n"
        "include = //depot/src/lib/public/win64/...    .p4gw/src/lib/public/win64\n");
    CHECK(config.has_value());
    if (config) {
        CHECK(config->rules.size() == 3);
        CHECK(!config->rules[2].exclude);
        CHECK(config->rules[2].repoSubtree == "src/lib/public/win64");
        CHECK(p4gw::includeRules(config->rules).size() == 2);
    }
}

TEST(config_parses_single_level_include) {
    // The direct-files pattern: map src, carve out src/build recursively, then
    // re-include only the files directly in src/build (a '/*' depot path).
    auto config = loadFromString(
        "include = //depot/main/src/...      .p4gw/src\n"
        "exclude = //depot/main/src/build/...\n"
        "include = //depot/main/src/build/*  .p4gw/src/build\n");
    CHECK(config.has_value());
    if (config) {
        CHECK(config->rules.size() == 3);
        CHECK(config->rules[0].recursive);
        CHECK(config->rules[1].exclude && config->rules[1].recursive);
        CHECK(!config->rules[2].exclude);
        CHECK(!config->rules[2].recursive);
        CHECK(config->rules[2].depotPath == "//depot/main/src/build/*");
        CHECK(config->rules[2].mirrorPath == ".p4gw/src/build");
        CHECK(config->rules[2].repoSubtree == "src/build");
    }
}

TEST(config_rejects_single_level_exclude) {
    // '/*' is only meaningful on an include; an exclude is always recursive.
    CHECK(!loadFromString(
              "include = //depot/x/... .p4gw/x\n"
              "exclude = //depot/x/build/*\n")
               .has_value());
}

TEST(effective_rule_single_level_covers_only_direct_children) {
    auto config = loadFromString(
        "include = //depot/main/src/...      .p4gw/src\n"
        "exclude = //depot/main/src/build/...\n"
        "include = //depot/main/src/build/*  .p4gw/src/build\n");
    CHECK(config.has_value());
    if (!config) return;
    const auto& rules = config->rules;
    // A file directly in src/build resolves to the single-level include.
    const auto* direct =
        p4gw::effectiveRuleForDepot(rules, "//depot/main/src/build/config.ini");
    CHECK(direct != nullptr && !direct->exclude && !direct->recursive);
    // A file in a sub-directory of src/build is NOT covered by the '/*' rule,
    // so it falls back to the recursive exclude.
    const auto* deep = p4gw::effectiveRuleForDepot(
        rules, "//depot/main/src/build/gen/output.txt");
    CHECK(deep != nullptr && deep->exclude);
    // The same, resolved on the repo (working-tree) side.
    const auto* directRepo =
        p4gw::effectiveRuleForRepo(rules, "src/build/config.ini");
    CHECK(directRepo != nullptr && !directRepo->exclude);
    const auto* deepRepo =
        p4gw::effectiveRuleForRepo(rules, "src/build/gen/output.txt");
    CHECK(deepRepo != nullptr && deepRepo->exclude);
    // A plain src file is unaffected.
    const auto* core =
        p4gw::effectiveRuleForDepot(rules, "//depot/main/src/core/main.cpp");
    CHECK(core != nullptr && !core->exclude && core->recursive);
}

TEST(effective_rule_for_depot_is_later_wins) {
    auto config = loadFromString(
        "include = //depot/src/...                     .p4gw/src\n"
        "exclude = //depot/src/lib/...\n"
        "include = //depot/src/lib/public/win64/...    .p4gw/src/lib/public/win64\n");
    CHECK(config.has_value());
    if (!config) return;
    const auto& rules = config->rules;
    // A plain file under src maps to the src include.
    const auto* core =
        p4gw::effectiveRuleForDepot(rules, "//depot/src/core/main.cpp");
    CHECK(core != nullptr && !core->exclude && core->repoSubtree == "src");
    // A file under the excluded lib (but not win64) resolves to the exclude.
    const auto* lib =
        p4gw::effectiveRuleForDepot(rules, "//depot/src/lib/other/a.cpp");
    CHECK(lib != nullptr && lib->exclude);
    // A file under the re-included win64 resolves back to that include - the
    // later rule wins over the earlier exclude, not the longest prefix.
    const auto* win64 = p4gw::effectiveRuleForDepot(
        rules, "//depot/src/lib/public/win64/x.lib");
    CHECK(win64 != nullptr && !win64->exclude &&
          win64->repoSubtree == "src/lib/public/win64");
    // Nothing covers a path outside every rule.
    CHECK(p4gw::effectiveRuleForDepot(rules, "//other/x") == nullptr);
}

TEST(effective_rule_for_repo_is_later_wins) {
    auto config = loadFromString(
        "include = //depot/src/...                     .p4gw/src\n"
        "exclude = //depot/src/lib/...\n"
        "include = //depot/src/lib/public/win64/...    .p4gw/src/lib/public/win64\n");
    CHECK(config.has_value());
    if (!config) return;
    const auto& rules = config->rules;
    const auto* core = p4gw::effectiveRuleForRepo(rules, "src/core/main.cpp");
    CHECK(core != nullptr && !core->exclude);
    const auto* lib = p4gw::effectiveRuleForRepo(rules, "src/lib/other/a.cpp");
    CHECK(lib != nullptr && lib->exclude);
    const auto* win64 =
        p4gw::effectiveRuleForRepo(rules, "src/lib/public/win64/x.lib");
    CHECK(win64 != nullptr && !win64->exclude);
    // A path under no mapped subtree (unmapped, pure-Git) resolves to nothing.
    CHECK(p4gw::effectiveRuleForRepo(rules, "bin/tool.exe") == nullptr);
}

TEST(effective_rule_for_repo_whole_repo_include_matches_everything) {
    auto config = loadFromString(
        "include = //depot/x/... .p4gw\n"
        "exclude = //depot/x/lib/...\n");
    CHECK(config.has_value());
    if (!config) return;
    const auto& rules = config->rules;
    // The empty-subtree include is the catch-all.
    const auto* any = p4gw::effectiveRuleForRepo(rules, "anything/here");
    CHECK(any != nullptr && !any->exclude);
    // ...except where a later exclude carves a subtree out.
    const auto* lib = p4gw::effectiveRuleForRepo(rules, "lib/z.cpp");
    CHECK(lib != nullptr && lib->exclude);
}

TEST(excluded_repo_subtree_maps_depot_to_worktree) {
    CHECK(p4gw::excludedRepoSubtree("//d/src/...", "src", "//d/src/lib/...") ==
          "src/lib");
    CHECK(p4gw::excludedRepoSubtree("//d/src/...", "src",
                                    "//d/src/lib/public/...") ==
          "src/lib/public");
    // A whole-repo mapping (empty subtree) yields a top-level exclude.
    CHECK(p4gw::excludedRepoSubtree("//d/...", "", "//d/lib/...") == "lib");
    // Not strictly under the mapping -> empty.
    CHECK(p4gw::excludedRepoSubtree("//d/src/...", "src", "//d/src/...")
              .empty());
    CHECK(p4gw::excludedRepoSubtree("//d/src/...", "src", "//d/other/...")
              .empty());
    // A peer with a shared name prefix must not match (boundary anchored).
    CHECK(p4gw::excludedRepoSubtree("//d/src/...", "src", "//d/srclib/...")
              .empty());
}

TEST(config_rejects_exclude_without_an_include) {
    CHECK(!loadFromString("exclude = //depot/x/lib/...\n").has_value());
}

TEST(config_rejects_exclude_outside_every_include) {
    CHECK(!loadFromString(
              "include = //depot/x/... .p4gw/x\n"
              "exclude = //depot/y/lib/...\n")
               .has_value());
}

TEST(config_rejects_exclude_at_include_root) {
    // Excluding the whole subtree (== an include's own depot path) is not a
    // carve-out; it would empty the mirror.
    CHECK(!loadFromString(
              "include = //depot/x/... .p4gw/x\n"
              "exclude = //depot/x/...\n")
               .has_value());
}

TEST(config_rejects_exclude_without_wildcard) {
    CHECK(!loadFromString(
              "include = //depot/x/... .p4gw/x\n"
              "exclude = //depot/x/lib\n")
               .has_value());
}

TEST(config_rejects_duplicate_exclude) {
    CHECK(!loadFromString(
              "include = //depot/x/... .p4gw/x\n"
              "exclude = //depot/x/lib/...\n"
              "exclude = //depot/x/lib/...\n")
               .has_value());
}

TEST(config_parses_ignore_patterns_in_order) {
    auto config = loadFromString(
        "include = //depot/x/src/... .p4gw/src\n"
        "ignore = /src/.vs/\n"
        "ignore = /src/**/*.vcxproj\n"
        "ignore = /src/**/*.pdb\n");
    CHECK(config.has_value());
    if (config) {
        CHECK(config->ignorePatterns.size() == 3);
        CHECK(config->ignorePatterns[0] == "/src/.vs/");
        CHECK(config->ignorePatterns[1] == "/src/**/*.vcxproj");
        CHECK(config->ignorePatterns[2] == "/src/**/*.pdb");
    }
}

TEST(config_keeps_ignore_pattern_verbatim) {
    // Patterns are taken as-is (not tokenized), so globs survive intact.
    auto config = loadFromString(
        "include = //depot/x/src/... .p4gw/src\n"
        "ignore = /src/build_scripts/p4_helpers.pyc\n");
    CHECK(config.has_value());
    if (config) {
        CHECK(config->ignorePatterns.size() == 1);
        CHECK(config->ignorePatterns[0] ==
              "/src/build_scripts/p4_helpers.pyc");
    }
}

TEST(config_dedupes_identical_ignore_patterns) {
    auto config = loadFromString(
        "include = //depot/x/src/... .p4gw/src\n"
        "ignore = /src/**/*.pdb\n"
        "ignore = /src/**/*.pdb\n");
    CHECK(config.has_value());
    if (config) {
        CHECK(config->ignorePatterns.size() == 1);
    }
}

TEST(config_rejects_empty_ignore) {
    CHECK(!loadFromString(
              "include = //depot/x/src/... .p4gw/src\n"
              "ignore =\n")
               .has_value());
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
    const fs::path root = fs::path("work") / "project" / "src";
    CHECK(fs::path(p4gw::resolveMirrorPath(".p4gw/src", root.string())) ==
          root / ".p4gw" / "src");

    // A parent-relative mirror still normalizes correctly.
    CHECK(fs::path(p4gw::resolveMirrorPath("../sibling", root.string())) ==
          fs::path("work") / "project" / "sibling");
}

TEST(config_keeps_absolute_mirror_path) {
    const fs::path absolute = fs::temp_directory_path() / "mirror";
    CHECK(fs::path(p4gw::resolveMirrorPath(absolute.string(), "elsewhere")) ==
          absolute);
}

TEST(config_defaults_baseline_branch) {
    auto config = loadFromString("include = //depot/yourproject/src/... .p4gw\n");
    CHECK(config.has_value());
    if (config) {
        CHECK(config->client.empty());
        CHECK(config->baselineBranch == "main");
    }
}

TEST(config_defaults_import_mode_to_worktree) {
    auto config = loadFromString("include = //depot/yourproject/src/... .p4gw\n");
    CHECK(config.has_value());
    if (config) {
        CHECK(config->importMode == p4gw::ImportMode::kWorktree);
    }
}

TEST(config_parses_import_mode_values) {
    auto checkout = loadFromString(
        "include = //depot/yourproject/src/... .p4gw\n"
        "import_mode = checkout\n");
    CHECK(checkout.has_value());
    if (checkout) CHECK(checkout->importMode == p4gw::ImportMode::kCheckout);

    auto worktree = loadFromString(
        "include = //depot/yourproject/src/... .p4gw\n"
        "import_mode = worktree\n");
    CHECK(worktree.has_value());
    if (worktree) CHECK(worktree->importMode == p4gw::ImportMode::kWorktree);
}

TEST(config_rejects_unknown_import_mode) {
    auto config = loadFromString(
        "include = //depot/yourproject/src/... .p4gw\n"
        "import_mode = bogus\n");
    CHECK(!config.has_value());
    if (!config) {
        CHECK(config.error().find("import_mode must be") != std::string::npos);
    }
}

TEST(depot_tracking_ref_derives_from_baseline) {
    p4gw::Config config;  // default baseline branch
    CHECK(p4gw::depotTrackingRef(config) == "refs/p4gw/main");
    config.baselineBranch = "p4-base";
    CHECK(p4gw::depotTrackingRef(config) == "refs/p4gw/p4-base");
}

TEST(config_requires_at_least_one_include) {
    auto config = loadFromString("client = aaron-dev\n");
    CHECK(!config.has_value());
}

TEST(config_rejects_depot_path_without_wildcard) {
    auto config = loadFromString("include = //depot/x .p4gw\n");
    CHECK(!config.has_value());
}

TEST(config_rejects_include_with_wrong_arity) {
    CHECK(!loadFromString("include = //depot/x/...\n").has_value());
    CHECK(!loadFromString("include = //depot/x/... a b\n").has_value());
}

TEST(config_rejects_duplicate_depot_or_mirror) {
    CHECK(!loadFromString(
              "include = //depot/x/... .p4gw/a\n"
              "include = //depot/x/... .p4gw/b\n")
               .has_value());
    CHECK(!loadFromString(
              "include = //depot/x/... .p4gw/a\n"
              "include = //depot/y/... .p4gw/a\n")
               .has_value());
}

TEST(config_rejects_unknown_keys) {
    auto config = loadFromString(
        "include = //depot/x/... .p4gw\n"
        "mapping_paht = typo\n");
    CHECK(!config.has_value());
}

TEST(config_rejects_malformed_lines) {
    auto config = loadFromString("include //depot/x/... .p4gw\n");
    CHECK(!config.has_value());
}

namespace {

// A whole depot mapped to a working-tree subtree - the include half of a rule
// list for the gitignore tests (depot path is irrelevant to buildGitignore).
p4gw::ViewRule inc(const std::string& subtree) {
    p4gw::ViewRule r;
    r.exclude = false;
    r.depotPath = "//depot/x/" + (subtree.empty() ? "" : subtree + "/") + "...";
    r.mirrorPath = subtree.empty() ? ".p4gw" : ".p4gw/" + subtree;
    r.repoSubtree = subtree;
    return r;
}

// A single-level (`/*`) include: only the files directly in `subtree` are
// mapped, its sub-directories are not.
p4gw::ViewRule incFiles(const std::string& subtree) {
    p4gw::ViewRule r;
    r.exclude = false;
    r.recursive = false;
    r.depotPath = "//depot/x/" + subtree + "/*";
    r.mirrorPath = ".p4gw/" + subtree;
    r.repoSubtree = subtree;
    return r;
}

// A carved-out subtree - the exclude half of a rule list.
p4gw::ViewRule exc(const std::string& subtree) {
    p4gw::ViewRule r;
    r.exclude = true;
    r.depotPath = "//depot/x/" + subtree + "/...";
    r.repoSubtree = subtree;
    return r;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

bool contains_line(const std::vector<std::string>& lines,
                   const std::string& line) {
    return std::find(lines.begin(), lines.end(), line) != lines.end();
}

}  // namespace

TEST(gitignore_allowlists_a_top_level_subtree) {
    const std::string out = p4gw::buildGitignore({inc("src")});
    // Ignore everything, then re-include just the mapped subtree and the
    // .gitignore itself. Unmapped content and the mirror are not negated.
    CHECK(contains(out, "/*\n"));
    CHECK(contains(out, "!/.gitignore\n"));
    // gw's .gitattributes is a loose root file, so the blanket /* would swallow
    // it without an explicit re-include - just like .gitignore.
    CHECK(contains(out, "!/.gitattributes\n"));
    CHECK(contains(out, "!/src/\n"));
    CHECK(!contains(out, "!/bin/"));
    CHECK(!contains(out, ".p4gw/\n"));  // covered by the blanket /* ignore
}

TEST(gitignore_allowlists_multiple_subtrees) {
    const std::string out = p4gw::buildGitignore({inc("src"), inc("config")});
    CHECK(contains(out, "!/src/\n"));
    CHECK(contains(out, "!/config/\n"));
}

TEST(gitignore_reincludes_ancestors_for_nested_subtree) {
    const std::string out = p4gw::buildGitignore({inc("a/b")});
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
    const std::string out = p4gw::buildGitignore({inc("a"), inc("a/b")});
    // `a` is tracked whole, so `a/b` is redundant and must not trigger a
    // `/a/*` that would re-exclude the rest of `a`.
    CHECK(contains(out, "!/a/\n"));
    CHECK(!contains(out, "/a/*\n"));
    CHECK(!contains(out, "!/a/b/\n"));
}

TEST(gitignore_reexcludes_carved_out_subtrees) {
    // The mapped subtree is re-included whole, then each carved-out directory
    // is re-excluded so it stays out of Git (it syncs in place / is unsynced).
    const std::string out =
        p4gw::buildGitignore({inc("src"), exc("src/lib"), exc("src/thirdparty")});
    CHECK(contains(out, "!/src/\n"));
    CHECK(contains(out, "/src/lib/\n"));
    CHECK(contains(out, "/src/thirdparty/\n"));
    // The re-exclusions must come after the subtree's re-include, or Git would
    // re-include them again.
    CHECK(out.find("!/src/\n") < out.find("/src/lib/\n"));
}

TEST(gitignore_reincludes_a_deeper_subtree_under_an_exclude) {
    // The win64 re-include: src is tracked, src/lib carved out, and
    // src/lib/public/win64 mapped back in. The carve-out must be emitted as an
    // intermediate (`/src/lib/*`, not the whole-dir `/src/lib/`) so Git can
    // still recurse down to the re-included win64 directory.
    const std::string out = p4gw::buildGitignore(
        {inc("src"), exc("src/lib"), inc("src/lib/public/win64")});
    CHECK(contains(out, "!/src/\n"));
    CHECK(contains(out, "!/src/lib/\n"));
    CHECK(contains(out, "/src/lib/*\n"));
    CHECK(contains(out, "!/src/lib/public/\n"));
    CHECK(contains(out, "/src/lib/public/*\n"));
    CHECK(contains(out, "!/src/lib/public/win64/\n"));
    // A carve-out with a re-include descendant is NOT emitted as a plain
    // whole-directory exclusion line (that would hide win64 again). Anchor at a
    // line boundary so it does not match the "!/src/lib/" re-include.
    CHECK(!contains(out, "\n/src/lib/\n"));
    // Ordering: each level's re-exclude of contents precedes the deeper
    // re-include, or the leaf would be hidden.
    CHECK(out.find("/src/lib/*\n") < out.find("!/src/lib/public/win64/\n"));
}

TEST(gitignore_single_level_reexcludes_child_directories) {
    // The direct-files pattern under a recursive parent: src is tracked whole,
    // src/build carved out, then re-included single-level. The '!/src/' keeps
    // src/build's own files; '/src/build/*/' re-excludes its child directories.
    const std::string out = p4gw::buildGitignore(
        {inc("src"), exc("src/build"), incFiles("src/build")});
    CHECK(contains(out, "!/src/\n"));
    CHECK(contains(out, "/src/build/*/\n"));
    // Not a plain whole-directory carve-out (that would hide the direct files
    // too). Anchor at a line boundary so it does not match '/src/build/*/'.
    CHECK(!contains(out, "\n/src/build/\n"));
    // The re-exclusion of children must come after the subtree's re-include.
    CHECK(out.find("!/src/\n") < out.find("/src/build/*/\n"));
}

TEST(gitignore_single_level_top_level_subtree) {
    // A single-level mapping with no recursive ancestor: re-include the subtree
    // (so its files show through the blanket /*) and re-exclude its children.
    const std::string out = p4gw::buildGitignore({incFiles("build")});
    CHECK(contains(out, "/*\n"));
    CHECK(contains(out, "!/build/\n"));
    CHECK(contains(out, "/build/*/\n"));
    CHECK(out.find("!/build/\n") < out.find("/build/*/\n"));
}

TEST(gitignore_reexcludes_under_whole_repo_mapping) {
    // A whole-repo mapping uses the denylist body; carved-out directories are
    // still ignored, by an ordinary ignore line.
    const std::string out = p4gw::buildGitignore({inc(""), exc("lib")});
    CHECK(contains(out, "p4gw.cfg\n"));  // denylist body
    CHECK(contains(out, "/lib/\n"));
}

TEST(gitignore_falls_back_to_denylist_for_whole_repo_mapping) {
    const std::string out = p4gw::buildGitignore({inc("")});
    // Nothing is unmapped, so allowlisting would only hide the repo's own
    // content; ignore just the gw-managed paths instead.
    CHECK(!contains(out, "/*\n"));
    CHECK(contains(out, "p4gw.cfg\n"));
    CHECK(contains(out, ".p4gw/\n"));
}

TEST(gitignore_appends_extra_ignore_patterns_last) {
    // Extra patterns land after the subtree's re-include, so Git actually
    // ignores those files under the tracked subtree.
    const std::string out =
        p4gw::buildGitignore({inc("src")}, {"/src/.vs/", "/src/**/*.pdb"});
    CHECK(contains(out, "!/src/\n"));
    CHECK(contains(out, "/src/.vs/\n"));
    CHECK(contains(out, "/src/**/*.pdb\n"));
    CHECK(out.find("!/src/\n") < out.find("/src/.vs/\n"));
}

TEST(gitignore_appends_extra_patterns_under_whole_repo_mapping) {
    // The denylist body also gets the extra patterns.
    const std::string out = p4gw::buildGitignore({inc("")}, {"/build/"});
    CHECK(contains(out, "p4gw.cfg\n"));  // denylist body
    CHECK(contains(out, "/build/\n"));
}

TEST(gitattributes_pins_eol_for_all_paths) {
    // The starter gw writes pins EOL verbatim with a catch-all '-text' rule.
    const std::string out = p4gw::buildGitattributes();
    CHECK(p4gw::gitattributesPinsEol(out));
    CHECK(contains(out, "* -text\n"));
}

TEST(gitattributes_pin_detects_catchall_text_variants) {
    CHECK(p4gw::gitattributesPinsEol("* -text\n"));
    CHECK(p4gw::gitattributesPinsEol("*\t-text\n"));
    CHECK(p4gw::gitattributesPinsEol("* text=auto\n"));
    CHECK(p4gw::gitattributesPinsEol("* text\n"));
    CHECK(p4gw::gitattributesPinsEol("* eol=lf\n"));
    // Comments and blank lines are ignored; the rule can sit anywhere.
    CHECK(p4gw::gitattributesPinsEol("# header\n\n*   -text\r\n"));
}

TEST(gitattributes_pin_rejects_non_catchall_or_unrelated) {
    CHECK(!p4gw::gitattributesPinsEol(""));
    CHECK(!p4gw::gitattributesPinsEol("# only a comment\n"));
    // A pattern that is not the catch-all does not pin every path.
    CHECK(!p4gw::gitattributesPinsEol("*.cpp -text\n"));
    // A catch-all with only non-EOL attributes is not an EOL pin.
    CHECK(!p4gw::gitattributesPinsEol("* diff=cpp\n"));
    // '-text' must be a whole attribute token, not a substring of another.
    CHECK(!p4gw::gitattributesPinsEol("* mytext\n"));
}

TEST(allowlist_tracking_lines_lists_each_mapped_subtree) {
    const auto lines =
        p4gw::allowlistTrackingLines({inc("src"), inc("files")});
    // One re-include per mapped subtree, in declaration order - exactly what
    // buildGitignore emits for the allowlist body.
    CHECK(lines.size() == 2);
    CHECK(lines[0] == "!/src/");
    CHECK(lines[1] == "!/files/");
}

TEST(allowlist_tracking_lines_covers_nested_reinclude_chain) {
    const auto lines = p4gw::allowlistTrackingLines({inc("a/b")});
    // The ancestor re-include, its child re-exclusion, then the deeper subtree.
    CHECK(contains_line(lines, "!/a/"));
    CHECK(contains_line(lines, "/a/*"));
    CHECK(contains_line(lines, "!/a/b/"));
}

TEST(allowlist_tracking_lines_empty_for_denylist_body) {
    // A whole-repo include falls back to the denylist body, which uses no
    // re-includes - so there is nothing to track line by line.
    CHECK(p4gw::allowlistTrackingLines({inc("")}).empty());
}

TEST(missing_allowlist_lines_finds_a_newly_added_include) {
    // The .gitignore was written for src only; adding a `files` include leaves
    // its re-include missing (the exact bug: import ships nothing through it).
    const std::string existing = p4gw::buildGitignore({inc("src")});
    const auto missing = p4gw::missingAllowlistTrackingLines(
        {inc("src"), inc("files")}, existing);
    CHECK(missing.size() == 1);
    CHECK(missing[0] == "!/files/");
}

TEST(missing_allowlist_lines_empty_when_file_covers_every_subtree) {
    const std::string existing =
        p4gw::buildGitignore({inc("src"), inc("files")});
    CHECK(p4gw::missingAllowlistTrackingLines({inc("src"), inc("files")},
                                              existing)
              .empty());
}

TEST(missing_allowlist_lines_matches_whole_lines_only) {
    // "!/src/" must not count as present just because "!/src/lib/" is - the
    // whole src subtree would be untracked despite the deeper line appearing.
    const std::string existing = "/*\n!/src/lib/public/win64/\n";
    const auto missing =
        p4gw::missingAllowlistTrackingLines({inc("src")}, existing);
    CHECK(missing.size() == 1);
    CHECK(missing[0] == "!/src/");
}

TEST(missing_allowlist_lines_ignores_redundant_intermediate_reinclude) {
    // Exclude src/devtools, then re-include only a deeper subtree. src is
    // tracked whole ('!/src/' with no '/src/*'), so Git already descends into
    // devtools - the "!/src/devtools/" intermediate re-include gw emits is
    // redundant. A hand-minimized .gitignore that drops it still tracks the
    // subtree, so the coverage check must NOT flag it.
    const std::vector<p4gw::ViewRule> rules = {
        inc("src"), exc("src/lib"), exc("src/thirdparty"), exc("src/devtools"),
        inc("src/devtools/datacache")};
    const std::string minimal =
        "/*\n"
        "!/.gitignore\n"
        "!/.gitattributes\n"
        "!/src/\n"
        "/src/lib/\n"
        "/src/thirdparty/\n"
        "/src/devtools/*\n"
        "!/src/devtools/datacache/\n";
    CHECK(p4gw::missingAllowlistTrackingLines(rules, minimal).empty());
    // The load-bearing deep re-include, by contrast, is still required: drop it
    // and the subtree really is untracked (its parent '/src/devtools/*' hides
    // it), so the check reports exactly that line.
    const std::string broken =
        "/*\n!/src/\n/src/lib/\n/src/thirdparty/\n/src/devtools/*\n";
    const auto missing = p4gw::missingAllowlistTrackingLines(rules, broken);
    CHECK(missing.size() == 1);
    CHECK(missing[0] == "!/src/devtools/datacache/");
}

TEST(gitignore_allowlist_detection) {
    // The starter allowlist opens with the bare '/*' root-ignore line; the
    // whole-repo mapping falls back to a denylist without one.
    CHECK(p4gw::gitignoreIsAllowlist(p4gw::buildGitignore({inc("src")})));
    CHECK(!p4gw::gitignoreIsAllowlist(p4gw::buildGitignore({inc("")})));
    // Whitespace and CRLF around the line are tolerated.
    CHECK(p4gw::gitignoreIsAllowlist("# header\r\n  /*  \r\n!/src/\n"));
    // '/*' must be the whole pattern, not a prefix of another.
    CHECK(!p4gw::gitignoreIsAllowlist("/*.obj\n/src/*\n"));
    CHECK(!p4gw::gitignoreIsAllowlist(""));
}

TEST(ripgrep_config_detects_no_ignore_vcs) {
    CHECK(p4gw::ripgrepConfigDisablesVcsIgnore("--no-ignore-vcs\n"));
    // The broader flags imply skipping VCS ignore files too.
    CHECK(p4gw::ripgrepConfigDisablesVcsIgnore("--smart-case\n--no-ignore\n"));
    CHECK(p4gw::ripgrepConfigDisablesVcsIgnore("-u\n"));
    CHECK(p4gw::ripgrepConfigDisablesVcsIgnore("--unrestricted\n"));
    // Whitespace, CRLF, and a missing trailing newline are tolerated.
    CHECK(p4gw::ripgrepConfigDisablesVcsIgnore("  --no-ignore-vcs  \r\n"));
    CHECK(p4gw::ripgrepConfigDisablesVcsIgnore("--no-ignore-vcs"));
    // Later flags win, as on the assembled rg command line.
    CHECK(!p4gw::ripgrepConfigDisablesVcsIgnore(
        "--no-ignore-vcs\n--ignore-vcs\n"));
    CHECK(p4gw::ripgrepConfigDisablesVcsIgnore(
        "--ignore-vcs\n--no-ignore-vcs\n"));
}

TEST(ripgrep_config_rejects_comments_and_unrelated) {
    CHECK(!p4gw::ripgrepConfigDisablesVcsIgnore(""));
    CHECK(!p4gw::ripgrepConfigDisablesVcsIgnore("# --no-ignore-vcs\n"));
    CHECK(!p4gw::ripgrepConfigDisablesVcsIgnore("--smart-case\n--hidden\n"));
    // The flag must be the whole argument, not a substring of one.
    CHECK(!p4gw::ripgrepConfigDisablesVcsIgnore("--no-ignore-vcs-extra\n"));
}

TEST(config_parses_rgignore_key) {
    auto def = loadFromString("include = //d/src/... .p4gw/src\n");
    CHECK(def.has_value() && def->manageRgignore);  // managed by default
    auto off = loadFromString(
        "include = //d/src/... .p4gw/src\nrgignore = off\n");
    CHECK(off.has_value() && !off->manageRgignore);
    auto managed = loadFromString(
        "include = //d/src/... .p4gw/src\nrgignore = managed\n");
    CHECK(managed.has_value() && managed->manageRgignore);
    auto bad = loadFromString(
        "include = //d/src/... .p4gw/src\nrgignore = maybe\n");
    CHECK(!bad.has_value());
    if (!bad) CHECK(contains(bad.error(), "rgignore"));
}

TEST(rgignore_section_reopens_root_without_dot_entries) {
    const std::string out = p4gw::buildRgignoreSection({inc("src")}, {}, "");
    CHECK(contains(out, "!/[!.]*\n"));
    // No plain '!/*' anywhere - that would expose .git and the mirror.
    CHECK(!contains(out, "\n!/*\n"));
}

TEST(rgignore_section_reopens_carveouts_and_intermediate_peers) {
    const std::string out = p4gw::buildRgignoreSection(
        {inc("src"), exc("src/thirdparty"), exc("src/lib"),
         inc("src/lib/public/win64")},
        {}, "");
    // Plain carve-out: reopened whole.
    CHECK(contains(out, "!/src/thirdparty/\n"));
    // Re-include chain: each intermediate's re-excluded peers reopened,
    // still sparing dot entries.
    CHECK(contains(out, "!/src/lib/[!.]*\n"));
    CHECK(contains(out, "!/src/lib/public/[!.]*\n"));
}

TEST(rgignore_section_reasserts_denylists_after_the_reopens) {
    const std::string out = p4gw::buildRgignoreSection(
        {inc("src")}, {"/src/**/*.pdb"},
        "# known binaries\r\n/bin/\n\n*.dds\r\n");
    // Both denylists repeated, after the root reopen, comments dropped.
    const size_t reopen = out.find("!/[!.]*\n");
    CHECK(reopen != std::string::npos);
    CHECK(out.find("/src/**/*.pdb\n") > reopen);
    CHECK(out.find("/bin/\n") > reopen);
    CHECK(out.find("*.dds\n") > reopen);
    CHECK(!contains(out, "known binaries"));
}

TEST(rgignore_section_empty_for_denylist_style) {
    // A whole-repo include yields the denylist .gitignore - no block.
    CHECK(p4gw::buildRgignoreSection({inc("")}, {}, "/bin/\n").empty());
    CHECK(p4gw::buildRgignoreSection({inc(""), exc("lib")}, {}, "").empty());
}

TEST(rgignore_upsert_creates_and_prepends) {
    const std::string block = std::string(p4gw::kRgignoreBeginMarker) +
                              "\nbody\n" + p4gw::kRgignoreEndMarker + "\n";
    // Fresh file: just the block.
    auto fresh = p4gw::upsertRgignore("", "body\n");
    CHECK(fresh.has_value() && *fresh == block);
    // Existing hand-written rules: block prepended so later lines still win.
    auto prepended = p4gw::upsertRgignore("/user-rule/\n", "body\n");
    CHECK(prepended.has_value() && *prepended == block + "\n/user-rule/\n");
}

TEST(rgignore_upsert_rewrites_only_the_managed_block) {
    const std::string before = "# mine\n/keep-above/\n" +
                               std::string(p4gw::kRgignoreBeginMarker) +
                               "\nold\n" + p4gw::kRgignoreEndMarker +
                               "\n/keep-below/\n";
    auto updated = p4gw::upsertRgignore(before, "new\n");
    CHECK(updated.has_value());
    if (updated) {
        CHECK(contains(*updated, "# mine\n/keep-above/\n"));
        CHECK(contains(*updated, "\nnew\n"));
        CHECK(!contains(*updated, "old"));
        CHECK(contains(*updated, "/keep-below/\n"));
    }
    // An empty body removes the block, leaving the rest untouched.
    auto removed = p4gw::upsertRgignore(before, "");
    CHECK(removed.has_value() &&
          *removed == "# mine\n/keep-above/\n/keep-below/\n");
    // Empty body against a file with no block changes nothing.
    auto noop = p4gw::upsertRgignore("/user-rule/\n", "");
    CHECK(noop.has_value() && *noop == "/user-rule/\n");
}

TEST(rgignore_upsert_rejects_a_torn_block) {
    // A begin marker with no end: rewriting through it could eat user rules.
    auto torn = p4gw::upsertRgignore(
        std::string(p4gw::kRgignoreBeginMarker) + "\nold\n/user-rule/\n",
        "new\n");
    CHECK(!torn.has_value());
}

TEST(rgignore_refresh_writes_updates_and_respects_opt_out) {
    // refreshRgignore talks only to the filesystem (no git/p4), so it can be
    // driven for real against a temp repo root.
    const fs::path root = fs::temp_directory_path() / "p4gw_test_rgignore";
    fs::remove_all(root);
    fs::create_directories(root);
    auto write = [&](const char* name, const std::string& content) {
        std::ofstream f(root / name, std::ios::binary);
        f << content;
    };
    auto read = [&](const char* name) {
        std::ifstream f(root / name, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    };
    p4gw::Config config;
    config.rules = {inc("src")};

    // No allowlist .gitignore yet: nothing to do, no file appears.
    auto r = p4gw::refreshRgignore(config, root.string());
    CHECK(r.has_value() && !*r && !fs::exists(root / ".rgignore"));

    // Allowlist + .ignore: the managed block is written and re-asserts .ignore.
    write(".gitignore", p4gw::buildGitignore(config.rules));
    write(".ignore", "/bin/\n");
    r = p4gw::refreshRgignore(config, root.string());
    CHECK(r.has_value() && *r);
    CHECK(contains(read(".rgignore"), "!/[!.]*\n"));
    CHECK(contains(read(".rgignore"), "/bin/\n"));

    // Unchanged inputs: no rewrite. A hand-written rule outside the block and
    // a change to .ignore: rewritten, hand rule kept, new pattern folded in.
    r = p4gw::refreshRgignore(config, root.string());
    CHECK(r.has_value() && !*r);
    write(".rgignore", read(".rgignore") + "/hand-rule/\n");
    write(".ignore", "/bin/\n*.pak\n");
    r = p4gw::refreshRgignore(config, root.string());
    CHECK(r.has_value() && *r);
    CHECK(contains(read(".rgignore"), "*.pak\n"));
    CHECK(contains(read(".rgignore"), "/hand-rule/\n"));

    // rgignore = off: hands off even with stale content in the block.
    write(".ignore", "/bin/\n*.pak\n*.dds\n");
    config.manageRgignore = false;
    r = p4gw::refreshRgignore(config, root.string());
    CHECK(r.has_value() && !*r);
    CHECK(!contains(read(".rgignore"), "*.dds"));
    config.manageRgignore = true;

    // .gitignore replaced by a hand-kept denylist: the block is removed, the
    // hand rule survives.
    write(".gitignore", "/.p4gw/\np4gw.cfg\n");
    r = p4gw::refreshRgignore(config, root.string());
    CHECK(r.has_value() && *r);
    CHECK(!contains(read(".rgignore"), "!/[!.]*"));
    CHECK(contains(read(".rgignore"), "/hand-rule/\n"));
    fs::remove_all(root);
}

TEST(rgignore_reopens_root_detection) {
    CHECK(p4gw::rgignoreReopensRoot("!/[!.]*\n"));
    CHECK(p4gw::rgignoreReopensRoot("!/*\n"));
    CHECK(p4gw::rgignoreReopensRoot("  !/*  \r\n"));
    // The managed block's begin marker counts even mid-file.
    CHECK(p4gw::rgignoreReopensRoot("/user-rule/\n" +
                                    std::string(p4gw::kRgignoreBeginMarker) +
                                    "\n"));
    CHECK(!p4gw::rgignoreReopensRoot(""));
    CHECK(!p4gw::rgignoreReopensRoot("!/content/\n/bin/\n"));
    CHECK(!p4gw::rgignoreReopensRoot("# !/*\n"));
}