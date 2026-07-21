// SPDX-License-Identifier: MIT

#pragma once

#include <expected>
#include <string>
#include <vector>

namespace p4gw {

// One line of the p4gw.cfg view - an `include` that maps a depot subtree into
// the mirror, or an `exclude` that carves one back out. The list mirrors a p4
// client view: rules are kept in declaration order and resolved *later-wins*
// per path (see effectiveRuleFor*), so an `include` after an `exclude` can map
// a deeper subtree back in (the win64/linux re-include pattern) exactly the way
// a later p4 view line overrides an earlier one.
struct ViewRule {
    // false = `include` (maps depotPath into the mirror); true = `exclude`
    // (carves depotPath out - the client view drops it or syncs it in place,
    // and gw gitignores it and ships nothing through it).
    bool exclude = false;

    // Depot path of the subtree, e.g. "//depot/yourproject/src/...". Ends with
    // "/...". Every p4 operation is scoped to the include rules' depot paths so
    // we never touch (or crawl) the rest of the workspace.
    std::string depotPath;

    // Directory the client view remaps `depotPath` into - p4's staging area,
    // which p4 syncs and gw reads/writes. Always lives under the repo's single
    // `.p4gw` container; relative values resolve against the directory holding
    // the p4gw.cfg file. Example: ".p4gw/src". Empty for an `exclude`.
    std::string mirrorPath;

    // Working-tree directory this rule governs (forward slashes, no trailing
    // slash). For an include it is `mirrorPath` with its leading `.p4gw`
    // container component dropped (".p4gw/src" -> "src", ".p4gw" -> "" i.e. the
    // whole repo). For an exclude it is the carved-out subtree relative to the
    // enclosing include (an exclude of "//d/src/lib/..." under a "src" include
    // -> "src/lib").
    std::string repoSubtree;
};

// How `gw import` builds the depot snapshot.
//   kCheckout: detach the user's own checkout onto the baseline, overlay the
//     mirror there, commit, switch back (the original behavior). Requires a
//     clean working tree.
//   kWorktree: build the snapshot in a hidden git worktree pinned to
//     refs/p4gw/<baseline>, so the user's checkout is never touched and import
//     works even with a dirty tree (the branch fast-forward/rebase half is
//     skipped when dirty). This is the default; `import_mode = checkout` in
//     p4gw.cfg opts back into the original behavior.
enum class ImportMode { kCheckout, kWorktree };

// Project configuration, loaded from a `p4gw.cfg` file at the root of the Git
// overlay repo. Simple `key = value` lines; `#` starts a comment.
struct Config {
    // The view rules (include/exclude), in declaration order. At least one
    // `include` is required (a config with none fails to load).
    std::vector<ViewRule> rules;

    // P4 client (workspace) name. Empty means use the ambient P4CLIENT.
    std::string client;

    // Name of the Git branch that tracks pristine P4 state.
    std::string baselineBranch = "main";

    // Extra `.gitignore` patterns (verbatim gitignore syntax), in declaration
    // order, one per `ignore` line in p4gw.cfg. These cover files that P4
    // ignores (build artifacts, IDE state) but that would otherwise be tracked
    // by Git under a mapped subtree, since the allowlist tracks the whole
    // subtree. Depot-specific knowledge lives here, in the (shareable) config,
    // not in gw's code. buildGitignore appends them last, after the allowlist
    // and any carve-out re-exclusions.
    std::vector<std::string> ignorePatterns;

    // How `gw import` stages the depot snapshot (see ImportMode). Default
    // kWorktree stages in a hidden worktree and never touches the checkout;
    // `import_mode = checkout` opts back into the original in-checkout staging.
    ImportMode importMode = ImportMode::kWorktree;

    // Whether gw maintains the managed block in the repo's `.rgignore` (see
    // buildRgignoreSection) and doctor checks for it. `rgignore = off` in
    // p4gw.cfg opts out: gw never touches `.rgignore` and doctor stays quiet.
    bool manageRgignore = true;
};

// The `include` rules of `rules`, in declaration order. Most consumers only
// need the mapped subtrees (per-mirror sync, p4 scoping); this hands them just
// those, skipping the `exclude` carve-outs. Pure; unit-tested.
std::vector<const ViewRule*> includeRules(const std::vector<ViewRule>& rules);

// The depot paths of the `exclude` rules of `rules`, in declaration order (each
// ends "/..."). Used to exempt intentional in-place / dropped view lines from
// the view check. Pure; unit-tested.
std::vector<std::string> excludeDepotPaths(const std::vector<ViewRule>& rules);

// The rule that governs a depot file, resolved later-wins: the *last* rule (in
// declaration order, not longest-prefix) whose depot path is a prefix of
// `depotFile`. nullptr if no rule covers it. An include result means the file
// maps to the mirror; an exclude result means it is carved out. Pure;
// unit-tested.
const ViewRule* effectiveRuleForDepot(const std::vector<ViewRule>& rules,
                                      const std::string& depotFile);

// The rule that governs a repo-relative working-tree path, resolved later-wins:
// the *last* rule whose `repoSubtree` is a prefix of `repoRel` (an empty
// subtree, i.e. a whole-repo include, matches everything). nullptr if none.
// "Tracked / shipped through the mirror" iff the result is an include. Pure;
// unit-tested.
const ViewRule* effectiveRuleForRepo(const std::vector<ViewRule>& rules,
                                     const std::string& repoRel);

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

// Repo-relative working-tree subtree of a depot path carved out of an include:
// the part of `excludeDepotPath` below `mappingDepotPath`, prefixed by the
// include's `repoSubtree`. ("//d/src/...", "src", "//d/src/lib/...") -> "src/lib";
// ("//d/...", "", "//d/lib/...") -> "lib". Empty when `excludeDepotPath` is not
// strictly under `mappingDepotPath`. Forward slashes, no trailing slash. Pure;
// unit-tested.
std::string excludedRepoSubtree(const std::string& mappingDepotPath,
                                const std::string& repoSubtree,
                                const std::string& excludeDepotPath);

// Builds the starter `.gitignore` content for a fresh repo. gw tracks only the
// depot subtree(s) the repo maps; everything else in the working tree -
// unmapped P4 content synced in place, the `.p4gw` mirror, and gw's own
// personal config - stays out of Git. The result is an allowlist: ignore
// everything at the root, then re-include exactly each mapped working-tree
// subtree (and `.gitignore` itself), applying the ordered include/exclude rules
// later-wins so an `exclude` carves a directory back out (`/src/lib/`) and a
// deeper re-`include` maps part of it back in (`!/src/lib/public/win64/`). A
// whole-repo include (empty repoSubtree) has nothing unmapped to hide, so it
// falls back to a plain denylist of just the gw-managed paths, still honoring
// any `exclude` carve-outs. Any `ignorePatterns` (from `ignore` lines in
// p4gw.cfg) are appended last, verbatim, so they ignore files P4 skips that
// would otherwise be tracked under a mapped subtree. Pure; unit-tested.
std::string buildGitignore(const std::vector<ViewRule>& rules,
                           const std::vector<std::string>& ignorePatterns = {});

// The allowlist re-include lines that make Git track each mapped `include`
// subtree: "!/src/", "!/files/", plus the ancestor re-include and child
// re-exclusion lines for a nested re-include chain ("!/a/", "/a/*", "!/a/b/").
// These are exactly the lines buildGitignore emits from the ordered rules,
// minus the plain carve-out re-exclusions (handled separately). Empty when the
// rules yield the denylist body (a whole-repo include, or nothing tracked).
// Order matches buildGitignore. Pure; unit-tested.
std::vector<std::string> allowlistTrackingLines(
    const std::vector<ViewRule>& rules);

// The subset of allowlistTrackingLines(rules) not already present as a complete
// line in `gitignoreContent` - the re-includes an existing allowlist .gitignore
// is missing to track every mapped subtree (e.g. after an `include` was added
// to p4gw.cfg without regenerating .gitignore, so `gw import` copies the mirror
// in but `git add` ignores it). Empty when the file already covers them or the
// rules yield the denylist body. Pure; unit-tested.
std::vector<std::string> missingAllowlistTrackingLines(
    const std::vector<ViewRule>& rules, const std::string& gitignoreContent);

// The starter `.gitattributes` gw init commits: a committed `* -text` so git
// stores every file's blob byte-for-byte as P4 synced it into the mirror
// (verbatim, no text-vs-binary guessing or CRLF<->LF translation), independent
// of the machine's core.autocrlf. Without it, line-ending handling falls to
// each machine's core.autocrlf and drifts, so commits made at different times
// store different endings for the same file and every `gw import --rebase`
// conflicts on line endings. Assumes a single client LineEnd across the team
// (an all-Windows CRLF shop); a mixed team wants `* text=auto` instead. Pure.
std::string buildGitattributes();

// Whether a `.gitattributes` body already pins end-of-line handling for every
// path - a catch-all `*` rule carrying a text/eol attribute (`-text`, `text`,
// `text=auto`, `eol=...`). Used by doctor to tell a deterministic setup from
// one left to core.autocrlf. Pure; unit-tested.
bool gitattributesPinsEol(const std::string& content);

// Whether a `.gitignore` body is the allowlist style `gw init` writes - a
// bare `/*` line that ignores everything and re-includes only the mapped
// subtrees. Used by doctor: an allowlist hides unmapped depot content synced
// in place from every tool that honors .gitignore (notably ripgrep), which
// the denylist style does not. Pure; unit-tested.
bool gitignoreIsAllowlist(const std::string& content);

// The marker lines bracketing the gw-managed block in `.rgignore`. gw only
// ever rewrites the lines between them, so hand-written rules outside the
// block survive every refresh.
inline constexpr const char* kRgignoreBeginMarker =
    "# >>> gw managed - do not edit inside this block, 'gw init'/'gw import' "
    "rewrite it >>>";
inline constexpr const char* kRgignoreEndMarker = "# <<< gw managed <<<";

// Body of the gw-managed `.rgignore` block. ripgrep honors .gitignore, so the
// allowlist hides unmapped depot content synced in place (bin/, content/) from
// every search; `.rgignore` outranks both .gitignore and .ignore per path, so
// this block reopens exactly what the allowlist hides - non-hidden root
// entries, the unmapped peers inside re-include chains, and the plain
// carve-outs - via `[!.]*` globs that spare dot entries (.git, the .p4gw
// mirror stay hidden). Because those reopens also outrank the denylists, the
// p4gw.cfg `ignore` patterns and the repo's `.ignore` body (passed in) are
// re-asserted after them. Empty when the rules yield the denylist .gitignore
// (whole-repo include), which hides nothing rg should see. Pure; unit-tested.
std::string buildRgignoreSection(const std::vector<ViewRule>& rules,
                                 const std::vector<std::string>& ignorePatterns,
                                 const std::string& dotIgnoreBody);

// Splices `sectionBody` into an `.rgignore` file's content as the gw-managed
// block: replaces an existing marker-delimited block in place, prepends the
// block to a file that has none (so later hand-written lines still win), or
// removes the block when `sectionBody` is empty. Content outside the markers
// is never touched. Errors when a begin marker has no matching end marker -
// rewriting through it could eat hand-written rules. Pure; unit-tested.
std::expected<std::string, std::string> upsertRgignore(
    const std::string& existing, const std::string& sectionBody);

// Rebuilds the gw-managed `.rgignore` block for the repo at `root` and writes
// the file when its content changes: reads `.gitignore` (the block only
// applies to the allowlist style - for anything else it is removed), builds
// the section from the rules plus the repo's `.ignore` body, and splices it
// in. No-op when `manageRgignore` is off. Returns whether the file was
// written. The file sits under the allowlist's root `/*`, so git never tracks
// it and it never reaches P4.
std::expected<bool, std::string> refreshRgignore(const Config& config,
                                                 const std::string& root);

// Whether an `.rgignore` body reopens the root entries the allowlist
// .gitignore hides - the gw-managed block (its begin marker) or a hand-rolled
// `!/*` / `!/[!.]*` line. Used by doctor to tell a covered repo from one
// whose searches silently skip unmapped depot content. Pure; unit-tested.
bool rgignoreReopensRoot(const std::string& content);

// Whether a ripgrep config file body (the file RIPGREP_CONFIG_PATH points
// at; one argument per line, `#` comments) leaves `--no-ignore-vcs` in
// effect, so rg skips .gitignore while still honoring the .ignore/.rgignore
// denylists. Recognizes the broader --no-ignore and -u/--unrestricted too,
// and the rg 14 negations (--ignore-vcs, --ignore) with later-flag-wins
// semantics, collapsed to one boolean - exact for the one-flag config
// doctor recommends. Pure; unit-tested.
bool ripgrepConfigDisablesVcsIgnore(const std::string& content);

// The hidden Git ref that tracks pristine depot state - the `origin/main`
// analog. Derived from the baseline branch name: "refs/p4gw/<baselineBranch>".
// It lives outside refs/heads/ so it never shows up in `git branch`; `gw
// import` advances it, and prepare/status/shelf read it as the canonical
// baseline. The like-named local branch is just a convenience pointer kept
// fast-forwarded to it. Pure; unit-tested.
std::string depotTrackingRef(const Config& config);

}  // namespace p4gw