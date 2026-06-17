#pragma once

#include <expected>
#include <string>
#include <vector>

namespace p4gw {

// One depot subtree this Git repo overlays, and where its files live on both
// sides of the mirror boundary. A repo can declare several of these (one per
// `mapping` line) so a single Git tree can ship more than one depot subtree -
// e.g. `src/` and `config/`. The starter `.gitignore` is an allowlist that
// tracks only the mapped subtrees; unmapped directories (`bin/`, `content/`,
// other depot content synced in place) stay out of Git unless re-included by
// hand (`!/dir/`). See buildGitignore.
struct Mapping {
    // Depot path of the subtree, e.g. "//depot/yourgame/src/...". Ends with
    // "/...". Used to scope every p4 operation so we never touch (or crawl)
    // the rest of the workspace.
    std::string depotPath;

    // Directory the client view remaps `depotPath` into - p4's staging area,
    // which p4 syncs and gw reads/writes. Always lives under the repo's single
    // `.p4gw` container; relative values resolve against the directory holding
    // the p4gw.cfg file. Example: ".p4gw/src".
    std::string mirrorPath;

    // Working-tree directory the mirror feeds, derived from `mirrorPath` by
    // dropping its leading `.p4gw` container component: ".p4gw/src" -> "src",
    // ".p4gw" -> "" (the whole repo). Forward slashes; no trailing slash.
    std::string repoSubtree;
};

// Project configuration, loaded from a `p4gw.cfg` file at the root of the Git
// overlay repo. Simple `key = value` lines; `#` starts a comment.
struct Config {
    // The depot subtrees this repo overlays, in declaration order. At least
    // one is required (a config with none fails to load).
    std::vector<Mapping> mappings;

    // P4 client (workspace) name. Empty means use the ambient P4CLIENT.
    std::string client;

    // Name of the Git branch that tracks pristine P4 state.
    std::string baselineBranch = "p4-main";
};

// Loads configuration from `path`. Unknown keys are an error so typos
// surface immediately.
std::expected<Config, std::string> loadConfig(const std::string& path);

// Path of the nearest `p4gw.cfg` file at `startDir` or any parent directory;
// empty if none. Entries named `p4gw.cfg` that are not regular files are
// skipped.
std::string findConfigFile(const std::string& startDir);

// Searches the current directory and its parents for a `p4gw.cfg` file and
// loads it. Returns the loaded config and sets `rootDir` to the directory
// containing the file.
std::expected<Config, std::string> findAndLoadConfig(std::string& rootDir);

// Absolute path of a mirror directory: `mirrorPath` resolved against `rootDir`
// (the directory containing the p4gw.cfg file) when relative.
std::string resolveMirrorPath(const std::string& mirrorPath,
                              const std::string& rootDir);

// Working-tree subtree a mirror feeds: the mirror path with its leading
// container component (`.p4gw`) removed. ".p4gw/src" -> "src", ".p4gw" -> "".
// Forward slashes, no trailing slash. Pure; unit-tested.
std::string mirrorRepoSubtree(const std::string& mirrorPath);

// Builds the starter `.gitignore` content for a fresh repo. gw tracks only the
// depot subtree(s) the repo maps; everything else in the working tree -
// unmapped P4 content synced in place, the `.p4gw` mirror, and gw's own
// personal config - stays out of Git. The result is an allowlist: ignore
// everything at the root, then re-include exactly each mapped working-tree
// subtree (and `.gitignore` itself). A whole-repo mapping (empty repoSubtree)
// has nothing unmapped to hide, so it falls back to a plain denylist of just
// the gw-managed paths. Pure; unit-tested.
std::string buildGitignore(const std::vector<Mapping>& mappings);

}  // namespace p4gw
