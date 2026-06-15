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

// Repo-relative path -> local path inside the mirror, for p4 commands.
std::string mirrorFilePath(const std::string& mirrorDir, const std::string& rel) {
    return (fs::path(mirrorDir) / fs::path(rel)).make_preferred().string();
}

// Stages the content of `HEAD:path` into the mirror (creating directories,
// clearing a read-only bit left by p4).
std::expected<std::string, std::string> stageBlob(const std::string& root,
                                                  const std::string& mirrorDir,
                                                  const std::string& path) {
    const fs::path dest = fs::path(mirrorDir) / fs::path(path);
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    if (ec) {
        return std::unexpected("failed to create directory " +
                               dest.parent_path().string() + ": " + ec.message());
    }
    if (fs::exists(dest, ec)) {
        fs::permissions(dest, fs::perms::owner_write, fs::perm_options::add, ec);
    }
    return git::catBlobToFile("HEAD", path, dest.string(), root);
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
    if (config->mirrorPath.empty()) {
        std::fprintf(stderr,
                     "gw prepare: no 'mirror_path' in p4gw.cfg - add it (see "
                     "'gw init')\n");
        return 1;
    }
    const std::string mirrorDir = resolveMirrorPath(*config, root);
    if (!fs::exists(mirrorDir)) {
        std::fprintf(stderr,
                     "gw prepare: mirror directory %s does not exist - check "
                     "the client view mapping ('gw doctor')\n",
                     mirrorDir.c_str());
        return 1;
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
                     "gw prepare: %zu file(s) are already open in P4 under "
                     "%s:\n",
                     opened->size(), config->depotPath.c_str());
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

    // The ops are already in execution order (deletes, edits, moves, adds);
    // batch the uniform kinds, run moves individually. Staging happens after
    // a file is opened (edit/move) and before `p4 add`; p4 itself removes
    // deleted files from the mirror.
    std::vector<std::string> editPaths;
    std::vector<std::string> addPaths;
    std::vector<std::string> deletePaths;
    for (const auto& op : *ops) {
        switch (op.kind) {
        case P4Op::Kind::Edit:   editPaths.push_back(op.path); break;
        case P4Op::Kind::Add:    addPaths.push_back(op.path); break;
        case P4Op::Kind::Delete: deletePaths.push_back(op.path); break;
        case P4Op::Kind::Move:   break;  // handled below
        }
    }

    auto toMirrorPaths = [&](const std::vector<std::string>& paths) {
        std::vector<std::string> local;
        local.reserve(paths.size());
        for (const auto& path : paths) {
            local.push_back(mirrorFilePath(mirrorDir, path));
        }
        return local;
    };

    if (!deletePaths.empty()) {
        auto result = p4::deleteFiles(*config, *cl, toMirrorPaths(deletePaths));
        if (!result) return fail(result.error());
    }
    if (!editPaths.empty()) {
        auto result = p4::editFiles(*config, *cl, toMirrorPaths(editPaths));
        if (!result) return fail(result.error());
        for (const auto& path : editPaths) {
            auto staged = stageBlob(root, mirrorDir, path);
            if (!staged) return fail(staged.error());
        }
    }
    for (const auto& op : *ops) {
        if (op.kind != P4Op::Kind::Move) continue;
        auto edited = p4::editFiles(*config, *cl,
                                    {mirrorFilePath(mirrorDir, op.path)});
        if (!edited) return fail(edited.error());
        auto moved = p4::moveFile(*config, *cl,
                                  mirrorFilePath(mirrorDir, op.path),
                                  mirrorFilePath(mirrorDir, op.newPath));
        if (!moved) return fail(moved.error());
        auto staged = stageBlob(root, mirrorDir, op.newPath);
        if (!staged) return fail(staged.error());
    }
    if (!addPaths.empty()) {
        for (const auto& path : addPaths) {
            auto staged = stageBlob(root, mirrorDir, path);
            if (!staged) return fail(staged.error());
        }
        auto result = p4::addFiles(*config, *cl, toMirrorPaths(addPaths));
        if (!result) return fail(result.error());
    }

    for (const auto& op : *ops) {
        switch (op.kind) {
        case P4Op::Kind::Edit:
            std::printf("  edit    %s\n", op.path.c_str());
            break;
        case P4Op::Kind::Add:
            std::printf("  add     %s\n", op.path.c_str());
            break;
        case P4Op::Kind::Delete:
            std::printf("  delete  %s\n", op.path.c_str());
            break;
        case P4Op::Kind::Move:
            std::printf("  move    %s -> %s\n", op.path.c_str(),
                        op.newPath.c_str());
            break;
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
