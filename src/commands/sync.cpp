#include <cstdio>

#include "commands.h"

namespace p4gw {

// Brings the baseline branch up to date with the depot:
//   1. Require a clean working tree.
//   2. p4 sync scoped to the configured depot path.
//   3. Commit the result on the baseline branch (p4-main) with the synced
//      changelist number in the message.
//   4. Optionally rebase the previously checked-out branch onto it.
// See PLAN.md milestone M3.
int cmdSync(const Args& args) {
    (void)args;
    std::fprintf(stderr, "gw sync: not implemented yet (PLAN.md milestone M3)\n");
    return 1;
}

}  // namespace p4gw
