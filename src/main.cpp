#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "commands.h"
#include "subprocess.h"

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
        "  shelf    Work with P4 shelves: list, or import <cl> into Git\n"
        "  doctor   Check the environment and the client-view mapping\n"
        "\n"
        "  integtest  Live-P4 integration tests: run|clean "
        "(see README-integtest.md)\n"
        "\n"
        "Run 'gw <command> --help' for a command's details and options.\n"
        "\n"
        "global options:\n"
        "  --verbose  Echo every git/p4 command as it runs\n"
        "  --help     Show this help\n"
        "  --version  Show version\n"
        "\n",
        kVersion);
}

}  // namespace

int main(int argc, char** argv) {
    // Pull the global `--verbose` flag out wherever it appears (before or after
    // the command) so it works as `gw --verbose status` or `gw status
    // --verbose`, and individual commands neither see it nor reject it as an
    // unknown option; it controls the process layer's command echoing. What
    // remains is the command name followed by that command's own arguments.
    std::vector<std::string> tokens;
    for (char** arg = argv + 1; arg != argv + argc; ++arg) {
        if (std::strcmp(*arg, "--verbose") == 0) {
            p4gw::setVerbose(true);
        } else {
            tokens.emplace_back(*arg);
        }
    }

    if (tokens.empty() || tokens[0] == "--help" || tokens[0] == "-h" ||
        tokens[0] == "help") {
        printUsage();
        return tokens.empty() ? 1 : 0;
    }
    if (tokens[0] == "--version") {
        std::printf("gw %s\n", kVersion);
        return 0;
    }

    const std::string command = tokens[0];
    const p4gw::Args args(tokens.begin() + 1, tokens.end());

    if (command == "setup") return p4gw::cmdSetup(args);
    if (command == "init") return p4gw::cmdInit(args);
    if (command == "import") return p4gw::cmdImport(args);
    if (command == "prepare") return p4gw::cmdPrepare(args);
    if (command == "status") return p4gw::cmdStatus(args);
    if (command == "shelf") return p4gw::cmdShelf(args);
    if (command == "doctor") return p4gw::cmdDoctor(args);
    if (command == "integtest") return p4gw::cmdIntegtest(argv[0], args);

    std::fprintf(stderr, "gw: unknown command '%s'\n\n", command.c_str());
    printUsage();
    return 1;
}
