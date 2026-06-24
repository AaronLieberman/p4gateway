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

namespace fs = std::filesystem;

namespace p4gw {

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
        if (arg == "--force-git-init") {
            forceGitInit = true;
        } else if (arg == "--allow-in-repo") {
            allowInRepo = true;
        } else {
            std::fprintf(stderr, "gw init: unknown option '%s'\n",
                         arg.c_str());
            std::fprintf(stderr,
                         "usage: gw init [--force-git-init] [--allow-in-repo]\n");
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
    std::vector<std::string> problems;
    for (const auto& mapping : config->mappings) {
        const std::string mirrorDir =
            resolveMirrorPath(mapping.mirrorPath, root);
        const auto mappingProblems = p4::checkSpecMapping(
            *spec, mapping.depotPath, root, mirrorDir,
            mapping.excludedDepotPaths);
        problems.insert(problems.end(), mappingProblems.begin(),
                        mappingProblems.end());
    }
    if (!problems.empty()) {
        for (const auto& problem : problems) {
            std::fprintf(stderr, "gw init: %s\n", problem.c_str());
        }
        std::fprintf(stderr,
                     "Fix the client view ('p4 client'), then rerun "
                     "'gw init'. Each mapping line belongs\nafter any view "
                     "line it overlaps - later lines win.\n");
        return 1;
    }
    std::printf("ok    client view maps all %zu mapping(s) into the mirror\n",
                config->mappings.size());

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

    // Gitignore each mirror container that lives inside the repo (the carved-
    // out `.p4gw` subtree p4 syncs into). Mappings share one container, so
    // dedupe to a single entry in the common case.
    std::vector<std::string> mirrorEntries;
    for (const auto& mapping : config->mappings) {
        const std::string mirrorDir =
            resolveMirrorPath(mapping.mirrorPath, root);
        std::error_code ec;
        const fs::path rel = fs::relative(mirrorDir, root, ec);
        if (ec || rel.empty() || rel.begin()->string() == "..") continue;
        const std::string entry = rel.begin()->string() + "/";
        if (std::find(mirrorEntries.begin(), mirrorEntries.end(), entry) ==
            mirrorEntries.end()) {
            mirrorEntries.push_back(entry);
        }
    }

    const fs::path gitignore = fs::path(root) / ".gitignore";
    if (!fs::exists(gitignore)) {
        {
            // Close (flush) before `git add` sees the file.
            std::ofstream file(gitignore);
            file << buildGitignore(config->mappings);
        }
        std::printf("Wrote starter .gitignore\n");
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
            }
        }
    }
    // In a brand-new (or --force-git-init) repo, commit the .gitignore so
    // the first 'gw import' starts from a clean tree.
    if (!git::revParse("HEAD", root)) {
        auto added = git::run({"add", ".gitignore"}, root);
        auto committed =
            added ? git::commit("gw init", root) : std::move(added);
        if (!committed) {
            std::printf("note: could not commit .gitignore (%s); commit it "
                        "yourself before 'gw import'\n",
                        committed.error().c_str());
        }
    }

    // If the repo is managed by git-branchless, point its main branch at the
    // gw baseline so `gw import --rebase` (which delegates to `git branchless
    // sync`) restacks stacks onto the depot state. We never initialize
    // branchless for the user - that is their choice - but when it is present
    // this keeps the two tools' idea of "trunk" aligned.
    if (git::isBranchless(root).value_or(false)) {
        auto mainBranch = git::configValue("branchless.core.mainBranch", root);
        if (mainBranch && *mainBranch != config->baselineBranch) {
            auto set = git::setConfig("branchless.core.mainBranch",
                                      config->baselineBranch, root);
            if (set) {
                std::printf("Pointed branchless's main branch at '%s'\n",
                            config->baselineBranch.c_str());
            } else {
                std::printf("note: could not set branchless.core.mainBranch "
                            "(%s); set it to '%s' by hand\n",
                            set.error().c_str(),
                            config->baselineBranch.c_str());
            }
        }
    }

    for (const auto& mapping : config->mappings) {
        const std::string mirrorDir =
            resolveMirrorPath(mapping.mirrorPath, root);
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
