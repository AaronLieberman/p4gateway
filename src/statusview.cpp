// SPDX-License-Identifier: MIT

#include "statusview.h"

namespace p4gw {

namespace {

// Appends "label   value" with the label padded so the values line up.
void appendRow(std::string& out, const char* label, const std::string& value) {
    constexpr int kWidth = 13;
    out += "  ";
    out += label;
    for (int pad = kWidth - static_cast<int>(std::string(label).size());
         pad > 0; --pad) {
        out += ' ';
    }
    out += value;
    out += '\n';
}

std::string baselineSummary(const StatusInfo& info) {
    std::string s = info.baselineBranch;
    if (!info.baselineExists) {
        return s + " (not created yet)";
    }
    if (info.onBaseline) {
        return s + " (you are here)";
    }
    if (info.ahead == 0 && info.behind == 0) {
        return s + " - in sync";
    }
    s += " - ";
    if (info.ahead > 0) {
        s += std::to_string(info.ahead) + " ahead";
    }
    if (info.behind > 0) {
        if (info.ahead > 0) s += ", ";
        s += std::to_string(info.behind) + " behind";
    }
    return s;
}

}  // namespace

std::string nextStep(const StatusInfo& info) {
    if (!info.hasCommits) {
        return "Sync from Perforce, then run  gw import  to create the " +
               info.baselineBranch + " baseline.";
    }
    if (info.onBaseline) {
        return "You're on the baseline. Start work with:  "
               "git switch -c <your-branch>";
    }
    if (info.hasPending) {
        return "A pending changelist is open - review and submit it in P4V.";
    }
    if (info.behind > 0) {
        return "Depot has moved on. Catch up and rebase with:  "
               "gw import --rebase";
    }
    if (info.ahead > 0 && info.dirty) {
        return "Commit your changes, then ship them with:  gw prepare";
    }
    if (info.ahead > 0) {
        return "Ship your work - open a Perforce changelist with:  gw prepare";
    }
    if (info.dirty) {
        return "Commit your work, then ship it with:  gw prepare";
    }
    return "Nothing to ship yet. Pull in depot changes with:  "
           "gw import --rebase";
}

std::string renderStatus(const StatusInfo& info) {
    std::string out = "gw status\n\n";

    if (!info.hasCommits) {
        out += "  No commits yet - this repo has no baseline.\n";
        out += "\nNext: " + nextStep(info) + "\n";
        return out;
    }

    appendRow(out, "Branch", info.detached ? "(detached HEAD)" : info.branch);
    appendRow(out, "Baseline", baselineSummary(info));

    std::string tree = "clean";
    if (info.dirty) {
        tree = std::to_string(info.changeCount) +
               (info.changeCount == 1 ? " uncommitted change"
                                      : " uncommitted changes");
    }
    appendRow(out, "Working tree", tree);

    std::string lastImport;
    if (!info.baselineExists) {
        lastImport = "none yet";
    } else if (!info.lastImportRelativeDate.empty()) {
        lastImport = info.lastImportRelativeDate;
    } else {
        lastImport = "imported (time unknown)";
    }
    appendRow(out, "Last import", lastImport);

    std::string pending;
    if (!info.p4Reachable) {
        pending = "unknown (P4 not reachable - run gw doctor)";
    } else if (!info.hasPending) {
        pending = "none";
    } else {
        pending = std::to_string(info.pendingCount) +
                  (info.pendingCount == 1 ? " file open (submit in P4V)"
                                          : " files open (submit in P4V)");
    }
    appendRow(out, "Pending CL", pending);

    out += "\nNext: " + nextStep(info) + "\n";
    return out;
}

}  // namespace p4gw