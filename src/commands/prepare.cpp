#include <cstdio>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

#include "commands.h"
#include "config.h"
#include "git.h"
#include "p4.h"
#include "p4ops.h"

namespace fs = std::filesystem;

namespace p4gw {

namespace {

// A mapping with its mirror directory resolved to an absolute path.
struct ResolvedMapping {
    const Mapping* mapping;
    std::string mirrorDir;
};

// The mapping that owns a repo-relative path: the one whose repoSubtree is the
// longest matching prefix (an empty subtree, i.e. the whole repo, is the
// catch-all). Returns nullptr for paths under no mapping (pure Git files like
// bin/ or content/).
const ResolvedMapping* routeFor(const std::vector<ResolvedMapping>& resolved,
                                const std::string& repoRel) {
    const ResolvedMapping* best = nullptr;
    size_t bestLen = 0;
    for (const auto& r : resolved) {
        const std::string& sub = r.mapping->repoSubtree;
        const bool matches =
            sub.empty() || repoRel.starts_with(sub + "/");
        if (!matches) continue;
        // Prefer the most specific subtree; an empty subtree wins only when
        // nothing more specific matches.
        const size_t len = sub.empty() ? 0 : sub.size() + 1;
        if (best == nullptr || len > bestLen) {
            best = &r;
            bestLen = len;
        }
    }
    return best;
}

// Local path of a repo-relative file inside its mapping's mirror, for p4
// commands: the path with the mapping's subtree prefix stripped, rooted at the
// mirror directory.
std::string mirrorFilePath(const ResolvedMapping& route,
                           const std::string& repoRel) {
    const std::string& sub = route.mapping->repoSubtree;
    const std::string within =
        sub.empty() ? repoRel : repoRel.substr(sub.size() + 1);
    return (fs::path(route.mirrorDir) / fs::path(within)).make_preferred()
        .string();
}

// Stages the content of `HEAD:repoRel` into the mirror file at `dest`
// (creating directories, clearing a read-only bit left by p4).
std::expected<std::string, std::string> stageBlob(const std::string& root,
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
    return git::catBlobToFile("HEAD", repoRel, dest, root);
}

}  // namespace

// Turns the commits on the current branch (relative to the baseline branch)
// into a pending P4 changelist, without touching the user's working tree:
//   1. Preflight: baseline is an ancestor of HEAD with commits on top.
//   2. Create a numbered pending CL described by the commit messages.
//   3. Stage the branch's file state into the mirror with explicit
//      p4 edit/add/delete/move - we know exactly what changed from Git.
//   4. Verify with a scoped `p4 reconcile -n`: anything it still finds is
//      an unexpected mirror/depot mismatch and gets a loud warning.
// gw never submits; review the CL and submit it from P4V.
int cmdPrepare(const Args& args) {
    bool verify = true;
    std::string messageOverride;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--no-verify") {
            verify = false;
        } else if (args[i] == "--message" && i + 1 < args.size()) {
            messageOverride = args[++i];
        } else {
            std::fprintf(stderr, "gw prepare: unknown option '%s'\n",
                         args[i].c_str());
            std::fprintf(stderr,
                         "usage: gw prepare [--message <text>] [--no-verify]\n");
            return 1;
        }
    }

    std::string root;
    auto config = findAndLoadConfig(root);
    if (!config) {
        std::fprintf(stderr, "gw prepare: %s\n", config.error().c_str());
        return 1;
    }
    std::vector<ResolvedMapping> resolved;
    for (const auto& mapping : config->mappings) {
        const std::string mirrorDir = resolveMirrorPath(mapping.mirrorPath, root);
        if (!fs::exists(mirrorDir)) {
            std::fprintf(stderr,
                         "gw prepare: mirror directory %s does not exist - "
                         "check the client view mapping ('gw doctor')\n",
                         mirrorDir.c_str());
            return 1;
        }
        resolved.push_back({&mapping, mirrorDir});
    }

    const std::string& baseline = config->baselineBranch;
    auto branch = git::currentBranch(root);
    if (!branch) {
        std::fprintf(stderr, "gw prepare: %s\n", branch.error().c_str());
        return 1;
    }
    if (*branch == baseline) {
        std::fprintf(stderr,
                     "gw prepare: you are on '%s' (the baseline branch) - "
                     "switch to the feature branch you want to ship\n",
                     baseline.c_str());
        return 1;
    }
    auto baselineExists = git::branchExists(baseline, root);
    if (!baselineExists) {
        std::fprintf(stderr, "gw prepare: %s\n", baselineExists.error().c_str());
        return 1;
    }
    if (!*baselineExists) {
        std::fprintf(stderr,
                     "gw prepare: baseline branch '%s' does not exist - run "
                     "'gw import' first\n",
                     baseline.c_str());
        return 1;
    }
    auto ancestor = git::isAncestor(baseline, "HEAD", root);
    if (!ancestor) {
        std::fprintf(stderr, "gw prepare: %s\n", ancestor.error().c_str());
        return 1;
    }
    if (!*ancestor) {
        std::fprintf(stderr,
                     "gw prepare: '%s' is not an ancestor of HEAD - run "
                     "'gw import --rebase' to rebase onto the latest depot "
                     "state first\n",
                     baseline.c_str());
        return 1;
    }

    auto changes = git::diffNameStatus(baseline, "HEAD", root);
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
        std::printf("Nothing to prepare: no file changes between '%s' and "
                    "HEAD.\n", baseline.c_str());
        return 0;
    }

    // Route each op to the mapping that owns its path. Changes outside every
    // mapping (pure-Git directories like bin/ or content/) are not shipped to
    // P4; collect them for an informational note. A rename across mapping
    // boundaries can't be a single `p4 move`, so it degrades to delete + add.
    struct Staged {
        std::string repoRel;     // HEAD:repoRel is the source content
        std::string mirrorPath;  // local mirror file to stage/open
    };
    struct MoveOp {
        std::string fromMirror;
        std::string toMirror;
        std::string repoTo;      // HEAD:repoTo content for the destination
    };
    std::vector<Staged> edits;
    std::vector<Staged> adds;
    std::vector<std::string> deletes;  // local mirror paths (p4 clears them)
    std::vector<MoveOp> moves;
    std::vector<std::string> unmapped;

    for (const auto& op : *ops) {
        switch (op.kind) {
        case P4Op::Kind::Edit: {
            const auto* route = routeFor(resolved, op.path);
            if (route) edits.push_back({op.path, mirrorFilePath(*route, op.path)});
            else unmapped.push_back(op.path);
            break;
        }
        case P4Op::Kind::Add: {
            const auto* route = routeFor(resolved, op.path);
            if (route) adds.push_back({op.path, mirrorFilePath(*route, op.path)});
            else unmapped.push_back(op.path);
            break;
        }
        case P4Op::Kind::Delete: {
            const auto* route = routeFor(resolved, op.path);
            if (route) deletes.push_back(mirrorFilePath(*route, op.path));
            else unmapped.push_back(op.path);
            break;
        }
        case P4Op::Kind::Move: {
            const auto* from = routeFor(resolved, op.path);
            const auto* to = routeFor(resolved, op.newPath);
            if (from && to && from->mapping == to->mapping) {
                moves.push_back({mirrorFilePath(*from, op.path),
                                 mirrorFilePath(*to, op.newPath), op.newPath});
            } else {
                // Cross-boundary (or partly-unmapped) rename: handle each end
                // on its own side of the p4 boundary.
                if (from) deletes.push_back(mirrorFilePath(*from, op.path));
                else unmapped.push_back(op.path);
                if (to) adds.push_back({op.newPath, mirrorFilePath(*to, op.newPath)});
                else unmapped.push_back(op.newPath);
            }
            break;
        }
        }
    }

    if (edits.empty() && adds.empty() && deletes.empty() && moves.empty()) {
        std::printf("Nothing to prepare: the changed files are all outside the "
                    "configured p4 mappings (pure Git).\n");
        return 0;
    }

    std::string description = messageOverride;
    if (description.empty()) {
        auto messages = git::commitMessages(baseline, "HEAD", root);
        if (!messages) {
            std::fprintf(stderr, "gw prepare: %s\n", messages.error().c_str());
            return 1;
        }
        description = *messages;
    }

    // Refuse to run if files are already open in the mirror (a previous
    // prepare's still-pending CL, a stray p4 edit). Opening them again would
    // silently move them between changelists; the user must resolve the
    // existing opens first. (A future --update/--abandon flow, PLAN.md M3,
    // will let the user opt in.)
    auto opened = p4::openedFilesTagged(*config);
    if (!opened) {
        std::fprintf(stderr, "gw prepare: %s\n", opened.error().c_str());
        return 1;
    }
    if (!opened->empty()) {
        std::fprintf(stderr,
                     "gw prepare: %zu file(s) are already open in P4 under the "
                     "configured mappings:\n",
                     opened->size());
        for (const auto& o : *opened) {
            std::fprintf(stderr, "  %s %s\n", o.action.c_str(),
                         o.depotFile.c_str());
        }
        std::fprintf(stderr,
                     "Opening them again would move them between changelists. "
                     "Submit or revert\nthe existing changelist first (see "
                     "'gw status'), then rerun 'gw prepare'.\n");
        return 1;
    }

    auto cl = p4::createChangelist(*config, description);
    if (!cl) {
        std::fprintf(stderr, "gw prepare: %s\n", cl.error().c_str());
        return 1;
    }
    std::printf("Created pending changelist %s\n", cl->c_str());

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
            auto staged = stageBlob(root, s.repoRel, s.mirrorPath);
            if (!staged) return fail(staged.error());
        }
    }
    for (const auto& move : moves) {
        auto edited = p4::editFiles(*config, *cl, {move.fromMirror});
        if (!edited) return fail(edited.error());
        auto moved =
            p4::moveFile(*config, *cl, move.fromMirror, move.toMirror);
        if (!moved) return fail(moved.error());
        auto staged = stageBlob(root, move.repoTo, move.toMirror);
        if (!staged) return fail(staged.error());
    }
    if (!adds.empty()) {
        for (const auto& s : adds) {
            auto staged = stageBlob(root, s.repoRel, s.mirrorPath);
            if (!staged) return fail(staged.error());
        }
        auto result = p4::addFiles(*config, *cl, mirrorPathsOf(adds));
        if (!result) return fail(result.error());
    }

    for (const auto& op : *ops) {
        const bool srcMapped = routeFor(resolved, op.path) != nullptr;
        switch (op.kind) {
        case P4Op::Kind::Edit:
            if (srcMapped) std::printf("  edit    %s\n", op.path.c_str());
            break;
        case P4Op::Kind::Add:
            if (srcMapped) std::printf("  add     %s\n", op.path.c_str());
            break;
        case P4Op::Kind::Delete:
            if (srcMapped) std::printf("  delete  %s\n", op.path.c_str());
            break;
        case P4Op::Kind::Move:
            if (srcMapped || routeFor(resolved, op.newPath)) {
                std::printf("  move    %s -> %s\n", op.path.c_str(),
                            op.newPath.c_str());
            }
            break;
        }
    }
    if (!unmapped.empty()) {
        std::printf("\n%zu changed file(s) are outside any p4 mapping and were "
                    "NOT added to the changelist (they stay in Git only):\n",
                    unmapped.size());
        for (const auto& path : unmapped) {
            std::printf("  skip    %s\n", path.c_str());
        }
    }

    if (verify) {
        auto preview = p4::reconcilePreview(*config);
        if (!preview) {
            std::fprintf(stderr, "gw prepare: verify failed: %s\n",
                         preview.error().c_str());
        } else if (!preview->empty()) {
            std::printf(
                "\nWARNING: the mirror does not fully match this branch.\n"
                "p4 reconcile would additionally open:\n%s"
                "This usually means the mirror was modified outside gw or a "
                "sync landed\nmid-prepare. Inspect before submitting. "
                "(--no-verify skips this check.)\n",
                preview->c_str());
        } else {
            std::printf("Verified: mirror matches the branch exactly.\n");
        }
    }

    std::printf("\nChangelist %s is ready - review and submit it from P4V "
                "(or: p4 submit -c %s).\nAfter it is submitted, run "
                "'gw import' to absorb the new depot state into '%s'.\n",
                cl->c_str(), cl->c_str(), baseline.c_str());
    return 0;
}

}  // namespace p4gw
