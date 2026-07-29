// SPDX-License-Identifier: MIT

#include "version.h"

#include <algorithm>
#include <cstddef>

namespace p4gw {

namespace {

// Long enough to stay unambiguous well past this repo's lifetime, short enough
// to read out over a support channel. `git show <this>` resolves it.
constexpr std::size_t kShortShaLength = 9;

std::string shortSha(const std::string& commit) {
    return commit.substr(0, std::min(commit.size(), kShortShaLength));
}

}  // namespace

std::string versionString(const VersionInfo& info) {
    // The patch number is the commit count. A shallow clone has one it cannot
    // trust, so it falls back to 0 and says ".shallow" - a version that is
    // honestly unnumbered beats one that silently understates the history.
    const std::string patch =
        (info.commits.empty() || info.shallow) ? "0" : info.commits;
    std::string version = info.base + "." + patch;

    if (info.commit.empty()) {
        version += "+unknown";
        return version;
    }
    version += "+g" + shortSha(info.commit);
    if (info.shallow) version += ".shallow";
    if (info.dirty) version += ".dirty";
    return version;
}

std::string versionDetail(const VersionInfo& info) {
    std::string detail = "gw " + versionString(info) + "\n";
    if (!info.commit.empty()) {
        detail += "  commit  " + info.commit + "\n";
    }
    if (!info.commitDate.empty()) {
        detail += "  date    " + info.commitDate + "\n";
    }
    if (!info.branch.empty()) {
        detail += "  branch  " + info.branch + "\n";
    }
    if (info.commit.empty()) {
        detail +=
            "  note    built outside a git checkout - no source revision to "
            "trace back to\n";
    }
    if (info.shallow) {
        detail +=
            "  note    built from a shallow clone - the commit count is not "
            "available\n";
    }
    if (info.dirty) {
        detail +=
            "  note    built from a modified working tree - the commit above "
            "does not fully describe it\n";
    }
    return detail;
}

}  // namespace p4gw
