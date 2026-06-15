#pragma once

#include <expected>
#include <string>
#include <vector>

namespace p4gw::git {

// One file-level change between two trees, from `git diff --name-status`.
struct FileChange {
    char status = '?';     // A(dd), M(odify), D(elete), R(ename)
    std::string path;      // path relative to the repo root
    std::string newPath;   // destination path for renames, otherwise empty
};

// Commit counts between two refs, as `git status` reports for an upstream:
// `ahead` commits are on `ref` but not `base`; `behind` are on `base` but
// not `ref`.
struct AheadBehind {
    int ahead = 0;
    int behind = 0;
};

// Runs `git <args>` in `cwd` and returns trimmed stdout, or an error message
// including git's output if the command failed.
std::expected<std::string, std::string> run(const std::vector<std::string>& args,
                                            const std::string& cwd = {});

std::expected<std::string, std::string> currentBranch(const std::string& cwd = {});

std::expected<std::string, std::string> revParse(const std::string& ref,
                                                 const std::string& cwd = {});

// Returns true if the working tree has uncommitted changes (staged or not),
// including untracked files that are not ignored.
std::expected<bool, std::string> isDirty(const std::string& cwd = {});

std::expected<bool, std::string> branchExists(const std::string& branch,
                                              const std::string& cwd = {});

std::expected<std::string, std::string> switchBranch(const std::string& branch,
                                                     const std::string& cwd = {});

// `git switch --orphan` - a new branch with no history; tracked files of the
// previous branch are removed from the working tree (restored by switching
// back), untracked files are left alone.
std::expected<std::string, std::string> switchOrphanBranch(
    const std::string& branch, const std::string& cwd = {});

// `git switch -c <branch> <startRef>`: create and check out `branch` rooted
// at `startRef`. Fails if the branch already exists.
std::expected<std::string, std::string> createBranch(const std::string& branch,
                                                     const std::string& startRef,
                                                     const std::string& cwd = {});

// `git merge-file <ours> <base> <theirs>`: a 3-way line merge writing the
// result back into `ours` in place (with conflict markers on overlap).
// Returns true when the merge had conflicts, false when it merged cleanly.
std::expected<bool, std::string> mergeFile(const std::string& ours,
                                           const std::string& base,
                                           const std::string& theirs,
                                           const std::string& cwd = {});

// True when `ancestor` is an ancestor of (or equal to) `descendant`.
std::expected<bool, std::string> isAncestor(const std::string& ancestor,
                                            const std::string& descendant,
                                            const std::string& cwd = {});

// Commits `ref` is ahead of / behind `base` (`git rev-list --left-right
// --count base...ref`). Both refs must exist.
std::expected<AheadBehind, std::string> aheadBehind(const std::string& base,
                                                    const std::string& ref,
                                                    const std::string& cwd = {});

// Lines of `git status --porcelain` - one per changed or untracked (not
// ignored) path. Empty means a clean working tree.
std::expected<std::vector<std::string>, std::string> statusLines(
    const std::string& cwd = {});

// Subject line (first line of the message) of the commit at `ref`.
std::expected<std::string, std::string> commitSubject(const std::string& ref,
                                                      const std::string& cwd = {});

// All tracked files, repo-relative with forward slashes.
std::expected<std::vector<std::string>, std::string> lsFiles(
    const std::string& cwd = {});

std::expected<std::string, std::string> addAll(const std::string& cwd = {});

// True when there is nothing staged to commit.
std::expected<bool, std::string> indexMatchesHead(const std::string& cwd = {});

std::expected<std::string, std::string> commit(const std::string& message,
                                               const std::string& cwd = {});

// `git rebase <onto>` for the currently checked-out branch. On conflict the
// error includes git's output; the repo is left in the normal mid-rebase
// state for the user to resolve.
std::expected<std::string, std::string> rebase(const std::string& onto,
                                               const std::string& cwd = {});

// Writes the blob `ref:path` to `destFile` (byte-exact, safe for binaries).
std::expected<std::string, std::string> catBlobToFile(const std::string& ref,
                                                      const std::string& path,
                                                      const std::string& destFile,
                                                      const std::string& cwd = {});

// Value of a `git config` key, or empty string if the key is unset.
std::expected<std::string, std::string> configValue(const std::string& key,
                                                    const std::string& cwd = {});

// File-level changes from `fromRef` to `toRef`, with rename detection.
std::expected<std::vector<FileChange>, std::string> diffNameStatus(
    const std::string& fromRef, const std::string& toRef,
    const std::string& cwd = {});

// Commit subjects and bodies from `fromRef` (exclusive) to `toRef`
// (inclusive), oldest first - used to build the P4 changelist description.
std::expected<std::string, std::string> commitMessages(const std::string& fromRef,
                                                       const std::string& toRef,
                                                       const std::string& cwd = {});

}  // namespace p4gw::git
