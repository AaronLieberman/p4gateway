// SPDX-License-Identifier: MIT

#include "restack.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

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

Absorption planAbsorption(const std::vector<LineCommit>& commits) {
    std::unordered_map<std::string, size_t> index;
    for (size_t i = 0; i < commits.size(); ++i) index[commits[i].oid] = i;

    std::vector<std::vector<size_t>> children(commits.size());
    for (size_t i = 0; i < commits.size(); ++i) {
        if (commits[i].parents.size() != 1) return {};  // merges: hands off
        auto parent = index.find(commits[i].parents.front());
        if (parent != index.end()) children[parent->second].push_back(i);
    }

    auto carriedByDepot = [](const LineCommit& commit) {
        if (commit.empty || commit.touched.empty()) return false;
        const std::unordered_set<std::string> differs(
            commit.differsFromDepot.begin(), commit.differsFromDepot.end());
        return std::none_of(
            commit.touched.begin(), commit.touched.end(),
            [&](const std::string& path) { return differs.contains(path); });
    };

    // Children come after parents, so walking backwards decides every child
    // before the parent that depends on it.
    std::vector<bool> absorbed(commits.size(), false);
    for (size_t i = commits.size(); i-- > 0;) {
        const auto& kids = children[i];
        absorbed[i] =
            carriedByDepot(commits[i]) ||
            (!kids.empty() && std::all_of(kids.begin(), kids.end(),
                                          [&](size_t k) { return absorbed[k]; }));
    }

    Absorption plan;
    for (size_t i = 0; i < commits.size(); ++i) {
        if (absorbed[i]) {
            plan.absorbed.push_back(commits[i].oid);
            continue;
        }
        auto parent = index.find(commits[i].parents.front());
        if (parent != index.end() && absorbed[parent->second]) {
            plan.frontier.push_back(commits[i].oid);
        }
    }
    return plan;
}

}  // namespace p4gw::restack
