#pragma once

#include <expected>
#include <string>
#include <vector>

namespace p4gw::mirror {

// Files gw itself maintains in the repo; they have no counterpart in the
// depot, so `gw import` never deletes them and `gw prepare` never opens
// them in a changelist.
bool isGwMetadataPath(const std::string& repoRelativePath);

// What `gw import` must do to make the working tree match the mirror.
// Paths are repo-relative with forward slashes.
struct SyncActions {
    std::vector<std::string> copies;   // every mirror file, copied over
    std::vector<std::string> deletes;  // tracked files absent from the mirror
};

// Pure: decides the actions from the mirror's file list and Git's tracked
// file list. Content comparison is left to Git (an unchanged copy results
// in an empty commit, reported as "already up to date").
SyncActions computeSyncActions(const std::vector<std::string>& mirrorFiles,
                               const std::vector<std::string>& trackedFiles);

// All regular files under `dir`, relative to it, with forward slashes.
std::expected<std::vector<std::string>, std::string> listFiles(
    const std::string& dir);

// Copies/deletes per `actions` between the mirror and the working tree.
// Copied files are made writable (mirror files are typically read-only).
std::expected<void, std::string> applySyncActions(const SyncActions& actions,
                                                  const std::string& mirrorDir,
                                                  const std::string& worktreeDir);

}  // namespace p4gw::mirror
