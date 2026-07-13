// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <vector>

namespace p4gw {

// Arguments remaining after the command name, e.g. for
// `gw prepare --verify` this holds {"--verify"}.
using Args = std::vector<std::string>;

// Each command returns a process exit code (0 = success).
int cmdSetup(const Args& args);
int cmdInit(const Args& args);
int cmdImport(const Args& args);
int cmdPrepare(const Args& args);
int cmdStatus(const Args& args);
int cmdShelf(const Args& args);
int cmdDoctor(const Args& args);

// Live-P4 integration tests; `gwExe` is the binary to spawn for the
// commands under test (normally argv[0]).
int cmdIntegtest(const std::string& gwExe, const Args& args);

}  // namespace p4gw