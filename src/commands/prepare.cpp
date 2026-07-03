#include <algorithm>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "commands.h"
#include "config.h"
#include "git.h"
#include "p4.h"
#include "p4ops.h"
#include "process.h"

namespace fs = std::filesystem;

namespace p4gw {

namespace {

// An include rule with its mirror directory resolved to an absolute path.
struct ResolvedMapping {
    const ViewRule* rule;
    std::string mirrorDir;
};

// The include that owns a repo-relative path, resolved later-wins over the
// ordered rules: the effective rule for the path must be an include, and it is
// routed to that include's mirror. Returns nullptr for paths under no rule
// (pure Git files like bin/ or content/) and for paths whose effective rule is
// an `exclude` carve-out (those sync in place / are unsynced and never ship
// through gw) - including a directory re-included under an excluded parent,
// which resolves back to its own include.
const ResolvedMapping* routeFor(const std::vector<ResolvedMapping>& resolved,
                                const std::vector<ViewRule>& rules,
                                const std::string& repoRel) {
    const ViewRule* eff = effectiveRuleForRepo(rules, repoRel);
    if (eff == nullptr || eff->exclude) return nullptr;
    for (const auto& r : resolved) {
        if (r.rule == eff) return &r;
    }
    return nullptr;
}

// Local path of a repo-relative file inside its include's mirror, for p4
// commands: the path with the include's subtree prefix stripped, rooted at the
// mirror directory.
std::string mirrorFilePath(const ResolvedMapping& route,
                           const std::string& repoRel) {
    const std::string& sub = route.rule->repoSubtree;
    const std::string within =
        sub.empty() ? repoRel : repoRel.substr(sub.size() + 1);
    return (fs::path(route.mirrorDir) / fs::path(within)).make_preferred()
        .string();
}

constexpr const char* kPrepareUsage =
    "usage: gw prepare [<commit>] [options]\n"
    "\n"
    "Turn the current branch's commits (everything since the depot baseline)\n"
    "into a new pending P4 changelist: stage the branch's file state into the\n"
    "mirror with explicit p4 edit/add/delete/move, then build the CL. gw never\n"
    "submits - review and submit it from P4V.\n"
    "\n"
    "arguments:\n"
    "  <commit>              Prepare only the slice of the stack up to <commit>\n"
    "                        (default: HEAD), so you can ship part of a branch\n"
    "\n"
    "options:\n"
    "  -m, --message <text>  Describe the changelist with <text> instead of the\n"
    "                        branch's commit messages\n"
    "  -n, --dry-run         Show the p4 operations a real run would perform and\n"
    "                        exit, touching neither P4 nor the mirror\n"
    "      --update <CL>     Refresh the existing pending changelist <CL> to match\n"
    "                        the branch instead of creating a new one (reverts its\n"
    "                        current opens first, then re-stages); keeps the CL's\n"
    "                        number and description\n"
    "      --shelf           Build a P4 shelf instead of a pending changelist:\n"
    "                        stage, shelve, then revert the opens so only the\n"
    "                        shelf remains and the mirror is left untouched\n"
    "      --verify          After staging, run a full 'p4 reconcile -n' over the\n"
    "                        whole subtree to catch unexpected mirror changes\n"
    "                        (scales with subtree size, not this change)\n"
    "  -h, --help            Show this help\n"
    "\n";

// Stages the content of `commit:repoRel` into the mirror file at `dest`
// (creating directories, clearing a read-only bit left by p4).
std::expected<std::string, std::string> stageBlob(const std::string& root,
                                                  const std::string& commit,
                                                  const std::string& repoRel,
                                                  const std::string& dest) {
    std::error_code ec;
    fs::create_directories(fs::path(dest).parent_path(), ec);
    if (ec) {
        return std::unexpected("failed to create directory " +
                               fs::path(dest).parent_path().string() + ": " +
                               ec.message());
    }
    if (fs::exists(dest, ec)) {
        fs::permissions(dest, fs::perms::owner_write, fs::perm_options::add, ec);
    }
    return git::catBlobToFile(commit, repoRel, dest, root);
}

// Byte-for-byte comparison of two files. A side that cannot be opened (e.g. a
// missing mirror file) compares unequal - the safe answer, since it keeps the
// edit rather than silently dropping a real change.
bool fileBytesEqual(const fs::path& a, const fs::path& b) {
    std::ifstream fa(a, std::ios::binary);
    std::ifstream fb(b, std::ios::binary);
    if (!fa || !fb) return false;
    char bufA[8192];
    char bufB[8192];
    while (true) {
        fa.read(bufA, sizeof bufA);
        fb.read(bufB, sizeof bufB);
        const std::streamsize na = fa.gcount();
        const std::streamsize nb = fb.gcount();
        if (na != nb) return false;
        if (na == 0) break;
        if (std::memcmp(bufA, bufB, static_cast<size_t>(na)) != 0) return false;
    }
    return true;
}

// True when the content of `commit:repoRel` is byte-identical to the file
// already in the mirror at `mirrorPath` - i.e. opening it for edit would put a
// no-op into the changelist. This happens when a file is changed early in the
// branch and reverted back to the depot content later: the endpoint git diff
// still lists it (its blob differs from the baseline's), yet the staged content
// matches what P4 already has, so the edit ships nothing. Such edits are dropped
// so they never enter the CL. The target blob is written to a temp file (the
// byte-exact path, since captured stdout is not binary-safe on Windows) and
// compared against the current mirror copy.
std::expected<bool, std::string> stagedMatchesMirror(const std::string& root,
                                                     const std::string& commit,
                                                     const std::string& repoRel,
                                                     const std::string& mirrorPath) {
    std::error_code ec;
    if (!fs::exists(mirrorPath, ec)) return false;  // no counterpart: a real edit
    const fs::path tmp = uniqueTempFile("p4gw_prepare_cmp", ".tmp");
    auto blob = git::catBlobToFile(commit, repoRel, tmp.string(), root);
    if (!blob) {
        fs::remove(tmp, ec);
        return std::unexpected(blob.error());
    }
    const bool equal = fileBytesEqual(tmp, mirrorPath);
    fs::remove(tmp, ec);
    return equal;
}

}  // namespace

// Turns the commits from the depot baseline through a target commit (HEAD by
// default) into a pending P4 changelist, without touching the user's working
// tree:
//   1. Preflight: baseline is an ancestor of the target with commits on top.
//   2. Create a numbered pending CL described by the commit messages.
//   3. Stage the target's file state into the mirror with explicit
//      p4 edit/add/delete/move - we know exactly what changed from Git.
//   4. Verify with a scoped `p4 reconcile -n`: anything it still finds is
//      an unexpected mirror/depot mismatch and gets a loud warning.
// An optional <commit> argument prepares only the slice of a stack up to that
// commit, so you can ship part of a branch without checking the commit out.
// gw never submits; review the CL and submit it from P4V. With --shelf the same
// staging produces a P4 shelf instead: gw shelves the staged content and then
// reverts the opens, so only the shelf remains and the mirror is left untouched.
// With --update <CL> the staging targets an existing pending changelist: gw
// reverts that CL's current opens to restore the mirror to the depot head, then
// re-stages the branch into the same CL - refreshing a pending CL after a rebase
// or review tweak without creating a new one and without a manual revert.
int cmdPrepare(const Args& args) {
    bool fullVerify = false;
    bool dryRun = false;
    bool shelf = false;
    bool update = false;
    std::string updateCl;
    std::string messageOverride;
    std::string target = "HEAD";
    bool targetSet = false;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--help" || args[i] == "-h") {
            std::printf("%s", kPrepareUsage);
            return 0;
        } else if (args[i] == "--verify") {
            fullVerify = true;
        } else if (args[i] == "--shelf") {
            shelf = true;
        } else if (args[i] == "--dry-run" || args[i] == "-n") {
            dryRun = true;
        } else if (args[i] == "--update" && i + 1 < args.size()) {
            update = true;
            updateCl = args[++i];
        } else if (args[i] == "--update") {
            std::fprintf(stderr,
                         "gw prepare: --update requires a changelist number "
                         "(e.g. 'gw prepare --update 12345')\n");
            return 1;
        } else if ((args[i] == "--message" || args[i] == "-m") &&
                   i + 1 < args.size()) {
            messageOverride = args[++i];
        } else if (!args[i].empty() && args[i][0] != '-' && !targetSet) {
            target = args[i];
            targetSet = true;
        } else {
            std::fprintf(stderr, "gw prepare: unknown option '%s'\n",
                         args[i].c_str());
            std::fprintf(stderr, "%s", kPrepareUsage);
            return 1;
        }
    }

    if (update && shelf) {
        std::fprintf(stderr,
                     "gw prepare: --update and --shelf cannot be combined - "
                     "--update refreshes a pending changelist, not a shelf.\n");
        return 1;
    }

    std::string root;
    auto config = findAndLoadConfig(root);
    if (!config) {
        std::fprintf(stderr, "gw prepare: %s\n", config.error().c_str());
        return 1;
    }
    std::vector<ResolvedMapping> resolved;
    for (const auto* rule : includeRules(config->rules)) {
        const std::string mirrorDir = resolveMirrorPath(rule->mirrorPath, root);
        if (!fs::exists(mirrorDir)) {
            std::fprintf(stderr,
                         "gw prepare: mirror directory %s does not exist - "
                         "check the client view mapping ('gw doctor')\n",
                         mirrorDir.c_str());
            return 1;
        }
        resolved.push_back({rule, mirrorDir});
    }

    // Everything ships relative to the pristine depot baseline - the hidden
    // refs/p4gw/<baseline> ref, not the like-named branch (which may now carry
    // your own local commits).
    const std::string depotRef = depotTrackingRef(*config);
    if (!git::revParse(depotRef, root)) {
        std::fprintf(stderr,
                     "gw prepare: no depot baseline yet - run 'gw import' "
                     "first\n");
        return 1;
    }
    if (targetSet && !git::revParse(target, root)) {
        std::fprintf(stderr,
                     "gw prepare: '%s' is not a valid commit\n", target.c_str());
        return 1;
    }
    auto ancestor = git::isAncestor(depotRef, target, root);
    if (!ancestor) {
        std::fprintf(stderr, "gw prepare: %s\n", ancestor.error().c_str());
        return 1;
    }
    if (!*ancestor) {
        std::fprintf(stderr,
                     "gw prepare: %s is not based on the latest depot state - "
                     "run 'gw import --rebase' to rebase onto it first\n",
                     targetSet ? target.c_str() : "HEAD");
        return 1;
    }

    auto changes = git::diffNameStatus(depotRef, target, root);
    if (!changes) {
        std::fprintf(stderr, "gw prepare: %s\n", changes.error().c_str());
        return 1;
    }
    auto ops = planP4Operations(*changes);
    if (!ops) {
        std::fprintf(stderr, "gw prepare: %s\n", ops.error().c_str());
        return 1;
    }
    if (ops->empty()) {
        std::printf("Nothing to prepare: no file changes between the depot "
                    "baseline and %s.\n",
                    targetSet ? target.c_str() : "HEAD");
        return 0;
    }

    // Route each op to the mapping that owns its path. Changes outside every
    // mapping (pure-Git directories like bin/ or content/) are not shipped to
    // P4; collect them for an informational note.
    struct Staged {
        std::string repoRel;     // target:repoRel is the source content
        std::string mirrorPath;  // local mirror file to stage/open
    };
    struct MoveOp {
        std::string fromMirror;
        std::string toMirror;
        std::string repoTo;      // target:repoTo content for the destination
    };
    std::vector<Staged> edits;
    std::vector<Staged> adds;
    std::vector<std::string> deletes;  // local mirror paths (p4 clears them)
    std::vector<MoveOp> moves;
    std::vector<std::string> unmapped;
    std::vector<std::pair<std::string, std::string>> crossBoundary;

    for (const auto& op : *ops) {
        switch (op.kind) {
        case P4Op::Kind::Edit: {
            const auto* route = routeFor(resolved, config->rules, op.path);
            if (route) edits.push_back({op.path, mirrorFilePath(*route, op.path)});
            else unmapped.push_back(op.path);
            break;
        }
        case P4Op::Kind::Add: {
            const auto* route = routeFor(resolved, config->rules, op.path);
            if (route) adds.push_back({op.path, mirrorFilePath(*route, op.path)});
            else unmapped.push_back(op.path);
            break;
        }
        case P4Op::Kind::Delete: {
            const auto* route = routeFor(resolved, config->rules, op.path);
            if (route) deletes.push_back(mirrorFilePath(*route, op.path));
            else unmapped.push_back(op.path);
            break;
        }
        case P4Op::Kind::Move: {
            const auto* from = routeFor(resolved, config->rules, op.path);
            const auto* to = routeFor(resolved, config->rules, op.newPath);
            if (from && to && from->rule == to->rule) {
                moves.push_back({mirrorFilePath(*from, op.path),
                                 mirrorFilePath(*to, op.newPath), op.newPath});
            } else if (!from && !to) {
                // A pure-Git rename, outside every mapping: nothing for P4.
                unmapped.push_back(op.path);
                unmapped.push_back(op.newPath);
            } else {
                // One end is in a different mapping (or outside P4 entirely):
                // a single `p4 move` can't express it, and faking it with
                // delete+add would lose the file's depot history. Refuse and
                // let the user run the move in P4 directly.
                crossBoundary.emplace_back(op.path, op.newPath);
            }
            break;
        }
        }
    }

    if (!crossBoundary.empty()) {
        std::fprintf(stderr,
                     "gw prepare: %zu rename(s) cross a p4 mapping boundary - "
                     "gw cannot stage these:\n",
                     crossBoundary.size());
        for (const auto& [from, to] : crossBoundary) {
            std::fprintf(stderr, "  %s -> %s\n", from.c_str(), to.c_str());
        }
        std::fprintf(stderr,
                     "A file that moves between two depot subtrees (or in or "
                     "out of P4 control)\nneeds a real 'p4 move' run in P4 "
                     "directly - gw only stages changes within a\nsingle "
                     "mapping. Do the move in P4, submit it, then 'gw import' "
                     "to pick up the\nnew depot layout before preparing the "
                     "rest of your branch.\n");
        return 1;
    }

    // --update targets an existing pending CL, so restore the mirror to the
    // depot head *now* by reverting that CL's opens: the CL currently holds the
    // previous prepare's staged content, and both the byte comparison below and
    // the staging further down assume the mirror sits at depot head. Reverting
    // first also frees the CL to be repopulated. The re-check afterwards catches
    // opens left in *other* changelists - restaging would move them into this
    // CL, the same harm the create path's guard prevents - so refuse. The CL
    // keeps its number and description. --dry-run mutates nothing, so it skips
    // the revert; its preview is then computed against the mirror's current
    // (pre-revert) content and may differ slightly from a live run.
    if (update && !dryRun) {
        auto reverted = p4::revertChangelist(*config, updateCl);
        if (!reverted) {
            std::fprintf(stderr, "gw prepare: %s\n", reverted.error().c_str());
            return 1;
        }
        auto stillOpen = p4::openedFilesTagged(*config);
        if (!stillOpen) {
            std::fprintf(stderr, "gw prepare: %s\n", stillOpen.error().c_str());
            return 1;
        }
        if (!stillOpen->empty()) {
            std::fprintf(stderr,
                         "gw prepare: %zu file(s) are open in another changelist "
                         "under the configured mappings:\n",
                         stillOpen->size());
            for (const auto& o : *stillOpen) {
                std::fprintf(stderr, "  %s %s\n", o.action.c_str(),
                             o.depotFile.c_str());
            }
            std::fprintf(stderr,
                         "Re-staging would move them into changelist %s. Submit "
                         "or revert those\nopens first (see 'gw status'), then "
                         "rerun 'gw prepare --update %s'.\n",
                         updateCl.c_str(), updateCl.c_str());
            return 1;
        }
    }

    // Drop edits whose staged content already matches the mirror: a file
    // changed and then reverted within the branch still shows up in the
    // endpoint git diff, but opening it would put a no-op into the changelist.
    // (In --update the mirror was just reverted to the depot head above, so this
    // compares against depot state exactly as in a fresh prepare.) Reading the
    // mirror and writing the comparison blob to a temp file is read-only with
    // respect to P4 and the mirror, so this is safe before the --dry-run cutoff.
    std::vector<std::string> unchanged;
    {
        std::vector<Staged> realEdits;
        realEdits.reserve(edits.size());
        for (auto& e : edits) {
            auto same = stagedMatchesMirror(root, target, e.repoRel, e.mirrorPath);
            if (!same) {
                std::fprintf(stderr, "gw prepare: %s\n", same.error().c_str());
                return 1;
            }
            if (*same) {
                unchanged.push_back(e.repoRel);
            } else {
                realEdits.push_back(std::move(e));
            }
        }
        edits = std::move(realEdits);
    }

    if (edits.empty() && adds.empty() && deletes.empty() && moves.empty()) {
        if (!unchanged.empty()) {
            std::printf("Nothing to prepare: the changed files match the depot "
                        "already (changed then reverted within the branch).\n");
        } else {
            std::printf("Nothing to prepare: the changed files are all outside "
                        "the configured p4 mappings (pure Git).\n");
        }
        return 0;
    }

    // Prints the p4 operations this prepare maps to (and the pure-Git files it
    // skips). Shared by the --dry-run preview and the post-apply confirmation,
    // so both read identically.
    auto isUnchanged = [&](const std::string& path) {
        return std::find(unchanged.begin(), unchanged.end(), path) !=
               unchanged.end();
    };
    auto printPlannedOps = [&]() {
        for (const auto& op : *ops) {
            const bool srcMapped = routeFor(resolved, config->rules, op.path) != nullptr;
            switch (op.kind) {
            case P4Op::Kind::Edit:
                if (srcMapped && !isUnchanged(op.path)) {
                    std::printf("  edit    %s\n", op.path.c_str());
                }
                break;
            case P4Op::Kind::Add:
                if (srcMapped) std::printf("  add     %s\n", op.path.c_str());
                break;
            case P4Op::Kind::Delete:
                if (srcMapped) std::printf("  delete  %s\n", op.path.c_str());
                break;
            case P4Op::Kind::Move:
                if (srcMapped || routeFor(resolved, config->rules, op.newPath)) {
                    std::printf("  move    %s -> %s\n", op.path.c_str(),
                                op.newPath.c_str());
                }
                break;
            }
        }
        if (!unmapped.empty()) {
            std::printf("\n%zu changed file(s) are outside any p4 mapping and "
                        "were NOT added to the changelist (they stay in Git "
                        "only):\n",
                        unmapped.size());
            for (const auto& path : unmapped) {
                std::printf("  skip    %s\n", path.c_str());
            }
        }
        if (!unchanged.empty()) {
            std::printf("\n%zu changed file(s) already match the depot (changed "
                        "then reverted within the branch) and were NOT added to "
                        "the changelist:\n",
                        unchanged.size());
            for (const auto& path : unchanged) {
                std::printf("  same    %s\n", path.c_str());
            }
        }
    };

    // --update leaves the existing CL's description untouched, so there is
    // nothing to build; a new CL is described by -m or the branch's commits.
    std::string description = messageOverride;
    if (!update && description.empty()) {
        auto messages = git::commitMessages(depotRef, target, root);
        if (!messages) {
            std::fprintf(stderr, "gw prepare: %s\n", messages.error().c_str());
            return 1;
        }
        description = *messages;
    }

    // Refuse to run if files are already open in the mirror (a previous
    // prepare's still-pending CL, a stray p4 edit). Opening them again would
    // silently move them between changelists; the user must resolve the
    // existing opens first. --update opts in to exactly this: it targets the
    // named CL, reverting its opens below to restore the mirror before
    // re-staging, so the guard is deferred to a post-revert re-check that only
    // trips on opens left in *other* changelists.
    if (!update) {
        auto opened = p4::openedFilesTagged(*config);
        if (!opened) {
            std::fprintf(stderr, "gw prepare: %s\n", opened.error().c_str());
            return 1;
        }
        if (!opened->empty()) {
            std::fprintf(stderr,
                         "gw prepare: %zu file(s) are already open in P4 under "
                         "the configured mappings:\n",
                         opened->size());
            for (const auto& o : *opened) {
                std::fprintf(stderr, "  %s %s\n", o.action.c_str(),
                             o.depotFile.c_str());
            }
            std::fprintf(stderr,
                         "Opening them again would move them between "
                         "changelists. Submit or revert\nthe existing "
                         "changelist first (see 'gw status'), then rerun "
                         "'gw prepare'.\n");
            return 1;
        }
    }

    // --dry-run stops here: everything above is read-only (git diff, the route
    // planning, the opened-files guard). Show exactly which p4 operations a
    // real run would open, and touch neither P4 nor the mirror.
    if (dryRun) {
        if (update) {
            std::printf("[dry-run] would revert changelist %s's current opens, "
                        "then re-stage into it and open:\n", updateCl.c_str());
        } else if (shelf) {
            std::printf("[dry-run] would create a shelf from (open, shelve, then "
                        "revert):\n");
        } else {
            std::printf("[dry-run] would create a pending changelist and "
                        "open:\n");
        }
        printPlannedOps();
        std::printf("\n[dry-run] no changes made. Rerun without --dry-run to "
                    "%s.\n",
                    update ? "refresh the changelist"
                    : shelf ? "create the shelf"
                            : "create the changelist and open these files");
        return 0;
    }

    // In --update the target CL was reverted (and its number validated) above,
    // so we reuse it directly; otherwise mint a fresh pending CL.
    std::expected<std::string, std::string> cl;
    if (update) {
        cl = updateCl;
        std::printf("Refreshing pending changelist %s\n", updateCl.c_str());
    } else {
        cl = p4::createChangelist(*config, description);
        if (!cl) {
            std::fprintf(stderr, "gw prepare: %s\n", cl.error().c_str());
            return 1;
        }
        std::printf("Created pending changelist %s\n", cl->c_str());
    }

    auto fail = [&](const std::string& message) {
        std::fprintf(stderr, "gw prepare: %s\n", message.c_str());
        std::fprintf(stderr,
                     "Changelist %s may be partially populated. Inspect it in "
                     "P4V; 'p4 revert -c %s' undoes the opens.\n",
                     cl->c_str(), cl->c_str());
        return 1;
    };

    // The ops run in execution order (deletes, edits, moves, adds). Staging
    // happens after a file is opened (edit/move) and before `p4 add`; p4 itself
    // removes deleted files from the mirror.
    auto mirrorPathsOf = [](const std::vector<Staged>& staged) {
        std::vector<std::string> local;
        local.reserve(staged.size());
        for (const auto& s : staged) local.push_back(s.mirrorPath);
        return local;
    };

    if (!deletes.empty()) {
        auto result = p4::deleteFiles(*config, *cl, deletes);
        if (!result) return fail(result.error());
    }
    if (!edits.empty()) {
        auto result = p4::editFiles(*config, *cl, mirrorPathsOf(edits));
        if (!result) return fail(result.error());
        for (const auto& s : edits) {
            auto staged = stageBlob(root, target, s.repoRel, s.mirrorPath);
            if (!staged) return fail(staged.error());
        }
    }
    for (const auto& move : moves) {
        auto edited = p4::editFiles(*config, *cl, {move.fromMirror});
        if (!edited) return fail(edited.error());
        auto moved =
            p4::moveFile(*config, *cl, move.fromMirror, move.toMirror);
        if (!moved) return fail(moved.error());
        auto staged = stageBlob(root, target, move.repoTo, move.toMirror);
        if (!staged) return fail(staged.error());
    }
    if (!adds.empty()) {
        for (const auto& s : adds) {
            auto staged = stageBlob(root, target, s.repoRel, s.mirrorPath);
            if (!staged) return fail(staged.error());
        }
        auto result = p4::addFiles(*config, *cl, mirrorPathsOf(adds));
        if (!result) return fail(result.error());
    }

    printPlannedOps();

    // Warns about anything p4 reconcile would still open beyond what we staged.
    auto reportPreview =
        [&](const std::expected<std::string, std::string>& preview) {
            if (!preview) {
                std::fprintf(stderr, "gw prepare: verify failed: %s\n",
                             preview.error().c_str());
            } else if (!preview->empty()) {
                std::printf(
                    "\nWARNING: the mirror does not fully match this branch.\n"
                    "p4 reconcile would additionally open:\n%s"
                    "This usually means the mirror was modified outside gw or a "
                    "sync landed\nmid-prepare. Inspect before submitting.\n",
                    preview->c_str());
            }
        };

    if (fullVerify) {
        // The full canary: `p4 reconcile -n` over the whole subtree also catches
        // strays we never touched. It scans every mirror file, so its cost
        // scales with subtree size, not this change - announce it (flushed) so
        // the wait does not look like a hang.
        std::printf("Verifying with p4 reconcile -n over the whole subtree "
                    "(scales with subtree size)...\n");
        std::fflush(stdout);
        auto preview = p4::reconcilePreview(*config);
        reportPreview(preview);
        if (preview && preview->empty()) {
            std::printf("Verified: mirror matches the branch exactly.\n");
        }
    } else {
        // Default: the fast scoped check - reconcile only the files we opened.
        // Confirms this change landed cleanly without scanning the whole subtree.
        std::vector<std::string> touched;
        for (const auto& s : edits) touched.push_back(s.mirrorPath);
        for (const auto& s : adds) touched.push_back(s.mirrorPath);
        for (const auto& d : deletes) touched.push_back(d);
        for (const auto& m : moves) {
            touched.push_back(m.fromMirror);
            touched.push_back(m.toMirror);
        }
        auto preview = p4::reconcilePreviewFiles(*config, touched);
        reportPreview(preview);
        if (preview && preview->empty()) {
            std::printf("Verified the changed files match the mirror.\n");
        }
        std::printf("(For a full check that also catches unexpected changes "
                    "elsewhere in the subtree, rerun with --verify.)\n");
    }

    if (shelf) {
        // Shelve the staged content, then revert the opens so the workspace is
        // left clean: the mirror returns to the depot head and the changelist
        // keeps only the shelf (no open files). Staging had to touch the mirror
        // to give p4 something to shelve, but the revert undoes it, so the net
        // effect on the working tree is nothing.
        auto shelved = p4::shelve(*config, *cl);
        if (!shelved) return fail(shelved.error());
        auto reverted = p4::revertChangelist(*config, *cl);
        if (!reverted) {
            std::fprintf(stderr,
                         "gw prepare: shelved changelist %s, but reverting the "
                         "opened files failed: %s\n",
                         cl->c_str(), reverted.error().c_str());
            std::fprintf(stderr,
                         "The shelf exists, but the mirror still holds the "
                         "staged content and the files remain open. Run "
                         "'p4 revert -c %s <depot_path>' to restore the "
                         "mirror.\n",
                         cl->c_str());
            return 1;
        }
        std::printf("\nShelved changelist %s is ready - unshelve or review it "
                    "from P4V (or: p4 unshelve -s %s).\nThe mirror and working "
                    "tree are unchanged. Reconstruct it in Git any time with "
                    "'gw shelf import %s'.\n",
                    cl->c_str(), cl->c_str(), cl->c_str());
        return 0;
    }

    std::printf("\nChangelist %s is ready - review and submit it from P4V "
                "(or: p4 submit -c %s).\nAfter it is submitted, run "
                "'gw import' to absorb the new depot state.\n",
                cl->c_str(), cl->c_str());
    return 0;
}

}  // namespace p4gw
