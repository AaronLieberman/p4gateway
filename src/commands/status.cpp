#include <cstdio>

#include "commands.h"
#include "config.h"
#include "git.h"
#include "p4.h"
#include "statusview.h"

namespace p4gw {

namespace {

// Number of non-empty lines in p4's `opened` output (one per opened file).
int countLines(const std::string& text) {
    int n = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        if (end > pos) ++n;  // ignore blank lines
        pos = end + 1;
    }
    return n;
}

constexpr const char* kStatusUsage =
    "usage: gw status\n"
    "\n"
    "Show where Git and P4 stand at a glance: the current branch and how it\n"
    "relates to the depot baseline, working-tree cleanliness, the last imported\n"
    "changelist, any pending changelist, and the single most useful next step.\n"
    "The Git side works offline; the P4 line degrades to a note when P4 can't\n"
    "be reached, so status never hard-fails on a down connection.\n"
    "\n"
    "options:\n"
    "  -h, --help  Show this help\n";

}  // namespace

// A one-screen read on where Git and P4 stand, written for someone still
// learning the mirror workflow: current branch, how it relates to the
// baseline, working-tree cleanliness, the last imported changelist, and any
// pending changelist - capped off with the single most useful next step.
// The Git side and last-import CL work offline; the pending-CL line degrades
// to a note when P4 can't be reached, so `gw status` never hard-fails on a
// down connection.
int cmdStatus(const Args& args) {
    for (const auto& arg : args) {
        if (arg == "--help" || arg == "-h") {
            std::printf("%s", kStatusUsage);
            return 0;
        }
        std::fprintf(stderr, "gw status: unexpected argument '%s'\n",
                     arg.c_str());
        std::fprintf(stderr, "%s", kStatusUsage);
        return 1;
    }

    std::string root;
    auto config = findAndLoadConfig(root);
    if (!config) {
        std::fprintf(stderr,
                     "gw status: %s\n"
                     "Run 'gw setup' to create a p4gw.cfg, then 'gw init'.\n",
                     config.error().c_str());
        return 1;
    }

    StatusInfo info;
    info.baselineBranch = config->baselineBranch;
    const std::string depotRef = depotTrackingRef(*config);

    info.hasCommits = git::revParse("HEAD", root).has_value();
    if (!info.hasCommits) {
        std::fputs(renderStatus(info).c_str(), stdout);
        return 0;
    }

    if (auto branch = git::currentBranch(root)) {
        info.branch = *branch;
        info.detached = (*branch == "HEAD");
    } else {
        std::fprintf(stderr, "gw status: %s\n", branch.error().c_str());
        return 1;
    }

    // The baseline is the hidden depot-tracking ref, not the like-named branch:
    // ahead/behind are measured against pristine depot state, so local commits
    // on the main branch itself still show up as "ahead".
    info.baselineExists = git::revParse(depotRef, root).has_value();

    if (info.baselineExists) {
        if (auto ab = git::aheadBehind(depotRef, "HEAD", root)) {
            info.ahead = ab->ahead;
            info.behind = ab->behind;
        }
    }
    // "On the baseline" now means sitting exactly on pristine depot state with
    // nothing of your own on top - the prompt to start a branch still fits.
    info.onBaseline = info.baselineExists && !info.detached &&
                      info.ahead == 0 && info.behind == 0 &&
                      info.branch == info.baselineBranch;

    if (auto lines = git::statusLines(root)) {
        info.dirty = !lines->empty();
        info.changeCount = static_cast<int>(lines->size());
    }

    if (info.baselineExists) {
        if (auto subject = git::commitSubject(depotRef, root)) {
            info.lastImportedCl = parseImportedCl(*subject);
        }
    }

    // P4 side: best-effort. A missing binary or dead connection just leaves
    // the pending-CL line as "unknown" rather than failing the command.
    if (auto opened = p4::openedFiles(*config)) {
        info.p4Reachable = true;
        info.hasPending = !opened->empty();
        info.pendingCount = countLines(*opened);
    }

    std::fputs(renderStatus(info).c_str(), stdout);
    return 0;
}

}  // namespace p4gw
