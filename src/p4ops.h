#pragma once

#include <expected>
#include <string>
#include <vector>

#include "git.h"

namespace p4gw {

// One explicit P4 action `gw prepare` will run against the mirror, derived
// from the git diff between the baseline branch and HEAD.
struct P4Op {
    enum class Kind { Edit, Add, Delete, Move };
    Kind kind = Kind::Edit;
    std::string path;     // repo-relative; the source path for Move
    std::string newPath;  // destination path for Move, otherwise empty
};

// Pure: maps git file changes onto P4 operations. Returns ops grouped in
// execution order: deletes, then edits, then moves, then adds. gw metadata
// files (.p4gw, .gitignore) are silently skipped - they never go to P4.
std::expected<std::vector<P4Op>, std::string> planP4Operations(
    const std::vector<git::FileChange>& changes);

}  // namespace p4gw
