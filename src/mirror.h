// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
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
// Returns the number of files actually copied - the ones copyNeeded flagged as
// changed - which is far fewer than `actions.copies.size()` on a re-import
// where most of the mirror is unchanged. With `trustStats` false the
// size+mtime fast path is bypassed and every file is recopied - the recovery
// mode for when the stamped stats may lie (a torn import followed by outside
// cleanup such as `git reset --hard`).
std::expected<std::size_t, std::string> applySyncActions(
    const SyncActions& actions, const std::string& mirrorDir,
    const std::string& worktreeDir, bool trustStats = true);

// Byte-for-byte comparison of two files (safe for binaries). A missing or
// unreadable file compares unequal.
bool filesIdentical(const std::filesystem::path& a,
                    const std::filesystem::path& b);

// Files whose working-tree copy the import fast path would *skip* (size and
// mtime match the mirror - see copyNeeded) yet whose bytes differ from the
// mirror. Legitimate divergence - a branch edit, a fresh p4 sync - rewrites
// mtime or size and is never flagged, so any hit means the stamped stats lie
// and the next import would silently keep stale content. `files` is the
// tracked mirror listing (mirror-relative, forward slashes), the same list
// import copies; the returned paths are a subset of it, in input order.
std::vector<std::string> findStaleFastPathFiles(
    const std::vector<std::string>& files, const std::string& mirrorDir,
    const std::string& worktreeDir);

// Marker file recording an import that started mutating the repo and has not
// finished. `gw import` writes it before the first mutation and removes it
// once the snapshot is committed; while it exists, import distrusts the
// size+mtime fast path and recopies everything, and `gw doctor` reports the
// torn import. Lives in the git dir (never the working tree or the mirror),
// so it survives `git reset --hard` / `git clean` and p4 operations alike.
std::string importPendingMarkerPath(const std::string& gitDir);

// Path of the hidden git worktree `gw import` builds snapshots in under
// worktree mode: `<gitDir>/p4gw/worktree`. Lives in the git dir for the same
// reasons as the pending marker (outside the working tree and the mirror), and
// so that wiping `.git` takes the worktree's files and its registration
// together. Pure.
std::string snapshotWorktreePath(const std::string& gitDir);

// ---- the have manifest: incremental import's cache ----
//
// After each import, gw records the `p4 have` state (every depot file and its
// have revision) that produced the snapshot, keyed by the snapshot's commit.
// The next import diffs a fresh `p4 have` against it: a file whose rev is
// unchanged was not rewritten by sync, so the mirror copy still matches the
// snapshot and import skips it without a stat; only changed/added files are
// copied and only vanished files deleted. Strictly a cache with a fallback -
// a missing or snapshot-mismatched manifest (first import, torn write, moved
// ref) falls back to the full mirror walk, and `--full` bypasses it -
// correctness never depends on the manifest being present or fresh.

// One manifest record: a depot file and its have revision.
struct ManifestEntry {
    std::string depotFile;  // //depot/...
    std::string rev;        // e.g. "5"
};

// Path of the have manifest for `baseline`: `<gitDir>/p4gw/have-<baseline>`
// (path separators in the branch name flattened). Same home as the snapshot
// worktree and for the same reasons. Pure; unit-tested.
std::string haveManifestPath(const std::string& gitDir,
                             const std::string& baseline);

// Serializes the manifest: a comment header, a `snapshot <sha>` binding line,
// then one `<depotFile>#<rev>` line per entry. Pure; unit-tested.
std::string renderHaveManifest(const std::string& snapshot,
                               const std::vector<ManifestEntry>& entries);

// Parses manifest text. Returns the snapshot SHA through `snapshot` and the
// entries; a malformed body (no snapshot line) yields an empty snapshot, which
// callers treat as "no manifest". Pure; unit-tested.
std::vector<ManifestEntry> parseHaveManifest(const std::string& text,
                                             std::string& snapshot);

// Diffs two rel-path -> rev lists for one mapping into sync actions: a rel in
// `now` that is absent from `then` or carries a different rev is a copy; a rel
// only in `then` is a delete. A rel with the same rev in both is skipped
// entirely - the whole point. Both lists are mapping-relative (the same paths
// applySyncActions consumes). Pure; unit-tested.
SyncActions diffHaveState(
    const std::vector<std::pair<std::string, std::string>>& then,
    const std::vector<std::pair<std::string, std::string>>& now);

// ---- syncback: putting the client back on the baseline's revisions ----
//
// The manifest is also a record of where the mirror *was* when the baseline
// was imported, which is what lets `gw syncback` undo a sync-forward. After a
// resolve done in P4 (sync the conflicting file to head, merge, submit) the
// client sits past the baseline on those files, so an import would absorb a
// half-synced depot state. Syncing them back to the manifest's revisions puts
// the mirror exactly where the snapshot came from - no full sync needed - and
// leaves the manifest itself still valid, since the have state matches it
// again.

// One file `gw syncback` must re-sync to restore the baseline's have state.
struct SyncbackAction {
    std::string depotFile;  // //depot/...
    std::string rev;        // target have revision; "0" means un-sync it
};

// What `gw syncback` must sync to put the client back on the revisions the
// baseline snapshot was built from.
struct SyncbackPlan {
    // Files whose have revision moved since the import, or that were synced
    // away entirely: sync back to the recorded revision.
    std::vector<SyncbackAction> restores;

    // Files synced in since the import, which the baseline never had: sync to
    // #0, which drops p4's copy from the mirror. They are absent from the
    // depot snapshot and therefore from the working tree too, so removing them
    // discards nothing of the user's - and a plain `p4 sync` brings them back.
    std::vector<SyncbackAction> removes;

    // Depot files that drifted but are open in P4, reported instead of acted
    // on: p4 will not overwrite an opened file, and syncing one back would
    // throw away an unsubmitted resolve - exactly the work syncback exists to
    // protect.
    std::vector<std::string> skippedOpen;
};

// Pure: diffs the baseline's recorded have state against the client's current
// one and returns the syncs that restore it. Both lists hold owned entries
// (already resolved through the ordered view rules, as import records them).
// A file whose revision never moved yields no action at all - the common case,
// and the reason syncback costs a few file syncs where a full re-sync costs a
// workspace. Each list is sorted by depot path. Pure; unit-tested.
SyncbackPlan planSyncback(const std::vector<ManifestEntry>& baseline,
                          const std::vector<ManifestEntry>& now,
                          const std::vector<std::string>& openedDepotFiles);

}  // namespace p4gw::mirror