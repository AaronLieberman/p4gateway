#include <cstdio>

#include "commands.h"

namespace p4gw {

// Converts the current branch's commits (relative to the baseline branch)
// into a pending P4 changelist:
//   1. Require a clean working tree and an up-to-date baseline.
//   2. Build the CL description from the commit messages in the range.
//   3. Create a numbered pending changelist.
//   4. p4 reconcile -c <CL> scoped to the configured depot path.
//   5. Print the CL for review; --shelve shelves it, --submit submits it.
// See PLAN.md milestone M4.
int cmdSubmit(const Args& args) {
    (void)args;
    std::fprintf(stderr, "gw submit: not implemented yet (PLAN.md milestone M4)\n");
    return 1;
}

}  // namespace p4gw
