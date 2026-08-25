// SPDX-License-Identifier: MIT

#include "config.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

#include "mirror.h"

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

// "//depot/x/..." (or the single-level "//depot/x/*") -> "//depot/x/" so a
// prefix test is anchored at a path boundary ("//d/src/" must not match
// "//d/srclib/"). The recursive-vs-single-level distinction is carried
// separately (ViewRule::scope), not by the stripped base. A path with no
// wildcard (a single-file rule's depot path) is returned unchanged - use
// `depotBaseOf` when the *containing directory* is what's wanted.
std::string stripDepotWildcard(const std::string& path) {
    if (path.ends_with("...")) return path.substr(0, path.size() - 3);
    if (path.ends_with("*")) return path.substr(0, path.size() - 1);
    return path;
}

// Whether a rule's governed path `base` (a directory for kRecursive /
// kDirectFiles, the path itself for kSingleFile) covers `path`. Directory
// bases arrive with a trailing '/' so prefix tests stay anchored at a path
// boundary.
bool scopeCovers(ViewScope scope, const std::string& base,
                 const std::string& path) {
    if (scope == ViewScope::kSingleFile) return path == base;
    if (!path.starts_with(base)) return false;
    if (scope == ViewScope::kRecursive) return true;
    return path.find('/', base.size()) == std::string::npos;
}

// Joins a mapping base with a path relative to it, tolerating an empty base
// (a whole-repo include, whose subtree is the repo root).
std::string joinSubpath(const std::string& base, const std::string& rel) {
    if (base.empty()) return rel;
    if (rel.empty()) return base;
    return base + "/" + rel;
}

}  // namespace

std::string depotBaseOf(const ViewRule& rule) {
    if (rule.scope != ViewScope::kSingleFile) {
        return stripDepotWildcard(rule.depotPath);  // already ends with '/'
    }
    const auto slash = rule.depotPath.rfind('/');
    return slash == std::string::npos ? std::string{}
                                      : rule.depotPath.substr(0, slash + 1);
}

std::string mappedMirrorPath(const ViewRule& rule) {
    return joinSubpath(rule.mirrorPath, rule.fileName);
}

std::string mappedRepoPath(const ViewRule& rule) {
    return joinSubpath(rule.repoSubtree, rule.fileName);
}

std::string mirrorSpecOf(const ViewRule& rule) {
    switch (rule.scope) {
        case ViewScope::kRecursive:
            return joinSubpath(rule.mirrorPath, "...");
        case ViewScope::kDirectFiles:
            return joinSubpath(rule.mirrorPath, "*");
        case ViewScope::kSingleFile:
            break;
    }
    return mappedMirrorPath(rule);
}

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
        // A single-file rule's base is the file itself (an exact match); a
        // subtree rule's is the wildcard-stripped directory.
        const std::string base = rule.scope == ViewScope::kSingleFile
                                     ? rule.depotPath
                                     : stripDepotWildcard(rule.depotPath);
        if (scopeCovers(rule.scope, base, depotFile)) {
            effective = &rule;  // later declaration wins
        }
    }
    return effective;
}

const ViewRule* effectiveRuleForRepo(const std::vector<ViewRule>& rules,
                                     const std::string& repoRel) {
    const ViewRule* effective = nullptr;
    for (const auto& rule : rules) {
        const std::string sub = mappedRepoPath(rule);
        bool matches;
        if (sub.empty()) {
            matches = true;  // whole-repo include (always recursive)
        } else if (repoRel == sub) {
            // The governed path itself: the file for a single-file rule, the
            // subtree's own name for a directory one.
            matches = true;
        } else if (rule.scope == ViewScope::kSingleFile) {
            matches = false;  // one path and nothing below it
        } else if (repoRel.starts_with(sub + "/")) {
            // A single-level rule covers only direct children: nothing may
            // follow the component after the subtree prefix.
            matches = rule.scope == ViewScope::kRecursive ||
                      repoRel.find('/', sub.size() + 1) == std::string::npos;
        } else {
            matches = false;
        }
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

// A working-tree path named in the config and whether it is (last-wins)
// tracked (an include) or carved out (an exclude).
struct Boundary {
    std::string subtree;             // forward slashes, no trailing slash
    std::vector<std::string> comps;  // subtree split into components
    bool tracked;                    // include (true) vs exclude (false)
    ViewScope scope;                 // how much of it the rule maps
};

// A path the allowlist must name: a kept tracked subtree ("leaf", re-included
// whole) or one of its ancestors ("intermediate", re-included but with its
// other children re-excluded via `/dir/*`). A single-file include is a leaf
// too, flagged so its line is written without a trailing slash.
struct LayoutDir {
    std::vector<std::string> components;
    bool isLeaf;
    bool isFile = false;
};

// A carved-out path the allowlist must re-exclude, and whether it names a file
// (a single-file `exclude`) rather than a directory.
struct Carveout {
    std::string subtree;
    bool isFile;
};

// The shape both allowlist-style files are generated from: the .gitignore
// emits it as ignore/re-include lines and the .rgignore managed block as the
// inverse reopen lines, so the two stay in lockstep by construction.
struct AllowlistLayout {
    bool wholeRepoMapped = false;
    bool anyTracked = false;
    // Carved-out paths with no re-included descendant, first-seen order.
    // (A carve-out that has one shows up as an intermediate in `dirs`.) A
    // single-file `exclude` is one of these too, flagged so its line is written
    // without a trailing slash.
    std::vector<Carveout> plainCarveouts;
    // Tracked subtrees mapped single-level (`/*`): their own files are kept
    // (by the subtree's re-include), but their sub-directories must be
    // re-excluded with a `/sub/*/` line. First-seen order.
    std::vector<std::string> singleLevelCarveouts;
    // Every directory the allowlist names, first-seen order.
    std::vector<LayoutDir> dirs;
};

AllowlistLayout computeAllowlistLayout(const std::vector<ViewRule>& rules) {
    AllowlistLayout layout;

    // Collapse the ordered rules to one decision per distinct working-tree
    // subtree, resolved later-wins: the last rule naming a subtree decides
    // whether it is tracked (include) or carved out (exclude). First-seen order
    // is kept for deterministic emission. An empty subtree is a whole-repo
    // include, handled separately by the emitters.
    std::vector<Boundary> boundaries;
    for (const auto& rule : rules) {
        // The path the rule governs, which for a single-file include is the
        // file itself - never empty, so only a subtree include can be the
        // whole-repo mapping.
        const std::string governed = mappedRepoPath(rule);
        if (governed.empty()) {
            if (!rule.exclude) layout.wholeRepoMapped = true;
            continue;
        }
        auto it = std::find_if(boundaries.begin(), boundaries.end(),
                               [&](const Boundary& b) {
                                   return b.subtree == governed;
                               });
        if (it == boundaries.end()) {
            boundaries.push_back({governed, pathComponents(governed),
                                  !rule.exclude, rule.scope});
        } else {
            it->tracked = !rule.exclude;  // later rule wins
            it->scope = rule.scope;       // ... and carries its wildcard
        }
    }

    layout.anyTracked =
        std::any_of(boundaries.begin(), boundaries.end(),
                    [](const Boundary& b) { return b.tracked; });

    for (const auto& b : boundaries) {
        if (b.tracked) continue;
        const bool hasReinclude =
            std::any_of(boundaries.begin(), boundaries.end(),
                        [&](const Boundary& o) {
                            return o.tracked &&
                                   isStrictAncestor(b.comps, o.comps);
                        });
        if (!hasReinclude) {
            layout.plainCarveouts.push_back(
                {b.subtree, b.scope == ViewScope::kSingleFile});
        }
    }

    // A tracked single-level subtree keeps its own files but re-excludes its
    // sub-directories (`/sub/*/`), so it is neither a plain carve-out nor a
    // whole re-include.
    for (const auto& b : boundaries) {
        if (b.tracked && b.scope == ViewScope::kDirectFiles) {
            layout.singleLevelCarveouts.push_back(b.subtree);
        }
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
    struct KeptLeaf {
        std::vector<std::string> comps;
        bool isFile;
    };
    std::vector<KeptLeaf> kept;
    for (const auto& b : boundaries) {
        if (!b.tracked) continue;
        const Boundary* anc = nearestBoundary(b.comps);
        if (anc != nullptr && anc->tracked) continue;  // already covered
        kept.push_back({b.comps, b.scope == ViewScope::kSingleFile});
    }

    // Every directory that must appear: each kept subtree plus all of its
    // ancestors. A directory is a "leaf" when it is exactly a tracked subtree
    // (tracked whole); ancestors are intermediate. First-seen order.
    auto findDir = [&](const std::vector<std::string>& c) -> LayoutDir* {
        for (auto& d : layout.dirs)
            if (d.components == c) return &d;
        return nullptr;
    };
    for (const auto& leaf : kept) {
        std::vector<std::string> prefix;
        for (size_t i = 0; i < leaf.comps.size(); ++i) {
            prefix.push_back(leaf.comps[i]);
            const bool isLeaf = (i + 1 == leaf.comps.size());
            if (LayoutDir* existing = findDir(prefix)) {
                existing->isLeaf = existing->isLeaf || isLeaf;
                existing->isFile = existing->isFile || (isLeaf && leaf.isFile);
            } else {
                layout.dirs.push_back({prefix, isLeaf, isLeaf && leaf.isFile});
            }
        }
    }
    return layout;
}

std::string joinComponents(const std::vector<std::string>& c) {
    std::string s;
    for (const auto& part : c) {
        s += '/';
        s += part;
    }
    return s;  // leading slash, no trailing slash, e.g. "/a/b"
}

// The re-include line for one layout entry: "!/dir/" for a directory, and
// "!/dir/file.txt" (no trailing slash, which would only match a directory) for
// a single-file include's leaf.
std::string reincludeLine(const LayoutDir& d) {
    return "!" + joinComponents(d.components) + (d.isFile ? "" : "/");
}

// The re-exclusion line for one plain carve-out - the mirror image:
// "/src/thirdparty/" for a carved-out directory, "/src/notes.txt" for a
// single-file `exclude`.
std::string carveoutLine(const Carveout& c) {
    return "/" + c.subtree + (c.isFile ? "" : "/");
}

// The allowlist body's re-include / child-re-exclude lines, in depth order: a
// "!/dir/" re-include for every directory the layout names, and a "/dir/*"
// child re-exclusion for each intermediate one (so the next deeper re-include
// shows only the mapped descendant through). The plain carve-out re-exclusions
// are emitted separately by the caller. No trailing newline on each entry.
std::vector<std::string> trackingLinesFromLayout(const AllowlistLayout& layout) {
    std::vector<std::string> lines;
    size_t maxDepth = 0;
    for (const auto& d : layout.dirs)
        maxDepth = std::max(maxDepth, d.components.size());
    for (size_t depth = 1; depth <= maxDepth; ++depth) {
        for (const auto& d : layout.dirs)
            if (d.components.size() == depth) lines.push_back(reincludeLine(d));
        for (const auto& d : layout.dirs)
            if (d.components.size() == depth && !d.isLeaf)
                lines.push_back(joinComponents(d.components) + "/*");
    }
    return lines;
}

// One line of the allowlist body, paired with the directory it names. The
// directory is what orders the lines against each other: Git resolves a path by
// the last pattern that matches it, so a line only has to follow the lines of
// its own ancestors ("/a/*" after "!/a/", "!/a/b/" after "/a/*"). Lines naming
// unrelated subtrees may sit in any order.
struct AllowlistLine {
    std::string line;
    std::vector<std::string> dir;
};

// Whether `comps` is `other` or one of its ancestors - the only relation that
// constrains two allowlist lines' relative order.
bool isAncestorOrSame(const std::vector<std::string>& comps,
                      const std::vector<std::string>& other) {
    if (comps.size() > other.size()) return false;
    return std::equal(comps.begin(), comps.end(), other.begin());
}

// The allowlist body's *load-bearing* lines: the ones whose absence changes
// what Git tracks, in the order buildGitignore emits them.
//
// A "!/dir/" re-include is load-bearing only when the directory would otherwise
// be excluded - by the root "/*" for a depth-1 subtree, or by an intermediate
// ancestor's "/parent/*". When the nearest tracked ancestor is tracked *whole*
// (a leaf, so no "/parent/*"), its own "!/anc/" already tracks the descendant
// and the deeper re-include is redundant; a hand-minimized .gitignore that
// drops it is still correct, so it must not be demanded back.
//
// Every other line here is load-bearing as written: a "/dir/*" is what keeps a
// re-included intermediate from handing Git the unmapped depot content beside
// the mapped subtree, "/sub/*/" does the same for a single-level mapping's
// child directories, and "/sub/" carves out an `exclude`. buildGitignore emits
// this set plus the redundant re-includes, so a generated file satisfies it.
std::vector<AllowlistLine> requiredAllowlistLines(const AllowlistLayout& layout) {
    auto emitsChildExclude = [&](const std::vector<std::string>& comps) {
        for (const auto& d : layout.dirs)
            if (d.components == comps) return !d.isLeaf;
        return false;  // not a named directory: no "/comps/*" line
    };
    size_t maxDepth = 0;
    for (const auto& d : layout.dirs)
        maxDepth = std::max(maxDepth, d.components.size());
    std::vector<AllowlistLine> lines;
    for (size_t depth = 1; depth <= maxDepth; ++depth) {
        for (const auto& d : layout.dirs) {
            if (d.components.size() != depth) continue;
            const std::vector<std::string> parent(d.components.begin(),
                                                  d.components.end() - 1);
            // Depth-1 dirs sit directly under the root "/*"; deeper ones need a
            // re-include only when their parent re-excludes them.
            if (parent.empty() || emitsChildExclude(parent)) {
                lines.push_back({reincludeLine(d), d.components});
            }
        }
        for (const auto& d : layout.dirs) {
            if (d.components.size() == depth && !d.isLeaf) {
                lines.push_back(
                    {joinComponents(d.components) + "/*", d.components});
            }
        }
    }
    // The trailing re-exclusions, in buildGitignore's order: the plain
    // carve-outs, then a single-level mapping's child directories.
    for (const auto& c : layout.plainCarveouts) {
        lines.push_back({carveoutLine(c), pathComponents(c.subtree)});
    }
    for (const auto& sub : layout.singleLevelCarveouts) {
        lines.push_back({"/" + sub + "/*/", pathComponents(sub)});
    }
    return lines;
}

// Just the re-include lines of the above - the subset that decides whether a
// mapped subtree is tracked at all.
std::vector<std::string> requiredTrackingLines(const AllowlistLayout& layout) {
    std::vector<std::string> lines;
    for (const auto& l : requiredAllowlistLines(layout)) {
        if (l.line.starts_with("!")) lines.push_back(l.line);
    }
    return lines;
}

// Whether `content` contains `line` as a complete line (trimmed, CR-tolerant),
// so "!/src/" is not mistaken for present when only "!/src/lib/" appears.
bool hasExactLine(const std::string& content, const std::string& line) {
    std::istringstream stream(content);
    std::string cur;
    while (std::getline(stream, cur)) {
        if (!cur.empty() && cur.back() == '\r') cur.pop_back();
        if (trim(cur) == line) return true;
    }
    return false;
}

// The index of the first line of `content` equal to `line` (trimmed,
// CR-tolerant) at or after line index `from`, or npos. Position matters
// because Git resolves a path by the *last* pattern that matches it, so a
// tracking line found only above the one that must precede it is as broken as
// one that is missing.
size_t lineIndexOf(const std::string& content, const std::string& line,
                   size_t from) {
    std::istringstream stream(content);
    std::string cur;
    size_t index = 0;
    while (std::getline(stream, cur)) {
        if (!cur.empty() && cur.back() == '\r') cur.pop_back();
        if (index >= from && trim(cur) == line) return index;
        ++index;
    }
    return std::string::npos;
}

}  // namespace

std::string buildGitignore(const std::vector<ViewRule>& rules,
                           const std::vector<std::string>& ignorePatterns) {
    const AllowlistLayout layout = computeAllowlistLayout(rules);

    // Re-excludes the plain carved-out subtrees (an `exclude` with no deeper
    // re-include), e.g. "/src/thirdparty/". A carve-out that *does* have a
    // re-included descendant is emitted as an intermediate in the allowlist
    // body instead (via `layout.dirs`), so it is not in this list.
    auto appendExclusions = [&](std::string& out) {
        if (layout.plainCarveouts.empty()) return;
        out += "\n# Paths under a mapped subtree that are carved out of "
               "the mirror\n# (an 'exclude' line): they sync in place / are "
               "unsynced, like unmapped\n# depot content, so Git ignores "
               "them.\n";
        for (const auto& c : layout.plainCarveouts) out += carveoutLine(c) + "\n";
    };

    // Re-excludes the sub-directories of a single-level (`/*`) mapped subtree,
    // keeping its own files (which the subtree's re-include still tracks). The
    // trailing-slash glob `/sub/*/` matches directories only, so `sub/file.txt`
    // stays tracked while `sub/child/` (and everything under it) is ignored.
    auto appendSingleLevel = [&](std::string& out) {
        if (layout.singleLevelCarveouts.empty()) return;
        out += "\n# Sub-directories of a single-level ('/*') mapped subtree: only "
               "the\n# directory's own files are mapped, so its child directories "
               "are ignored\n# (like unmapped depot content).\n";
        for (const auto& sub : layout.singleLevelCarveouts) {
            out += "/" + sub + "/*/\n";
        }
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
    if (layout.wholeRepoMapped || !layout.anyTracked) {
        std::string out = kGwDenylist;
        appendExclusions(out);
        appendExtra(out);
        return out;
    }

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
    for (const auto& line : trackingLinesFromLayout(layout)) out += line + "\n";
    // The allowlist re-includes each mapped subtree whole (`!/src/`); a later
    // `/src/thirdparty/` line then carves the (plain) excluded directories back
    // out, and `/src/build/*/` re-excludes a single-level subtree's children.
    // Git applies the patterns in order, so these must come last.
    appendExclusions(out);
    appendSingleLevel(out);
    appendExtra(out);
    return out;
}

std::vector<std::string> allowlistTrackingLines(
    const std::vector<ViewRule>& rules) {
    const AllowlistLayout layout = computeAllowlistLayout(rules);
    // The denylist body (whole-repo include, or nothing tracked) uses no
    // re-includes, so there is nothing to track line by line.
    if (layout.wholeRepoMapped || !layout.anyTracked) return {};
    return trackingLinesFromLayout(layout);
}

std::vector<std::string> missingAllowlistTrackingLines(
    const std::vector<ViewRule>& rules, const std::string& gitignoreContent) {
    const AllowlistLayout layout = computeAllowlistLayout(rules);
    // The denylist body (whole-repo include, or nothing tracked) uses no
    // re-includes, so there is nothing to track line by line.
    if (layout.wholeRepoMapped || !layout.anyTracked) return {};
    // Only the load-bearing re-includes matter: a redundant intermediate line
    // (e.g. "!/src/devtools/" when "!/src/" already tracks src whole) is not
    // required, so its absence must not be flagged as an untracked subtree.
    std::vector<std::string> missing;
    for (const auto& line : requiredTrackingLines(layout)) {
        if (!hasExactLine(gitignoreContent, line)) missing.push_back(line);
    }
    return missing;
}

std::vector<std::string> allowlistRepairLines(
    const std::vector<ViewRule>& rules, const std::string& gitignoreContent) {
    const AllowlistLayout layout = computeAllowlistLayout(rules);
    // The denylist body (whole-repo include, or nothing tracked) uses no
    // re-includes, so there is nothing to repair line by line.
    if (layout.wholeRepoMapped || !layout.anyTracked) return {};

    // Walk the load-bearing lines in emission order and record where each one
    // sits in the file. A line counts as present only *below* every line of its
    // own ancestors: Git takes the last match, so a "/game/*" found above the
    // "!/game/core/" it must precede is as broken as one that is missing.
    //
    // Repairs land at the end of the file, so an appended line takes effect
    // after everything already there - which in turn breaks any descendant line
    // still sitting above it. Carrying `kAppended` down the ancestor chain
    // re-appends those too, in order, so the repaired tail reads correctly: a
    // lone "/game/*" appended under an existing "!/game/core/" would otherwise
    // re-ignore the very subtree it was added to expose.
    static constexpr size_t kAppended = std::string::npos;
    const std::vector<AllowlistLine> required = requiredAllowlistLines(layout);
    std::vector<size_t> position(required.size(), kAppended);
    std::vector<std::string> repair;
    for (size_t i = 0; i < required.size(); ++i) {
        size_t from = 0;  // first line index this line may occupy
        bool ancestorAppended = false;
        for (size_t j = 0; j < i && !ancestorAppended; ++j) {
            if (!isAncestorOrSame(required[j].dir, required[i].dir)) continue;
            if (position[j] == kAppended) ancestorAppended = true;
            else from = std::max(from, position[j] + 1);
        }
        position[i] = ancestorAppended
                          ? kAppended
                          : lineIndexOf(gitignoreContent, required[i].line, from);
        if (position[i] == kAppended) repair.push_back(required[i].line);
    }
    return repair;
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
            const std::string& depot = tokens[0];
            const std::string& mirror = tokens[1];

            // The depot side names what to map, and its wildcard says how much:
            // '/...' the whole subtree, '/*' only the files directly in that
            // directory (the p4 single-level view wildcard, which pairs with a
            // recursive 'exclude' to keep a directory's own files while
            // dropping its sub-directories), and no wildcard at all a single
            // file.
            ViewRule rule;
            rule.exclude = false;
            rule.depotPath = depot;
            if (depot.ends_with("/...")) {
                rule.scope = ViewScope::kRecursive;
            } else if (depot.ends_with("/*")) {
                rule.scope = ViewScope::kDirectFiles;
            } else if (depot.ends_with("...") || depot.ends_with("*") ||
                       depot.ends_with("/") || depot.empty()) {
                // A wildcard that is not its own path component ('src...'), or
                // a bare directory with no wildcard: neither names a subtree
                // nor a file.
                return std::unexpected(
                    where + ": depot path '" + depot +
                    "' must end with '/...' (whole subtree), '/*' (direct "
                    "files only), or name a single file");
            } else {
                rule.scope = ViewScope::kSingleFile;
            }

            // The mirror side carries the same wildcard, so a config line reads
            // like the client view line it stands for. Older configs left it
            // off; those are still accepted, with the depot side's wildcard
            // inferred (and doctor recommending the explicit form).
            if (rule.scope == ViewScope::kSingleFile) {
                if (mirror.ends_with("/...") || mirror.ends_with("/*") ||
                    mirror.ends_with("/")) {
                    return std::unexpected(
                        where + ": mirror path '" + mirror +
                        "' has a directory wildcard, but depot path '" + depot +
                        "' maps a single file - end the mirror path with the "
                        "file name");
                }
                const auto depotSlash = depot.rfind('/');
                const std::string depotName = depot.substr(depotSlash + 1);
                const auto mirrorSlash = mirror.rfind('/');
                if (mirrorSlash == std::string::npos) {
                    return std::unexpected(
                        where + ": mirror path '" + mirror +
                        "' must sit under the '.p4gw' mirror container, e.g. "
                        "'.p4gw/" + depotName + "'");
                }
                rule.fileName = mirror.substr(mirrorSlash + 1);
                if (rule.fileName != depotName) {
                    return std::unexpected(
                        where + ": mirror path '" + mirror + "' renames '" +
                        depotName + "' to '" + rule.fileName +
                        "' - gw maps a file under its own name; use '" +
                        mirror.substr(0, mirrorSlash + 1) + depotName + "'");
                }
                rule.mirrorPath = mirror.substr(0, mirrorSlash);
            } else {
                const char* wildcard =
                    rule.scope == ViewScope::kRecursive ? "/..." : "/*";
                const char* other =
                    rule.scope == ViewScope::kRecursive ? "/*" : "/...";
                if (mirror.ends_with(other)) {
                    return std::unexpected(
                        where + ": depot path '" + depot + "' ends with '" +
                        wildcard + "' but mirror path '" + mirror +
                        "' ends with '" + other +
                        "' - both sides take the same wildcard");
                }
                if (mirror.ends_with(wildcard)) {
                    rule.mirrorPath =
                        mirror.substr(0, mirror.size() - std::strlen(wildcard));
                } else if (mirror.ends_with("/")) {
                    return std::unexpected(
                        where + ": mirror path '" + mirror +
                        "' must end with '" + wildcard +
                        "' to match depot path '" + depot + "'");
                } else if (mirror.ends_with("...") || mirror.ends_with("*")) {
                    // A wildcard that is not its own path component would be
                    // silently taken as part of the directory name.
                    return std::unexpected(
                        where + ": mirror path '" + mirror +
                        "' ends with a wildcard that is not its own path "
                        "component - write '" + stripDepotWildcard(mirror) +
                        wildcard + "'");
                } else {
                    rule.mirrorPath = mirror;
                    rule.mirrorWildcardImplied = true;
                }
                if (rule.mirrorPath.empty()) {
                    return std::unexpected(where + ": mirror path '" + mirror +
                                           "' names no mirror directory");
                }
            }
            rule.repoSubtree = mirrorRepoSubtree(rule.mirrorPath);

            const std::string mapped = mappedMirrorPath(rule);
            for (const auto& existing : config.rules) {
                if (existing.exclude) continue;
                if (existing.depotPath == rule.depotPath) {
                    return std::unexpected(where + ": depot path '" +
                                           rule.depotPath +
                                           "' is mapped twice");
                }
                if (mappedMirrorPath(existing) == mapped) {
                    return std::unexpected(where + ": mirror path '" + mapped +
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
            // A subtree carve-out is always recursive ('/...'); a wildcard-less
            // path carves out that one file. '/*' is neither.
            ViewScope excludeScope = ViewScope::kRecursive;
            if (excludePath.ends_with("/...")) {
                excludeScope = ViewScope::kRecursive;
            } else if (excludePath.ends_with("/*")) {
                return std::unexpected(
                    where + ": exclude path '" + excludePath +
                    "' cannot use '/*'; a subtree exclude is always recursive - "
                    "end it with '/...', or name a single file to carve out "
                    "just that file");
            } else if (excludePath.ends_with("...") ||
                       excludePath.ends_with("*") ||
                       excludePath.ends_with("/") || excludePath.empty()) {
                return std::unexpected(
                    where + ": exclude path '" + excludePath +
                    "' must end with '/...' (whole subtree) or name a single "
                    "file");
            } else {
                excludeScope = ViewScope::kSingleFile;
            }
            std::string subtree;
            for (auto it = config.rules.rbegin(); it != config.rules.rend();
                 ++it) {
                // A single-file include maps one file, so nothing can be
                // carved out of it; only subtree includes can enclose.
                if (it->exclude || it->scope == ViewScope::kSingleFile) continue;
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
            rule.scope = excludeScope;
            // `subtree` is the whole carved-out path relative to the repo. For
            // a single-file exclude that path *is* the file, so split its name
            // off to keep `repoSubtree` a directory, exactly as an `include`
            // carries it (mappedRepoPath joins the two back).
            if (excludeScope == ViewScope::kSingleFile) {
                const auto slash = subtree.rfind('/');
                if (slash == std::string::npos) {
                    rule.fileName = subtree;
                    rule.repoSubtree.clear();
                } else {
                    rule.fileName = subtree.substr(slash + 1);
                    rule.repoSubtree = subtree.substr(0, slash);
                }
            } else {
                rule.repoSubtree = subtree;
            }
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
        } else if (key == "rgignore") {
            if (value == "managed") {
                config.manageRgignore = true;
            } else if (value == "off") {
                config.manageRgignore = false;
            } else {
                return std::unexpected(
                    where + ": rgignore must be 'managed' or 'off', got '" +
                    value + "'");
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
                               "include = //depot/yourproject/src/... "
                               ".p4gw/src/...");
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

bool gitignoreIsAllowlist(const std::string& content) {
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // The allowlist's signature is the bare '/*' root-ignore line; a
        // trimmed '/*' cannot be a comment, so no comment handling needed.
        if (trim(line) == "/*") return true;
    }
    return false;
}

bool ripgrepConfigDisablesVcsIgnore(const std::string& content) {
    // ripgrep's config format: one argument per line; blank lines and lines
    // whose first non-whitespace byte is '#' are skipped. Later flags win on
    // the assembled command line, so scan in order and keep the last word.
    bool disables = false;
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string arg = trim(line);
        if (arg.empty() || arg[0] == '#') continue;
        if (arg == "--no-ignore-vcs" || arg == "--no-ignore" ||
            arg == "--unrestricted" || arg == "-u" || arg == "-uu" ||
            arg == "-uuu") {
            disables = true;
        } else if (arg == "--ignore-vcs" || arg == "--ignore") {
            disables = false;
        }
    }
    return disables;
}

std::string buildRgignoreSection(const std::vector<ViewRule>& rules,
                                 const std::vector<std::string>& ignorePatterns,
                                 const std::string& dotIgnoreBody) {
    const AllowlistLayout layout = computeAllowlistLayout(rules);
    // The denylist .gitignore (whole-repo include) hides nothing rg should
    // see, so there is no block to maintain.
    if (layout.wholeRepoMapped || !layout.anyTracked) return {};

    // The reopens use '[!.]*' rather than '*' so dot entries are never
    // whitelisted: a whitelist overrides ripgrep's hidden filter, and a plain
    // '!/*' was measured to expose .git and the .p4gw mirror (double hits on
    // the whole depot).
    std::string out =
        "# ripgrep reads .gitignore, so the allowlist hides unmapped depot\n"
        "# content synced in place (bin/, content/) from every search. These\n"
        "# reopen what the allowlist hides - searches see the tree a plain P4\n"
        "# user's would - while '[!.]*' keeps dot entries (.git, the .p4gw\n"
        "# mirror) hidden.\n"
        "!/[!.]*\n";
    for (const auto& d : layout.dirs) {
        if (!d.isLeaf) out += "!" + joinComponents(d.components) + "/[!.]*\n";
    }
    for (const auto& c : layout.plainCarveouts) out += "!" + carveoutLine(c) + "\n";

    // The reopens above outrank every ignore file per path, including the
    // denylists, so anything that must stay hidden from searches is repeated
    // here - later lines in the same file win again.
    if (!ignorePatterns.empty()) {
        out += "# p4gw.cfg 'ignore' patterns, re-asserted (the reopens above\n"
               "# would override them)\n";
        for (const auto& p : ignorePatterns) out += p + "\n";
    }
    bool wroteIgnoreHeader = false;
    std::istringstream lines(dotIgnoreBody);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string pattern = trim(line);
        if (pattern.empty() || pattern[0] == '#') continue;
        if (!wroteIgnoreHeader) {
            out += "# .ignore patterns, re-asserted (the reopens above would\n"
                   "# override their root-level lines)\n";
            wroteIgnoreHeader = true;
        }
        out += pattern + "\n";
    }
    return out;
}

std::expected<std::string, std::string> upsertRgignore(
    const std::string& existing, const std::string& sectionBody) {
    const std::string block =
        sectionBody.empty()
            ? std::string()
            : std::string(kRgignoreBeginMarker) + "\n" + sectionBody +
                  kRgignoreEndMarker + "\n";

    // Find a marker only at the start of a line, so a copy of the marker text
    // embedded elsewhere (say, quoted in a comment) is not mistaken for it.
    auto findLine = [&](const std::string& marker, size_t from) {
        size_t pos = existing.find(marker, from);
        while (pos != std::string::npos && pos != 0 &&
               existing[pos - 1] != '\n') {
            pos = existing.find(marker, pos + 1);
        }
        return pos;
    };

    const size_t begin = findLine(kRgignoreBeginMarker, 0);
    if (begin == std::string::npos) {
        if (block.empty()) return existing;
        if (existing.empty()) return block;
        // Prepend: hand-written rules after the block keep the last word,
        // since later lines in the same ignore file win.
        return block + "\n" + existing;
    }
    size_t end = findLine(kRgignoreEndMarker, begin);
    if (end == std::string::npos) {
        return std::unexpected(
            std::string("found the begin marker ('") + kRgignoreBeginMarker +
            "') but no end marker - rewriting through it could eat "
            "hand-written rules; restore the end marker or delete the block");
    }
    end += std::string(kRgignoreEndMarker).size();
    if (end < existing.size() && existing[end] == '\n') ++end;
    return existing.substr(0, begin) + block + existing.substr(end);
}

std::expected<bool, std::string> refreshRgignore(const Config& config,
                                                 const std::string& root) {
    if (!config.manageRgignore) return false;

    auto readAll = [](const fs::path& p) -> std::string {
        std::ifstream in(p, std::ios::binary);
        if (!in) return {};
        std::ostringstream text;
        text << in.rdbuf();
        return std::move(text).str();
    };

    // The block only applies against the allowlist style; check the repo's
    // real .gitignore, not the config shape - a hand-kept denylist means the
    // root reopen would wrongly override the user's own root-level ignores.
    std::string body;
    const fs::path gitignorePath = fs::path(root) / ".gitignore";
    if (fs::exists(gitignorePath) &&
        gitignoreIsAllowlist(readAll(gitignorePath))) {
        body = buildRgignoreSection(config.rules, config.ignorePatterns,
                                    readAll(fs::path(root) / ".ignore"));
    }

    const fs::path rgignorePath = fs::path(root) / ".rgignore";
    const bool exists = fs::exists(rgignorePath);
    const std::string existing = exists ? readAll(rgignorePath) : "";
    if (body.empty() && !exists) return false;

    auto updated = upsertRgignore(existing, body);
    if (!updated) {
        return std::unexpected(rgignorePath.string() + ": " + updated.error());
    }
    if (*updated == existing) return false;

    std::ofstream out(rgignorePath, std::ios::binary | std::ios::trunc);
    out << *updated;
    out.close();
    if (!out) {
        return std::unexpected("cannot write " + rgignorePath.string());
    }
    return true;
}

bool rgignoreReopensRoot(const std::string& content) {
    if (content.find(kRgignoreBeginMarker) != std::string::npos) return true;
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string pattern = trim(line);
        if (pattern == "!/*" || pattern == "!/[!.]*") return true;
    }
    return false;
}

std::vector<UnmanagedFile> classifyUnmanaged(
    const std::vector<ViewRule>& rules,
    const std::vector<std::string>& trackedFiles,
    const std::vector<std::string>& baselineFiles) {
    const std::unordered_set<std::string> inBaseline(baselineFiles.begin(),
                                                     baselineFiles.end());
    std::vector<UnmanagedFile> unmanaged;
    for (const auto& path : trackedFiles) {
        UnmanagedFile file;
        file.path = path;
        // gw's own files are checked first: a whole-repo include would
        // otherwise claim them as mapped, and they are Git-only by definition.
        if (mirror::isGwMetadataPath(path)) {
            file.kind = UnmanagedKind::GwMetadata;
        } else {
            const ViewRule* effective = effectiveRuleForRepo(rules, path);
            if (effective != nullptr && !effective->exclude) continue;  // mapped
            if (effective == nullptr) {
                file.kind = UnmanagedKind::Unmapped;
            } else {
                file.kind = UnmanagedKind::Excluded;
                file.excludedBy = effective->depotPath;
            }
        }
        file.inBaseline = file.kind != UnmanagedKind::GwMetadata &&
                          inBaseline.contains(path);
        unmanaged.push_back(std::move(file));
    }
    return unmanaged;
}

std::vector<UnmanagedFile> orphanedFiles(
    const std::vector<UnmanagedFile>& files) {
    std::vector<UnmanagedFile> orphans;
    for (const auto& file : files) {
        if (file.inBaseline) orphans.push_back(file);
    }
    return orphans;
}

PruneCheck checkPrune(const std::vector<ViewRule>& rules,
                      const std::vector<std::string>& deleted,
                      const std::vector<std::string>& otherwiseChanged) {
    PruneCheck check;
    check.notDeletes = otherwiseChanged;
    for (const auto& path : deleted) {
        if (mirror::isGwMetadataPath(path)) {
            check.metadata.push_back(path);
            continue;
        }
        const ViewRule* effective = effectiveRuleForRepo(rules, path);
        if (effective != nullptr && !effective->exclude) {
            check.managed.push_back(path);
            continue;
        }
        check.deletes.push_back(path);
    }
    return check;
}

bool pruneAllowed(const PruneCheck& check) {
    return !check.deletes.empty() && check.notDeletes.empty() &&
           check.managed.empty() && check.metadata.empty();
}

std::string depotTrackingRef(const Config& config) {
    return "refs/p4gw/" + config.baselineBranch;
}

}  // namespace p4gw