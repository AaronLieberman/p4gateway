#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
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

// An opened mirror file as `gw import` sees it.
struct OpenedMirrorFile {
    std::string path;  // repo-relative, forward slashes
    bool hasDepotRev;  // true: read the depot head; false (add-like): omit
};

// Sync actions plus the files `gw import` must fetch from the depot head
// instead of copying from the mirror.
struct ImportPlan {
    SyncActions actions;
    std::vector<std::string> depotReads;  // repo-relative; from depot head
};

// Pure: adjusts base sync actions so files already opened in the mirror are
// not taken from their (possibly un-submitted) working copy:
//   - hasDepotRev: dropped from copies/deletes and listed in `depotReads`,
//     so import fetches the head revision from the depot instead.
//   - !hasDepotRev (add-like): dropped from copies; it has no depot state and
//     must not appear in the baseline.
ImportPlan planImport(const SyncActions& base,
                      const std::vector<OpenedMirrorFile>& opened);

// All regular files under `dir`, relative to it, with forward slashes.
std::expected<std::vector<std::string>, std::string> listFiles(
    const std::string& dir);

// Pure: whether a mirror file must be copied into the working tree, given the
// size and modification time of the mirror source and (if it exists) of the
// working-tree target. A file is skipped only when the target already matches
// in both size and mtime - the same heuristic rsync uses by default. Because
// `applySyncActions` stamps the mirror's mtime onto the file it copies, an
// unchanged mirror file reads as already-present on the next import and is not
// recopied. On a big subtree where most files are untouched this turns a full
// copy into a cheap stat of each file.
bool copyNeeded(std::uintmax_t srcSize,
                std::filesystem::file_time_type srcMtime, bool dstExists,
                std::uintmax_t dstSize,
                std::filesystem::file_time_type dstMtime);

// Copies/deletes per `actions` between the mirror and the working tree.
// Copied files are made writable (mirror files are typically read-only).
std::expected<void, std::string> applySyncActions(const SyncActions& actions,
                                                  const std::string& mirrorDir,
                                                  const std::string& worktreeDir);

}  // namespace p4gw::mirror
