#pragma once

#include <string>

namespace p4gw {

// A snapshot of where Git and P4 stand, gathered by `gw status`. The struct
// is plain data so the formatting and the "what next" advice can be unit
// tested without touching real git/p4.
struct StatusInfo {
    // ---- Git side (always available) ----
    bool hasCommits = false;        // false on a fresh repo (unborn branch)
    std::string branch;             // current branch name
    bool detached = false;          // HEAD is not on a branch
    std::string baselineBranch;     // e.g. "main"
    bool baselineExists = false;    // the baseline branch has been created
    bool onBaseline = false;        // current branch is the baseline
    int ahead = 0;                  // commits on the branch, not yet shipped
    int behind = 0;                 // depot commits the branch hasn't rebased onto
    bool dirty = false;             // working tree has uncommitted changes
    int changeCount = 0;            // number of changed/untracked paths

    // When the last `gw import` ran, as git's relative date of the baseline
    // commit (e.g. "2 hours ago"); empty when unknown or never imported.
    std::string lastImportRelativeDate;

    // ---- P4 side (may be unavailable) ----
    bool p4Reachable = false;       // did `p4 opened` succeed?
    bool hasPending = false;        // gw-opened files exist under depot_path
    int pendingCount = 0;           // number of opened files
};

// The single most relevant next action for the user, given their state.
// Intentionally one suggestion, not a menu - the point is to orient a new
// user, not to enumerate options.
std::string nextStep(const StatusInfo& info);

// The full human-readable status report (the aligned block plus the
// "Next:" line), ready to print.
std::string renderStatus(const StatusInfo& info);

}  // namespace p4gw
