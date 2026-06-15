#pragma once

#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace p4gw {

// What a shelved file does, mapped from p4's action strings. Anything we
// don't model explicitly (branch, integrate, ...) becomes `Other` and is
// treated like an add (take the shelved content as-is).
enum class ShelveAction { Edit, Add, Delete, MoveAdd, MoveDelete, Other };

// One file in a shelved changelist, parsed from `p4 -ztag describe -S`.
struct ShelvedFile {
    std::string depotFile;            // e.g. //depot/game/src/anim/Blend.cpp
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
std::expected<ShelfInfo, std::string> parseShelveDescribe(const std::string& out);

// Repo-relative path for a depot file under `depotPath` (the configured
// "//depot/.../..." scope), or nullopt if the file is outside the subtree.
// "//depot/game/src/..." + "//depot/game/src/anim/B.cpp" -> "anim/B.cpp".
std::optional<std::string> depotToRepoRelative(const std::string& depotPath,
                                               const std::string& depotFile);

}  // namespace p4gw
