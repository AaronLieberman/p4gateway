#pragma once

#include <expected>
#include <string>

namespace p4gw {

// Project configuration, loaded from a `.p4gw` file at the root of the Git
// overlay repo. Simple `key = value` lines; `#` starts a comment.
struct Config {
    // Depot path of the subtree this Git repo overlays, e.g.
    // "//depot/yourgame/src/...". Used to scope every p4 operation so we
    // never touch (or crawl) the rest of the workspace.
    std::string depotPath;

    // P4 client (workspace) name. Empty means use the ambient P4CLIENT.
    std::string client;

    // Name of the Git branch that tracks pristine P4 state.
    std::string baselineBranch = "p4-main";
};

// Loads configuration from `path`. Unknown keys are an error so typos
// surface immediately.
std::expected<Config, std::string> loadConfig(const std::string& path);

// Searches the current directory and its parents for a `.p4gw` file and
// loads it. Returns the loaded config and sets `rootDir` to the directory
// containing the file.
std::expected<Config, std::string> findAndLoadConfig(std::string& rootDir);

}  // namespace p4gw
