#pragma once

#include <string>
#include <vector>

namespace p4gw {

// Arguments remaining after the command name, e.g. for
// `gw submit --shelve feature/foo` this holds {"--shelve", "feature/foo"}.
using Args = std::vector<std::string>;

// Each command returns a process exit code (0 = success).
int cmdInit(const Args& args);
int cmdSync(const Args& args);
int cmdSubmit(const Args& args);
int cmdStatus(const Args& args);
int cmdDoctor(const Args& args);

}  // namespace p4gw
