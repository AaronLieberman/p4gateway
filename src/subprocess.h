// SPDX-License-Identifier: MIT

#pragma once

// The subprocess runner - the only place gw spawns child processes.
//
// This file must NOT be named process.h: MSVC's <thread> pulls in the CRT
// header <process.h> for _beginthreadex, and cl.exe searches /I directories
// before the system include path even for angle-bracket includes - so a
// project header named process.h silently shadows the CRT one, and any TU
// that constructs a std::thread fails with "'_beginthreadex': is not a
// member of '`global namespace'".

#include <expected>
#include <string>
#include <vector>

namespace p4gw {

struct RunResult {
    int exitCode = 0;
    // The child's two streams, captured separately as raw bytes (no text-mode
    // translation). Data parsers read stdoutText; the "no results" notices p4
    // emits ("no file(s) to reconcile", "file(s) not opened") arrive on
    // stderrText, so they can never be mistaken for - or hide - real output.
    std::string stdoutText;
    std::string stderrText;

    // stdout followed by stderr - for error messages that should show
    // everything the child said.
    std::string combined() const { return stdoutText + stderrText; }
};

struct RunOptions {
    std::string cwd;         // run in this directory if non-empty
    std::string stdinFile;   // redirect stdin from this file if non-empty
    std::string stdoutFile;  // redirect stdout to this file if non-empty;
                             // stderr is still captured in RunResult::output
};

// When enabled, every spawned command line is echoed to stderr before it runs
// (the `--verbose` flag). Off by default; set once from main.
void setVerbose(bool on);

// The local login name (Windows `USERNAME`, else POSIX `USER`/`LOGNAME`), or an
// empty string if none of those are set. Used to default a new repo's Git
// identity to the local account.
std::string currentUser();

// Runs an external command and captures its output. The child is spawned
// directly (CreateProcessW / posix_spawnp) with the arguments passed verbatim
// - no shell, so nothing is subject to shell quoting or expansion - and the
// executable is resolved against PATH. If `cwd` is non-empty the command runs
// in that directory.
//
// Returns an error string only if the process could not be started; a
// non-zero exit code is reported through RunResult::exitCode.
std::expected<RunResult, std::string> run(const std::string& exe,
                                          const std::vector<std::string>& args,
                                          const std::string& cwd = {});

std::expected<RunResult, std::string> run(const std::string& exe,
                                          const std::vector<std::string>& args,
                                          const RunOptions& options);

// Returns a path in the system temp directory built from `prefix` and `suffix`
// but guaranteed unique to this process and call: the process id and a
// per-process counter are spliced in between. Two concurrent gw runs (or two
// users sharing /tmp) therefore never collide on the same scratch file. The
// caller owns the returned path and is responsible for removing it.
std::string uniqueTempFile(const std::string& prefix,
                           const std::string& suffix = {});

// True when `path` is owned by the current user. Mismatched ownership is what
// breaks libgit2-based tools (git-branchless refuses to open the repo) even
// when the git CLI tolerates it - a confusing failure worth flagging. On
// Windows this compares the directory's owner SID to the process user's SID;
// on POSIX it compares st_uid to the effective uid.
std::expected<bool, std::string> isOwnedByCurrentUser(const std::string& path);

// Reads an environment variable, returning empty if it is unset. (getenv_s on
// Windows: MSVC deprecates std::getenv with C4996, so this is the one place
// that reads the environment on both platforms.)
std::string envValue(const char* name);

}  // namespace p4gw