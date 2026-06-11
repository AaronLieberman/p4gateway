#include <cstdio>
#include <cstring>
#include <string>

#include "commands.h"

namespace {

constexpr const char* kVersion = "0.1.0-dev";

void printUsage() {
    std::printf(
        "gw %s - develop in Git, submit via p4 reconcile\n"
        "\n"
        "usage: gw <command> [options]\n"
        "\n"
        "commands:\n"
        "  init    Set up a Git overlay inside a P4 workspace subtree\n"
        "  sync    p4 sync and commit the result to the baseline branch\n"
        "  submit  Turn the current branch into a pending P4 changelist\n"
        "  status  Show Git/P4 state at a glance\n"
        "  doctor  Check the environment for common configuration problems\n"
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

    if (command == "init") return p4gw::cmdInit(args);
    if (command == "sync") return p4gw::cmdSync(args);
    if (command == "submit") return p4gw::cmdSubmit(args);
    if (command == "status") return p4gw::cmdStatus(args);
    if (command == "doctor") return p4gw::cmdDoctor(args);

    std::fprintf(stderr, "gw: unknown command '%s'\n\n", command.c_str());
    printUsage();
    return 1;
}
