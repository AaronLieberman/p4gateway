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

// True when `ancestor` is an ancestor of (or equal to) `descendant`.
std::expected<bool, std::string> isAncestor(const std::string& ancestor,
                                            const std::string& descendant,
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
