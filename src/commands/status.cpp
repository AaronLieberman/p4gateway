#include <cstdio>

#include "commands.h"

namespace p4gw {

// Shows where Git and P4 stand relative to each other:
//   - current branch and commits ahead of the baseline branch
//   - working tree cleanliness
//   - last imported changelist on the baseline branch
//   - pending changelists created by gw prepare
// See PLAN.md milestone M3.
int cmdStatus(const Args& args) {
    (void)args;
    std::fprintf(stderr, "gw status: not implemented yet (PLAN.md milestone M3)\n");
    return 1;
}

}  // namespace p4gw
