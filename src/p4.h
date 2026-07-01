#pragma once

#include <expected>
#include <string>
#include <vector>

#include "config.h"

namespace p4gw::p4 {

// Runs `p4 <args>` (adding `-c <client>` when the config names one) and
// returns stdout, or an error message including p4's output on failure.
std::expected<std::string, std::string> run(const Config& config,
                                            const std::vector<std::string>& args);

// ---- pure client-spec helpers (no process calls; unit-tested) ----

// One mapping line from the View: section of a client spec.
struct ViewLine {
    std::string depot;    // e.g. //depot/project/src/...
    std::string client;   // e.g. //aaron-dev/src/.p4gw/...
    bool exclude = false; // leading '-'
    bool overlay = false; // leading '+'
};

// A problem found while checking the client view. `message` is self-contained
// and human-readable. When `excludePath` is non-empty the problem is a subtree
// diverted out of the mirror, and `excludePath` is the depot path to declare as
// an `exclude` in p4gw.cfg to resolve it - init groups these into one hint
// rather than repeating the explanation per line.
struct ViewProblem {
    std::string message;
    std::string excludePath;
};

// Value of a single-line field ("Root", "Client", "LineEnd", ...) in
// `p4 client -o` output; empty string if absent.
std::string specField(const std::string& spec, const std::string& field);

// Parses the View: section of `p4 client -o` output. Handles quoted paths
// and -/+ prefixes.
std::vector<ViewLine> parseClientView(const std::string& spec);

// Checks that the view maps the *bulk* of `depotPath` to exactly
// `expectedClientPath` (both in `//.../...` form): the last view line covering
// `depotPath` (it or a broader scope) must be that remap, so a later line can't
// shadow it back into the wrong place. Lines that don't overlap `depotPath` are
// ignored - other custom mappings are fine. Narrower lines *under* `depotPath`
// may legitimately carve the subtree up (a `-` exclusion of a per-platform peer
// directory, or a re-include of a deeper directory back into the mirror), so
// those are allowed. Two things are flagged unless declared in
// `excludedDepotPaths` (the subtrees the config carves out to sync in place,
// gitignored): a narrower line *under* `depotPath` that lands anywhere but the
// mirror (it diverts part of the subtree out of the mirror - caught even when
// the repo is the client root); and any other line whose client side falls
// under `repoClientPrefix` (the Git repo's location, e.g. "//client/src/"; pass
// empty to skip) but not under the mirror. Either way P4 must never write into
// a Git-tracked path. `extraMirrorPrefixes` are the client-side prefixes of the
// config's *other* mirrors (a repo can map several subtrees, and a re-`include`
// nests one mirror inside another): a line landing in any of them is a mirror
// mapping, not a repo leak, so it is exempt too. Returns the problems found;
// empty means consistent.
std::vector<ViewProblem> checkViewMapping(
    const std::vector<ViewLine>& view, const std::string& depotPath,
    const std::string& expectedClientPath,
    const std::string& repoClientPrefix,
    const std::vector<std::string>& excludedDepotPaths = {},
    const std::vector<std::string>& extraMirrorPrefixes = {});

// The minimal set of depot paths that cover all of `excludePaths`: duplicates
// removed and any path strictly under another in the set dropped (excluding the
// ancestor already covers the descendant). Order follows first appearance. Used
// to suggest the fewest `exclude` lines that resolve a batch of diversions.
// Pure; unit-tested.
std::vector<std::string> minimalExcludePaths(
    const std::vector<std::string>& excludePaths);

// "C:\work\project\mirror" under client root "C:\work\project" for client
// "aaron-dev" -> "//aaron-dev/mirror" + suffix; empty if `localDir` is not
// inside the client root.
std::string clientViewPath(const std::string& clientName,
                           const std::string& clientRoot,
                           const std::string& localDir,
                           const std::string& suffix);

// Full mapping consistency check against `p4 client -o` output: `depotPath`
// must map into the mirror directory and nothing may map into the repo
// directory. `excludedDepotPaths` are subtrees the config carves out of the
// mirror (synced in place / gitignored), whose in-place view lines are exempt
// from the repo-mapping rule. `otherMirrorDirs` are the config's *other* mirror
// directories (from other includes / re-includes); their client-side prefixes
// are exempted too, so a repo that maps several subtrees - or nests one mirror
// inside another via a re-include - does not flag its sibling mirrors as repo
// leaks. Returns the problems found; empty means consistent.
std::vector<ViewProblem> checkSpecMapping(
    const std::string& spec, const std::string& depotPath,
    const std::string& repoDir, const std::string& mirrorDir,
    const std::vector<std::string>& excludedDepotPaths = {},
    const std::vector<std::string>& otherMirrorDirs = {});

// ---- wrappers over the p4 CLI ----

std::expected<std::string, std::string> info(const Config& config);

// Full `p4 client -o` output for the configured client.
std::expected<std::string, std::string> clientSpec(const Config& config);

// The client root directory, from the spec's Root: field.
std::expected<std::string, std::string> clientRoot(const Config& config);

// Creates a new numbered pending changelist with `description` and returns
// its number.
std::expected<std::string, std::string> createChangelist(
    const Config& config, const std::string& description);

// Open explicit local files in changelist `cl`. Calls are chunked so long
// file lists never overflow the platform command-line limit.
std::expected<std::string, std::string> editFiles(
    const Config& config, const std::string& cl,
    const std::vector<std::string>& files);
std::expected<std::string, std::string> addFiles(
    const Config& config, const std::string& cl,
    const std::vector<std::string>& files);
std::expected<std::string, std::string> deleteFiles(
    const Config& config, const std::string& cl,
    const std::vector<std::string>& files);

// `p4 move` (the source must already be opened for edit).
std::expected<std::string, std::string> moveFile(const Config& config,
                                                 const std::string& cl,
                                                 const std::string& from,
                                                 const std::string& to);

// `p4 reconcile -n` scoped to the configured depot path - the full canary that
// scans the whole subtree. Returns p4's preview output; an empty string means
// nothing unexpected to reconcile.
std::expected<std::string, std::string> reconcilePreview(const Config& config);

// `p4 reconcile -n` scoped to an explicit `files` list (chunked to stay under
// the command-line limit). The fast check: it inspects only the files prepare
// touched, so it confirms the change landed but does not scan the rest of the
// subtree for strays. Empty output means clean.
std::expected<std::string, std::string> reconcilePreviewFiles(
    const Config& config, const std::vector<std::string>& files);

// True when `p4 reconcile` output means "nothing to reconcile". Matched
// case-insensitively: p4 says "No file(s) to reconcile." for an explicit file
// list but "<path> - no file(s) to reconcile." for a directory, and the
// difference must not read as a spurious mismatch. Pure; unit-tested.
bool reconcileReportsClean(const std::string& output);

// Highest submitted changelist among the synced revisions of the subtree
// (`p4 changes -m1 -s submitted <depot_path>#have`); empty if none.
std::expected<std::string, std::string> latestSubmittedCl(const Config& config);

// `p4 opened` scoped to the configured depot path; empty if nothing is open.
std::expected<std::string, std::string> openedFiles(const Config& config);

// Current P4 user name, from `p4 info` ("User name:"). Used to scope
// `gw shelf list` to the caller's own changelists.
std::expected<std::string, std::string> currentUser(const Config& config);

// Current P4 client (workspace) name, from `p4 info` ("Client name:"). Used to
// scope `gw shelf list` to the current workspace when the config does not name
// a client explicitly (it relies on the ambient P4CLIENT).
std::expected<std::string, std::string> currentClient(const Config& config);

// `p4 -ztag changes -s <status> [-u <user>] [-c <client>] <depot_path>`:
// pending or shelved changelists under the configured subtree (status is
// "pending" or "shelved"). When `user` is non-empty, limits to that user's
// changelists; when `client` is non-empty, limits to that workspace's
// changelists. Returns raw -ztag output for shelf.h's parseChanges.
std::expected<std::string, std::string> changes(const Config& config,
                                                const std::string& status,
                                                const std::string& user,
                                                const std::string& client);

// `p4 -ztag describe -S <cl>`: the shelved-file listing for a changelist.
// Returns raw -ztag output for shelf.h's parseShelveDescribe.
std::expected<std::string, std::string> describeShelved(const Config& config,
                                                        const std::string& cl);

// `p4 print -q -o <destFile> <fileSpec>`: writes the content of a depot file
// revision to `destFile`, byte-exact (safe for binaries). `fileSpec` carries
// the revision, e.g. "//depot/foo#5" or the shelved form "//depot/foo@=12345".
// This is the single primitive both `gw shelf import` and `gw import`'s
// opened-files preflight (printHeadToFile) use to read depot content.
std::expected<std::string, std::string> printDepotFile(
    const Config& config, const std::string& fileSpec,
    const std::string& destFile);

// One file from `p4 opened`, parsed from the structured -ztag form.
struct OpenedFile {
    std::string depotFile;  // //depot/...
    std::string action;     // edit, add, delete, move/add, move/delete, ...
};

// Parses `p4 -ztag opened` output into per-file records (pure; unit-tested).
std::vector<OpenedFile> parseTaggedOpened(const std::string& ztagOutput);

// Keeps only opened files whose effective (later-wins) rule is an `include`.
// A file whose governing rule is an `exclude` - or that no rule covers - is
// gitignored / synced in place, so gw mirrors and ships nothing through it and
// must ignore P4 opens on it: those belong to the user's own P4 work, not a gw
// changelist. Resolving through the ordered rules (not a flat exclude list) is
// what keeps a re-`include`d subtree under an excluded parent (win64 under an
// excluded lib) correctly shippable. Pure; unit-tested.
std::vector<OpenedFile> filterExcludedOpens(
    const std::vector<OpenedFile>& opened,
    const std::vector<ViewRule>& rules);

// Repo-relative path (forward slashes) of `depotFile` within the `depotPath`
// subtree ("//.../..."), or empty if it is not under it (pure).
std::string depotRelativePath(const std::string& depotPath,
                              const std::string& depotFile);

// True for p4 open actions that introduce a file with no head revision at
// this depot path (add, move/add, branch); `gw import` omits these (pure).
bool isAddAction(const std::string& action);

// Structured `p4 -ztag opened` scoped to the configured depot path, with files
// under an `exclude`d subtree removed; empty if nothing relevant is open.
std::expected<std::vector<OpenedFile>, std::string> openedFilesTagged(
    const Config& config);

// Extracts every "... depotFile <path>" value from `p4 -ztag` output, in order
// (pure; unit-tested). Used to read the file list from `p4 -ztag have`.
std::vector<std::string> parseTaggedDepotFiles(const std::string& ztagOutput);

// Depot files p4 has synced under `depotPath` (`p4 -ztag have <depotPath>`).
// These are the files the mirror is *supposed* to contain; `gw import` uses
// the set to ignore strays p4 doesn't track. A subtree with nothing synced is
// a normal empty result, not an error. `depotPath` must be the configured,
// scoped path - never an unscoped wildcard.
std::expected<std::vector<std::string>, std::string> haveFiles(
    const Config& config, const std::string& depotPath);

// Writes the head revision of `depotFile` to `dest` (byte-exact, safe for
// binaries); a thin convenience over printDepotFile for `<depotFile>#head`.
// `depotFile` must be an explicit path - never an unscoped wildcard.
std::expected<void, std::string> printHeadToFile(const Config& config,
                                                 const std::string& depotFile,
                                                 const std::string& dest);

// ---- workflow wrappers ----
// Used by `gw integtest` today (and groundwork for a future submit
// workflow). Callers must pass an explicitly scoped pathSpec - never an
// unscoped wildcard.

// Depot directory (no trailing /...) the client view maps `localDir` to,
// from `p4 where`.
std::expected<std::string, std::string> whereDepotDir(const Config& config,
                                                      const std::string& localDir);

// `p4 sync <pathSpec>`; already-up-to-date is success.
std::expected<std::string, std::string> sync(const Config& config,
                                             const std::string& pathSpec);

// `p4 sync -f <pathSpec>`; force-syncs over writable files; already-up-to-date
// is success.
std::expected<std::string, std::string> syncForce(const Config& config,
                                                  const std::string& pathSpec);

// `p4 revert <pathSpec>`; nothing opened is success.
std::expected<std::string, std::string> revert(const Config& config,
                                               const std::string& pathSpec);

// `p4 revert -w -c <cl> <depot_path>...` scoped to the configured mappings:
// reverts exactly the files this CL opened under the subtree, restoring the
// mirror to the depot head and clearing the open state (`-w` also deletes files
// that were opened for add, which a plain revert would leave behind). Used by
// `gw prepare --shelf` to leave only the shelf behind. Nothing opened is
// success.
std::expected<std::string, std::string> revertChangelist(const Config& config,
                                                         const std::string& cl);

// `p4 reconcile -c <cl> <pathSpec>` - the real thing, unlike
// reconcilePreview; nothing to reconcile is success with empty output.
std::expected<std::string, std::string> reconcileToCl(const Config& config,
                                                      const std::string& cl,
                                                      const std::string& pathSpec);

// `p4 opened -c <cl>`; empty if the changelist has no open files.
std::expected<std::string, std::string> openedInCl(const Config& config,
                                                   const std::string& cl);

// `p4 submit -c <cl>`. gw's own commands never call this - submitting is
// the user's move (P4V); it exists for the integration tests.
std::expected<std::string, std::string> submit(const Config& config,
                                               const std::string& cl);

// `p4 shelve -c <cl>`: shelve the files open in `cl`. Used by the integration
// tests to build a shelf for `gw shelf import` to read.
std::expected<std::string, std::string> shelve(const Config& config,
                                               const std::string& cl);

// `p4 shelve -d -c <cl>`: discard the shelved files from `cl`.
std::expected<std::string, std::string> deleteShelve(const Config& config,
                                                     const std::string& cl);

// `p4 change -d <cl>` (empty pending changelists only).
std::expected<std::string, std::string> deleteChangelist(const Config& config,
                                                         const std::string& cl);

// `p4 obliterate -y <depotFile>`: permanently removes a file AND its history
// from the depot. Integration-test cleanup only - gw commands never call this.
// `depotFile` must be an explicit path, never an unscoped wildcard; a file that
// isn't in the depot ("no file(s) to obliterate") is treated as success.
std::expected<std::string, std::string> obliterate(const Config& config,
                                                   const std::string& depotFile);

// `p4 -ztag changes -s pending -u <user>` with NO path filter: every pending
// changelist owned by `user`, including empty and shelved-only ones that a
// path-scoped query would miss. Metadata-only and bounded by user; used solely
// by integtest cleanup to sweep changelists left by an aborted run. Returns raw
// -ztag output for shelf.h's parseChanges.
std::expected<std::string, std::string> pendingChangelistsForUser(
    const Config& config, const std::string& user);

// ---- throwaway-server safety checks (used by integtest) ----

// ServerID from `p4 info` output; tries the "ServerID" and "Server ID" labels.
// Empty when the server has no server.id set (pure; unit-tested).
std::string serverIdFromInfo(const std::string& info);

// Security level parsed from `p4 configure show security` output (the
// "security=N" form); 0 when no such line is present (pure; unit-tested).
int securityLevelFromShow(const std::string& configureShowOutput);

// `p4 info` -> the server's ServerID (empty if unset).
std::expected<std::string, std::string> serverId(const Config& config);

// `p4 configure show security` -> the server's security level (0 if unset).
std::expected<int, std::string> securityLevel(const Config& config);

}  // namespace p4gw::p4
