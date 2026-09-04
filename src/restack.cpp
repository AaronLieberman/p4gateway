// SPDX-License-Identifier: MIT

#include "restack.h"

#include <algorithm>

namespace p4gw::restack {

namespace {

bool contains(const std::vector<std::string>& haystack,
              const std::string& needle) {
    return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

}  // namespace

RestackSelection selectStacks(const std::vector<std::string>& allRoots,
                              const std::vector<std::string>& currentStackRoots,
                              const std::vector<std::string>& parkedRoots,
                              bool allStacks, bool force) {
    RestackSelection selection;
    if (!allStacks) {
        // An explicit ask for the stack you are on: parking never blocks it.
        selection.carryRoots = currentStackRoots;
        return selection;
    }
    for (const auto& root : allRoots) {
        if (!force && contains(parkedRoots, root)) {
            selection.skippedParked.push_back(root);
        } else {
            selection.carryRoots.push_back(root);
        }
    }
    return selection;
}

std::vector<std::string> staleParkedRoots(
    const std::vector<std::string>& parkedRoots,
    const std::vector<std::string>& visibleRoots) {
    std::vector<std::string> stale;
    for (const auto& parked : parkedRoots) {
        if (!contains(visibleRoots, parked)) stale.push_back(parked);
    }
    return stale;
}

}  // namespace p4gw::restack
