#include <cstdio>
#include <cstring>
#include <string>

#include "commands.h"

namespace {

constexpr const char* kVersion = "0.1.0-dev";

void printUsage() {
    std::printf(
        "gw %s - work in Git, ship Perforce changelists\n"
        "\n"
        "usage: gw <command> [options]\n"
        "\n"
        "commands:\n"
        "  setup    Write the p4gw.cfg config template (offline; edit it, then run init)\n"
        "  init     Verify the client-view mapping and set up the Git repo\n"
        "  import   Commit the synced mirror state to the baseline branch (--rebase)\n"
        "  prepare  Open the current branch's changes in a pending P4 changelist\n"
        "  status   Show Git/P4 state at a glance\n"
        "  doctor   Check the environment and the client-view mapping\n"
        "\n"
        "  integtest  Live-P4 integration tests: init|run "
        "(see PLAN-integrationtests.md)\n"
        "\n"
        "global options:\n"
        "  --help     Show this help\n"
        "  --version  Show version\n",
        kVersion);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || std::strcmp(argv[1], "--help") == 0 ||
        std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "help") == 0) {
        printUsage();
        return argc < 2 ? 1 : 0;
    }
    if (std::strcmp(argv[1], "--version") == 0) {
        std::printf("gw %s\n", kVersion);
        return 0;
    }

    const std::string command = argv[1];
    const p4gw::Args args(argv + 2, argv + argc);

    if (command == "setup") return p4gw::cmdSetup(args);
    if (command == "init") return p4gw::cmdInit(args);
    if (command == "import") return p4gw::cmdImport(args);
    if (command == "prepare") return p4gw::cmdPrepare(args);
    if (command == "status") return p4gw::cmdStatus(args);
    if (command == "doctor") return p4gw::cmdDoctor(args);
    if (command == "integtest") return p4gw::cmdIntegtest(argv[0], args);

    std::fprintf(stderr, "gw: unknown command '%s'\n\n", command.c_str());
    printUsage();
    return 1;
}
