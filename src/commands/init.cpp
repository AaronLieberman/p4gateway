#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "commands.h"
#include "config.h"
#include "git.h"
#include "p4.h"
#include "process.h"

namespace fs = std::filesystem;

namespace p4gw {

namespace {

// Default the new repo's Git identity to the local login account. These commits
// never reach P4 (P4 only ever sees filesystem state), so there is no reason to
// inherit the user's global Git name/email here - scope the identity to the
// minimum we know. Only fills in keys the user hasn't already set repo-locally,
// so a deliberate local identity (or a re-run of 'gw init') is left untouched.
void defaultLocalIdentity(const std::string& root) {
    const std::string login = currentUser();
    if (login.empty()) return;
    const std::pair<const char*, std::string> entries[] = {
        {"user.name", login},
        {"user.email", login + "@localhost"},
    };
    bool setAny = false;
    for (const auto& [key, value] : entries) {
        auto existing = git::configValue(key, root, /*localOnly=*/true);
        if (existing && !existing->empty()) continue;  // respect a manual choice
        auto set = git::setConfig(key, value, root);
        if (set) {
            setAny = true;
        } else {
            std::printf("note: could not set %s (%s); commits will fall back to "
                        "your global Git identity\n",
                        key, set.error().c_str());
        }
    }
    if (setAny) {
        std::printf("Set the repo's Git identity to '%s' (local account - these "
                    "commits stay local; P4 only sees filesystem state)\n",
                    login.c_str());
    }
}

constexpr const char* kInitUsage =
    "usage: gw init [options]\n"
    "\n"
    "The verifying half of getting started ('gw setup' writes the config):\n"
    "load p4gw.cfg, ask p4 for the client spec and verify the view maps every\n"
    "include into the mirror (and nothing into the repo outside it - a hard\n"
    "failure if not), then git init if needed and write/commit a starter\n"
    ".gitignore allowlist. Idempotent: re-running re-verifies and reuses what\n"
    "exists.\n"
    "\n"
    "options:\n"
    "  --force-git-init  Remove any existing .git and start the Git repo over\n"
    "  --allow-in-repo   Permit the overlay root to sit inside an outer Git repo\n"
    "                    (creates its own isolated .git there)\n"
    "  -h, --help        Show this help\n"
    "\n";

}  // namespace

// The verifying half of getting started ('gw setup' writes the config):
//   1. Load p4gw.cfg (error pointing at 'gw setup' if absent or unfilled).
//   2. Ask p4 for the client spec and verify the view maps depot_path into
//      the mirror and nothing into this repo - fail loudly if not.
//   3. git init if needed (--force-git-init starts the repo over), write
//      and commit a starter .gitignore.
//   4. Point at the next steps: sync, then 'gw import'.
// Idempotent: re-running re-verifies the mapping and reuses what exists.
int cmdInit(const Args& args) {
    bool forceGitInit = false;
    bool allowInRepo = false;
    for (const auto& arg : args) {
        if (arg == "--help" || arg == "-h") {
            std::printf("%s", kInitUsage);
            return 0;
        } else if (arg == "--force-git-init") {
            forceGitInit = true;
        } else if (arg == "--allow-in-repo") {
            allowInRepo = true;
        } else {
            std::fprintf(stderr, "gw init: unknown option '%s'\n",
                         arg.c_str());
            std::fprintf(stderr, "%s", kInitUsage);
            return 1;
        }
    }

    std::string root;
    auto config = findAndLoadConfig(root);
    if (!config) {
        std::fprintf(stderr, "gw init: %s\n", config.error().c_str());
        return 1;
    }
    // The whole point of init (vs. setup) is verification, so a dead p4
    // connection is a hard failure, not a skipped check.
    auto spec = p4::clientSpec(*config);
    if (!spec) {
        std::fprintf(stderr,
                     "gw init: cannot read the client spec: %s\n"
                     "init verifies the client view, so it needs a working "
                     "p4 connection\n(P4PORT/P4USER/P4CLIENT or a "
                     ".p4config).\n", spec.error().c_str());
        return 1;
    }
    std::vector<p4::ViewProblem> problems;
    const auto includes = includeRules(config->rules);
    const auto allExcludes = excludeDepotPaths(config->rules);
    for (const auto* rule : includes) {
        const std::string mirrorDir =
            resolveMirrorPath(rule->mirrorPath, root);
        std::vector<std::string> otherMirrors;
        for (const auto* other : includes) {
            if (other != rule) {
                otherMirrors.push_back(
                    resolveMirrorPath(other->mirrorPath, root));
            }
        }
        const auto mappingProblems = p4::checkSpecMapping(
            *spec, rule->depotPath, root, mirrorDir, allExcludes, otherMirrors);
        problems.insert(problems.end(), mappingProblems.begin(),
                        mappingProblems.end());
    }
    if (!problems.empty()) {
        // Two kinds of problem want two kinds of advice. "General" ones (a
        // missing/mis-ordered remap) are fixed in the client view; "diversion"
        // ones (a subtree sub-path syncing in place) are fixed in p4gw.cfg.
        // Group the latter into a single hint with copy-pasteable `exclude`
        // lines, rather than repeating the same explanation per view line.
        std::vector<std::string> divertPaths;
        bool anyGeneral = false;
        for (const auto& problem : problems) {
            if (problem.excludePath.empty()) {
                std::fprintf(stderr, "gw init: %s\n", problem.message.c_str());
                anyGeneral = true;
            } else {
                divertPaths.push_back(problem.excludePath);
            }
        }
        if (anyGeneral) {
            std::fprintf(stderr,
                         "Fix the client view ('p4 client'), then rerun "
                         "'gw init'. Each include's remap line\nbelongs after "
                         "any view line it overlaps - later lines win.\n");
        }
        if (!divertPaths.empty()) {
            if (anyGeneral) std::fprintf(stderr, "\n");
            std::fprintf(
                stderr,
                "gw init: %zu client view line(s) sync part of a mapped subtree "
                "in place,\noutside the mirror. gw would track those P4-owned "
                "files in Git unless you\ndeclare them. Add to p4gw.cfg:\n\n",
                divertPaths.size());
            for (const auto& ex : p4::minimalExcludePaths(divertPaths)) {
                std::fprintf(stderr, "    exclude = %s\n", ex.c_str());
            }
            std::fprintf(stderr, "\nthen rerun 'gw init'.\n");
        }
        return 1;
    }
    std::printf("ok    client view maps all %zu include(s) into the mirror\n",
                includes.size());

    const fs::path gitDir = fs::path(root) / ".git";
    if (forceGitInit && fs::exists(gitDir)) {
        std::error_code ec;
        fs::remove_all(gitDir, ec);
        if (ec) {
            std::fprintf(stderr, "gw init: cannot remove %s: %s\n",
                         gitDir.string().c_str(), ec.message().c_str());
            return 1;
        }
        std::printf("Removed the existing Git repo (--force-git-init)\n");
    }

    auto toplevel = git::run({"rev-parse", "--show-toplevel"}, root);
    if (toplevel) {
        if (!fs::equivalent(fs::path(*toplevel), fs::path(root))) {
            if (!allowInRepo) {
                std::fprintf(stderr,
                             "gw init: %s is inside the Git repo at %s - the "
                             "overlay root must be its own repo\n",
                             root.c_str(), toplevel->c_str());
                return 1;
            }
            std::printf("note  %s is inside an outer Git repo (--allow-in-repo)\n",
                        root.c_str());
            // root has no .git of its own — create one so it is isolated
            // from the outer repo; git operations then resolve to root, not
            // to the enclosing repo.
            if (!fs::exists(fs::path(root) / ".git")) {
                auto initialized =
                    git::run({"init", "-b", config->baselineBranch}, root);
                if (!initialized) {
                    std::fprintf(stderr, "gw init: %s\n",
                                 initialized.error().c_str());
                    return 1;
                }
                std::printf("Initialized empty Git repository in %s\n",
                            root.c_str());
            }
        } else {
            std::printf("Using the existing Git repo at %s\n", root.c_str());
        }
    } else {
        auto initialized =
            git::run({"init", "-b", config->baselineBranch}, root);
        if (!initialized) {
            std::fprintf(stderr, "gw init: %s\n", initialized.error().c_str());
            return 1;
        }
        std::printf("Initialized empty Git repository in %s\n", root.c_str());
    }

    // Scope this repo's commit identity to the local login account by default,
    // before the first commit below uses it.
    defaultLocalIdentity(root);

    // Enable git's untracked-file cache for this repo. On a big mapped subtree
    // `gw import`'s `git add -A` re-walks the whole working tree to find what
    // changed; the cache lets git skip directories whose mtime is unchanged,
    // cutting that scan on re-imports. It is a per-repo, on-disk cache - no
    // background process (unlike core.fsmonitor) - and safe on local disks. A
    // deliberate local setting is left untouched.
    {
        auto existing =
            git::configValue("core.untrackedCache", root, /*localOnly=*/true);
        if (existing && existing->empty()) {
            auto set = git::setConfig("core.untrackedCache", "true", root);
            if (set) {
                std::printf("Enabled core.untrackedCache (speeds up 'gw import' "
                            "file scans; per-repo, no background process)\n");
            } else {
                std::printf("note: could not enable core.untrackedCache (%s)\n",
                            set.error().c_str());
            }
        }
    }

    // Gitignore each mirror container that lives inside the repo (the carved-
    // out `.p4gw` subtree p4 syncs into). Mappings share one container, so
    // dedupe to a single entry in the common case.
    std::vector<std::string> mirrorEntries;
    for (const auto* rule : includes) {
        const std::string mirrorDir =
            resolveMirrorPath(rule->mirrorPath, root);
        std::error_code ec;
        const fs::path rel = fs::relative(mirrorDir, root, ec);
        if (ec || rel.empty() || rel.begin()->string() == "..") continue;
        const std::string entry = rel.begin()->string() + "/";
        if (std::find(mirrorEntries.begin(), mirrorEntries.end(), entry) ==
            mirrorEntries.end()) {
            mirrorEntries.push_back(entry);
        }
    }

    // Re-exclusions for each carved-out subtree (an `exclude` line): they sync
    // in place like unmapped depot content and must stay out of Git, the same
    // as `/src/thirdparty/`. buildGitignore writes these for a fresh allowlist;
    // for an existing .gitignore we append any that are missing below.
    std::vector<std::string> excludeEntries;
    for (const auto& rule : config->rules) {
        if (!rule.exclude || rule.repoSubtree.empty()) continue;
        // A carve-out with a deeper re-`include` is not a plain re-exclusion
        // (that would hide the re-included subtree); buildGitignore handles it
        // as a nested allowlist. Only append the plain carve-outs here.
        const bool hasReinclude =
            std::any_of(config->rules.begin(), config->rules.end(),
                        [&](const ViewRule& o) {
                            return !o.exclude &&
                                   o.repoSubtree.starts_with(
                                       rule.repoSubtree + "/");
                        });
        if (hasReinclude) continue;
        const std::string entry = "/" + rule.repoSubtree + "/";
        if (std::find(excludeEntries.begin(), excludeEntries.end(), entry) ==
            excludeEntries.end()) {
            excludeEntries.push_back(entry);
        }
    }

    const fs::path gitignore = fs::path(root) / ".gitignore";
    bool wroteGitignore = false;  // gw authored or appended to .gitignore
    if (!fs::exists(gitignore)) {
        {
            // Close (flush) before `git add` sees the file.
            std::ofstream file(gitignore);
            file << buildGitignore(config->rules, config->ignorePatterns);
        }
        std::printf("Wrote starter .gitignore\n");
        wroteGitignore = true;
    } else {
        std::printf("Keeping the existing .gitignore - gw only auto-generates "
                    "the allowlist for a fresh one. Make sure yours ignores "
                    "p4gw.cfg and any unmapped depot content that syncs in "
                    "place, so Git tracks only the mapped subtrees.\n");
        std::ifstream in(gitignore);
        std::string content((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
        in.close();
        std::ofstream out(gitignore, std::ios::app);
        for (const auto& entry : mirrorEntries) {
            if (content.find(entry) == std::string::npos) {
                out << "\n# gw's mirror directory - P4-managed, not for Git\n"
                    << entry << "\n";
                std::printf("Added %s to .gitignore\n", entry.c_str());
                wroteGitignore = true;
            }
        }
        for (const auto& entry : excludeEntries) {
            if (content.find(entry) == std::string::npos) {
                out << "\n# carved out of the mirror ('exclude') - syncs in "
                       "place, not for Git\n"
                    << entry << "\n";
                std::printf("Added %s to .gitignore\n", entry.c_str());
                wroteGitignore = true;
            }
        }
        for (const auto& pattern : config->ignorePatterns) {
            if (content.find(pattern) == std::string::npos) {
                out << "\n# p4gw.cfg 'ignore' - a file P4 ignores, not for Git\n"
                    << pattern << "\n";
                std::printf("Added %s to .gitignore\n", pattern.c_str());
                wroteGitignore = true;
            }
        }
    }
    // Land .gitignore so the first 'gw import' starts from a clean tree. On a
    // brand-new (or --force-git-init) repo there is no history to disturb, so
    // commit it as the first commit. On a repo that already has commits we do
    // NOT inject a commit onto the user's current branch (it could be a feature
    // branch, or carry their own staged changes); instead, when gw wrote or
    // changed the file, point them at committing it themselves.
    if (!git::revParse("HEAD", root)) {
        auto added = git::run({"add", ".gitignore"}, root);
        auto committed =
            added ? git::commit("gw init", root) : std::move(added);
        if (!committed) {
            std::printf("note: could not commit .gitignore (%s); commit it "
                        "yourself before 'gw import'\n",
                        committed.error().c_str());
        }
    } else if (wroteGitignore) {
        std::printf("Commit the updated .gitignore before 'gw import' (the tree "
                    "must be clean):\n"
                    "  git add .gitignore && git commit -m \"gw: ignore mirror "
                    "and carved-out dirs\"\n");
    }

    // If the repo is managed by git-branchless, its main branch is the trunk
    // `gw import --rebase` restacks onto (via `git branchless sync`), so it
    // should be the gw baseline. We don't modify branchless's config - that is
    // the user's tool to manage - we just flag a mismatch so the restack does
    // not land on the wrong trunk.
    if (git::isBranchless(root).value_or(false)) {
        auto mainBranch = git::configValue("branchless.core.mainBranch", root);
        if (mainBranch && !mainBranch->empty() &&
            *mainBranch != config->baselineBranch) {
            std::printf("note  git-branchless's main branch is '%s', not the gw "
                        "baseline '%s';\n      'gw import --rebase' restacks "
                        "onto branchless's main branch, so align them - set\n"
                        "      baseline_branch in p4gw.cfg, or repoint "
                        "branchless: git config --worktree\n"
                        "      branchless.core.mainBranch %s\n",
                        mainBranch->c_str(), config->baselineBranch.c_str(),
                        config->baselineBranch.c_str());
        }
    }

    for (const auto* rule : includes) {
        const std::string mirrorDir =
            resolveMirrorPath(rule->mirrorPath, root);
        if (fs::exists(mirrorDir)) {
            std::printf("Mirror directory exists: %s\n", mirrorDir.c_str());
        } else {
            std::printf("Mirror directory %s does not exist yet - it appears "
                        "on the first sync.\n", mirrorDir.c_str());
        }
    }
    std::printf("\nAll set. Sync (any tool you like), then run 'gw import' "
                "to build the '%s' baseline.\n",
                config->baselineBranch.c_str());
    return 0;
}

}  // namespace p4gw
