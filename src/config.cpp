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

// "//depot/x/..." -> "//depot/x/" so a prefix test is anchored at a path
// boundary ("//d/src/" must not match "//d/srclib/").
std::string stripDepotWildcard(const std::string& path) {
    if (path.ends_with("...")) return path.substr(0, path.size() - 3);
    return path;
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
    const std::string base = stripDepotWildcard(mappingDepotPath);  // //d/src/
    const std::string excl = stripDepotWildcard(excludeDepotPath);  // //d/src/lib/
    if (excl.size() <= base.size() || !excl.starts_with(base)) return {};
    std::string rel = excl.substr(base.size());                 // lib/  or  a/b/
    while (!rel.empty() && rel.back() == '/') rel.pop_back();    // lib   or  a/b
    if (rel.empty()) return repoSubtree;
    return repoSubtree.empty() ? rel : repoSubtree + "/" + rel;
}

std::vector<const ViewRule*> includeRules(const std::vector<ViewRule>& rules) {
    std::vector<const ViewRule*> includes;
    for (const auto& rule : rules) {
        if (!rule.exclude) includes.push_back(&rule);
    }
    return includes;
}

std::vector<std::string> excludeDepotPaths(const std::vector<ViewRule>& rules) {
    std::vector<std::string> paths;
    for (const auto& rule : rules) {
        if (rule.exclude) paths.push_back(rule.depotPath);
    }
    return paths;
}

const ViewRule* effectiveRuleForDepot(const std::vector<ViewRule>& rules,
                                      const std::string& depotFile) {
    const ViewRule* effective = nullptr;
    for (const auto& rule : rules) {
        if (depotFile.starts_with(stripDepotWildcard(rule.depotPath))) {
            effective = &rule;  // later declaration wins
        }
    }
    return effective;
}

const ViewRule* effectiveRuleForRepo(const std::vector<ViewRule>& rules,
                                     const std::string& repoRel) {
    const ViewRule* effective = nullptr;
    for (const auto& rule : rules) {
        const std::string& sub = rule.repoSubtree;
        const bool matches =
            sub.empty() || repoRel == sub || repoRel.starts_with(sub + "/");
        if (matches) effective = &rule;  // later declaration wins
    }
    return effective;
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

bool isStrictAncestor(const std::vector<std::string>& anc,
                      const std::vector<std::string>& desc) {
    if (anc.size() >= desc.size()) return false;
    return std::equal(anc.begin(), anc.end(), desc.begin());
}

const std::string kGwDenylist =
    "# gw's local config - personal, never goes to Git or P4\n"
    "p4gw.cfg\n"
    "# P4 connection config - personal, never goes to Git\n"
    "p4.ini\n"
    ".p4config\n"
    "\n# gw's mirror directory - P4-managed, not for Git\n"
    ".p4gw/\n";

// A working-tree subtree named in the config and whether it is (last-wins)
// tracked (an include) or carved out (an exclude).
struct Boundary {
    std::string subtree;             // forward slashes, no trailing slash
    std::vector<std::string> comps;  // subtree split into components
    bool tracked;                    // include (true) vs exclude (false)
};

}  // namespace

std::string buildGitignore(const std::vector<ViewRule>& rules,
                           const std::vector<std::string>& ignorePatterns) {
    // Collapse the ordered rules to one decision per distinct working-tree
    // subtree, resolved later-wins: the last rule naming a subtree decides
    // whether it is tracked (include) or carved out (exclude). First-seen order
    // is kept for deterministic emission. An empty subtree is a whole-repo
    // include, handled separately below.
    std::vector<Boundary> boundaries;
    bool wholeRepoMapped = false;
    for (const auto& rule : rules) {
        if (rule.repoSubtree.empty()) {
            if (!rule.exclude) wholeRepoMapped = true;
            continue;
        }
        auto it = std::find_if(boundaries.begin(), boundaries.end(),
                               [&](const Boundary& b) {
                                   return b.subtree == rule.repoSubtree;
                               });
        if (it == boundaries.end()) {
            boundaries.push_back({rule.repoSubtree,
                                  pathComponents(rule.repoSubtree),
                                  !rule.exclude});
        } else {
            it->tracked = !rule.exclude;  // later rule wins
        }
    }

    // Re-excludes the plain carved-out subtrees (an `exclude` with no deeper
    // re-include), e.g. "/src/thirdparty/". A carve-out that *does* have a
    // re-included descendant is emitted as an intermediate in the allowlist
    // body instead (see below), so it is skipped here.
    auto appendExclusions = [&](std::string& out) {
        std::vector<std::string> plain;
        for (const auto& b : boundaries) {
            if (b.tracked) continue;
            const bool hasReinclude =
                std::any_of(boundaries.begin(), boundaries.end(),
                            [&](const Boundary& o) {
                                return o.tracked &&
                                       isStrictAncestor(b.comps, o.comps);
                            });
            if (!hasReinclude) plain.push_back(b.subtree);
        }
        if (plain.empty()) return;
        out += "\n# Directories under a mapped subtree that are carved out of "
               "the mirror\n# (an 'exclude' line): they sync in place / are "
               "unsynced, like unmapped\n# depot content, so Git ignores "
               "them.\n";
        for (const auto& sub : plain) out += "/" + sub + "/\n";
    };

    // Extra ignore patterns from p4gw.cfg `ignore` lines, appended verbatim.
    // These are files P4 ignores (build output, IDE state) that would otherwise
    // be tracked under a mapped subtree; they must come after the allowlist's
    // re-includes to take effect, so they go last.
    auto appendExtra = [&](std::string& out) {
        if (ignorePatterns.empty()) return;
        out += "\n# Extra ignore patterns (p4gw.cfg 'ignore' lines): files P4\n"
               "# ignores that would otherwise be tracked under a mapped "
               "subtree.\n";
        for (const auto& p : ignorePatterns) out += p + "\n";
    };

    // A whole-repo include leaves nothing unmapped to hide, so an allowlist
    // would only ignore the repo's own content. Fall back to a plain denylist
    // of the gw-managed paths (personal config + the mirror container), plus
    // any carved-out directories.
    const bool anyTracked =
        std::any_of(boundaries.begin(), boundaries.end(),
                    [](const Boundary& b) { return b.tracked; });
    if (wholeRepoMapped || !anyTracked) {
        std::string out = kGwDenylist;
        appendExclusions(out);
        appendExtra(out);
        return out;
    }

    // Keep each tracked subtree unless a *tracked* boundary already contains it
    // (its ancestor tracks it whole, so a redundant child line would only force
    // a `/dir/*` that re-excludes the rest). A tracked subtree whose nearest
    // enclosing boundary is an *exclude* is a genuine re-include and is kept, so
    // its ancestor chain re-opens a path back into a carved-out directory.
    auto nearestBoundary = [&](const std::vector<std::string>& comps)
        -> const Boundary* {
        const Boundary* best = nullptr;
        for (const auto& b : boundaries) {
            if (!isStrictAncestor(b.comps, comps)) continue;
            if (best == nullptr || b.comps.size() > best->comps.size()) {
                best = &b;
            }
        }
        return best;
    };
    std::vector<std::vector<std::string>> kept;
    for (const auto& b : boundaries) {
        if (!b.tracked) continue;
        const Boundary* anc = nearestBoundary(b.comps);
        if (anc != nullptr && anc->tracked) continue;  // already covered
        kept.push_back(b.comps);
    }

    // Every directory that must appear in the file: each kept subtree plus all
    // of its ancestors. A directory is a "leaf" when it is exactly a tracked
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
        "# gw's own tracked metadata, re-included so the root '/*' does not\n"
        "# swallow it (.gitattributes pins line endings - see 'gw init').\n"
        "!/.gitignore\n"
        "!/.gitattributes\n";

    // Emit by depth: at each level re-include the needed directories, then
    // re-exclude the children of any intermediate one, so the next (deeper)
    // level's re-includes show only the mapped descendants through. An
    // intermediate is either an ancestor of a tracked leaf or a carved-out
    // directory that has a re-included descendant; both need `/dir/*`.
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
    // `/src/thirdparty/` line then carves the (plain) excluded directories back
    // out. Git applies the patterns in order, so these must come last.
    appendExclusions(out);
    appendExtra(out);
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
            ViewRule rule;
            rule.exclude = false;
            rule.depotPath = tokens[0];
            rule.mirrorPath = tokens[1];
            rule.repoSubtree = mirrorRepoSubtree(tokens[1]);
            if (!rule.depotPath.ends_with("/...")) {
                return std::unexpected(
                    where + ": depot path '" + rule.depotPath +
                    "' must end with '/...'");
            }
            for (const auto& existing : config.rules) {
                if (existing.exclude) continue;
                if (existing.depotPath == rule.depotPath) {
                    return std::unexpected(where + ": depot path '" +
                                           rule.depotPath +
                                           "' is mapped twice");
                }
                if (existing.mirrorPath == rule.mirrorPath) {
                    return std::unexpected(where + ": mirror path '" +
                                           rule.mirrorPath +
                                           "' is used by two includes");
                }
            }
            config.rules.push_back(std::move(rule));
        } else if (key == "exclude") {
            // An `exclude` carves a depot subtree out of an earlier `include`.
            // Rules are ordered and resolved later-wins (like a p4 view), so an
            // exclude may appear in any position and binds to the *enclosing*
            // include - the last prior include whose depot path strictly
            // contains it. gw gitignores the carve-out and ships nothing
            // through it (the client view drops it or syncs it in place).
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
            std::string subtree;
            for (auto it = config.rules.rbegin(); it != config.rules.rend();
                 ++it) {
                if (it->exclude) continue;
                subtree = excludedRepoSubtree(it->depotPath, it->repoSubtree,
                                              excludePath);
                if (!subtree.empty()) break;
            }
            if (subtree.empty()) {
                return std::unexpected(
                    where + ": exclude path '" + excludePath +
                    "' is not strictly under any preceding 'include' depot "
                    "path");
            }
            for (const auto& existing : config.rules) {
                if (existing.exclude && existing.depotPath == excludePath) {
                    return std::unexpected(where + ": exclude path '" +
                                           excludePath + "' is listed twice");
                }
            }
            ViewRule rule;
            rule.exclude = true;
            rule.depotPath = excludePath;
            rule.repoSubtree = subtree;
            config.rules.push_back(std::move(rule));
        } else if (key == "client") {
            config.client = value;
        } else if (key == "baseline_branch") {
            config.baselineBranch = value;
        } else if (key == "import_mode") {
            if (value == "checkout") {
                config.importMode = ImportMode::kCheckout;
            } else if (value == "worktree") {
                config.importMode = ImportMode::kWorktree;
            } else {
                return std::unexpected(
                    where + ": import_mode must be 'checkout' or 'worktree', "
                    "got '" + value + "'");
            }
        } else if (key == "ignore") {
            // A verbatim gitignore pattern, taken as-is (not tokenized) so globs
            // like "/src/**/*.pdb" survive intact. buildGitignore appends it
            // after the allowlist. Silently skip exact duplicates.
            if (value.empty()) {
                return std::unexpected(where +
                                       ": 'ignore' takes a gitignore pattern");
            }
            if (std::find(config.ignorePatterns.begin(),
                          config.ignorePatterns.end(),
                          value) == config.ignorePatterns.end()) {
                config.ignorePatterns.push_back(value);
            }
        } else {
            return std::unexpected(where + ": unknown key '" + key + "'");
        }
    }

    if (includeRules(config.rules).empty()) {
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

std::string buildGitattributes() {
    return
        "# gw stores line endings verbatim - P4 is the source of truth.\n"
        "#\n"
        "# The mirror is a P4-owned subtree; gw copies its bytes into the working\n"
        "# tree exactly as P4 synced them (the client LineEnd decides - CRLF on\n"
        "# Windows). '-text' tells git to store those bytes unchanged and never\n"
        "# guess text-vs-binary or translate CRLF<->LF, so the blob is byte-for-\n"
        "# byte what P4 has. That keeps git and P4 from ever disagreeing about a\n"
        "# file's contents, and - because every commit stores the same bytes -\n"
        "# stops the CRLF/LF conflicts you get when line-ending handling is left\n"
        "# to each machine's core.autocrlf.\n"
        "#\n"
        "# This assumes everyone's P4 client uses the same LineEnd; it is the\n"
        "# right choice for an all-Windows (CRLF) team. A mixed CRLF/LF team\n"
        "# would instead want '* text=auto', which normalizes every blob to LF\n"
        "# regardless of client.\n"
        "* -text\n";
}

bool gitattributesPinsEol(const std::string& content) {
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        // First whitespace-separated token is the path pattern; the rest are
        // attributes. We only care about a catch-all '*' pattern.
        std::istringstream tokens(trimmed);
        std::string pattern;
        tokens >> pattern;
        if (pattern != "*") continue;

        std::string attr;
        while (tokens >> attr) {
            if (attr == "text" || attr == "-text" ||
                attr.starts_with("text=") || attr.starts_with("eol=")) {
                return true;
            }
        }
    }
    return false;
}

std::string depotTrackingRef(const Config& config) {
    return "refs/p4gw/" + config.baselineBranch;
}

}  // namespace p4gw
