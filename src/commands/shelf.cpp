// SPDX-License-Identifier: MIT

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
#include "subprocess.h"
#include "shelf.h"

namespace fs = std::filesystem;

namespace p4gw {

namespace {

constexpr const char* kShelfImportUsage =
    "usage: gw shelf import <changelist> [options]\n"
    "\n"
    "Bring a P4 shelf into Git as a new branch off the baseline: branch at the\n"
    "latest imported depot state, then replay the shelf's delta on top with a\n"
    "git 3-way merge (conflicts surface as normal git markers). Reads everything\n"
    "with 'p4 print' - the mirror is never touched and no P4 file is opened.\n"
    "\n"
    "arguments:\n"
    "  <changelist>          The pending/shelved CL number to import\n"
    "\n"
    "options:\n"
    "  -b, --branch <name>   Name the new branch <name> (default: shelf-<cl>)\n"
    "  -h, --help            Show this help\n"
    "\n";

void importUsage() {
    std::fprintf(stderr, "%s", kShelfImportUsage);
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
        if (args[i] == "--help" || args[i] == "-h") {
            std::printf("%s", kShelfImportUsage);
            return 0;
        } else if ((args[i] == "--branch" || args[i] == "-b") &&
                   i + 1 < args.size()) {
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

    // The branch is created off the pristine depot baseline (the hidden ref),
    // so it must exist and the working tree must be clean (we are about to
    // switch branches).
    const std::string& baseline = config->baselineBranch;
    const std::string depotRef = depotTrackingRef(*config);
    if (!git::revParse(depotRef, root)) {
        std::fprintf(stderr,
                     "gw shelf import: no depot baseline yet - run 'gw import' "
                     "first\n");
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
                     "gw shelf import: changelist %s has no shelved files\n",
                     cl.c_str());
        return 1;
    }

    // Map a shelved depot file to its working-tree path through the include
    // that effectively owns it (later-wins over the ordered rules, so a
    // re-included subtree resolves to its own include). Returns false for files
    // under no include or under an `exclude` carve-out.
    auto locate = [&](const std::string& depotFile,
                      std::string& repoRel) -> bool {
        const ViewRule* rule = effectiveRuleForDepot(config->rules, depotFile);
        if (rule == nullptr || rule->exclude) return false;
        const std::string rel =
            p4::depotRelativePath(rule->depotPath, depotFile);
        if (rel.empty()) return false;
        repoRel = rule->repoSubtree.empty() ? rel
                                            : rule->repoSubtree + "/" + rel;
        return true;
    };

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

    auto created = git::createBranch(branch, depotRef, root);
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
        std::string rel;
        if (!locate(file.depotFile, rel)) {
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
                uniqueTempFile("p4gw_shelf_base", "_" + file.rev);
            const fs::path theirsTmp = uniqueTempFile("p4gw_shelf_theirs");
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
        std::printf("  skip    %s (outside the configured mappings)\n",
                    path.c_str());
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

constexpr const char* kShelfListUsage =
    "usage: gw shelf list [options]\n"
    "\n"
    "List your pending and shelved changelists under the configured subtree,\n"
    "newest first, so a CL number is easy to pick for 'gw shelf import'. By\n"
    "default only the current workspace's changelists are shown.\n"
    "\n"
    "options:\n"
    "  -a, --all          List across every workspace you own, not just this one\n"
    "  -u, --user <name>  List another user's changelists (implies --all, since\n"
    "                     their shelves live in their own workspaces)\n"
    "  -h, --help         Show this help\n"
    "\n";

void listUsage() {
    std::fprintf(stderr, "%s", kShelfListUsage);
}

// `gw shelf list`: the caller's pending and shelved changelists under the
// configured subtree, newest first, so a CL number is easy to pick for
// `gw shelf import`. By default only the current workspace's changelists are
// shown; `--all` widens to every workspace the user owns, and `--user <name>`
// lists another user's changelists (implying `--all`, since their shelves live
// in their own workspaces).
int shelfList(const Args& args) {
    bool all = false;
    std::string userOverride;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--help" || args[i] == "-h") {
            std::printf("%s", kShelfListUsage);
            return 0;
        } else if (args[i] == "--all" || args[i] == "-a") {
            all = true;
        } else if (args[i] == "--user" || args[i] == "-u") {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr,
                             "gw shelf list: --user requires a username\n");
                listUsage();
                return 1;
            }
            userOverride = args[++i];
            all = true;  // another user's shelves span their own workspaces
        } else {
            std::fprintf(stderr, "gw shelf list: unexpected argument '%s'\n",
                         args[i].c_str());
            listUsage();
            return 1;
        }
    }

    std::string root;
    auto config = findAndLoadConfig(root);
    if (!config) {
        std::fprintf(stderr, "gw shelf list: %s\n", config.error().c_str());
        return 1;
    }

    std::string user = userOverride;
    if (user.empty()) {
        auto current = p4::currentUser(*config);
        if (!current) {
            std::fprintf(stderr, "gw shelf list: %s\n", current.error().c_str());
            return 1;
        }
        user = *current;
    }

    // Default scope is the current workspace; `--all`/`--user` clears it to
    // list across every workspace. The config may name the client explicitly
    // or rely on the ambient P4CLIENT (queried via `p4 info`).
    std::string client;
    if (!all) {
        if (!config->client.empty()) {
            client = config->client;
        } else {
            auto current = p4::currentClient(*config);
            if (!current) {
                std::fprintf(stderr, "gw shelf list: %s\n",
                             current.error().c_str());
                return 1;
            }
            client = *current;
        }
    }

    auto pending = p4::changes(*config, "pending", user, client);
    if (!pending) {
        std::fprintf(stderr, "gw shelf list: %s\n", pending.error().c_str());
        return 1;
    }
    auto shelved = p4::changes(*config, "shelved", user, client);
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

    // Describe the scope so it is clear whether the list is the current
    // workspace, every workspace, or another user's changelists.
    std::string scope;
    if (!userOverride.empty()) {
        scope = "for user '" + user + "' across all workspaces";
    } else if (all) {
        scope = "across all your workspaces";
    } else {
        scope = "in workspace '" + client + "'";
    }

    if (byCl.empty()) {
        std::printf("No pending or shelved changelists under the configured "
                    "mappings %s.\n",
                    scope.c_str());
        return 0;
    }

    std::printf("Pending and shelved changelists under the configured "
                "mappings %s:\n\n",
                scope.c_str());
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
    static constexpr const char* kShelfUsage =
        "usage: gw shelf <subcommand> [options]\n"
        "\n"
        "Work with P4 shelves from Git.\n"
        "\n"
        "subcommands:\n"
        "  list    Show your pending and shelved changelists, newest first\n"
        "  import  Bring a shelved changelist into Git as a new branch\n"
        "\n"
        "Run 'gw shelf <subcommand> --help' for that subcommand's options.\n"
        "\n"
        "options:\n"
        "  -h, --help  Show this help\n"
        "\n";
    auto usage = [] { std::fprintf(stderr, "%s", kShelfUsage); };
    if (args.empty()) {
        std::fprintf(stderr, "gw shelf: missing subcommand\n");
        usage();
        return 1;
    }
    const std::string sub = args.front();
    if (sub == "--help" || sub == "-h") {
        std::printf("%s", kShelfUsage);
        return 0;
    }
    const Args rest(args.begin() + 1, args.end());
    if (sub == "list") return shelfList(rest);
    if (sub == "import") return shelfImport(rest);

    std::fprintf(stderr, "gw shelf: unknown subcommand '%s'\n", sub.c_str());
    usage();
    return 1;
}

}  // namespace p4gw