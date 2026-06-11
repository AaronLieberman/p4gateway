#pragma once

#include <string>
#include <vector>

namespace p4gw {

// Arguments remaining after the command name, e.g. for
// `gw prepare --no-verify` this holds {"--no-verify"}.
using Args = std::vector<std::string>;

// Each command returns a process exit code (0 = success).
int cmdSetup(const Args& args);
int cmdInit(const Args& args);
int cmdImport(const Args& args);
int cmdPrepare(const Args& args);
int cmdStatus(const Args& args);
int cmdDoctor(const Args& args);

}  // namespace p4gw
