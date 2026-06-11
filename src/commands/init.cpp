#include <cstdio>

#include "commands.h"

namespace p4gw {

// Sets up a Git overlay inside an existing P4 workspace subtree:
//   1. Verify git and p4 are on PATH and the P4 connection works.
//   2. Write a .p4gw config (depot path, client, baseline branch).
//   3. git init (if needed), write a starter .gitignore.
//   4. Commit the current synced state on the baseline branch.
// See PLAN.md milestone M2.
int cmdInit(const Args& args) {
    (void)args;
    std::fprintf(stderr, "gw init: not implemented yet (PLAN.md milestone M2)\n");
    return 1;
}

}  // namespace p4gw
