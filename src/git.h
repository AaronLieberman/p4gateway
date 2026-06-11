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

// Returns true if the working tree has uncommitted changes (staged or not).
std::expected<bool, std::string> isDirty(const std::string& cwd = {});

// File-level changes from `fromRef` to `toRef`, with rename detection.
std::expected<std::vector<FileChange>, std::string> diffNameStatus(
    const std::string& fromRef, const std::string& toRef,
    const std::string& cwd = {});

// Commit subjects and bodies from `fromRef` (exclusive) to `toRef`
// (inclusive), oldest first — used to build the P4 changelist description.
std::expected<std::string, std::string> commitMessages(const std::string& fromRef,
                                                       const std::string& toRef,
                                                       const std::string& cwd = {});

}  // namespace p4gw::git
