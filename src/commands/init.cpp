#include <cstdio>
#include <filesystem>
#include <fstream>

#include "commands.h"
#include "config.h"
#include "git.h"
#include "p4.h"

namespace fs = std::filesystem;

namespace p4gw {

// The verifying half of getting started ('gw setup' writes the config):
//   1. Load .p4gw (error pointing at 'gw setup' if absent or unfilled).
//   2. Ask p4 for the client spec and verify the view maps depot_path into
//      the mirror and nothing into this repo - fail loudly if not.
//   3. git init if needed (--force-git-init starts the repo over), write
//      and commit a starter .gitignore.
//   4. Point at the next steps: sync, then 'gw import'.
// Idempotent: re-running re-verifies the mapping and reuses what exists.
int cmdInit(const Args& args) {
    bool forceGitInit = false;
    for (const auto& arg : args) {
        if (arg == "--force-git-init") {
            forceGitInit = true;
        } else {
            std::fprintf(stderr, "gw init: unknown option '%s'\n",
                         arg.c_str());
            std::fprintf(stderr, "usage: gw init [--force-git-init]\n");
            return 1;
        }
    }

    std::string root;
    auto config = findAndLoadConfig(root);
    if (!config) {
        std::fprintf(stderr, "gw init: %s\n", config.error().c_str());
        return 1;
    }
    if (!config->depotPath.ends_with("/...")) {
        std::fprintf(stderr,
                     "gw init: depot_path must end with '/...' (got '%s') - "
                     "edit %s\n", config->depotPath.c_str(),
                     (fs::path(root) / ".p4gw").string().c_str());
        return 1;
    }
    if (config->mirrorPath.empty()) {
        std::fprintf(stderr,
                     "gw init: no 'mirror_path' in .p4gw - edit %s "
                     "('gw setup' writes the template)\n",
                     (fs::path(root) / ".p4gw").string().c_str());
        return 1;
    }
    const std::string mirrorDir = resolveMirrorPath(*config, root);

    // The whole point of init (vs. setup) is verification, so a dead p4
    // connection is a hard failure, not a skipped check.
    auto problems = p4::verifyViewMapping(*config, root, mirrorDir);
    if (!problems) {
        std::fprintf(stderr,
                     "gw init: cannot read the client spec: %s\n"
                     "init verifies the client view, so it needs a working "
                     "p4 connection\n(P4PORT/P4USER/P4CLIENT or a "
                     ".p4config).\n", problems.error().c_str());
        return 1;
    }
    if (!problems->empty()) {
        for (const auto& problem : *problems) {
            std::fprintf(stderr, "gw init: %s\n", problem.c_str());
        }
        std::fprintf(stderr,
                     "Fix the client view ('p4 client'), then rerun "
                     "'gw init'. The mapping line belongs\nat the END of the "
                     "view - later lines win.\n");
        return 1;
    }
    std::printf("ok    client view maps %s into the mirror\n",
                config->depotPath.c_str());

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
            std::fprintf(stderr,
                         "gw init: %s is inside the Git repo at %s - the "
                         "overlay root must be its own repo\n",
                         root.c_str(), toplevel->c_str());
            return 1;
        }
        std::printf("Using the existing Git repo at %s\n", root.c_str());
    } else {
        auto initialized =
            git::run({"init", "-b", config->baselineBranch}, root);
        if (!initialized) {
            std::fprintf(stderr, "gw init: %s\n", initialized.error().c_str());
            return 1;
        }
        std::printf("Initialized empty Git repository in %s\n", root.c_str());
    }

    const fs::path gitignore = fs::path(root) / ".gitignore";
    if (!fs::exists(gitignore)) {
        {
            // Close (flush) before `git add` sees the file.
            std::ofstream file(gitignore);
            file << "# gw's local config - personal, never goes to Git or P4\n"
                    ".p4gw\n";
        }
        std::printf("Wrote starter .gitignore\n");
    } else {
        std::printf("Keeping the existing .gitignore - make sure it ignores "
                    ".p4gw\n");
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

    if (fs::exists(mirrorDir)) {
        std::printf("Mirror directory exists: %s\n", mirrorDir.c_str());
    } else {
        std::printf("Mirror directory %s does not exist yet - it appears on "
                    "the first sync.\n", mirrorDir.c_str());
    }
    std::printf("\nAll set. Sync (any tool you like), then run 'gw import' "
                "to build the '%s' baseline.\n",
                config->baselineBranch.c_str());
    return 0;
}

}  // namespace p4gw
