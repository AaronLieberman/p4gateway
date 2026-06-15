#include <cstdio>
#include <filesystem>
#include <fstream>

#include "commands.h"
#include "config.h"

namespace fs = std::filesystem;

namespace p4gw {

// The offline half of getting started: writes the .p4gw config template in
// the current directory, with placeholders and comments for everything the
// user still has to fill in. Makes no p4 or git calls. 'gw init' then
// verifies the client mapping against this file and sets up the Git repo.
int cmdSetup(const Args& args) {
    std::string depotPath;
    std::string mirrorPath = "../p4gw-mirror";
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
    const fs::path target = cwd / ".p4gw";
    const std::string existing = findConfigFile(cwd.string());
    if (!existing.empty()) {
        if (fs::path(existing).parent_path() != cwd) {
            std::fprintf(stderr,
                         "gw setup: a .p4gw already exists at %s - nested "
                         "overlays are not supported; run setup at that root "
                         "or remove it first\n", existing.c_str());
            return 1;
        }
        if (!force) {
            std::fprintf(stderr,
                         "gw setup: .p4gw already exists here - edit it "
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
                "# Which slice of the depot this Git repo overlays. Every p4\n"
                "# command is scoped to this path - required. Example:\n"
                "#   depot_path = //depot/yourgame/src/...\n"
                "depot_path = " << depotPath << "\n"
                "\n"
                "# Where the client view maps depot_path to - gw's staging\n"
                "# area, synced by p4 and never edited by hand. Relative\n"
                "# paths are resolved against the directory containing this\n"
                "# file.\n"
                "mirror_path = " << mirrorPath << "\n"
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
        std::printf("%d. Edit .p4gw and fill in depot_path.\n", step++);
    }
    std::printf(
        "%d. Add this line at the END of your client view (p4 client):\n"
        "\n"
        "     %s //%s/<workspace-relative path of %s>/...\n"
        "\n"
        "   so the depot subtree syncs into the mirror instead of this\n"
        "   directory. Later view lines win, so keep it last.\n",
        step++, depotShown.c_str(), clientShown.c_str(), mirrorPath.c_str());
    std::printf("%d. Run 'gw init' to verify the mapping and set up the Git "
                "repo.\n", step);
    return 0;
}

}  // namespace p4gw
