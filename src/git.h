#pragma once

#include <expected>
#include <string>
#include <vector>

namespace p4gw::git {

// One file-level change between two trees, from `git diff --name-status`.
struct FileChange {
    char status = '?';     // A(dd), M(odify), D(elete), R(ename)
    std::string path;      // path relative to the repo root
    std::string newPath;   // destination path for renames, otherwise empty
};

// Commit counts between two refs, as `git status` reports for an upstream:
// `ahead` commits are on `ref` but not `base`; `behind` are on `base` but
// not `ref`.
struct AheadBehind {
    int ahead = 0;
    int behind = 0;
};

// Runs `git <args>` in `cwd` and returns trimmed stdout, or an error message
// including git's output if the command failed.
std::expected<std::string, std::string> run(const std::vector<std::string>& args,
                                            const std::string& cwd = {});

std::expected<std::string, std::string> currentBranch(const std::string& cwd = {});

std::expected<std::string, std::string> revParse(const std::string& ref,
                                                 const std::string& cwd = {});

// Returns true if the working tree has uncommitted changes (staged or not),
// including untracked files that are not ignored.
std::expected<bool, std::string> isDirty(const std::string& cwd = {});

std::expected<bool, std::string> branchExists(const std::string& branch,
                                              const std::string& cwd = {});

std::expected<std::string, std::string> switchBranch(const std::string& branch,
                                                     const std::string& cwd = {});

// `git switch --detach <ref>`: check out `ref`'s commit without being on any
// branch. Used to build a commit (e.g. a depot snapshot) off to the side
// without rewriting the branch the user is on.
std::expected<std::string, std::string> switchDetached(const std::string& ref,
                                                       const std::string& cwd = {});

// `git switch -f <branch>` / `git switch -f --detach <ref>`: like the plain
// switches but discard working-tree modifications that would block the
// checkout. Used only by import's failure recovery, where the tree was
// verified clean at the start and everything discarded is the torn import's
// own partial output.
std::expected<std::string, std::string> switchBranchForce(
    const std::string& branch, const std::string& cwd = {});
std::expected<std::string, std::string> switchDetachedForce(
    const std::string& ref, const std::string& cwd = {});

// `git clean -fd [-- <paths>]`: remove untracked files and directories under
// the given repo-relative paths (the whole tree when `paths` is empty).
// Ignored files - the mirror, build output, gw's own config - are never
// touched (no -x). Used only by import's failure recovery to sweep the
// partial copies a torn import left behind.
std::expected<std::string, std::string> cleanUntracked(
    const std::vector<std::string>& paths, const std::string& cwd = {});

// Absolute path of the repository's git directory (`git rev-parse
// --absolute-git-dir`), correct for worktrees where `.git` is a file. gw
// stores its import-pending marker there: outside the working tree so `git
// reset`/`git clean` can't remove it, and outside the mirror so p4 never
// sees it.
std::expected<std::string, std::string> gitDir(const std::string& cwd = {});

// `git worktree add --detach <path> <ref>`: create a linked worktree checked
// out (detached) at `ref`. Used by import's worktree mode for the hidden
// snapshot worktree under the git dir; the shared object DB and refs mean a
// commit there advances only that worktree's private HEAD, never the user's.
std::expected<std::string, std::string> worktreeAdd(const std::string& path,
                                                    const std::string& ref,
                                                    const std::string& cwd = {});

// `git worktree prune`: drop registrations whose working directory is gone.
// Required before re-adding a worktree at a path that was deleted by hand
// (bare `worktree add` refuses a still-registered missing path).
std::expected<std::string, std::string> worktreePrune(const std::string& cwd = {});

// `git reset --hard <ref>` in `cwd`. Used only on gw's own snapshot worktree
// to force it back to the depot baseline (never on the user's checkout).
std::expected<std::string, std::string> resetHard(const std::string& ref,
                                                  const std::string& cwd = {});

// `git update-ref <ref> <target>`: point `ref` at `target` without checking it
// out. Used to advance the hidden depot-tracking ref and to fast-forward a
// non-current branch.
std::expected<void, std::string> updateRef(const std::string& ref,
                                           const std::string& target,
                                           const std::string& cwd = {});

// `git merge --ff-only <ref>` on the current branch: advance to `ref` only if
// it is a fast-forward (the current branch is an ancestor of `ref`). Fails
// rather than creating a merge commit when histories have diverged.
std::expected<std::string, std::string> mergeFastForward(const std::string& ref,
                                                         const std::string& cwd = {});

// SHA of the most recent commit reachable from `ref` whose message matches the
// extended-regex `pattern`, or an empty string if none match. Used to find the
// last `Import depot state` commit when migrating a legacy repo.
std::expected<std::string, std::string> latestCommitMatching(
    const std::string& pattern, const std::string& ref,
    const std::string& cwd = {});

// `git switch --orphan` - a new branch with no history; tracked files of the
// previous branch are removed from the working tree (restored by switching
// back), untracked files are left alone.
std::expected<std::string, std::string> switchOrphanBranch(
    const std::string& branch, const std::string& cwd = {});

// `git switch -c <branch> <startRef>`: create and check out `branch` rooted
// at `startRef`. Fails if the branch already exists.
std::expected<std::string, std::string> createBranch(const std::string& branch,
                                                     const std::string& startRef,
                                                     const std::string& cwd = {});

// `git merge-file <ours> <base> <theirs>`: a 3-way line merge writing the
// result back into `ours` in place (with conflict markers on overlap).
// Returns true when the merge had conflicts, false when it merged cleanly.
std::expected<bool, std::string> mergeFile(const std::string& ours,
                                           const std::string& base,
                                           const std::string& theirs,
                                           const std::string& cwd = {});

// True when `ancestor` is an ancestor of (or equal to) `descendant`.
std::expected<bool, std::string> isAncestor(const std::string& ancestor,
                                            const std::string& descendant,
                                            const std::string& cwd = {});

// Commits `ref` is ahead of / behind `base` (`git rev-list --left-right
// --count base...ref`). Both refs must exist.
std::expected<AheadBehind, std::string> aheadBehind(const std::string& base,
                                                    const std::string& ref,
                                                    const std::string& cwd = {});

// Lines of `git status --porcelain` - one per changed or untracked (not
// ignored) path. Empty means a clean working tree.
std::expected<std::vector<std::string>, std::string> statusLines(
    const std::string& cwd = {});

// Subject line (first line of the message) of the commit at `ref`.
std::expected<std::string, std::string> commitSubject(const std::string& ref,
                                                      const std::string& cwd = {});

// All tracked files, repo-relative with forward slashes.
std::expected<std::vector<std::string>, std::string> lsFiles(
    const std::string& cwd = {});

std::expected<std::string, std::string> addAll(const std::string& cwd = {});

// Of `paths`, the subset git treats as ignored: matched by a .gitignore rule
// and not already tracked - exactly what `git add -A` skips silently but an
// explicit `git add <path>` refuses ("paths are ignored ... use -f"). Runs
// `git check-ignore -z --stdin`, whose exit status is 0 when it printed at
// least one path, 1 when none matched (not an error here), and >=128 on a real
// failure. An empty input is a no-op. Repo-relative, forward-slash paths.
std::expected<std::vector<std::string>, std::string> ignoredPaths(
    const std::vector<std::string>& paths, const std::string& cwd = {});

// `git add -A --pathspec-from-file=<tmp> --pathspec-file-nul`: stage exactly
// `paths` (repo-relative; modifications, additions, and deletions alike)
// without rescanning the rest of the tree. The paths travel through a
// NUL-separated temp file, so the list length never hits a command-line
// limit. Gitignored paths are dropped first (via ignoredPaths), because naming
// one explicitly makes git refuse the whole batch - the have-manifest fast
// path's diff surfaces the build output p4 syncs into the mirror but Git must
// not track, and the full walk's `git add -A` skips exactly those. An empty
// list (or one left empty after filtering) is a no-op. Used by import's
// have-manifest fast path.
std::expected<void, std::string> addPaths(const std::vector<std::string>& paths,
                                          const std::string& cwd = {});

// True when there is nothing staged to commit.
std::expected<bool, std::string> indexMatchesHead(const std::string& cwd = {});

std::expected<std::string, std::string> commit(const std::string& message,
                                               const std::string& cwd = {});

// `git rebase <onto>` for the currently checked-out branch. On conflict the
// error includes git's output; the repo is left in the normal mid-rebase
// state for the user to resolve.
std::expected<std::string, std::string> rebase(const std::string& onto,
                                               const std::string& cwd = {});

// True when git-branchless is initialized in *this* repo - its main-branch
// config key is set in the repository's worktree or local config (branchless
// writes it to the per-worktree config). Read those scopes only, never
// global/system, so a global git-branchless setup doesn't make every repo look
// branchless. Branchless tracks commit visibility in its own event log and
// works detached with implicit branches, so gw restacks via `git branchless
// sync` rather than a single-branch `git rebase` and moves HEAD by commit.
std::expected<bool, std::string> isBranchless(const std::string& cwd = {});

// `git branchless sync`: restack every visible (draft) stack onto the latest
// location of branchless's main branch, recording the rewrites so the
// pre-rebase commits go obsolete (hidden from the smartlog). The branchless
// analog of rebasing all descendants of the baseline at once. On conflict the
// error includes branchless's output and the repo is left mid-rebase for the
// user to resolve.
std::expected<std::string, std::string> branchlessSync(const std::string& cwd = {});

// `git config <key> <value>`: set a repo-local config value. Used to point
// branchless's main branch at the gw baseline so its restack lands on the
// depot state.
std::expected<void, std::string> setConfig(const std::string& key,
                                           const std::string& value,
                                           const std::string& cwd = {});

// Writes the blob `ref:path` to `destFile` (byte-exact, safe for binaries).
std::expected<std::string, std::string> catBlobToFile(const std::string& ref,
                                                      const std::string& path,
                                                      const std::string& destFile,
                                                      const std::string& cwd = {});

// Value of a `git config` key, or empty string if the key is unset. With
// `localOnly`, reads the repository's local config scope only (`--local`),
// ignoring global/system config.
std::expected<std::string, std::string> configValue(const std::string& key,
                                                    const std::string& cwd = {},
                                                    bool localOnly = false);

// File-level changes from `fromRef` to `toRef`, with rename detection.
std::expected<std::vector<FileChange>, std::string> diffNameStatus(
    const std::string& fromRef, const std::string& toRef,
    const std::string& cwd = {});

// Commit subjects and bodies from `fromRef` (exclusive) to `toRef`
// (inclusive), oldest first - used to build the P4 changelist description.
std::expected<std::string, std::string> commitMessages(const std::string& fromRef,
                                                       const std::string& toRef,
                                                       const std::string& cwd = {});

}  // namespace p4gw::git
