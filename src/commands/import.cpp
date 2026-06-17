#include <cstdio>
#include <filesystem>

#include "commands.h"
#include "config.h"
#include "git.h"
#include "mirror.h"
#include "p4.h"

namespace fs = std::filesystem;

namespace p4gw {

// Absorbs the mirror's current state - whatever p4 last synced into it, by any
// tool - into Git, modelled on `git fetch` / `git pull --rebase`:
//
//   * The hidden ref refs/p4gw/<baseline> tracks pristine depot state (the
//     `origin/main` analog). `gw import` always advances it with a fresh
//     snapshot commit, built off to the side so the branch you are on is never
//     rewritten by the import itself.
//   * Your current branch is then brought up to the new depot state: a clean
//     fast-forward when it carries no local commits, or - with --rebase - your
//     commits are replayed on top. Without --rebase, divergent local commits
//     are left exactly where they are (never stomped) and you are told to
//     rebase.
//   * The like-named local branch (e.g. p4-main) is a convenience pointer kept
//     fast-forwarded to the hidden ref whenever it has no local commits, so the
//     feature-branch workflow (`git rebase p4-main`, prepare against it) keeps
//     working.
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
    const std::string depotRef = depotTrackingRef(*config);

    // Branchless users work detached with implicit branches and track commit
    // visibility in their own event log. When we detect it, import accepts a
    // detached HEAD and hands the restack to `git branchless sync` so every
    // visible stack moves and the old commits go obsolete - instead of
    // rebasing only the branch the user happens to be on.
    const bool branchless = git::isBranchless(root).value_or(false);
    if (branchless) {
        // `git branchless sync` restacks onto *its* main branch, so it must be
        // the gw baseline or the restack would land on the wrong trunk.
        auto mainBranch = git::configValue("branchless.core.mainBranch", root);
        if (mainBranch && *mainBranch != baseline) {
            auto set =
                git::setConfig("branchless.core.mainBranch", baseline, root);
            if (!set) {
                std::fprintf(stderr, "gw import: %s\n", set.error().c_str());
                return 1;
            }
            std::printf("note  pointed branchless's main branch at '%s'\n",
                        baseline.c_str());
        }
    }

    // Where the user is working. For branchless we track the commit (HEAD may
    // be detached); otherwise the branch name, and a detached HEAD is an error.
    // Empty on an unborn branch (no commits yet).
    const bool hasCommits = git::revParse("HEAD", root).has_value();
    std::string originalBranch;  // non-branchless: branch to return to
    std::string originalHead;    // branchless: commit to return to
    if (hasCommits) {
        if (branchless) {
            auto head = git::revParse("HEAD", root);
            if (!head) {
                std::fprintf(stderr, "gw import: %s\n", head.error().c_str());
                return 1;
            }
            originalHead = *head;
        } else {
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
        }
    }

    // Resolve the depot-tracking ref. If it does not exist yet but a legacy
    // baseline branch does, this is a repo from before the hidden-ref model:
    // seed the ref from the branch's most recent import commit (or its tip if
    // there isn't one) so existing repos keep working without surprises.
    std::string oldDepot;
    if (auto tip = git::revParse(depotRef, root)) {
        oldDepot = *tip;
    } else {
        auto baselineExists = git::branchExists(baseline, root);
        if (baselineExists && *baselineExists) {
            std::string seed;
            auto importCommit = git::latestCommitMatching(
                "^Import depot state", "refs/heads/" + baseline, root);
            if (importCommit && !importCommit->empty()) {
                seed = *importCommit;
            } else if (auto branchTip =
                           git::revParse("refs/heads/" + baseline, root)) {
                seed = *branchTip;
            }
            if (!seed.empty()) {
                auto seeded = git::updateRef(depotRef, seed, root);
                if (!seeded) {
                    std::fprintf(stderr, "gw import: %s\n",
                                 seeded.error().c_str());
                    return 1;
                }
                oldDepot = seed;
            }
        }
    }

    // From here failures may have moved HEAD off the user's branch; report it.
    auto fail = [&](const std::string& message) {
        std::fprintf(stderr, "gw import: %s\n", message.c_str());
        auto where = git::currentBranch(root);
        if (where && *where == "HEAD" && !originalHead.empty()) {
            std::fprintf(stderr,
                         "note: HEAD is detached mid-import; return with: "
                         "git switch --detach %s\n",
                         originalHead.c_str());
        } else if (where && *where == "HEAD" && !originalBranch.empty()) {
            std::fprintf(stderr,
                         "note: HEAD is detached mid-import; return with: "
                         "git switch -f %s\n",
                         originalBranch.c_str());
        } else if (where) {
            std::fprintf(stderr, "note: you are on '%s'\n", where->c_str());
        }
        return 1;
    };

    // Position the working tree on a pristine depot base, then overlay the
    // mirror. With an existing depot ref we detach onto it so the user's branch
    // is untouched; for the very first import we create the baseline branch and
    // commit there (there is no prior depot state to base a snapshot on).
    const bool firstImport = oldDepot.empty();
    if (firstImport) {
        if (hasCommits) {
            // Existing history, no baseline yet: start it as an orphan.
            auto switched = git::switchOrphanBranch(baseline, root);
            if (!switched) return fail(switched.error());
        } else {
            // Unborn branch: make sure the first commit lands on the baseline.
            auto unborn = git::run({"symbolic-ref", "--short", "HEAD"}, root);
            if (unborn && *unborn != baseline) {
                auto switched = git::switchOrphanBranch(baseline, root);
                if (!switched) return fail(switched.error());
            }
        }
    } else {
        auto switched = git::switchDetached(oldDepot, root);
        if (!switched) return fail(switched.error());
    }

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

    // Commit the snapshot (when there is anything new) and advance the depot
    // ref to it. newDepot stays at oldDepot when the mirror already matched.
    std::string newDepot = oldDepot;
    if (!*clean) {
        // Best-effort label: the company tool syncs different paths to
        // different CLs, so this is informational, not a guarantee.
        std::string message = "Import depot state";
        auto cl = p4::latestSubmittedCl(*config);
        if (cl && !cl->empty()) {
            message += " at CL " + *cl;
        }
        auto committed = git::commit(message, root);
        if (!committed) return fail(committed.error());
        auto tip = git::revParse("HEAD", root);
        if (!tip) return fail(tip.error());
        newDepot = *tip;
        auto advanced = git::updateRef(depotRef, newDepot, root);
        if (!advanced) return fail(advanced.error());
    }

    const bool importedNew = newDepot != oldDepot;

    if (branchless) {
        // Return HEAD to where the user was working (detached, branchless-
        // style) before restacking, so `git branchless sync` carries HEAD onto
        // the rewritten commit. The recorded commit still exists - sync marks
        // it obsolete, it is not lost (recoverable with `git undo`).
        if (!originalHead.empty()) {
            auto back = git::switchDetached(originalHead, root);
            if (!back) return fail(back.error());
        }

        // A first import against an empty mirror never created any commit.
        if (newDepot.empty()) {
            std::printf("Nothing to import - the mirror has no files yet.\n");
            return 0;
        }
        if (importedNew) {
            std::printf(
                "Imported depot state to '%s' (%zu files, %zu deleted)\n",
                depotRef.c_str(), copiedFiles, deletedFiles);
        } else {
            std::printf("Already up to date - the mirror matches the depot "
                        "baseline.\n");
        }

        // Keep the convenience baseline branch - branchless's main branch -
        // tracking the depot: create it if missing, fast-forward it when it
        // carries no local commits.
        auto baselineExists = git::branchExists(baseline, root);
        if (!baselineExists) return fail(baselineExists.error());
        if (*baselineExists) {
            auto branchFf =
                git::isAncestor("refs/heads/" + baseline, newDepot, root);
            if (!branchFf) return fail(branchFf.error());
            if (*branchFf) {
                auto moved =
                    git::updateRef("refs/heads/" + baseline, newDepot, root);
                if (!moved) return fail(moved.error());
            }
        } else {
            auto created =
                git::updateRef("refs/heads/" + baseline, newDepot, root);
            if (!created) return fail(created.error());
        }

        // Without --rebase, leave the user's stacks where they are (the same
        // "never stomp" default as the branch workflow) and point them at sync.
        if (!rebase) {
            std::printf("Your stacks were left as-is. Restack them onto the "
                        "new depot state with: gw import --rebase "
                        "(or git sync).\n");
            return 0;
        }

        // Restack every visible stack onto the new baseline in one shot: all
        // descendants of the old depot state move together, and branchless
        // records the rewrites so the pre-import commits become obsolete -
        // unlike the single-branch `git rebase` of the named-branch workflow.
        auto synced = git::branchlessSync(root);
        if (!synced) {
            std::fflush(stdout);  // keep messages ordered with stderr
            std::fprintf(stderr, "gw import: branchless sync stopped:\n%s\n",
                         synced.error().c_str());
            std::fprintf(stderr,
                         "Resolve the conflicts, then 'git rebase --continue' "
                         "(or 'git rebase --abort' to undo).\n");
            return 1;
        }
        std::printf("Restacked your visible commits onto the new depot "
                    "state.\n");
        return 0;
    }

    // Return to the user's branch: we may be on a detached snapshot, or on the
    // freshly created baseline after a first import with pre-existing history.
    // An unborn first import has no branch to go back to and stays on baseline.
    if (!originalBranch.empty()) {
        auto cur = git::currentBranch(root);
        if (cur && *cur != originalBranch) {
            auto switchedBack = git::switchBranch(originalBranch, root);
            if (!switchedBack) return fail(switchedBack.error());
        }
    }

    // A first import against an empty mirror never created any commit, so there
    // is no depot state to sync a branch to.
    if (newDepot.empty()) {
        std::printf("Nothing to import - the mirror has no files yet.\n");
        return 0;
    }

    if (importedNew) {
        std::printf("Imported depot state to '%s' (%zu files, %zu deleted)\n",
                    depotRef.c_str(), copiedFiles, deletedFiles);
    } else {
        std::printf("Already up to date - the mirror matches the depot "
                    "baseline.\n");
    }

    // Bring the user's branch up to the new depot state. `ffable` is true when
    // the branch has no local commits the depot lacks (a clean fast-forward);
    // `behind` is true when the depot has commits the branch lacks.
    const std::string current =
        originalBranch.empty() ? baseline : originalBranch;
    auto ffable = git::isAncestor(current, newDepot, root);
    if (!ffable) return fail(ffable.error());
    auto contains = git::isAncestor(newDepot, current, root);
    if (!contains) return fail(contains.error());
    const bool behind = !*contains;

    if (*ffable) {
        auto ff = git::mergeFastForward(newDepot, root);  // no-op when in sync
        if (!ff) return fail(ff.error());
    } else if (behind && rebase) {
        auto rebased = git::rebase(newDepot, root);
        if (!rebased) {
            std::fflush(stdout);  // keep messages ordered with stderr
            std::fprintf(stderr, "gw import: rebase stopped:\n%s\n",
                         rebased.error().c_str());
            std::fprintf(stderr,
                         "Resolve the conflicts, then 'git rebase --continue' "
                         "(or 'git rebase --abort' to undo).\n");
            return 1;
        }
        std::printf("Rebased '%s' onto the new depot state.\n", current.c_str());
    }

    // Keep the convenience baseline branch tracking the depot ref when it has
    // no local commits of its own (a clean fast-forward). If it has diverged,
    // it is the user's working branch - leave it alone.
    if (current != baseline) {
        auto baselineExists = git::branchExists(baseline, root);
        if (baselineExists && *baselineExists) {
            auto branchFf =
                git::isAncestor("refs/heads/" + baseline, newDepot, root);
            if (branchFf && *branchFf) {
                auto moved =
                    git::updateRef("refs/heads/" + baseline, newDepot, root);
                if (!moved) return fail(moved.error());
            }
        }
    }

    if (originalBranch.empty() || (originalBranch == baseline && *ffable)) {
        std::printf("You are on '%s'. Start work with: git switch -c <branch>\n",
                    baseline.c_str());
        return 0;
    }
    if (behind && !*ffable && !rebase) {
        std::printf("'%s' has local commits and was left as-is. Rebase onto "
                    "the new depot state with: gw import --rebase (or "
                    "git rebase %s)\n",
                    current.c_str(), baseline.c_str());
    }
    return 0;
}

}  // namespace p4gw
