// SPDX-License-Identifier: MIT

#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "commands.h"
#include "config.h"
#include "git.h"
#include "mirror.h"
#include "p4.h"

namespace fs = std::filesystem;

namespace p4gw {

namespace {

constexpr const char* kSyncbackUsage =
    "usage: gw syncback [options]\n"
    "\n"
    "Sync the mirror back to the exact revisions the current depot baseline was\n"
    "imported from - the counterpart to syncing a file forward to resolve it.\n"
    "\n"
    "The usual reason: 'gw prepare' built a changelist, P4 asked for a resolve\n"
    "before submitting, so you synced the conflicting file to head, merged, and\n"
    "submitted. The client now sits past the baseline on those files, so\n"
    "'gw import' would absorb a half-synced depot state - and syncing the rest\n"
    "of the workspace to match costs a rebuild you did not ask for. 'gw syncback'\n"
    "puts just those files back on the baseline's revisions instead, so the\n"
    "mirror matches what gw already imported and you keep working against it.\n"
    "\n"
    "The target revisions come from the have manifest gw records on every\n"
    "import, so the baseline must have been imported by this repo, and the\n"
    "manifest must still describe it (rerun 'gw import' if not).\n"
    "\n"
    "Run it after the submit, not before: a file still open in P4 is left alone\n"
    "and reported, since syncing it back would discard an unsubmitted resolve.\n"
    "\n"
    "options:\n"
    "  -n, --dry-run  Show the syncs a real run would perform and exit,\n"
    "                 touching neither P4 nor the mirror\n"
    "  -h, --help     Show this help\n"
    "\n";

// First 12 characters of a commit SHA, for messages.
std::string shortSha(const std::string& sha) {
    return sha.size() > 12 ? sha.substr(0, 12) : sha;
}

}  // namespace

// Restores the client's have state to the revisions the depot baseline was
// imported from, so a sync done to resolve a file in P4 does not force a
// full-workspace sync before the next import:
//   1. The have manifest (bound to the current refs/p4gw/<baseline> tip)
//      supplies the target revision of every file the baseline covers.
//   2. A fresh `p4 have` per mapping says where the client actually sits.
//   3. Files whose revision moved are synced back to the recorded one; files
//      synced in since the import are synced to #0 (the baseline never had
//      them, so they are in neither the snapshot nor the working tree).
//   4. Files open in P4 are reported and left alone - p4 will not overwrite
//      them, and doing so would throw away an unsubmitted resolve.
// The manifest is deliberately not rewritten: it still describes the baseline,
// and once the client is back on those revisions it is accurate again, so the
// next `gw import` takes its fast path and finds nothing to do.
int cmdSyncback(const Args& args) {
    bool dryRun = false;
    for (const auto& arg : args) {
        if (arg == "--help" || arg == "-h") {
            std::printf("%s", kSyncbackUsage);
            return 0;
        } else if (arg == "--dry-run" || arg == "-n") {
            dryRun = true;
        } else {
            std::fprintf(stderr, "gw syncback: unknown option '%s'\n",
                         arg.c_str());
            std::fprintf(stderr, "%s", kSyncbackUsage);
            return 1;
        }
    }

    std::string root;
    auto config = findAndLoadConfig(root);
    if (!config) {
        std::fprintf(stderr, "gw syncback: %s\n", config.error().c_str());
        return 1;
    }
    for (const auto* rule : includeRules(config->rules)) {
        const std::string mirrorDir = resolveMirrorPath(rule->mirrorPath, root);
        if (!fs::exists(mirrorDir)) {
            std::fprintf(stderr,
                         "gw syncback: mirror directory %s does not exist - "
                         "check the client view mapping ('gw doctor')\n",
                         mirrorDir.c_str());
            return 1;
        }
    }

    auto gitDirPath = git::gitDir(root);
    if (!gitDirPath) {
        std::fprintf(stderr, "gw syncback: %s\n", gitDirPath.error().c_str());
        return 1;
    }
    const std::string depotRef = depotTrackingRef(*config);
    auto depotTip = git::revParse(depotRef, root);
    if (!depotTip) {
        std::fprintf(stderr,
                     "gw syncback: no depot baseline yet - there is nothing to "
                     "sync back to. Run 'gw import' first.\n");
        return 1;
    }

    // The manifest is import's cache, and everywhere else a missing or stale
    // one just costs a full mirror walk. Here it is the only record of which
    // revisions the snapshot was built from, and no fallback can reconstruct
    // them (Git stores content, not p4 revisions), so an invalid manifest is a
    // hard stop rather than a guess that would leave the mirror matching
    // neither state.
    const std::string manifestPath =
        mirror::haveManifestPath(*gitDirPath, config->baselineBranch);
    std::string snapshot;
    std::vector<mirror::ManifestEntry> baselineHave;
    if (std::ifstream manifestFile(manifestPath, std::ios::binary);
        manifestFile) {
        std::ostringstream text;
        text << manifestFile.rdbuf();
        baselineHave =
            mirror::parseHaveManifest(std::move(text).str(), snapshot);
    }
    if (snapshot.empty()) {
        std::fprintf(stderr,
                     "gw syncback: no usable have manifest at %s - gw has no "
                     "record of the revisions\nthe baseline was imported from, "
                     "and they cannot be recovered from Git.\nRun 'gw import' "
                     "to record them (it rewrites the manifest); syncback works "
                     "from the next import on.\n",
                     manifestPath.c_str());
        return 1;
    }
    if (snapshot != *depotTip) {
        std::fprintf(stderr,
                     "gw syncback: the have manifest records revisions for "
                     "snapshot %s, but the\ndepot baseline '%s' is at %s. "
                     "Syncing back to another snapshot's revisions\nwould leave "
                     "the mirror matching neither. Run 'gw import' to re-record "
                     "it.\n",
                     shortSha(snapshot).c_str(), depotRef.c_str(),
                     shortSha(*depotTip).c_str());
        return 1;
    }

    // Where the client sits now. Filtered exactly as import filters it when it
    // writes the manifest: `p4 have` on an include's depot path also lists
    // files the client view sends elsewhere (an `exclude` carve-out synced in
    // place, a deeper re-include's own nested mirror), and those are not this
    // mapping's to move.
    std::vector<mirror::ManifestEntry> nowHave;
    for (const auto* rule : includeRules(config->rules)) {
        auto haveDepot = p4::haveFiles(*config, rule->depotPath);
        if (!haveDepot) {
            std::fprintf(stderr, "gw syncback: %s\n", haveDepot.error().c_str());
            return 1;
        }
        for (const auto& entry :
             p4::filterHaveToRule(*haveDepot, config->rules, rule)) {
            nowHave.push_back({entry.depotFile, entry.rev});
        }
    }

    auto opened = p4::openedFilesTagged(*config);
    if (!opened) {
        std::fprintf(stderr, "gw syncback: %s\n", opened.error().c_str());
        return 1;
    }
    std::vector<std::string> openedDepotFiles;
    openedDepotFiles.reserve(opened->size());
    for (const auto& file : *opened) {
        openedDepotFiles.push_back(file.depotFile);
    }

    const auto plan =
        mirror::planSyncback(baselineHave, nowHave, openedDepotFiles);
    const size_t actionable = plan.restores.size() + plan.removes.size();

    // Files left open are reported whether or not anything else moved: a run
    // that restores nothing because the only drift is locked behind an open
    // changelist must not read as "already in sync".
    auto reportSkipped = [&]() {
        if (plan.skippedOpen.empty()) return;
        std::printf("\nWARNING: %zu file(s) drifted from the baseline but are "
                    "open in P4, and were\nleft alone:\n",
                    plan.skippedOpen.size());
        for (const auto& depotFile : plan.skippedOpen) {
            std::printf("  open     %s\n", depotFile.c_str());
        }
        std::printf("p4 will not overwrite a file you have open, and syncing "
                    "one back would discard\nan unsubmitted resolve. Submit or "
                    "revert the changelist that holds them (see\n'gw status'), "
                    "then rerun 'gw syncback'.\n");
    };

    if (actionable == 0) {
        if (plan.skippedOpen.empty()) {
            std::printf("The client is already on the baseline's revisions - "
                        "nothing to sync back.\n");
        } else {
            std::printf("Nothing to sync back that is not open in P4.\n");
            reportSkipped();
        }
        return 0;
    }

    auto printPlan = [&]() {
        for (const auto& action : plan.restores) {
            std::printf("  restore  %s#%s\n", action.depotFile.c_str(),
                        action.rev.c_str());
        }
        for (const auto& action : plan.removes) {
            std::printf("  remove   %s (synced since the import; the baseline "
                        "never had it)\n",
                        action.depotFile.c_str());
        }
    };

    if (dryRun) {
        std::printf("[dry-run] would sync %zu file(s) back to the baseline "
                    "(%s):\n",
                    actionable, shortSha(*depotTip).c_str());
        printPlan();
        reportSkipped();
        std::printf("\n[dry-run] no changes made. Rerun without --dry-run to "
                    "sync them back.\n");
        return 0;
    }

    // One `p4 sync` over the whole plan: restores and removes name disjoint
    // files, so the order between them carries no meaning.
    std::vector<std::string> fileSpecs;
    fileSpecs.reserve(actionable);
    for (const auto& action : plan.restores) {
        fileSpecs.push_back(action.depotFile + "#" + action.rev);
    }
    for (const auto& action : plan.removes) {
        fileSpecs.push_back(action.depotFile + "#0");
    }
    auto synced = p4::syncToRevisions(*config, fileSpecs);
    if (!synced) {
        std::fprintf(stderr, "gw syncback: %s\n", synced.error().c_str());
        std::fprintf(stderr,
                     "The mirror may be partially restored. Rerun 'gw syncback' "
                     "once the cause is\nfixed - it recomputes the plan from "
                     "the client's current state, so a partial\nrun is safe to "
                     "repeat.\n");
        return 1;
    }

    std::printf("Synced %zu file(s) back to the depot baseline (%s):\n",
                actionable, shortSha(*depotTip).c_str());
    printPlan();
    std::printf("\nThe mirror is back on the revisions gw imported, so "
                "'gw import' sees no change\nand 'gw prepare' diffs against the "
                "same depot state as before.\n");

    if (!plan.restores.empty()) {
        std::printf("\nNote: if one of these files carried a change you just "
                    "submitted, the mirror no\nlonger holds it, so preparing "
                    "that branch again would ship it a second time.\nThat "
                    "settles itself the next time you sync for real and run "
                    "'gw import' - the\ncommit rebases away as an empty "
                    "change.\n");
    }
    reportSkipped();
    return 0;
}

}  // namespace p4gw
