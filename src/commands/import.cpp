#include <cstdio>
#include <filesystem>

#include "commands.h"
#include "config.h"
#include "git.h"
#include "mirror.h"
#include "p4.h"

namespace fs = std::filesystem;

namespace p4gw {

// Commits the current state of the mirror directory - whatever p4 last
// synced into it, by any tool - as a new commit on the baseline branch.
// Analogous to `git fetch` (or `git pull` with --rebase):
//   1. Require a clean working tree; remember the current branch.
//   2. Check out the baseline branch (created on first import).
//   3. Make the working tree match the mirror, commit.
//   4. Switch back; with --rebase, rebase the branch onto the new baseline.
int cmdImport(const Args& args) {
    bool rebase = false;
    for (const auto& arg : args) {
        if (arg == "--rebase") {
            rebase = true;
        } else {
            std::fprintf(stderr, "gw import: unknown option '%s'\n", arg.c_str());
            std::fprintf(stderr, "usage: gw import [--rebase]\n");
            return 1;
        }
    }

    std::string root;
    auto config = findAndLoadConfig(root);
    if (!config) {
        std::fprintf(stderr, "gw import: %s\n", config.error().c_str());
        return 1;
    }
    for (const auto& mapping : config->mappings) {
        const std::string mirrorDir = resolveMirrorPath(mapping.mirrorPath, root);
        if (!fs::exists(mirrorDir)) {
            std::fprintf(stderr,
                         "gw import: mirror directory %s does not exist.\n"
                         "Check the client view mapping ('gw doctor') and sync "
                         "before importing.\n",
                         mirrorDir.c_str());
            return 1;
        }
    }

    auto dirty = git::isDirty(root);
    if (!dirty) {
        std::fprintf(stderr, "gw import: %s\n", dirty.error().c_str());
        return 1;
    }
    if (*dirty) {
        std::fprintf(stderr,
                     "gw import: working tree is not clean - commit, stash, "
                     "or gitignore your changes first\n");
        return 1;
    }

    const std::string& baseline = config->baselineBranch;

    // A repo fresh from `gw init` has no commits yet; in that case we stay
    // where we are and the import becomes the baseline's first commit.
    const bool hasCommits = git::revParse("HEAD", root).has_value();
    std::string originalBranch;
    if (hasCommits) {
        auto branch = git::currentBranch(root);
        if (!branch) {
            std::fprintf(stderr, "gw import: %s\n", branch.error().c_str());
            return 1;
        }
        originalBranch = *branch;
        if (originalBranch == "HEAD") {
            std::fprintf(stderr,
                         "gw import: HEAD is detached - switch to a branch "
                         "first\n");
            return 1;
        }
    } else {
        // Unborn branch: make sure the first commit lands on the baseline.
        auto unborn = git::run({"symbolic-ref", "--short", "HEAD"}, root);
        if (unborn && *unborn != baseline) {
            auto switched = git::switchOrphanBranch(baseline, root);
            if (!switched) {
                std::fprintf(stderr, "gw import: %s\n",
                             switched.error().c_str());
                return 1;
            }
        }
    }

    if (hasCommits && originalBranch != baseline) {
        auto exists = git::branchExists(baseline, root);
        if (!exists) {
            std::fprintf(stderr, "gw import: %s\n", exists.error().c_str());
            return 1;
        }
        auto switched = *exists ? git::switchBranch(baseline, root)
                                : git::switchOrphanBranch(baseline, root);
        if (!switched) {
            std::fprintf(stderr, "gw import: %s\n", switched.error().c_str());
            return 1;
        }
    }

    // From here on we are on the baseline branch; any failure should report
    // that fact so the user knows where they were left.
    auto fail = [&](const std::string& message) {
        std::fprintf(stderr, "gw import: %s\n", message.c_str());
        std::fprintf(stderr, "note: you are on branch '%s'\n", baseline.c_str());
        return 1;
    };

    auto tracked = git::lsFiles(root);
    if (!tracked) return fail(tracked.error());

    // Files already opened in the mirror (a mid-prepare CL, a stray p4 edit)
    // have an un-submitted working copy. The baseline must stay pristine
    // submitted depot state, so for those files read the depot head instead of
    // copying the mirror, and omit add-only files (no depot revision exists).
    auto opened = p4::openedFilesTagged(*config);
    if (!opened) return fail(opened.error());

    // Each mapping is its own depot subtree, synced into its own mirror and
    // living under its own working-tree directory; import them independently
    // and tally for the summary.
    size_t copiedFiles = 0;
    size_t deletedFiles = 0;
    for (const auto& mapping : config->mappings) {
        const std::string mirrorDir =
            resolveMirrorPath(mapping.mirrorPath, root);
        const std::string subtreePrefix =
            mapping.repoSubtree.empty() ? std::string{}
                                        : mapping.repoSubtree + "/";
        const std::string worktreeDir =
            mapping.repoSubtree.empty()
                ? root
                : (fs::path(root) / mapping.repoSubtree).string();

        auto mirrorFiles = mirror::listFiles(mirrorDir);
        if (!mirrorFiles) return fail(mirrorFiles.error());

        // Tracked files under this subtree, made mirror-relative so they line
        // up with the mirror's own (subtree-rooted) listing.
        std::vector<std::string> trackedHere;
        for (const auto& path : *tracked) {
            if (subtreePrefix.empty()) {
                trackedHere.push_back(path);
            } else if (path.starts_with(subtreePrefix)) {
                trackedHere.push_back(path.substr(subtreePrefix.size()));
            }
        }

        const auto actions =
            mirror::computeSyncActions(*mirrorFiles, trackedHere);

        std::vector<mirror::OpenedMirrorFile> openedMirror;
        for (const auto& o : *opened) {
            std::string rel =
                p4::depotRelativePath(mapping.depotPath, o.depotFile);
            if (rel.empty()) continue;  // belongs to a different mapping
            openedMirror.push_back({std::move(rel), !p4::isAddAction(o.action)});
        }
        const auto plan = mirror::planImport(actions, openedMirror);

        auto applied =
            mirror::applySyncActions(plan.actions, mirrorDir, worktreeDir);
        if (!applied) return fail(applied.error());

        // Restore depot-head content for files open in the mirror.
        if (!plan.depotReads.empty()) {
            std::string depotBase = mapping.depotPath;
            if (depotBase.ends_with("...")) {
                depotBase.resize(depotBase.size() - 3);
            }
            for (const auto& rel : plan.depotReads) {
                const fs::path dest = fs::path(worktreeDir) / fs::path(rel);
                std::error_code ec;
                fs::create_directories(dest.parent_path(), ec);
                if (ec) {
                    return fail("failed to create directory " +
                                dest.parent_path().string() + ": " +
                                ec.message());
                }
                auto printed =
                    p4::printHeadToFile(*config, depotBase + rel, dest.string());
                if (!printed) return fail(printed.error());
                fs::permissions(dest, fs::perms::owner_write,
                                fs::perm_options::add, ec);
            }
        }
        copiedFiles += plan.actions.copies.size() + plan.depotReads.size();
        deletedFiles += plan.actions.deletes.size();
    }

    auto added = git::addAll(root);
    if (!added) return fail(added.error());
    auto clean = git::indexMatchesHead(root);
    if (!clean) return fail(clean.error());

    if (*clean && hasCommits) {
        std::printf("Already up to date - the mirror matches '%s'.\n",
                    baseline.c_str());
    } else {
        // Best-effort label: the company tool syncs different paths to
        // different CLs, so this is informational, not a guarantee.
        std::string message = "Import depot state";
        auto cl = p4::latestSubmittedCl(*config);
        if (cl && !cl->empty()) {
            message += " at CL " + *cl;
        }
        auto committed = git::commit(message, root);
        if (!committed) return fail(committed.error());
        std::printf("Committed depot state to '%s' (%zu files, %zu deleted)\n",
                    baseline.c_str(), copiedFiles, deletedFiles);
    }

    if (originalBranch.empty() || originalBranch == baseline) {
        std::printf("You are on '%s'. Start work with: git switch -c <branch>\n",
                    baseline.c_str());
        return 0;
    }

    auto switchedBack = git::switchBranch(originalBranch, root);
    if (!switchedBack) return fail(switchedBack.error());

    if (rebase) {
        auto rebased = git::rebase(baseline, root);
        if (!rebased) {
            std::fflush(stdout);  // keep messages ordered with stderr
            std::fprintf(stderr, "gw import: rebase stopped:\n%s\n",
                         rebased.error().c_str());
            std::fprintf(stderr,
                         "Resolve the conflicts, then 'git rebase --continue' "
                         "(or 'git rebase --abort' to undo).\n");
            return 1;
        }
        std::printf("Rebased '%s' onto '%s'.\n", originalBranch.c_str(),
                    baseline.c_str());
    } else {
        std::printf("Back on '%s'. Rebase onto the new baseline with: "
                    "git rebase %s (or rerun with --rebase)\n",
                    originalBranch.c_str(), baseline.c_str());
    }
    return 0;
}

}  // namespace p4gw
