#include <cstdio>
#include <filesystem>
#include <fstream>

#include "commands.h"
#include "config.h"
#include "git.h"

namespace fs = std::filesystem;

namespace p4gw {

// Sets up the Git side of the overlay and tells the user how to set up the
// P4 side. Deliberately minimal: gw never edits the client spec and never
// moves files around — `gw doctor` verifies the mapping, `gw import` builds
// the baseline from the mirror once it has been synced.
//   1. Refuse if a .p4gw already exists here or in a parent.
//   2. git init (if needed; refuse to nest inside another repo).
//   3. Write .p4gw and a starter .gitignore; commit the .gitignore.
//   4. Print the client view line to add and the next steps.
int cmdInit(const Args& args) {
    std::string depotPath;
    std::string mirrorPath = "../p4gw-mirror";
    std::string client;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--depot-path" && i + 1 < args.size()) {
            depotPath = args[++i];
        } else if (args[i] == "--mirror-path" && i + 1 < args.size()) {
            mirrorPath = args[++i];
        } else if (args[i] == "--client" && i + 1 < args.size()) {
            client = args[++i];
        } else {
            std::fprintf(stderr, "gw init: unknown option '%s'\n",
                         args[i].c_str());
            std::fprintf(stderr,
                         "usage: gw init --depot-path //depot/.../src/... "
                         "[--mirror-path <dir>] [--client <name>]\n");
            return 1;
        }
    }
    if (depotPath.empty()) {
        std::fprintf(stderr, "gw init: --depot-path is required, e.g. "
                             "--depot-path //depot/yourgame/src/...\n");
        return 1;
    }
    if (!depotPath.ends_with("/...")) {
        std::fprintf(stderr, "gw init: --depot-path must end with '/...' "
                             "(got '%s')\n", depotPath.c_str());
        return 1;
    }

    std::string existingRoot;
    if (findAndLoadConfig(existingRoot)) {
        std::printf("Already initialized: %s/.p4gw exists. Nothing to do.\n",
                    existingRoot.c_str());
        return 0;
    }

    const std::string cwd = fs::current_path().string();
    auto toplevel = git::run({"rev-parse", "--show-toplevel"});
    if (toplevel) {
        if (!fs::equivalent(fs::path(*toplevel), fs::path(cwd))) {
            std::fprintf(stderr,
                         "gw init: this directory is inside the Git repo at "
                         "%s — run gw init at a repo root, not nested in "
                         "one\n", toplevel->c_str());
            return 1;
        }
        std::printf("Using the existing Git repo at %s\n", cwd.c_str());
    } else {
        Config defaults;
        auto initialized =
            git::run({"init", "-b", defaults.baselineBranch}, cwd);
        if (!initialized) {
            std::fprintf(stderr, "gw init: %s\n", initialized.error().c_str());
            return 1;
        }
        std::printf("Initialized empty Git repository in %s\n", cwd.c_str());
    }

    {
        std::ofstream file(fs::path(cwd) / ".p4gw");
        if (!file) {
            std::fprintf(stderr, "gw init: cannot write .p4gw\n");
            return 1;
        }
        file << "# Which slice of the depot this Git repo overlays. Every p4\n"
                "# command is scoped to this path — required.\n"
                "depot_path = " << depotPath << "\n"
                "\n"
                "# Where the client view maps depot_path to — gw's staging\n"
                "# area, synced by p4 and never edited by hand. Relative\n"
                "# paths are resolved against this directory.\n"
                "mirror_path = " << mirrorPath << "\n";
        if (!client.empty()) {
            file << "\n# P4 client name; omit to use the ambient P4CLIENT.\n"
                    "client = " << client << "\n";
        }
    }
    std::printf("Wrote .p4gw\n");

    const fs::path gitignore = fs::path(cwd) / ".gitignore";
    if (!fs::exists(gitignore)) {
        {
            // Close (flush) before `git add` sees the file.
            std::ofstream file(gitignore);
            file << "# gw's local config — personal, never goes to Git or P4\n"
                    ".p4gw\n";
        }
        std::printf("Wrote starter .gitignore\n");
        auto added = git::run({"add", ".gitignore"}, cwd);
        auto committed =
            added ? git::commit("gw init", cwd) : std::move(added);
        if (!committed) {
            std::printf("note: could not commit .gitignore (%s); commit it "
                        "yourself before 'gw import'\n",
                        committed.error().c_str());
        }
    } else {
        std::printf("Keeping the existing .gitignore — make sure it ignores "
                    ".p4gw\n");
    }

    // The one manual step: route the depot subtree into the mirror. Doctor
    // verifies it precisely once p4 can be asked about the client spec.
    const std::string clientName = client.empty() ? "<your-client>" : client;
    std::printf(
        "\nNext steps:\n"
        "1. Add this line at the END of your client view (p4 client):\n"
        "\n"
        "     %s //%s/<workspace-relative path of %s>/...\n"
        "\n"
        "   so the depot subtree syncs into the mirror instead of this\n"
        "   directory. Later view lines win, so keep it last.\n"
        "2. Run 'gw doctor' to verify the mapping.\n"
        "3. Sync (any tool you like) to populate the mirror.\n"
        "4. Run 'gw import' to create the '%s' baseline branch from it.\n",
        depotPath.c_str(), clientName.c_str(), mirrorPath.c_str(),
        Config{}.baselineBranch.c_str());
    return 0;
}

}  // namespace p4gw
