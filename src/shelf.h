// SPDX-License-Identifier: MIT

#pragma once

#include <expected>
#include <string>
#include <vector>

namespace p4gw {

// What a shelved file does, mapped from p4's action strings. Anything we
// don't model explicitly (branch, integrate, ...) becomes `Other` and is
// treated like an add (take the shelved content as-is).
enum class ShelveAction { Edit, Add, Delete, MoveAdd, MoveDelete, Other };

// One file in a shelved changelist, parsed from `p4 -ztag describe -S`.
struct ShelvedFile {
    std::string depotFile;            // e.g. //depot/project/src/anim/Blend.cpp
    ShelveAction action = ShelveAction::Other;
    std::string type;                 // p4 filetype, e.g. "text", "binary"
    std::string rev;                  // base revision the shelf opened from
};

// The pieces of a shelved changelist `gw shelf import` needs.
struct ShelfInfo {
    std::string change;               // changelist number
    std::string description;          // shelf description (commit message)
    std::vector<ShelvedFile> files;
};

// Maps a p4 action string ("edit", "move/add", ...) to ShelveAction.
ShelveAction parseShelveAction(const std::string& action);

// True for p4 filetypes whose content is not line-based text, so a 3-way
// line merge would corrupt them (binary, apple, resource, ...).
bool isBinaryType(const std::string& type);

// Parses `p4 -ztag describe -S <cl>` output into a ShelfInfo. Tagged lines
// look like "... key value"; a field's value may continue on following
// untagged lines (the multi-line description). Indexed file fields
// (depotFile0, action0, ...) are grouped per file.
//
// For mapping a shelved depotFile to a repo-relative path, use
// p4::depotRelativePath (src/p4.h).
std::expected<ShelfInfo, std::string> parseShelveDescribe(const std::string& out);

// A pending or shelved changelist, for `gw shelf list`.
struct ChangeListing {
    std::string change;       // changelist number
    std::string description;  // full description (callers display line 1)
    bool shelved = false;     // the changelist has shelved files
};

// Parses `p4 -ztag changes ...` output into changelist records (change number
// and description), in the order p4 returned them. `shelved` is left false;
// the caller cross-references the `-s shelved` query to set it.
std::vector<ChangeListing> parseChanges(const std::string& out);

}  // namespace p4gw