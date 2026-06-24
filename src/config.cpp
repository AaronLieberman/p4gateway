#include "config.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace p4gw {

namespace {

std::string trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

// Splits an `include` value into whitespace-separated tokens, honoring double
// quotes around paths that contain spaces.
std::vector<std::string> tokenize(const std::string& value) {
    std::vector<std::string> tokens;
    size_t pos = 0;
    while (pos < value.size()) {
        while (pos < value.size() && (value[pos] == ' ' || value[pos] == '\t')) {
            ++pos;
        }
        if (pos >= value.size()) break;
        std::string token;
        if (value[pos] == '"') {
            const auto close = value.find('"', pos + 1);
            if (close == std::string::npos) {
                token = value.substr(pos + 1);
                pos = value.size();
            } else {
                token = value.substr(pos + 1, close - pos - 1);
                pos = close + 1;
            }
        } else {
            const auto end = value.find_first_of(" \t", pos);
            token = value.substr(
                pos, (end == std::string::npos ? value.size() : end) - pos);
            pos = (end == std::string::npos) ? value.size() : end;
        }
        tokens.push_back(std::move(token));
    }
    return tokens;
}

}  // namespace

std::string mirrorRepoSubtree(const std::string& mirrorPath) {
    fs::path normalized = fs::path(mirrorPath).lexically_normal();
    auto it = normalized.begin();
    if (it == normalized.end()) return {};
    ++it;  // drop the leading `.p4gw` container component
    fs::path subtree;
    for (; it != normalized.end(); ++it) {
        if (it->string() == ".") continue;
        subtree /= *it;
    }
    return subtree.generic_string();
}

std::string excludedRepoSubtree(const std::string& mappingDepotPath,
                                const std::string& repoSubtree,
                                const std::string& excludeDepotPath) {
    // Drop the trailing "..." but keep the slash so prefix tests are anchored
    // at a path boundary (".../src/" must not match ".../srclib/").
    auto stripWildcard = [](std::string p) {
        if (p.ends_with("...")) p.resize(p.size() - 3);
        return p;
    };
    const std::string base = stripWildcard(mappingDepotPath);   // //d/src/
    const std::string excl = stripWildcard(excludeDepotPath);   // //d/src/lib/
    if (excl.size() <= base.size() || !excl.starts_with(base)) return {};
    std::string rel = excl.substr(base.size());                 // lib/  or  a/b/
    while (!rel.empty() && rel.back() == '/') rel.pop_back();    // lib   or  a/b
    if (rel.empty()) return repoSubtree;
    return repoSubtree.empty() ? rel : repoSubtree + "/" + rel;
}

namespace {

// Splits a forward-slash path into its non-empty components.
std::vector<std::string> pathComponents(const std::string& p) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : p) {
        if (c == '/') {
            if (!cur.empty()) {
                parts.push_back(cur);
                cur.clear();
            }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    return parts;
}

const std::string kGwDenylist =
    "# gw's local config - personal, never goes to Git or P4\n"
    "p4gw.cfg\n"
    "# P4 connection config - personal, never goes to Git\n"
    "p4.ini\n"
    ".p4config\n"
    "\n# gw's mirror directory - P4-managed, not for Git\n"
    ".p4gw/\n";

}  // namespace

std::string buildGitignore(const std::vector<Mapping>& mappings) {
    // Distinct mapped working-tree subtrees, in declaration order. An empty
    // subtree means a mapping covers the whole repo.
    std::vector<std::string> subtrees;
    bool wholeRepoMapped = false;
    for (const auto& m : mappings) {
        if (m.repoSubtree.empty()) {
            wholeRepoMapped = true;
            continue;
        }
        if (std::find(subtrees.begin(), subtrees.end(), m.repoSubtree) ==
            subtrees.end()) {
            subtrees.push_back(m.repoSubtree);
        }
    }

    // Distinct carved-out subtrees across all mappings, in declaration order.
    // These are directories under a mapped subtree that the user excluded from
    // the mirror (an `exclude` line); they sync in place / are unsynced and
    // must stay out of Git, so they get re-excluded after the allowlist below.
    std::vector<std::string> excludedSubtrees;
    for (const auto& m : mappings) {
        for (const auto& ex : m.excludedSubtrees) {
            if (ex.empty()) continue;
            if (std::find(excludedSubtrees.begin(), excludedSubtrees.end(),
                          ex) == excludedSubtrees.end()) {
                excludedSubtrees.push_back(ex);
            }
        }
    }

    // Re-excludes the carved-out subtrees, e.g. "/src/thirdparty/". Appended to
    // whichever body is built below: in the allowlist they re-exclude a tracked
    // subtree's children; in the denylist they ignore an otherwise-tracked dir.
    auto appendExclusions = [&](std::string& out) {
        if (excludedSubtrees.empty()) return;
        out += "\n# Directories under a mapped subtree that are carved out of "
               "the mirror\n# (an 'exclude' line): they sync in place / are "
               "unsynced, like unmapped\n# depot content, so Git ignores "
               "them.\n";
        for (const auto& ex : excludedSubtrees) {
            out += "/" + ex + "/\n";
        }
    };

    // A whole-repo mapping leaves nothing unmapped to hide, so an allowlist
    // would only ignore the repo's own content. Fall back to a plain denylist
    // of the gw-managed paths (personal config + the mirror container), plus
    // any carved-out directories.
    if (wholeRepoMapped || subtrees.empty()) {
        std::string out = kGwDenylist;
        appendExclusions(out);
        return out;
    }

    // The mapped subtrees as component vectors. Drop any nested under another
    // mapped subtree: the ancestor's re-include already exposes it, and a
    // redundant child line would be re-excluded by the ancestor's `/dir/*`.
    std::vector<std::vector<std::string>> leaves;
    for (const auto& s : subtrees) leaves.push_back(pathComponents(s));
    auto isAncestor = [](const std::vector<std::string>& anc,
                         const std::vector<std::string>& desc) {
        if (anc.size() >= desc.size()) return false;
        return std::equal(anc.begin(), anc.end(), desc.begin());
    };
    std::vector<std::vector<std::string>> kept;
    for (const auto& leaf : leaves) {
        bool nested = false;
        for (const auto& other : leaves) {
            if (&other != &leaf && isAncestor(other, leaf)) {
                nested = true;
                break;
            }
        }
        if (!nested) kept.push_back(leaf);
    }

    // Every directory that must appear in the file: each kept subtree plus all
    // of its ancestors. A directory is a "leaf" when it is exactly a mapped
    // subtree (tracked whole); ancestors are intermediate (we re-include the
    // dir but re-exclude its other children). Recorded in first-seen order so
    // emission is deterministic.
    struct Dir {
        std::vector<std::string> components;
        bool isLeaf;
    };
    std::vector<Dir> dirs;
    auto findDir = [&](const std::vector<std::string>& c) -> Dir* {
        for (auto& d : dirs)
            if (d.components == c) return &d;
        return nullptr;
    };
    for (const auto& leaf : kept) {
        std::vector<std::string> prefix;
        for (size_t i = 0; i < leaf.size(); ++i) {
            prefix.push_back(leaf[i]);
            const bool isLeaf = (i + 1 == leaf.size());
            if (Dir* existing = findDir(prefix)) {
                existing->isLeaf = existing->isLeaf || isLeaf;
            } else {
                dirs.push_back({prefix, isLeaf});
            }
        }
    }

    auto join = [](const std::vector<std::string>& c) {
        std::string s;
        for (const auto& part : c) {
            s += '/';
            s += part;
        }
        return s;  // leading slash, no trailing slash, e.g. "/a/b"
    };

    std::string out =
        "# gw tracks only the depot subtree(s) this repo maps. Everything else\n"
        "# in the working tree - unmapped P4 content synced in place, the .p4gw\n"
        "# mirror, and gw's own p4gw.cfg/p4.ini/.p4config - stays out of Git. To\n"
        "# keep a Git-only directory, add a line like '!/notes/'.\n"
        "/*\n"
        "!/.gitignore\n";

    // Emit by depth: at each level re-include the needed directories, then
    // re-exclude the children of any intermediate one, so the next (deeper)
    // level's re-includes show only the mapped descendants through.
    size_t maxDepth = 0;
    for (const auto& d : dirs) maxDepth = std::max(maxDepth, d.components.size());
    for (size_t depth = 1; depth <= maxDepth; ++depth) {
        for (const auto& d : dirs)
            if (d.components.size() == depth)
                out += "!" + join(d.components) + "/\n";
        for (const auto& d : dirs)
            if (d.components.size() == depth && !d.isLeaf)
                out += join(d.components) + "/*\n";
    }
    // The allowlist re-includes each mapped subtree whole (`!/src/`); a later
    // `/src/thirdparty/` line then carves the excluded directories back out.
    // Git applies the patterns in order, so these must come last.
    appendExclusions(out);
    return out;
}

std::expected<Config, std::string> loadConfig(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        return std::unexpected("cannot open config file: " + path);
    }

    Config config;
    std::string line;
    int lineNumber = 0;
    while (std::getline(file, line)) {
        ++lineNumber;
        const std::string stripped = trim(line);
        if (stripped.empty() || stripped[0] == '#') continue;

        const auto eq = stripped.find('=');
        if (eq == std::string::npos) {
            return std::unexpected(path + ":" + std::to_string(lineNumber) +
                                   ": expected 'key = value'");
        }
        const std::string key = trim(stripped.substr(0, eq));
        const std::string value = trim(stripped.substr(eq + 1));
        const std::string where = path + ":" + std::to_string(lineNumber);

        if (key == "include") {
            const auto tokens = tokenize(value);
            if (tokens.size() != 2) {
                return std::unexpected(
                    where + ": 'include' takes two values: "
                    "<depot_path> <mirror_path>");
            }
            Mapping mapping;
            mapping.depotPath = tokens[0];
            mapping.mirrorPath = tokens[1];
            mapping.repoSubtree = mirrorRepoSubtree(tokens[1]);
            if (!mapping.depotPath.ends_with("/...")) {
                return std::unexpected(
                    where + ": depot path '" + mapping.depotPath +
                    "' must end with '/...'");
            }
            for (const auto& existing : config.mappings) {
                if (existing.depotPath == mapping.depotPath) {
                    return std::unexpected(where + ": depot path '" +
                                           mapping.depotPath +
                                           "' is mapped twice");
                }
                if (existing.mirrorPath == mapping.mirrorPath) {
                    return std::unexpected(where + ": mirror path '" +
                                           mapping.mirrorPath +
                                           "' is used by two mappings");
                }
            }
            config.mappings.push_back(std::move(mapping));
        } else if (key == "exclude") {
            // An `exclude` carves a depot subtree out of the most recently
            // declared mapping: the client view drops it or syncs it in place,
            // and gw gitignores it rather than mirroring it.
            if (config.mappings.empty()) {
                return std::unexpected(
                    where + ": 'exclude' must follow the 'include' it carves "
                    "out of");
            }
            const auto tokens = tokenize(value);
            if (tokens.size() != 1) {
                return std::unexpected(
                    where + ": 'exclude' takes one value: <depot_path>");
            }
            const std::string& excludePath = tokens[0];
            if (!excludePath.ends_with("/...")) {
                return std::unexpected(where + ": exclude path '" + excludePath +
                                       "' must end with '/...'");
            }
            Mapping& owner = config.mappings.back();
            const std::string subtree = excludedRepoSubtree(
                owner.depotPath, owner.repoSubtree, excludePath);
            if (subtree.empty()) {
                return std::unexpected(
                    where + ": exclude path '" + excludePath +
                    "' is not strictly under its mapping's depot path '" +
                    owner.depotPath + "'");
            }
            if (std::find(owner.excludedDepotPaths.begin(),
                          owner.excludedDepotPaths.end(),
                          excludePath) != owner.excludedDepotPaths.end()) {
                return std::unexpected(where + ": exclude path '" + excludePath +
                                       "' is listed twice");
            }
            owner.excludedDepotPaths.push_back(excludePath);
            owner.excludedSubtrees.push_back(subtree);
        } else if (key == "client") {
            config.client = value;
        } else if (key == "baseline_branch") {
            config.baselineBranch = value;
        } else {
            return std::unexpected(where + ": unknown key '" + key + "'");
        }
    }

    if (config.mappings.empty()) {
        return std::unexpected(path + ": no 'include' lines - add at least one "
                               "('gw setup' writes the template). Format: "
                               "include = //depot/yourproject/src/... .p4gw/src");
    }
    return config;
}

std::string findConfigFile(const std::string& startDir) {
    fs::path dir = fs::absolute(startDir);
    while (true) {
        const fs::path candidate = dir / "p4gw.cfg";
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) {
            return candidate.string();
        }
        if (!dir.has_parent_path() || dir.parent_path() == dir) {
            return {};
        }
        dir = dir.parent_path();
    }
}

std::expected<Config, std::string> findAndLoadConfig(std::string& rootDir) {
    const std::string file = findConfigFile(fs::current_path().string());
    if (file.empty()) {
        return std::unexpected(
            "no p4gw.cfg config found in this directory or any parent; "
            "run 'gw setup' first");
    }
    rootDir = fs::path(file).parent_path().string();
    return loadConfig(file);
}

std::string resolveMirrorPath(const std::string& mirrorPath,
                              const std::string& rootDir) {
    fs::path mirror(mirrorPath);
    if (mirror.is_relative()) {
        mirror = fs::path(rootDir) / mirror;
    }
    return mirror.lexically_normal().string();
}

std::string depotTrackingRef(const Config& config) {
    return "refs/p4gw/" + config.baselineBranch;
}

}  // namespace p4gw
