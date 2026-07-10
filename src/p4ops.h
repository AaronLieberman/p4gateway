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
// files (p4gw.cfg, .gitignore) are silently skipped - they never go to P4.
std::expected<std::vector<P4Op>, std::string> planP4Operations(
    const std::vector<git::FileChange>& changes);

// The git commit range a `gw prepare` invocation ships: the diff is `base..
// target` and the staged content comes from `target`. `base` is exclusive.
struct SliceRange {
    std::string base;    // a git ref (the depot baseline, a parent, or A of A..B)
    std::string target;  // a git ref (HEAD, a named commit, or B of A..B)
};

// Pure: interprets the `gw prepare` positional argument into a commit range.
//   ""              -> baseline..HEAD          (the whole stack; the default)
//   <commit>        -> <commit>^..<commit>     (just that one commit)
//   <commit> +stack -> baseline..<commit>      (the stack up through it)
//   <a>..<b>        -> <a>..<b>                (an explicit range; either side
//                                               may be empty: ""..b is
//                                               baseline..b, a.. is a..HEAD)
// `depotRef` is substituted for the baseline. Rejects a symmetric "a...b" range
// and `--stack` combined with an explicit range. It only builds ref *strings*;
// validating that they resolve and are correctly ordered is the caller's job.
std::expected<SliceRange, std::string> resolveSliceRange(
    const std::string& spec, bool stack, const std::string& depotRef);

}  // namespace p4gw
