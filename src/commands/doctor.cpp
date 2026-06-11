#include <cstdio>

#include "commands.h"
#include "process.h"

namespace p4gw {

// Checks that the environment is sane for the overlay workflow. Currently
// verifies the required tools are reachable; milestone M1 adds checks for
// the P4 connection, client `allwrite` option, LineEnd vs core.autocrlf
// agreement, and that no files are opened outside gw's changelists.
int cmdDoctor(const Args& args) {
    (void)args;

    int failures = 0;

    auto git = run("git", {"--version"});
    if (git && git->exitCode == 0) {
        std::printf("ok    git found: %s", git->output.c_str());
    } else {
        std::printf("FAIL  git not found on PATH\n");
        ++failures;
    }

    auto p4 = run("p4", {"-V"});
    if (p4 && p4->exitCode == 0) {
        std::printf("ok    p4 found\n");
    } else {
        std::printf("FAIL  p4 not found on PATH\n");
        ++failures;
    }

    if (failures == 0) {
        std::printf("\nAll checks passed.\n");
    } else {
        std::printf("\n%d check(s) failed.\n", failures);
    }
    return failures == 0 ? 0 : 1;
}

}  // namespace p4gw
