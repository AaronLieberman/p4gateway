// SPDX-License-Identifier: MIT

#include "restack.h"

namespace p4gw::restack {

RestackPlan planRestack(const std::vector<StackCandidate>& candidates,
                        const std::vector<long long>& restackTimes,
                        int depthLimit, bool carryAll) {
    RestackPlan plan;
    for (const auto& candidate : candidates) {
        int depth = 0;
        for (long long restackTime : restackTimes) {
            // Strictly newer: a snapshot taken in the same second you wrote the
            // commit has not left you behind.
            if (restackTime > candidate.lastAuthoredAt) ++depth;
        }
        // HEAD first: standing on a stack outranks how old it is.
        const bool carry =
            carryAll || candidate.holdsHead || depth <= depthLimit;
        if (carry) {
            plan.carryRoots.push_back(candidate.root);
        } else {
            plan.parkedLabels.push_back(candidate.label);
        }
    }
    return plan;
}

}  // namespace p4gw::restack
