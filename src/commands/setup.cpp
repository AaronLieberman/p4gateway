// SPDX-License-Identifier: MIT

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "commands.h"
#include "config.h"

namespace fs = std::filesystem;

namespace p4gw {

namespace {

constexpr const char* kSetupUsage =
    "usage: gw setup [options]\n"
    "\n"
    "Write the p4gw.cfg config template in the current directory, with\n"
    "placeholders and comments for everything you still have to fill in. Makes\n"
    "no p4 or git calls (the offline half of getting started); 'gw init' then\n"
    "verifies the client view against this file and sets up the Git repo.\n"
    "\n"
    "options:\n"
    "  --depot-path <//depot/.../src/...>  Pre-fill the include's depot path\n"
    "                                      (must end with '/...')\n"
    "  --mirror-path <dir>                 Mirror directory (default: .p4gw)\n"
    "  --client <name>                     Pre-fill the P4 client name\n"
    "  --force                             Overwrite an existing p4gw.cfg here\n"
    "  -h, --help                          Show this help\n"
    "\n";

}  // namespace

// The offline half of getting started: writes the p4gw.cfg config template in
// the current directory, with placeholders and comments for everything the
// user still has to fill in. Makes no p4 or git calls. 'gw init' then
// verifies the client mapping against this file and sets up the Git repo.
int cmdSetup(const Args& args) {
    std::string depotPath;
    std::string mirrorPath = ".p4gw";
    std::string client;
    bool force = false;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--help" || args[i] == "-h") {
            std::printf("%s", kSetupUsage);
            return 0;
        } else if (args[i] == "--depot-path" && i + 1 < args.size()) {
            depotPath = args[++i];
        } else if (args[i] == "--mirror-path" && i + 1 < args.size()) {
            mirrorPath = args[++i];
        } else if (args[i] == "--client" && i + 1 < args.size()) {
            client = args[++i];
        } else if (args[i] == "--force") {
            force = true;
        } else {
            std::fprintf(stderr, "gw setup: unknown option '%s'\n",
                         args[i].c_str());
            std::fprintf(stderr, "%s", kSetupUsage);
            return 1;
        }
    }
    if (!depotPath.empty() && !depotPath.ends_with("/...")) {
        std::fprintf(stderr, "gw setup: --depot-path must end with '/...' "
                             "(got '%s')\n", depotPath.c_str());
        return 1;
    }

    const fs::path cwd = fs::current_path();
    const fs::path target = cwd / "p4gw.cfg";
    const std::string existing = findConfigFile(cwd.string());
    if (!existing.empty()) {
        if (fs::path(existing).parent_path() != cwd) {
            std::fprintf(stderr,
                         "gw setup: a p4gw.cfg already exists at %s - nested "
                         "overlays are not supported; run setup at that root "
                         "or remove it first\n", existing.c_str());
            return 1;
        }
        if (!force) {
            std::fprintf(stderr,
                         "gw setup: p4gw.cfg already exists here - edit it "
                         "directly, or rerun with --force to overwrite it\n");
            return 1;
        }
    }

    {
        std::ofstream file(target);
        if (!file) {
            std::fprintf(stderr, "gw setup: cannot write %s\n",
                         target.string().c_str());
            return 1;
        }
        file << "# gw configuration - personal (it names your client and "
                "local paths);\n"
                "# keep it out of Git and P4. Read by every gw command.\n"
                "\n"
                "# gw's view is an ordered list of 'include' / 'exclude' lines, "
                "resolved\n"
                "# later-wins per path - just like a p4 client view. An "
                "'include' ties a depot\n"
                "# subtree to the mirror directory the client view remaps it "
                "into; the mirror\n"
                "# always lives under the repo's single '.p4gw' container, and "
                "its path below\n"
                "# the container is the working-tree directory the subtree "
                "occupies\n"
                "# ('.p4gw/src' -> 'src/', '.p4gw' -> the whole repo). Add one "
                "'include' per\n"
                "# subtree; the starter .gitignore tracks only the mapped "
                "subtrees, so\n"
                "# unmapped directories stay out of Git. An 'exclude' carves a "
                "subtree back out\n"
                "# of an earlier 'include' (the client view drops it or syncs "
                "it in place; gw\n"
                "# keeps it out of the mirror and gitignores it, like unmapped "
                "depot content).\n"
                "# Intermix them freely, and an 'include' deeper than an "
                "'exclude' maps that\n"
                "# part back into the mirror (the win64-yes-linux-no pattern). "
                "An include's\n"
                "# depot path ends in '/...' (map the whole subtree) or '/*' "
                "(map only the\n"
                "# files directly in that directory, no sub-directories); an "
                "exclude is always\n"
                "# '/...' and must fall under a preceding include. Format and "
                "examples:\n"
                "#   include = <depot_path ending in /... or /*>  <mirror_path>\n"
                "#   include = //depot/yourproject/src/...     .p4gw/src\n"
                "#   include = //depot/yourproject/config/...  .p4gw/config\n"
                "#   exclude = //depot/yourproject/src/thirdparty/...\n"
                "#   exclude = //depot/yourproject/src/lib/...\n"
                "#   include = //depot/yourproject/src/lib/public/win64/... "
                ".p4gw/src/lib/public/win64\n"
                "# Direct files of a directory only (exclude it, then re-include "
                "with '/*'):\n"
                "#   exclude = //depot/yourproject/src/build/...\n"
                "#   include = //depot/yourproject/src/build/*  .p4gw/src/build\n";
        if (depotPath.empty()) {
            file << "#include = //depot/yourproject/src/... " << mirrorPath << "\n";
        } else {
            file << "include = " << depotPath << " " << mirrorPath << "\n";
        }
        file << "\n"
                "# Optional 'ignore' lines add extra .gitignore patterns "
                "(verbatim gitignore\n"
                "# syntax). The allowlist tracks a whole mapped subtree, but P4 "
                "ignores build\n"
                "# output and IDE state that would otherwise land in Git - list "
                "those here so\n"
                "# Git skips them too. Depot-specific, so share these with your "
                "team. One\n"
                "# pattern per line:\n"
                "#   ignore = /src/.vs/\n"
                "#   ignore = /src/**/*.vcxproj\n"
                "#   ignore = /src/**/*.pdb\n"
                "\n"
                "# P4 client name; leave commented out to use the ambient\n"
                "# P4CLIENT (e.g. from a .p4config / p4.ini).\n";
        if (client.empty()) {
            file << "#client = your-client-name\n";
        } else {
            file << "client = " << client << "\n";
        }
        file << "\n"
                "# How 'gw import' builds the depot snapshot. 'worktree' (the\n"
                "# default) stages it in a hidden git worktree, so import never\n"
                "# touches your checkout and works even with a dirty tree (it\n"
                "# just skips bringing your branch up until the tree is clean).\n"
                "# It uses about one extra copy of the source on disk. 'checkout'\n"
                "# stages the snapshot in your own working tree instead and needs\n"
                "# a clean tree.\n"
                "#import_mode = checkout\n"
                "\n"
                "# gw keeps a managed block in .rgignore so ripgrep still\n"
                "# searches unmapped depot content the allowlist .gitignore\n"
                "# hides (the file sits under the allowlist's '/*', so it never\n"
                "# reaches Git or P4). 'off' stops gw from touching .rgignore\n"
                "# and silences the matching 'gw doctor' check.\n"
                "#rgignore = off\n";
    }
    std::printf("Wrote %s\n", target.string().c_str());

    const std::string depotShown =
        depotPath.empty() ? "<depot_path>" : depotPath;
    const std::string clientShown = client.empty() ? "<your-client>" : client;
    int step = 1;
    std::printf("\nNext steps:\n");
    if (depotPath.empty()) {
        std::printf("%d. Edit p4gw.cfg and add an 'include' line per depot "
                    "subtree.\n", step++);
    }
    std::printf(
        "%d. Add a remap line to your client view (p4 client) for each "
        "include:\n"
        "\n"
        "     %s //%s/<workspace-relative path of %s>/...\n"
        "\n"
        "   so the depot subtree syncs into the mirror instead of this\n"
        "   directory. Later view lines win, so each remap must come after\n"
        "   any broader line it overlaps.\n",
        step++, depotShown.c_str(), clientShown.c_str(), mirrorPath.c_str());
    std::printf("%d. Run 'gw init' to verify the include(s) and set up the "
                "Git repo.\n", step);
    return 0;
}

}  // namespace p4gw