#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "commands.h"
#include "config.h"
#include "git.h"
#include "p4.h"
#include "shelf.h"

namespace fs = std::filesystem;

namespace p4gw {

namespace {

void importUsage() {
    std::fprintf(stderr,
                 "usage: gw shelf import <changelist> [--branch <name>]\n");
}

// Writes the shelved content of `depotFile` (the @=cl revision) into the
// working-tree file at `dest`, creating parent directories. Used for adds and
// for binary edits, where a line merge isn't meaningful.
std::expected<void, std::string> writeShelvedContent(const Config& config,
                                                     const std::string& cl,
                                                     const std::string& depotFile,
                                                     const fs::path& dest) {
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    if (ec) {
        return std::unexpected("failed to create directory " +
                               dest.parent_path().string() + ": " + ec.message());
    }
    auto printed = p4::printDepotFile(config, depotFile + "@=" + cl,
                                      dest.string());
    if (!printed) return std::unexpected(printed.error());
    return {};
}

// Brings a P4 shelf into Git as a new branch off the baseline: branch at the
// latest imported depot state, then replay the shelf's delta on top with a
// git 3-way merge. The mirror is never touched and no P4 file is opened - all
// content is read with `p4 print`, so the result is independent of the
// mirror's sync/opened state.
int shelfImport(const Args& args) {
    std::string cl;
    std::string branchOverride;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--branch" && i + 1 < args.size()) {
            branchOverride = args[++i];
        } else if (!args[i].empty() && args[i][0] == '-') {
            std::fprintf(stderr, "gw shelf import: unknown option '%s'\n",
                         args[i].c_str());
            importUsage();
            return 1;
        } else if (cl.empty()) {
            cl = args[i];
        } else {
            std::fprintf(stderr, "gw shelf import: unexpected argument '%s'\n",
                         args[i].c_str());
            importUsage();
            return 1;
        }
    }
    if (cl.empty()) {
        std::fprintf(stderr, "gw shelf import: missing changelist number\n");
        importUsage();
        return 1;
    }

    std::string root;
    auto config = findAndLoadConfig(root);
    if (!config) {
        std::fprintf(stderr, "gw shelf import: %s\n", config.error().c_str());
        return 1;
    }

    // The branch is created off the baseline, so the baseline must exist and
    // the working tree must be clean (we are about to switch branches).
    const std::string& baseline = config->baselineBranch;
    auto baselineExists = git::branchExists(baseline, root);
    if (!baselineExists) {
        std::fprintf(stderr, "gw shelf import: %s\n",
                     baselineExists.error().c_str());
        return 1;
    }
    if (!*baselineExists) {
        std::fprintf(stderr,
                     "gw shelf import: baseline branch '%s' does not exist - "
                     "run 'gw import' first\n",
                     baseline.c_str());
        return 1;
    }
    auto dirty = git::isDirty(root);
    if (!dirty) {
        std::fprintf(stderr, "gw shelf import: %s\n", dirty.error().c_str());
        return 1;
    }
    if (*dirty) {
        std::fprintf(stderr,
                     "gw shelf import: working tree is not clean - commit, "
                     "stash, or gitignore your changes first\n");
        return 1;
    }

    auto described = p4::describeShelved(*config, cl);
    if (!described) {
        std::fprintf(stderr, "gw shelf import: %s\n", described.error().c_str());
        return 1;
    }
    auto shelf = parseShelveDescribe(*described);
    if (!shelf) {
        std::fprintf(stderr, "gw shelf import: %s\n", shelf.error().c_str());
        return 1;
    }
    if (shelf->files.empty()) {
        std::fprintf(stderr,
                     "gw shelf import: changelist %s has no shelved files "
                     "under %s\n",
                     cl.c_str(), config->depotPath.c_str());
        return 1;
    }

    const std::string branch =
        branchOverride.empty() ? "shelf-" + cl : branchOverride;
    auto branchTaken = git::branchExists(branch, root);
    if (branchTaken && *branchTaken) {
        std::fprintf(stderr,
                     "gw shelf import: branch '%s' already exists - pass "
                     "--branch <name> to choose another\n",
                     branch.c_str());
        return 1;
    }

    auto created = git::createBranch(branch, baseline, root);
    if (!created) {
        std::fprintf(stderr, "gw shelf import: %s\n", created.error().c_str());
        return 1;
    }

    // From here we are on the new branch; report it on any failure.
    auto fail = [&](const std::string& message) {
        std::fprintf(stderr, "gw shelf import: %s\n", message.c_str());
        std::fprintf(stderr,
                     "note: you are on a partially built branch '%s'; "
                     "'git switch %s && git branch -D %s' discards it\n",
                     branch.c_str(), baseline.c_str(), branch.c_str());
        return 1;
    };

    std::vector<std::string> conflicts;
    std::vector<std::string> skipped;
    int applied = 0;

    for (const auto& file : shelf->files) {
        const std::string rel =
            p4::depotRelativePath(config->depotPath, file.depotFile);
        if (rel.empty()) {
            skipped.push_back(file.depotFile);
            continue;
        }
        const fs::path dest = fs::path(root) / fs::path(rel);

        switch (file.action) {
        case ShelveAction::Delete:
        case ShelveAction::MoveDelete: {
            std::error_code ec;
            fs::remove(dest, ec);
            std::printf("  delete  %s\n", rel.c_str());
            ++applied;
            break;
        }
        case ShelveAction::Add:
        case ShelveAction::MoveAdd:
        case ShelveAction::Other: {
            auto wrote = writeShelvedContent(*config, cl, file.depotFile, dest);
            if (!wrote) return fail(wrote.error());
            std::printf("  add     %s\n", rel.c_str());
            ++applied;
            break;
        }
        case ShelveAction::Edit: {
            std::error_code ec;
            const bool haveOurs = fs::exists(dest, ec);
            // Binary content, no base revision, or a file the baseline does
            // not carry: a line merge is impossible or meaningless, so take
            // the shelved content wholesale.
            if (isBinaryType(file.type) || file.rev.empty() || !haveOurs) {
                auto wrote =
                    writeShelvedContent(*config, cl, file.depotFile, dest);
                if (!wrote) return fail(wrote.error());
                std::printf("  edit    %s%s\n", rel.c_str(),
                            isBinaryType(file.type) ? " (binary, took shelved)"
                                                    : "");
                ++applied;
                break;
            }
            const fs::path baseTmp =
                fs::temp_directory_path() / ("p4gw_shelf_base_" + file.rev);
            const fs::path theirsTmp =
                fs::temp_directory_path() / "p4gw_shelf_theirs";
            auto base = p4::printDepotFile(*config, file.depotFile + "#" +
                                                        file.rev,
                                           baseTmp.string());
            if (!base) return fail(base.error());
            auto theirs = p4::printDepotFile(*config, file.depotFile + "@=" + cl,
                                             theirsTmp.string());
            if (!theirs) return fail(theirs.error());
            auto merged = git::mergeFile(dest.string(), baseTmp.string(),
                                         theirsTmp.string(), root);
            fs::remove(baseTmp, ec);
            fs::remove(theirsTmp, ec);
            if (!merged) return fail(merged.error());
            if (*merged) {
                conflicts.push_back(rel);
                std::printf("  edit    %s (CONFLICT)\n", rel.c_str());
            } else {
                std::printf("  edit    %s\n", rel.c_str());
            }
            ++applied;
            break;
        }
        }
    }

    for (const auto& path : skipped) {
        std::printf("  skip    %s (outside %s)\n", path.c_str(),
                    config->depotPath.c_str());
    }

    const std::string message =
        (shelf->description.empty() ? "(no description)" : shelf->description) +
        "\n\nImported from shelved CL " + cl + ".\n";

    if (!conflicts.empty()) {
        std::printf("\n%zu file(s) had merge conflicts:\n", conflicts.size());
        for (const auto& path : conflicts) {
            std::printf("    %s\n", path.c_str());
        }
        std::printf(
            "\nThe shelf was based on an older depot state than '%s'. Resolve "
            "the\nconflict markers, then 'git add -A && git commit'. Suggested "
            "message:\n\n%s\n",
            baseline.c_str(), message.c_str());
        return 0;
    }

    auto added = git::addAll(root);
    if (!added) return fail(added.error());
    auto clean = git::indexMatchesHead(root);
    if (!clean) return fail(clean.error());
    if (*clean) {
        std::printf(
            "\nShelf %s matches '%s' exactly - nothing to commit on '%s'.\n",
            cl.c_str(), baseline.c_str(), branch.c_str());
        return 0;
    }
    auto committed = git::commit(message, root);
    if (!committed) return fail(committed.error());

    std::printf(
        "\nImported shelved CL %s onto new branch '%s' (%d file(s)).\n"
        "Work on it, then 'gw prepare' to build a fresh pending changelist.\n",
        cl.c_str(), branch.c_str(), applied);
    return 0;
}

// First line of a (possibly multi-line) description, trailing blanks trimmed.
std::string firstLine(const std::string& desc) {
    const size_t nl = desc.find('\n');
    std::string line = (nl == std::string::npos) ? desc : desc.substr(0, nl);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
        line.pop_back();
    }
    return line;
}

// `gw shelf list`: the caller's pending and shelved changelists under the
// configured subtree, newest first, so a CL number is easy to pick for
// `gw shelf import`.
int shelfList(const Args& args) {
    if (!args.empty()) {
        std::fprintf(stderr, "gw shelf list: unexpected argument '%s'\n",
                     args.front().c_str());
        std::fprintf(stderr, "usage: gw shelf list\n");
        return 1;
    }

    std::string root;
    auto config = findAndLoadConfig(root);
    if (!config) {
        std::fprintf(stderr, "gw shelf list: %s\n", config.error().c_str());
        return 1;
    }

    auto user = p4::currentUser(*config);
    if (!user) {
        std::fprintf(stderr, "gw shelf list: %s\n", user.error().c_str());
        return 1;
    }
    auto pending = p4::changes(*config, "pending", *user);
    if (!pending) {
        std::fprintf(stderr, "gw shelf list: %s\n", pending.error().c_str());
        return 1;
    }
    auto shelved = p4::changes(*config, "shelved", *user);
    if (!shelved) {
        std::fprintf(stderr, "gw shelf list: %s\n", shelved.error().c_str());
        return 1;
    }

    // Merge keyed by CL number (for newest-first ordering). Pending carries
    // the listing; the shelved query sets the flag and adds any shelf whose
    // files were reverted from the workspace (so it isn't in "pending").
    std::map<long long, ChangeListing> byCl;
    for (auto& c : parseChanges(*pending)) {
        byCl[std::stoll(c.change)] = c;
    }
    for (auto& c : parseChanges(*shelved)) {
        ChangeListing& entry = byCl[std::stoll(c.change)];
        if (entry.change.empty()) entry = c;  // shelf not open in the workspace
        entry.shelved = true;
    }

    if (byCl.empty()) {
        std::printf("No pending or shelved changelists under %s.\n",
                    config->depotPath.c_str());
        return 0;
    }

    std::printf("Pending and shelved changelists under %s:\n\n",
                config->depotPath.c_str());
    for (auto it = byCl.rbegin(); it != byCl.rend(); ++it) {
        const ChangeListing& c = it->second;
        std::printf("  %-8s %-9s %s\n", c.change.c_str(),
                    c.shelved ? "[shelved]" : "", firstLine(c.description).c_str());
    }
    std::printf("\nImport a shelf with: gw shelf import <changelist>\n");
    return 0;
}

}  // namespace

// `gw shelf <subcommand>`: work with P4 shelves. `list` shows pending/shelved
// changelists; `import` brings a shelf into Git as a branch.
int cmdShelf(const Args& args) {
    auto usage = [] {
        std::fprintf(stderr, "usage: gw shelf list\n"
                             "       gw shelf import <changelist> "
                             "[--branch <name>]\n");
    };
    if (args.empty()) {
        std::fprintf(stderr, "gw shelf: missing subcommand\n");
        usage();
        return 1;
    }
    const std::string sub = args.front();
    const Args rest(args.begin() + 1, args.end());
    if (sub == "list") return shelfList(rest);
    if (sub == "import") return shelfImport(rest);

    std::fprintf(stderr, "gw shelf: unknown subcommand '%s'\n", sub.c_str());
    usage();
    return 1;
}

}  // namespace p4gw
