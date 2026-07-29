// SPDX-License-Identifier: MIT

#include <string>

#include "test_framework.h"
#include "version.h"

using namespace p4gw;

namespace {

// A stamp from an ordinary clean build of main.
VersionInfo cleanBuild() {
    VersionInfo info;
    info.base = "1.0";
    info.commits = "50";
    info.commit = "547aba8b9c1de2f3a4b5c6d7e8f90123456789ab";
    info.commitDate = "2026-07-29T12:34:56-07:00";
    info.branch = "main";
    return info;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST(version_string_is_base_plus_commit_count_and_sha) {
    CHECK(versionString(cleanBuild()) == "1.0.50+g547aba8b9");
}

TEST(version_string_flags_a_modified_tree) {
    VersionInfo info = cleanBuild();
    info.dirty = true;
    CHECK(versionString(info) == "1.0.50+g547aba8b9.dirty");
}

TEST(version_string_drops_an_untrustworthy_shallow_count) {
    // A shallow clone knows its sha but not how many commits precede it, so
    // the patch number must not pretend otherwise.
    VersionInfo info = cleanBuild();
    info.shallow = true;
    info.commits = "1";
    CHECK(versionString(info) == "1.0.0+g547aba8b9.shallow");
}

TEST(version_string_without_git_reports_unknown) {
    VersionInfo info;
    info.base = "1.0";
    CHECK(versionString(info) == "1.0.0+unknown");
}

TEST(version_string_survives_a_short_sha) {
    // Nothing here should read past the end of a sha shorter than the
    // abbreviation length.
    VersionInfo info = cleanBuild();
    info.commit = "547ab";
    CHECK(versionString(info) == "1.0.50+g547ab");
}

TEST(version_detail_reports_the_full_commit_and_branch) {
    const std::string detail = versionDetail(cleanBuild());
    CHECK(contains(detail, "gw 1.0.50+g547aba8b9\n"));
    CHECK(contains(detail, "547aba8b9c1de2f3a4b5c6d7e8f90123456789ab"));
    CHECK(contains(detail, "2026-07-29T12:34:56-07:00"));
    CHECK(contains(detail, "branch  main"));
    CHECK(!contains(detail, "note"));
}

TEST(version_detail_omits_unknown_fields) {
    VersionInfo info = cleanBuild();
    info.branch.clear();  // detached HEAD, as in a CI PR checkout
    info.commitDate.clear();
    const std::string detail = versionDetail(info);
    CHECK(!contains(detail, "branch"));
    CHECK(!contains(detail, "date"));
    CHECK(contains(detail, "commit  547aba8b9c1de2f3a4b5c6d7e8f90123456789ab"));
}

TEST(version_detail_notes_a_modified_tree) {
    VersionInfo info = cleanBuild();
    info.dirty = true;
    CHECK(contains(versionDetail(info), "modified working tree"));
}

TEST(version_detail_notes_a_build_outside_git) {
    VersionInfo info;
    info.base = "1.0";
    CHECK(contains(versionDetail(info), "outside a git checkout"));
}

TEST(build_version_is_stamped_in) {
    // The generated stamp must at least carry the project's major.minor; the
    // git-derived fields are environment-dependent and not asserted here.
    CHECK(!buildVersion().base.empty());
    CHECK(!versionString(buildVersion()).empty());
}
