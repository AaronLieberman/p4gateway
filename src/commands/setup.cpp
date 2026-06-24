#include <cstdio>
#include <filesystem>
#include <fstream>

#include "commands.h"
#include "config.h"

namespace fs = std::filesystem;

namespace p4gw {

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
        if (args[i] == "--depot-path" && i + 1 < args.size()) {
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
            std::fprintf(stderr,
                         "usage: gw setup [--depot-path //depot/.../src/...] "
                         "[--mirror-path <dir>] [--client <name>] [--force]\n");
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
                "# Each 'mapping' line ties a depot subtree to the mirror "
                "directory the\n"
                "# client view remaps it into. The mirror always lives under "
                "the repo's\n"
                "# single '.p4gw' container; its path below the container is "
                "the working-\n"
                "# tree directory the subtree occupies ('.p4gw/src' -> 'src/', "
                "'.p4gw' ->\n"
                "# the whole repo). Add one line per subtree; the starter "
                ".gitignore tracks\n"
                "# only the mapped subtrees, so unmapped directories stay out "
                "of Git. Format:\n"
                "#   mapping = <depot_path ending in /...>  <mirror_path>\n"
                "# Example (two subtrees):\n"
                "#   mapping = //depot/yourgame/src/...     .p4gw/src\n"
                "#   mapping = //depot/yourgame/config/...  .p4gw/config\n";
        if (depotPath.empty()) {
            file << "#mapping = //depot/yourgame/src/... " << mirrorPath << "\n";
        } else {
            file << "mapping = " << depotPath << " " << mirrorPath << "\n";
        }
        file << "\n"
                "# Optional 'exclude' lines carve a depot subtree out of the "
                "mapping above it:\n"
                "# the client view drops it (a '-' line) or syncs it in place, "
                "and gw keeps\n"
                "# it out of the mirror and gitignores it - just like unmapped "
                "depot content,\n"
                "# even though it lives under a mapped subtree. One line per "
                "carved-out path,\n"
                "# each ending in '/...' and under its mapping's depot path:\n"
                "#   exclude = //depot/yourgame/src/thirdparty/...\n"
                "#   exclude = //depot/yourgame/src/devtools/...\n"
                "\n"
                "# P4 client name; leave commented out to use the ambient\n"
                "# P4CLIENT (e.g. from a .p4config / p4.ini).\n";
        if (client.empty()) {
            file << "#client = your-client-name\n";
        } else {
            file << "client = " << client << "\n";
        }
    }
    std::printf("Wrote %s\n", target.string().c_str());

    const std::string depotShown =
        depotPath.empty() ? "<depot_path>" : depotPath;
    const std::string clientShown = client.empty() ? "<your-client>" : client;
    int step = 1;
    std::printf("\nNext steps:\n");
    if (depotPath.empty()) {
        std::printf("%d. Edit p4gw.cfg and add a 'mapping' line per depot "
                    "subtree.\n", step++);
    }
    std::printf(
        "%d. Add a remap line to your client view (p4 client) for each "
        "mapping:\n"
        "\n"
        "     %s //%s/<workspace-relative path of %s>/...\n"
        "\n"
        "   so the depot subtree syncs into the mirror instead of this\n"
        "   directory. Later view lines win, so each remap must come after\n"
        "   any broader line it overlaps.\n",
        step++, depotShown.c_str(), clientShown.c_str(), mirrorPath.c_str());
    std::printf("%d. Run 'gw init' to verify the mapping(s) and set up the "
                "Git repo.\n", step);
    return 0;
}

}  // namespace p4gw
