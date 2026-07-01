#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unordered_set>

#include "commands.h"
#include "config.h"
#include "git.h"
#include "mirror.h"
#include "p4.h"

namespace fs = std::filesystem;

namespace p4gw {

namespace {

constexpr const char* kImportUsage =
    "usage: gw import [options]\n"
    "\n"
    "Commit the mirror's current state - whatever you last synced, with any\n"
    "tool - to the depot baseline (the hidden refs/p4gw/<baseline> ref), then\n"
    "bring your branch up to it. Like 'git fetch' / 'git pull --rebase'.\n"
    "A branch with no local commits fast-forwards; divergent commits are left\n"
    "untouched unless you pass --rebase.\n"
    "\n"
    "options:\n"
    "  -r, --rebase  Replay your local commits on top of the new depot state\n"
    "                (otherwise divergent branches are left as-is)\n"
    "  -h, --help    Show this help\n"
    "\n";

}  // namespace

// Absorbs the mirror's current state - whatever p4 last synced into it, by any
// tool - into Git, modelled on `git fetch` / `git pull --rebase`:
//
//   * The hidden ref refs/p4gw/<baseline> tracks pristine depot state (the
//     `origin/main` analog). `gw import` always advances it with a fresh
//     snapshot commit, built off to the side so the branch you are on is never
//     rewritten by the import itself.
//   * Where you are working is then brought up to the new depot state. Because
//     the baseline lives on the hidden ref, import never needs you to be on a
//     branch - a detached HEAD is fine. On a named branch it fast-forwards (no
//     local commits) or, with --rebase, replays your commits on top. Detached,
//     it leaves HEAD where it is or - with --rebase - rebases that line; a
//     git-branchless repo instead restacks every visible stack via
//     `git branchless sync` (the one place a branchless command is needed).
//     Without --rebase, divergent commits are left exactly where they are
//     (never stomped) and you are told how to restack.
//   * The like-named local branch (e.g. main) is a convenience pointer kept
//     fast-forwarded to the hidden ref whenever it has no local commits, so the
//     feature-branch workflow (`git rebase main`, prepare against it) keeps
//     working.
int cmdImport(const Args& args) {
    bool rebase = false;
    for (const auto& arg : args) {
        if (arg == "--help" || arg == "-h") {
            std::printf("%s", kImportUsage);
            return 0;
        } else if (arg == "--rebase" || arg == "-r") {
            rebase = true;
        } else {
            std::fprintf(stderr, "gw import: unknown option '%s'\n", arg.c_str());
            std::fprintf(stderr, "%s", kImportUsage);
            return 1;
        }
    }

    std::string root;
    auto config = findAndLoadConfig(root);
    if (!config) {
        std::fprintf(stderr, "gw import: %s\n", config.error().c_str());
        return 1;
    }
    for (const auto* rule : includeRules(config->rules)) {
        const std::string mirrorDir = resolveMirrorPath(rule->mirrorPath, root);
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
        // `git branchless sync` restacks onto *its* main branch, so a mismatch
        // with the gw baseline would land the restack on the wrong trunk. We do
        // not modify branchless's config (the user's tool to manage) - just warn.
        auto mainBranch = git::configValue("branchless.core.mainBranch", root);
        if (mainBranch && !mainBranch->empty() && *mainBranch != baseline) {
            std::printf("note  git-branchless's main branch is '%s', not the "
                        "baseline '%s';\n      --rebase restacks onto "
                        "branchless's main branch. Align them to avoid "
                        "restacking\n      onto the wrong trunk (set "
                        "baseline_branch, or 'git config --worktree "
                        "branchless.core.mainBranch %s').\n",
                        mainBranch->c_str(), baseline.c_str(), baseline.c_str());
        }
    }

    // Where the user is working. The depot baseline lives on the hidden ref
    // refs/p4gw/<baseline>, so import never needs a branch - a detached HEAD is
    // fine. We track the branch name when on one (the named-branch flow brings
    // it up to date); otherwise we track the commit and restack it detached,
    // the way the branchless flow already does. `originalHead` is set whenever
    // there are commits; `originalBranch` only when HEAD is on a branch. Both
    // empty on an unborn branch (no commits yet).
    const bool hasCommits = git::revParse("HEAD", root).has_value();
    std::string originalBranch;  // set only when HEAD is on a named branch
    std::string originalHead;    // the commit HEAD points at (branch or detached)
    if (hasCommits) {
        auto head = git::revParse("HEAD", root);
        if (!head) {
            std::fprintf(stderr, "gw import: %s\n", head.error().c_str());
            return 1;
        }
        originalHead = *head;
        // Branchless works detached by design; for the named-branch flow we
        // also need the branch name, but a detached HEAD is no longer an error.
        if (!branchless) {
            auto branch = git::currentBranch(root);
            if (!branch) {
                std::fprintf(stderr, "gw import: %s\n", branch.error().c_str());
                return 1;
            }
            if (*branch != "HEAD") originalBranch = *branch;
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
            // The baseline branch may already exist: gw init creates it (with
            // the committed .gitignore) on a fresh repo. `git switch --orphan`
            // fails on an existing branch, so when it exists just switch to it
            // and commit the first snapshot on top of the .gitignore. Only when
            // the history lives on some *other* branch and no baseline exists
            // yet do we start the baseline as a clean orphan root.
            auto baselineExists = git::branchExists(baseline, root);
            if (!baselineExists) return fail(baselineExists.error());
            if (*baselineExists) {
                auto switched = git::switchBranch(baseline, root);
                if (!switched) return fail(switched.error());
            } else {
                auto switched = git::switchOrphanBranch(baseline, root);
                if (!switched) return fail(switched.error());
            }
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

    // Import touches the P4 server (per-mapping `p4 have`, opened-file lookup),
    // walks and copies the whole mirror into the working tree, and hashes it
    // all into Git's index - any of which can run for a while on a large depot.
    // Emit a line at the start of each phase, prefixed with elapsed-since-start
    // (flushed so it shows immediately), so the wait reads as progress and the
    // gaps between lines show which phase is the slow one.
    const auto importStart = std::chrono::steady_clock::now();
    auto progress = [&importStart](const std::string& message) {
        const double secs = std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - importStart)
                                .count();
        std::printf("[+%5.1fs] %s\n", secs, message.c_str());
        std::fflush(stdout);
    };

    // Files already opened in the mirror (a mid-prepare CL, a stray p4 edit)
    // have an un-submitted working copy. The baseline must stay pristine
    // submitted depot state, so for those files read the depot head instead of
    // copying the mirror, and omit add-only files (no depot revision exists).
    progress("Reading P4 state...");
    auto opened = p4::openedFilesTagged(*config);
    if (!opened) return fail(opened.error());

    // Each mapping is its own depot subtree, synced into its own mirror and
    // living under its own working-tree directory; import them independently
    // and tally for the summary.
    size_t copiedFiles = 0;
    size_t deletedFiles = 0;
    for (const auto* rule : includeRules(config->rules)) {
        const std::string mirrorDir =
            resolveMirrorPath(rule->mirrorPath, root);
        const std::string subtreePrefix =
            rule->repoSubtree.empty() ? std::string{}
                                      : rule->repoSubtree + "/";
        const std::string worktreeDir =
            rule->repoSubtree.empty()
                ? root
                : (fs::path(root) / rule->repoSubtree).string();

        progress("Listing mirror files under " + mirrorDir + "...");
        auto mirrorFiles = mirror::listFiles(mirrorDir);
        if (!mirrorFiles) return fail(mirrorFiles.error());

        // p4 only ever removes files it deleted a revision of, so anything it
        // never tracked (build output, leftovers from a botched sync, hand
        // edits) lingers in the mirror. Keep only the files p4 reports as
        // synced; strays are ignored so they never land in the baseline. A
        // failed `p4 have` is an error (don't mistake it for "everything is a
        // stray"); an empty result is legitimately nothing synced.
        progress("Querying p4 have for " + rule->depotPath + "...");
        auto haveDepot = p4::haveFiles(*config, rule->depotPath);
        if (!haveDepot) return fail(haveDepot.error());
        std::unordered_set<std::string> haveRel;
        for (const auto& depotFile : *haveDepot) {
            std::string rel =
                p4::depotRelativePath(rule->depotPath, depotFile);
            if (!rel.empty()) haveRel.insert(std::move(rel));
        }
        std::vector<std::string> mirrorTracked;
        for (auto& path : *mirrorFiles) {
            if (haveRel.contains(path)) mirrorTracked.push_back(std::move(path));
        }

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
            mirror::computeSyncActions(mirrorTracked, trackedHere);

        std::vector<mirror::OpenedMirrorFile> openedMirror;
        for (const auto& o : *opened) {
            std::string rel =
                p4::depotRelativePath(rule->depotPath, o.depotFile);
            if (rel.empty()) continue;  // belongs to a different mapping
            openedMirror.push_back({std::move(rel), !p4::isAddAction(o.action)});
        }
        const auto plan = mirror::planImport(actions, openedMirror);

        // "Scan" count: import reconciles the *whole* mirror against the
        // working tree every time (it is not diff-based), then copies only the
        // files that actually changed - so this is how many it inspects, not how
        // many it copies. Say "Importing", not "Syncing": nothing is synced from
        // P4 here, gw only moves already-synced mirror files into the repo.
        const size_t toScan = plan.actions.copies.size();
        const size_t toDelete = plan.actions.deletes.size();
        if (toScan != 0 || toDelete != 0 || !plan.depotReads.empty()) {
            progress("Importing " + rule->depotPath + " (" +
                     std::to_string(toScan) + " mirror file(s) to scan, " +
                     std::to_string(toDelete) + " to delete)...");
        }

        auto applied =
            mirror::applySyncActions(plan.actions, mirrorDir, worktreeDir);
        if (!applied) return fail(applied.error());
        const size_t actuallyCopied = *applied;

        // Restore depot-head content for files open in the mirror.
        if (!plan.depotReads.empty()) {
            std::string depotBase = rule->depotPath;
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
        copiedFiles += actuallyCopied + plan.depotReads.size();
        deletedFiles += plan.actions.deletes.size();
    }

    progress("Staging snapshot in Git...");
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

    progress("Snapshot staged. Updating branch...");
    const bool importedNew = newDepot != oldDepot;

    // Working detached: either branchless (which works detached by design) or a
    // plain detached HEAD. The hidden ref is the baseline, so no branch was
    // needed - return HEAD to where the user was and bring it up to date from
    // there. `originalBranch` empty with a recorded `originalHead` is the
    // detached case; branchless always lands here too.
    const bool workingDetached =
        branchless || (originalBranch.empty() && !originalHead.empty());
    if (workingDetached) {
        // Return HEAD to where the user was working (detached) before the
        // restack. The recorded commit still exists; a branchless sync marks it
        // obsolete (recoverable with `git undo`), a plain rebase leaves the
        // pre-rebase commit reachable via the reflog.
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
                "Imported depot state to '%s' (%zu file(s) updated, %zu deleted)\n",
                depotRef.c_str(), copiedFiles, deletedFiles);
        } else {
            std::printf("Already up to date - the mirror matches the depot "
                        "baseline.\n");
        }

        // Keep the convenience baseline branch tracking the depot: create it if
        // missing, fast-forward it when it carries no local commits.
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

        // Without --rebase, leave the user's work where it is (the same "never
        // stomp" default as the branch workflow) and point them at the restack.
        if (!rebase) {
            if (branchless) {
                std::printf("Your stacks were left as-is. Restack them onto the "
                            "new depot state with: gw import --rebase "
                            "(or git sync).\n");
            } else {
                std::printf("Your detached work was left as-is. Rebase it onto "
                            "the new depot state with: gw import --rebase.\n");
            }
            return 0;
        }

        // Restack onto the new baseline. Branchless moves every visible stack in
        // one shot and records the rewrites (obsoleting the old commits) - the
        // one case that genuinely needs a branchless command. A plain detached
        // HEAD has no such stack model, so rebase the one line HEAD points at.
        if (branchless) {
            auto synced = git::branchlessSync(root);
            if (!synced) {
                std::fflush(stdout);  // keep messages ordered with stderr
                std::fprintf(stderr, "gw import: branchless sync stopped:\n%s\n",
                             synced.error().c_str());
                std::fprintf(stderr,
                             "Resolve the conflicts, then 'git rebase "
                             "--continue' (or 'git rebase --abort' to undo).\n");
                return 1;
            }
            std::printf("Restacked your visible commits onto the new depot "
                        "state.\n");
        } else {
            auto rebased = git::rebase(newDepot, root);
            if (!rebased) {
                std::fflush(stdout);  // keep messages ordered with stderr
                std::fprintf(stderr, "gw import: rebase stopped:\n%s\n",
                             rebased.error().c_str());
                std::fprintf(stderr,
                             "Resolve the conflicts, then 'git rebase "
                             "--continue' (or 'git rebase --abort' to undo).\n");
                return 1;
            }
            std::printf("Rebased your detached work onto the new depot state "
                        "(HEAD is still detached).\n");
        }
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
        std::printf("Imported depot state to '%s' (%zu file(s) updated, %zu deleted)\n",
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
