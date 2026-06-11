#pragma once

#include <expected>
#include <string>
#include <vector>

namespace p4gw {

struct RunResult {
    int exitCode = 0;
    std::string output;  // stdout and stderr combined
};

// Runs an external command and captures its output. Arguments are quoted as
// needed for the platform shell. If `cwd` is non-empty the command runs in
// that directory.
//
// Returns an error string only if the process could not be started; a
// non-zero exit code is reported through RunResult::exitCode.
std::expected<RunResult, std::string> run(const std::string& exe,
                                          const std::vector<std::string>& args,
                                          const std::string& cwd = {});

}  // namespace p4gw
