// SPDX-License-Identifier: MIT

#pragma once

#include <string>

namespace p4gw {

// The build stamp: what git said about the source this binary was built from.
// cmake/GitVersion.cmake fills these in at build time (see cmake/version.cpp.in
// for the generated definition of buildVersion()). Nothing here is ever
// hand-maintained - the only number a human sets is the MAJOR.MINOR in the
// top-level project() call, which arrives as `base`.
//
// Every git-derived field is best-effort: a build from a source copy with no
// git available leaves them empty, and the version simply reports an unknown
// provenance rather than failing.
struct VersionInfo {
    std::string base;        // "1.0": the project's major.minor
    std::string commits;     // commit count behind HEAD; empty when unknown
    std::string commit;      // full commit sha; empty when unknown
    std::string commitDate;  // committer date, ISO-8601; empty when unknown
    std::string branch;      // branch name; empty when detached or unknown
    bool dirty = false;      // tracked files were modified at build time
    bool shallow = false;    // shallow clone: no trustworthy commit count
};

// The stamp compiled into this binary.
const VersionInfo& buildVersion();

// One line, as printed by the usage banner: "1.0.50+g547aba8b9", with
// ".dirty" / ".shallow" appended when those apply and "+unknown" as the
// metadata when git had nothing to say.
std::string versionString(const VersionInfo& info);

// The multi-line `gw --version` report: the version line plus whichever of
// commit / date / branch are known, and a note when the tree was modified.
std::string versionDetail(const VersionInfo& info);

}  // namespace p4gw
