// SPDX-License-Identifier: MIT

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>

#include "commands.h"
#include "config.h"
#include "git.h"
#include "mirror.h"
#include "p4.h"

namespace fs = std::filesystem;

namespace p4gw {

namespace {

// Files copied and deleted while building a snapshot, for the summary line.
struct SnapshotStats {
    size_t copied = 0;
    size_t deleted = 0;
};

// Overlays the user's tracked root meta files (.gitignore, .gitattributes)
// from `root` onto the snapshot worktree at `stagingRoot`, returning whether
// any actually differed. In worktree mode the staging tree is detached at the
// OLD baseline, so its meta files are frozen there; if the user changed them
// (added a re-include and re-ran `gw init`, edited an ignore rule) the
// worktree's `git add` would apply stale ignore rules and silently drop
// newly tracked files, and the new baseline would never record the update.
// Copies only when content differs so a steady-state import never churns the
// worktree. Checkout mode stages in `root` itself and calls this not at all.
std::expected<bool, std::string> syncWorktreeMetaFiles(
    const std::string& root, const std::string& stagingRoot) {
    // Compare on rules, not bytes: the worktree copy is checked out through
    // .gitattributes (often LF) while the user's is written verbatim (CRLF on
    // Windows), so a raw compare would fire on every import. Stripping '\r'
    // reacts only to a real change in the ignore/attribute rules.
    auto normalized = [](std::string s) {
        std::erase(s, '\r');
        return s;
    };
    bool changed = false;
    for (const char* meta : {".gitignore", ".gitattributes"}) {
        const fs::path src = fs::path(root) / meta;
        std::error_code ec;
        if (!fs::exists(src, ec)) continue;  // nothing to propagate
        std::ifstream in(src, std::ios::binary);
        if (!in) return std::unexpected("cannot read " + src.string());
        std::ostringstream buf;
        buf << in.rdbuf();
        const std::string want = std::move(buf).str();

        const fs::path dst = fs::path(stagingRoot) / meta;
        std::string have;
        bool haveExists = false;
        if (std::ifstream cur(dst, std::ios::binary); cur) {
            haveExists = true;
            std::ostringstream cbuf;
            cbuf << cur.rdbuf();
            have = std::move(cbuf).str();
        }
        if (haveExists && normalized(have) == normalized(want)) continue;

        std::ofstream out(dst, std::ios::binary | std::ios::trunc);
        if (!out) return std::unexpected("cannot write " + dst.string());
        out << want;
        out.flush();
        if (!out) return std::unexpected("cannot write " + dst.string());
        changed = true;
    }
    return changed;
}

// Builds and commits the depot snapshot at `stagingRoot` and advances
// `depotRef` to it. `stagingRoot` is the user's own checkout in checkout mode
// and the hidden worktree in worktree mode; either way it must already be
// positioned on `oldDepot` (or unborn on the very first import). The mirror is
// always resolved against `root` (it lives only in the main working tree).
// `manifestPath` is the have manifest (see mirror.h): when it is bound to
// `oldDepot` and `fullCopy` is off, the mirror walk and stat pass are replaced
// by a have diff and the git staging is scoped to the touched paths; the fresh
// have state is persisted back after the ref advances.
// Returns the new depot tip: equal to `oldDepot` when the mirror already
// matched (nothing committed), or empty when there was never anything to
// commit (a first import against an empty mirror). The caller owns HEAD
// positioning, the pending marker, and failure recovery, so this returns a
// plain std::unexpected on error rather than driving any restore.
std::expected<std::string, std::string> buildSnapshot(
    const Config& config, const std::string& root,
    const std::string& stagingRoot, const std::string& depotRef,
    const std::string& oldDepot, const std::string& manifestPath,
    bool fullCopy,
    const std::function<void(const std::string&)>& progress,
    SnapshotStats& stats) {
    // The persisted have manifest is valid only when it is bound to the exact
    // snapshot the staging tree sits on. Anything else - first import, torn
    // write, a hand-moved ref, --full - and import falls back to the full
    // mirror walk below; the manifest is a cache, never a source of truth.
    std::vector<mirror::ManifestEntry> manifestEntries;
    bool manifestValid = false;
    if (!fullCopy && !oldDepot.empty()) {
        std::ifstream manifestFile(manifestPath, std::ios::binary);
        if (manifestFile) {
            std::ostringstream text;
            text << manifestFile.rdbuf();
            std::string snapshot;
            manifestEntries =
                mirror::parseHaveManifest(std::move(text).str(), snapshot);
            manifestValid = !snapshot.empty() && snapshot == oldDepot;
        }
    }

    // Worktree mode stages in a tree detached at the old baseline, so bring its
    // .gitignore/.gitattributes up to the user's current ones before staging.
    // When they changed, a file that was always synced can become newly tracked
    // without its p4 revision ever moving - which the rev-diff fast path cannot
    // see - so fall back to the full mirror walk, which reconciles the whole
    // mirror against the tracked set. (Checkout mode stages in `root`, where the
    // meta files are already current, so this is a worktree-only concern.)
    if (stagingRoot != root) {
        auto metaChanged = syncWorktreeMetaFiles(root, stagingRoot);
        if (!metaChanged) return std::unexpected(metaChanged.error());
        if (*metaChanged) manifestValid = false;
    }

    // The full walk needs the tracked-file list to compute deletes; the
    // manifest diff derives deletes from the manifest itself, so skip the
    // scan entirely on the fast path.
    std::vector<std::string> tracked;
    if (!manifestValid) {
        auto trackedFiles = git::lsFiles(stagingRoot);
        if (!trackedFiles) return std::unexpected(trackedFiles.error());
        tracked = std::move(*trackedFiles);
    }

    // Files already opened in the mirror (a mid-prepare CL, a stray p4 edit)
    // have an un-submitted working copy. The baseline must stay pristine
    // submitted depot state, so for those files read the depot head instead of
    // copying the mirror, and omit add-only files (no depot revision exists).
    progress("Reading P4 state...");
    auto opened = p4::openedFilesTagged(config);
    if (!opened) return std::unexpected(opened.error());

    // Each mapping is its own depot subtree, synced into its own mirror and
    // living under its own working-tree directory; import them independently
    // and tally for the summary.
    std::vector<mirror::ManifestEntry> newManifest;  // fresh have, all mappings
    std::vector<std::string> touched;  // staging-relative paths (fast path)
    for (const auto* rule : includeRules(config.rules)) {
        const std::string mirrorDir =
            resolveMirrorPath(rule->mirrorPath, root);
        const std::string subtreePrefix =
            rule->repoSubtree.empty() ? std::string{}
                                      : rule->repoSubtree + "/";
        const std::string worktreeDir =
            rule->repoSubtree.empty()
                ? stagingRoot
                : (fs::path(stagingRoot) / rule->repoSubtree).string();

        // The have listing serves three masters: the stray filter (full walk),
        // the manifest diff (fast path), and the manifest written for the next
        // run. A failed `p4 have` is an error (don't mistake it for "everything
        // is a stray"); an empty result is legitimately nothing synced.
        progress("Querying p4 have for " + rule->depotPath + "...");
        auto haveDepot = p4::haveFiles(config, rule->depotPath);
        if (!haveDepot) return std::unexpected(haveDepot.error());

        // The have listing covers the include's whole depot subtree, so it
        // also names files the client view sends elsewhere: `exclude`
        // carve-outs synced in place and deeper re-includes owned by their own
        // mapping. Those never live in *this* mirror, so everything the
        // manifest fast path consumes - the diff inputs and the manifest
        // written for next time - keeps only the entries this rule effectively
        // owns. (The full walk below intersects with the mirror listing, which
        // enforces the same boundary physically, so its stray filter stays on
        // the unfiltered set.)
        const auto owned =
            p4::filterHaveToRule(*haveDepot, config.rules, rule);
        std::vector<std::pair<std::string, std::string>> nowRel;
        nowRel.reserve(owned.size());
        for (const auto& entry : owned) {
            std::string rel =
                p4::depotRelativePath(rule->depotPath, entry.depotFile);
            if (!rel.empty()) nowRel.emplace_back(std::move(rel), entry.rev);
        }
        for (const auto& entry : owned) {
            newManifest.push_back({entry.depotFile, entry.rev});
        }

        mirror::SyncActions actions;
        if (manifestValid) {
            // Fast path: diff the fresh have state against the manifest. A rev
            // that did not move was not rewritten by sync, so the mirror copy
            // still matches the snapshot the staging tree sits on - skip it
            // without so much as a stat. Deletes come from the manifest, so
            // the mirror is never even listed. The stored entries pass through
            // the same effective-rule filter as the fresh ones (with the
            // *current* rules), so a manifest written before a config change
            // can never generate actions outside this mapping's mirror.
            std::vector<std::pair<std::string, std::string>> thenRel;
            for (const auto& entry : manifestEntries) {
                if (effectiveRuleForDepot(config.rules, entry.depotFile) !=
                    rule) {
                    continue;
                }
                std::string rel =
                    p4::depotRelativePath(rule->depotPath, entry.depotFile);
                if (!rel.empty()) thenRel.emplace_back(std::move(rel), entry.rev);
            }
            actions = mirror::diffHaveState(thenRel, nowRel);
            // Parity with computeSyncActions: gw's own metadata never has a
            // depot counterpart to delete it with (a whole-repo include could
            // otherwise map a depot .gitignore over it).
            std::erase_if(actions.deletes, [&](const std::string& rel) {
                return mirror::isGwMetadataPath(subtreePrefix + rel);
            });
            progress("Have manifest for " + rule->depotPath + ": " +
                     std::to_string(actions.copies.size()) + " changed, " +
                     std::to_string(actions.deletes.size()) + " deleted, " +
                     std::to_string(nowRel.size() - actions.copies.size()) +
                     " unchanged (skipped)");
        } else {
            // Full walk: list the mirror and reconcile it against the tracked
            // files. p4 only ever removes files it deleted a revision of, so
            // anything it never tracked (build output, leftovers from a
            // botched sync, hand edits) lingers in the mirror. Keep only the
            // files p4 reports as synced; strays are ignored so they never
            // land in the baseline.
            progress("Listing mirror files under " + mirrorDir + "...");
            auto mirrorFiles = mirror::listFiles(mirrorDir);
            if (!mirrorFiles) return std::unexpected(mirrorFiles.error());

            // Unfiltered on purpose: a nested re-include's mirror lives inside
            // this one, so its files show up in this mirror listing and must
            // stay in the intersection (dropping them here would delete and
            // recopy the nested subtree on every full walk).
            std::unordered_set<std::string> haveRel;
            for (const auto& entry : *haveDepot) {
                std::string rel =
                    p4::depotRelativePath(rule->depotPath, entry.depotFile);
                if (!rel.empty()) haveRel.insert(std::move(rel));
            }
            std::vector<std::string> mirrorTracked;
            for (auto& path : *mirrorFiles) {
                if (haveRel.contains(path)) {
                    mirrorTracked.push_back(std::move(path));
                }
            }

            // Tracked files under this subtree, made mirror-relative so they
            // line up with the mirror's own (subtree-rooted) listing.
            std::vector<std::string> trackedHere;
            for (const auto& path : tracked) {
                if (subtreePrefix.empty()) {
                    trackedHere.push_back(path);
                } else if (path.starts_with(subtreePrefix)) {
                    trackedHere.push_back(path.substr(subtreePrefix.size()));
                }
            }

            actions = mirror::computeSyncActions(mirrorTracked, trackedHere);
        }

        std::vector<mirror::OpenedMirrorFile> openedMirror;
        for (const auto& o : *opened) {
            std::string rel =
                p4::depotRelativePath(rule->depotPath, o.depotFile);
            if (rel.empty()) continue;  // belongs to a different mapping
            openedMirror.push_back({std::move(rel), !p4::isAddAction(o.action)});
        }
        const auto plan = mirror::planImport(actions, openedMirror);

        // "Scan" count (full walk only; the fast path printed its own tally):
        // the walk reconciles the *whole* mirror against the working tree, then
        // copies only the files that actually changed - so this is how many it
        // inspects, not how many it copies. Say "Importing", not "Syncing":
        // nothing is synced from P4 here, gw only moves already-synced mirror
        // files into the repo.
        const size_t toScan = plan.actions.copies.size();
        const size_t toDelete = plan.actions.deletes.size();
        if (!manifestValid &&
            (toScan != 0 || toDelete != 0 || !plan.depotReads.empty())) {
            progress("Importing " + rule->depotPath + " (" +
                     std::to_string(toScan) + " mirror file(s) to scan, " +
                     std::to_string(toDelete) + " to delete)...");
        }

        // On the fast path, remember exactly which staging paths this mapping
        // touches so the git staging below can be scoped to them.
        if (manifestValid) {
            for (const auto& rel : plan.actions.copies) {
                touched.push_back(subtreePrefix + rel);
            }
            for (const auto& rel : plan.actions.deletes) {
                touched.push_back(subtreePrefix + rel);
            }
            for (const auto& rel : plan.depotReads) {
                touched.push_back(subtreePrefix + rel);
            }
        }

        auto applied = mirror::applySyncActions(plan.actions, mirrorDir,
                                                worktreeDir,
                                                /*trustStats=*/!fullCopy);
        if (!applied) return std::unexpected(applied.error());
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
                    return std::unexpected("failed to create directory " +
                                           dest.parent_path().string() + ": " +
                                           ec.message());
                }
                auto printed =
                    p4::printHeadToFile(config, depotBase + rel, dest.string());
                if (!printed) return std::unexpected(printed.error());
                fs::permissions(dest, fs::perms::owner_write,
                                fs::perm_options::add, ec);
            }
        }
        stats.copied += actuallyCopied + plan.depotReads.size();
        stats.deleted += plan.actions.deletes.size();
    }

    progress("Staging snapshot in Git...");
    if (manifestValid) {
        // The fast path knows exactly which paths it touched; stage just
        // those instead of rescanning the whole tree. An empty list means the
        // index is already right and the no-op is free.
        auto added = git::addPaths(touched, stagingRoot);
        if (!added) return std::unexpected(added.error());
    } else {
        auto added = git::addAll(stagingRoot);
        if (!added) return std::unexpected(added.error());
    }
    auto clean = git::indexMatchesHead(stagingRoot);
    if (!clean) return std::unexpected(clean.error());

    // Commit the snapshot (when there is anything new) and advance the depot
    // ref to it. newDepot stays at oldDepot when the mirror already matched.
    std::string newDepot = oldDepot;
    if (!*clean) {
        // Summarize what this import actually did - locally derived, so no p4
        // query. A CL number was tried once but dropped: a `p4 changes #have`
        // intersect costs seconds on a big depot, and after prepare -> submit
        // -> import (no resync) it reports your own submitted CL, not the
        // synced baseline - misleading exactly when you'd trust it. The commit
        // date carries the "when"; status renders that.
        std::string message = "Import depot state (" +
                              std::to_string(stats.copied) + " file(s) updated, " +
                              std::to_string(stats.deleted) + " deleted)";
        auto committed = git::commit(message, stagingRoot);
        if (!committed) return std::unexpected(committed.error());
        auto tip = git::revParse("HEAD", stagingRoot);
        if (!tip) return std::unexpected(tip.error());
        newDepot = *tip;
        auto advanced = git::updateRef(depotRef, newDepot, stagingRoot);
        if (!advanced) return std::unexpected(advanced.error());
    }

    // Persist the fresh have state, bound to the snapshot it produced, so the
    // next import can take the fast path. Written only after the ref is safely
    // advanced, via temp-file + rename so a torn write can never leave a
    // plausible-looking manifest with a valid snapshot line. Best effort: a
    // failed write just costs the next import a full walk.
    if (!newDepot.empty()) {
        const std::string tmpPath = manifestPath + ".tmp";
        std::error_code ec;
        fs::create_directories(fs::path(manifestPath).parent_path(), ec);
        bool written = false;
        if (!ec) {
            std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
            if (out) {
                out << mirror::renderHaveManifest(newDepot, newManifest);
                written = out.good();
            }
        }
        if (written) {
            fs::rename(tmpPath, manifestPath, ec);
            written = !ec;
        }
        if (!written) {
            fs::remove(tmpPath, ec);
            std::printf("note: could not write the have manifest (%s); the "
                        "next import will do a full mirror walk\n",
                        manifestPath.c_str());
        }
    }
    return newDepot;
}

// Ensures the hidden snapshot worktree exists, is registered, and sits
// detached at `oldDepot` with a clean tree, returning its path. A healthy
// worktree is reused untouched - its stamped mtimes are the size+mtime fast
// path. `forceReset` (the pending marker) resets even a matching worktree.
// Sets `fullCopyNeeded` whenever it (re)created or reset the worktree, since
// the stamps are then meaningless. Runs its git commands from the main repo
// (`root`).
std::expected<std::string, std::string> ensureSnapshotWorktree(
    const std::string& root, const std::string& gitDirPath,
    const std::string& oldDepot, bool forceReset, bool& fullCopyNeeded) {
    const std::string wtPath = mirror::snapshotWorktreePath(gitDirPath);

    // A single rev-parse inside the worktree proves the directory, its `.git`
    // file, and the `.git/worktrees/<id>` registration are all present and
    // consistent. If it fails - directory or registration deleted, or a moved
    // repo whose gitdir back-pointers dangle - recreate from scratch.
    auto head = git::revParse("HEAD", wtPath);
    if (!head) {
        std::error_code ec;
        fs::remove_all(wtPath, ec);  // best effort; may not exist
        // `worktree add` refuses a still-registered missing path, so prune the
        // stale registration first.
        auto pruned = git::worktreePrune(root);
        if (!pruned) return std::unexpected(pruned.error());
        auto added = git::worktreeAdd(wtPath, oldDepot, root);
        if (!added) return std::unexpected(added.error());
        fullCopyNeeded = true;
        return wtPath;
    }

    // Registered and readable. Reset only when it isn't already the pristine
    // base: a stamped-stat mismatch (HEAD moved, a crash mid-import, hand
    // meddling) or the pending marker. The healthy path never resets.
    auto dirty = git::isDirty(wtPath);
    if (!dirty) return std::unexpected(dirty.error());
    if (*head != oldDepot || *dirty || forceReset) {
        auto reset = git::resetHard(oldDepot, wtPath);
        if (!reset) return std::unexpected(reset.error());
        auto cleaned = git::cleanUntracked({}, wtPath);
        if (!cleaned) return std::unexpected(cleaned.error());
        fullCopyNeeded = true;
    }
    return wtPath;
}

constexpr const char* kImportUsage =
    "usage: gw import [options]\n"
    "\n"
    "Commit the mirror's current state - whatever you last synced, with any\n"
    "tool - to the depot baseline (the hidden refs/p4gw/<baseline> ref), then\n"
    "bring your branch up to it. Like 'git fetch' / 'git pull --rebase'.\n"
    "A branch with no local commits fast-forwards; divergent commits are left\n"
    "untouched unless you pass --rebase.\n"
    "\n"
    "By default (import_mode = worktree) the snapshot is built in a hidden\n"
    "worktree, so import works even with a dirty tree - it just skips bringing\n"
    "your branch up (do that yourself once the tree is clean). Set\n"
    "'import_mode = checkout' in p4gw.cfg to stage in your own tree instead.\n"
    "\n"
    "options:\n"
    "  -r, --rebase  Replay your local commits on top of the new depot state\n"
    "                (otherwise divergent branches are left as-is)\n"
    "      --full    Recopy every mirror file, ignoring the size+mtime fast\n"
    "                path (use when the working tree may not match what the\n"
    "                stamped stats claim - 'gw doctor --verify' checks that)\n"
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
    bool fullCopy = false;
    for (const auto& arg : args) {
        if (arg == "--help" || arg == "-h") {
            std::printf("%s", kImportUsage);
            return 0;
        } else if (arg == "--rebase" || arg == "-r") {
            rebase = true;
        } else if (arg == "--full") {
            fullCopy = true;
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

    // A marker in the git dir means an earlier import started mutating the
    // repo and never finished (crash, kill, a file locked past the retries).
    // Its stamped mtimes can no longer be trusted - outside cleanup like `git
    // reset --hard` restores content but can leave stats that still match the
    // mirror, and the fast path would then skip files whose bytes differ. So
    // while the marker exists, recopy everything; it comes off below once a
    // snapshot lands cleanly.
    auto gitDirPath = git::gitDir(root);
    if (!gitDirPath) {
        std::fprintf(stderr, "gw import: %s\n", gitDirPath.error().c_str());
        return 1;
    }
    const std::string marker = mirror::importPendingMarkerPath(*gitDirPath);
    const bool markerPresent = fs::exists(marker);
    if (markerPresent) {
        std::printf("note  a previous import did not finish - ignoring the "
                    "size+mtime fast path and recopying every mirror file\n");
        fullCopy = true;
    }

    const std::string& baseline = config->baselineBranch;
    const std::string depotRef = depotTrackingRef(*config);

    // Resolve the depot-tracking ref (moved above the clean-tree check because
    // worktree mode's dirty-tree decision depends on whether a baseline exists;
    // it only reads the ref, or seeds it with update-ref for a legacy repo, so
    // it is safe here). If the ref does not exist yet but a legacy baseline
    // branch does, this is a repo from before the hidden-ref model: seed the
    // ref from the branch's most recent import commit (or its tip if there
    // isn't one) so existing repos keep working without surprises.
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

    // Worktree mode builds the snapshot in a hidden worktree, so the user's
    // checkout is never touched and a dirty tree is fine. It needs a commit to
    // detach at, though, so the very first import (no depot ref yet) always
    // falls back to checkout mode.
    const bool worktreeMode =
        config->importMode == ImportMode::kWorktree && !oldDepot.empty();
    if (config->importMode == ImportMode::kWorktree && oldDepot.empty()) {
        std::printf("note  import_mode is 'worktree', but this is the first "
                    "import - building it in your checkout this once\n");
    }

    // Clean-tree check. Checkout mode rewrites the user's working tree, so it
    // must be clean. Worktree mode leaves the checkout alone; a dirty tree only
    // means the branch fast-forward/rebase half is skipped (see the epilogue),
    // so record it and carry on.
    bool dirtyTree = false;
    {
        auto dirty = git::isDirty(root);
        if (!dirty) {
            std::fprintf(stderr, "gw import: %s\n", dirty.error().c_str());
            return 1;
        }
        if (*dirty) {
            if (!worktreeMode) {
                std::fprintf(stderr,
                             "gw import: working tree is not clean - commit, "
                             "stash, or gitignore your changes first\n");
                return 1;
            }
            dirtyTree = true;
        }
    }

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
        // Track the branch name whenever HEAD is on one, so import returns the
        // user exactly where they were. Branchless users work detached *or* on a
        // branch, and import must preserve whichever it found (a detached HEAD is
        // not an error).
        auto branch = git::currentBranch(root);
        if (!branch) {
            std::fprintf(stderr, "gw import: %s\n", branch.error().c_str());
            return 1;
        }
        if (*branch != "HEAD") originalBranch = *branch;
    }

    // From here failures may have moved HEAD off the user's branch or half-
    // written the working tree (checkout mode) or the hidden worktree
    // (worktree mode). Once `mutating` is set, fail() restores the pre-import
    // state. The pending marker is deliberately left in place either way so the
    // next import distrusts the stamped stats (checkout mode) or resets the
    // worktree (worktree mode) and recopies everything.
    bool mutating = false;
    auto fail = [&](const std::string& message) {
        std::fprintf(stderr, "gw import: %s\n", message.c_str());
        if (mutating && worktreeMode) {
            // The user's checkout was never touched - only the hidden worktree
            // holds partial output, and the marker (kept) triggers a reset on
            // the next run.
            std::fprintf(stderr,
                         "note: your checkout was not touched; the next "
                         "'gw import' resets the snapshot worktree and recopies "
                         "every mirror file\n");
            return 1;
        }
        // Checkout mode: the tree was verified clean at the start, so
        // everything the restore discards is the import's own partial output -
        // `switch -f` puts tracked content back and `clean -fd` sweeps the
        // untracked partial copies (never ignored files: the mirror and build
        // output stay put).
        if (mutating) {
            bool restored = false;
            std::string restoredTo;
            if (!originalBranch.empty()) {
                restored =
                    git::switchBranchForce(originalBranch, root).has_value();
                restoredTo = "'" + originalBranch + "'";
            } else if (!originalHead.empty()) {
                restored =
                    git::switchDetachedForce(originalHead, root).has_value();
                restoredTo = "detached HEAD at " + originalHead;
            } else {
                restored = true;  // unborn branch: no prior commit to return to
                restoredTo = "its pre-import state";
            }
            std::vector<std::string> subtrees;
            for (const auto* rule : includeRules(config->rules)) {
                if (rule->repoSubtree.empty()) {
                    subtrees.clear();  // a whole-repo include: sweep everywhere
                    break;
                }
                subtrees.push_back(rule->repoSubtree);
            }
            if (restored && git::cleanUntracked(subtrees, root).has_value()) {
                std::fprintf(stderr,
                             "note: working tree restored to %s; the next "
                             "'gw import' will recopy every mirror file\n",
                             restoredTo.c_str());
                return 1;
            }
        }
        // Nothing was mutated, or the restore itself failed: tell the user
        // where HEAD is and how to get back by hand.
        auto where = git::currentBranch(root);
        if (where && *where == "HEAD" && !originalBranch.empty()) {
            std::fprintf(stderr,
                         "note: HEAD is detached mid-import; return with: "
                         "git switch -f %s\n",
                         originalBranch.c_str());
        } else if (where && *where == "HEAD" && !originalHead.empty()) {
            std::fprintf(stderr,
                         "note: HEAD is detached mid-import; return with: "
                         "git switch --detach %s\n",
                         originalHead.c_str());
        } else if (where) {
            std::fprintf(stderr, "note: you are on '%s'\n", where->c_str());
        }
        return 1;
    };

    // From here the command moves HEAD and rewrites working-tree files.
    // Record that in the marker so a torn import (crash, kill, a locked file)
    // stays detectable: `gw doctor` reports it and the next import distrusts
    // the size+mtime fast path. Removed once the snapshot is committed.
    {
        std::ofstream markerFile(marker, std::ios::trunc);
        if (!markerFile) {
            std::fprintf(stderr, "gw import: cannot write %s\n", marker.c_str());
            return 1;
        }
        markerFile << "A 'gw import' run started and has not finished. If no "
                      "import is running,\nit was interrupted: run 'gw import' "
                      "again (it will recopy every mirror\nfile instead of "
                      "trusting file sizes and timestamps).\n";
    }
    mutating = true;

    // Position the staging area on a pristine depot base, then overlay the
    // mirror into it. `stagingRoot` is where the snapshot is built and where
    // the mirror is copied.
    //   - Worktree mode: the hidden worktree, (re)created detached at oldDepot.
    //     The user's own checkout and HEAD are never touched.
    //   - Checkout mode with an existing depot ref: detach the user's checkout
    //     onto it so their branch is untouched.
    //   - First import (no depot ref, always checkout mode): create the
    //     baseline branch and commit there (no prior depot state to base on).
    std::string stagingRoot = root;
    const bool firstImport = oldDepot.empty();
    if (worktreeMode) {
        bool resetForced = false;
        auto wt = ensureSnapshotWorktree(root, *gitDirPath, oldDepot,
                                         /*forceReset=*/markerPresent,
                                         resetForced);
        if (!wt) return fail(wt.error());
        stagingRoot = *wt;
        if (resetForced) fullCopy = true;
    } else if (firstImport) {
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

    // Build and commit the depot snapshot at the staging area positioned above
    // (the user's checkout in checkout mode, the hidden worktree in worktree
    // mode). The mirror is always resolved against the main tree (`root`).
    SnapshotStats snapStats;
    const std::string manifestPath =
        mirror::haveManifestPath(*gitDirPath, baseline);
    auto snap = buildSnapshot(*config, root, stagingRoot, depotRef, oldDepot,
                              manifestPath, fullCopy, progress, snapStats);
    if (!snap) return fail(snap.error());
    const std::string newDepot = *snap;
    const size_t copiedFiles = snapStats.copied;
    const size_t deletedFiles = snapStats.deleted;

    // The snapshot is committed (or nothing had changed): the working tree
    // and its stamped mtimes now faithfully mirror the depot state, so the
    // fast-path stats are trustworthy again. The branch juggling below only
    // moves refs and checks out commits - git keeps that consistent on its
    // own - so the marker comes off here, not at the end.
    {
        std::error_code markerEc;
        fs::remove(marker, markerEc);  // best effort; leftover costs a recopy
    }

    progress("Snapshot staged. Updating branch...");
    const bool importedNew = newDepot != oldDepot;

    // Worktree mode with a dirty tree: the depot baseline was imported, but a
    // fast-forward/rebase/branchless-sync would touch the user's uncommitted
    // work, so skip that half (a ref-only convenience update of the baseline
    // branch stays safe). In worktree mode the user's HEAD never moved, so the
    // switch-backs below are unnecessary too.
    const bool skipBranchUpdate = worktreeMode && dirtyTree;

    // Working detached: either branchless (which works detached by design) or a
    // plain detached HEAD. The hidden ref is the baseline, so no branch was
    // needed - return HEAD to where the user was and bring it up to date from
    // there. `originalBranch` empty with a recorded `originalHead` is the
    // detached case; branchless always lands here too.
    const bool workingDetached =
        branchless || (originalBranch.empty() && !originalHead.empty());
    if (workingDetached) {
        // Return HEAD to exactly where the user was - on their branch, or
        // detached at their commit. The recorded commit still exists; a
        // branchless sync marks it obsolete (recoverable with `git undo`), a
        // plain rebase leaves the pre-rebase commit reachable via the reflog.
        // Worktree mode never moved HEAD, so there is nothing to return.
        if (!worktreeMode) {
            if (!originalBranch.empty()) {
                auto back = git::switchBranch(originalBranch, root);
                if (!back) return fail(back.error());
            } else if (!originalHead.empty()) {
                auto back = git::switchDetached(originalHead, root);
                if (!back) return fail(back.error());
            }
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
        // missing, fast-forward it when it carries no local commits. Skip it
        // when the user is *on* that branch - moving its ref would drag HEAD,
        // and the restack below brings it up to date instead.
        if (originalBranch != baseline) {
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
        }

        // Dirty tree (worktree mode): the baseline is imported and the
        // convenience branch advanced above, but restacking would touch the
        // user's uncommitted work, so leave it.
        if (skipBranchUpdate) {
            std::printf("note: working tree is not clean - the depot baseline "
                        "was imported, but your work was left as-is. Commit or "
                        "stash, then rerun 'gw import --rebase'.\n");
            return 0;
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
            // Does HEAD carry local work of its own, or is it sitting at/behind
            // the baseline (e.g. detached on the depot ref, tracking it like
            // origin/main)? If it has no divergent work, there is nothing to
            // carry - fast-forward it to the new baseline after the sync (which
            // still restacks the user's *other* stacks). isAncestor(originalHead,
            // newDepot) is true exactly when HEAD is already contained in the new
            // baseline's history.
            auto headBehind = git::isAncestor(originalHead, newDepot, root);
            if (!headBehind) return fail(headBehind.error());

            // `git branchless sync` keeps a *branch* checkout on its branch, but
            // a detached HEAD sitting on a rewritten commit is left on the main
            // branch, not carried to the rewrite. So ride a detached HEAD with
            // divergent work through the sync on an ephemeral branch and detach
            // at its restacked tip afterward; a real branch just rides along on
            // its own.
            const std::string carrier = "gw-import-restack";
            // Mode-independent: both modes reach here with HEAD detached at
            // originalHead (checkout mode restored it after staging; worktree
            // mode never moved it), and branchless sync repositions HEAD the
            // same way regardless of where the snapshot was staged.
            const bool useCarrier = originalBranch.empty() &&
                                    !originalHead.empty() && !*headBehind;
            if (useCarrier) {
                // `-c` (not `-C`) refuses to clobber an existing branch, so a
                // name collision - or a leftover from an interrupted sync - is a
                // clear error, never silent data loss.
                auto made = git::run({"switch", "-c", carrier, originalHead},
                                     root);
                if (!made) return fail(made.error());
            }
            auto synced = git::branchlessSync(root);
            if (!synced) {
                std::fflush(stdout);  // keep messages ordered with stderr
                std::fprintf(stderr, "gw import: branchless sync stopped:\n%s\n",
                             synced.error().c_str());
                std::fprintf(stderr,
                             "Resolve the conflicts, then 'git rebase "
                             "--continue' (or 'git rebase --abort' to undo).\n");
                if (useCarrier) {
                    std::fprintf(stderr,
                                 "note: your work rides the temporary branch "
                                 "'%s'; after resolving, run 'git switch "
                                 "--detach %s && git branch -D %s'.\n",
                                 carrier.c_str(), carrier.c_str(),
                                 carrier.c_str());
                }
                return 1;
            }
            // Put HEAD back deterministically (sync repositions it - a detached
            // HEAD on a rewritten or dropped commit is left on main): detach at
            // the ephemeral branch's restacked tip and drop it, or return to the
            // user's real branch.
            bool mergedAway = false;
            if (useCarrier) {
                auto tip = git::revParse(carrier, root);
                if (tip) {
                    auto det = git::switchDetached(*tip, root);
                    if (!det) return fail(det.error());
                    auto dropped = git::run({"branch", "-D", carrier}, root);
                    if (!dropped) return fail(dropped.error());
                } else {
                    // The whole carried line was already applied upstream, so
                    // branchless obsoleted it and removed the ephemeral branch,
                    // leaving HEAD on the baseline branch. The user was detached,
                    // so detach at the new baseline too - their work now lives in
                    // that commit. Sweep the branch if it somehow lingers.
                    (void)git::run({"branch", "-D", carrier}, root);
                    auto det = git::switchDetached(newDepot, root);
                    if (!det) return fail(det.error());
                    mergedAway = true;
                }
            } else if (!originalBranch.empty()) {
                auto back = git::switchBranch(originalBranch, root);
                if (!back) return fail(back.error());
            } else {
                // Detached with no divergent work (at/behind the baseline):
                // fast-forward HEAD to the new baseline, the way being on the
                // depot ref and pulling would.
                auto det = git::switchDetached(newDepot, root);
                if (!det) return fail(det.error());
            }
            if (mergedAway) {
                std::printf("Restacked your visible commits. The commit you had "
                            "checked out was already in the depot state; HEAD is "
                            "detached at the new depot baseline.\n");
            } else {
                std::printf("Restacked your visible commits onto the new depot "
                            "state.\n");
            }
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
    // Worktree mode never moved HEAD, so there is nothing to return.
    if (!worktreeMode && !originalBranch.empty()) {
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

    // Keep the managed .rgignore block fresh: it re-asserts the repo's .ignore
    // patterns, which can change between imports. The file is untracked (under
    // the allowlist's '/*'), so this never dirties the tree; only the
    // marker-delimited block is rewritten. Best-effort - a failure is a note.
    if (auto rg = refreshRgignore(*config, root); !rg) {
        std::printf("note: could not update .rgignore (%s)\n",
                    rg.error().c_str());
    } else if (*rg) {
        std::printf("Refreshed the managed .rgignore block\n");
    }

    // Dirty tree (worktree mode): the depot baseline was imported, but a
    // fast-forward would touch the user's uncommitted work. Still advance the
    // convenience baseline branch when the user isn't on it (a ref move never
    // touches the tree); otherwise leave it. Then point them at the manual
    // catch-up and stop.
    if (skipBranchUpdate) {
        const std::string current =
            originalBranch.empty() ? baseline : originalBranch;
        if (current != baseline) {
            auto baselineExists = git::branchExists(baseline, root);
            if (baselineExists && *baselineExists) {
                auto branchFf =
                    git::isAncestor("refs/heads/" + baseline, newDepot, root);
                if (branchFf && *branchFf) {
                    auto moved = git::updateRef("refs/heads/" + baseline,
                                                newDepot, root);
                    if (!moved) return fail(moved.error());
                }
            }
        }
        std::printf("note: working tree is not clean - the depot baseline was "
                    "imported to '%s', but '%s' was left as-is. Commit or "
                    "stash, then rerun 'gw import' (or git merge --ff-only "
                    "%s).\n",
                    depotRef.c_str(), current.c_str(), baseline.c_str());
        return 0;
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