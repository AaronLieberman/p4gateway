#pragma once

#include <expected>
#include <string>
#include <vector>

namespace p4gw {

struct RunResult {
    int exitCode = 0;
    std::string output;  // stdout and stderr combined
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

// Runs an external command and captures its output. Arguments are quoted as
// needed for the platform shell. If `cwd` is non-empty the command runs in
// that directory.
//
// Returns an error string only if the process could not be started; a
// non-zero exit code is reported through RunResult::exitCode.
std::expected<RunResult, std::string> run(const std::string& exe,
                                          const std::vector<std::string>& args,
                                          const std::string& cwd = {});

std::expected<RunResult, std::string> run(const std::string& exe,
                                          const std::vector<std::string>& args,
                                          const RunOptions& options);

}  // namespace p4gw
