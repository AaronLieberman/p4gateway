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

    // Directory the client view maps `depot_path` into - gw's staging area,
    // which p4 syncs and gw reads/writes. Relative values are resolved
    // against the directory containing the .p4gw file.
    std::string mirrorPath;

    // P4 client (workspace) name. Empty means use the ambient P4CLIENT.
    std::string client;

    // Name of the Git branch that tracks pristine P4 state.
    std::string baselineBranch = "p4-main";
};

// Loads configuration from `path`. Unknown keys are an error so typos
// surface immediately.
std::expected<Config, std::string> loadConfig(const std::string& path);

// Path of the nearest `.p4gw` file at `startDir` or any parent directory;
// empty if none. Entries named `.p4gw` that are not regular files (e.g. a
// directory holding the mirror) are skipped.
std::string findConfigFile(const std::string& startDir);

// Searches the current directory and its parents for a `.p4gw` file and
// loads it. Returns the loaded config and sets `rootDir` to the directory
// containing the file.
std::expected<Config, std::string> findAndLoadConfig(std::string& rootDir);

// Absolute path of the mirror directory: `mirror_path` resolved against
// `rootDir` (the directory containing the .p4gw file) when relative.
std::string resolveMirrorPath(const Config& config, const std::string& rootDir);

}  // namespace p4gw
