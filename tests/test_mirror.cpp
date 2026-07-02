#include <fstream>
#include <iterator>

#include "mirror.h"
#include "test_framework.h"

namespace {

namespace fs = std::filesystem;

// A throwaway directory for the filesystem-backed tests, wiped on entry and
// exit so a crashed earlier run can't poison this one.
struct TempDir {
    fs::path path;
    explicit TempDir(const char* name)
        : path(fs::temp_directory_path() / name) {
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

void writeFile(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << content;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
}

}  // namespace

TEST(mirror_sync_copies_everything_and_deletes_vanished) {
    const std::vector<std::string> mirrorFiles{"a.cpp", "sub/b.h"};
    const std::vector<std::string> tracked{"a.cpp", "sub/b.h", "gone.cpp"};
    const auto actions = p4gw::mirror::computeSyncActions(mirrorFiles, tracked);
    CHECK(actions.copies == (std::vector<std::string>{"a.cpp", "sub/b.h"}));
    CHECK(actions.deletes == (std::vector<std::string>{"gone.cpp"}));
}

TEST(mirror_sync_never_deletes_gw_metadata) {
    const auto actions = p4gw::mirror::computeSyncActions(
        {"a.cpp"}, {"a.cpp", ".gitignore", ".gitattributes", "p4gw.cfg"});
    CHECK(actions.deletes.empty());
}

TEST(mirror_sync_handles_fresh_repo) {
    const auto actions =
        p4gw::mirror::computeSyncActions({"x.cpp", "y.cpp"}, {});
    CHECK(actions.copies.size() == 2);
    CHECK(actions.deletes.empty());
}

TEST(gw_metadata_paths_are_top_level_only) {
    CHECK(p4gw::mirror::isGwMetadataPath("p4gw.cfg"));
    CHECK(p4gw::mirror::isGwMetadataPath(".gitignore"));
    CHECK(p4gw::mirror::isGwMetadataPath(".gitattributes"));
    CHECK(!p4gw::mirror::isGwMetadataPath("sub/.gitignore"));
    CHECK(!p4gw::mirror::isGwMetadataPath("main.cpp"));
}

TEST(copy_needed_skips_matching_size_and_mtime) {
    const auto t = std::filesystem::file_time_type{} +
                   std::chrono::seconds{1000};
    // Same size and mtime: the working-tree copy is already current.
    CHECK(!p4gw::mirror::copyNeeded(42, t, /*dstExists=*/true, 42, t));
}

TEST(copy_needed_when_target_missing) {
    const auto t = std::filesystem::file_time_type{};
    CHECK(p4gw::mirror::copyNeeded(0, t, /*dstExists=*/false, 0, t));
}

TEST(copy_needed_on_size_or_mtime_difference) {
    const auto t0 = std::filesystem::file_time_type{};
    const auto t1 = t0 + std::chrono::seconds{1};
    // Same mtime, different size.
    CHECK(p4gw::mirror::copyNeeded(10, t0, /*dstExists=*/true, 11, t0));
    // Same size, different mtime (mirror file was resynced in place).
    CHECK(p4gw::mirror::copyNeeded(10, t1, /*dstExists=*/true, 10, t0));
}

TEST(files_identical_compares_bytes) {
    TempDir dir("p4gw_test_filecmp");
    writeFile(dir.path / "a", "hello");
    writeFile(dir.path / "same", "hello");
    writeFile(dir.path / "case", "hellO");   // same size, different bytes
    writeFile(dir.path / "longer", "hello!");
    CHECK(p4gw::mirror::filesIdentical(dir.path / "a", dir.path / "same"));
    CHECK(!p4gw::mirror::filesIdentical(dir.path / "a", dir.path / "case"));
    CHECK(!p4gw::mirror::filesIdentical(dir.path / "a", dir.path / "longer"));
    CHECK(!p4gw::mirror::filesIdentical(dir.path / "a", dir.path / "missing"));
}

TEST(stale_fast_path_flags_only_stat_matching_content_mismatches) {
    TempDir dir("p4gw_test_stale");
    const fs::path mirror = dir.path / "mirror";
    const fs::path wt = dir.path / "wt";

    // Same size and mtime but different bytes: the fast path would skip it
    // even though the content diverged - the one case that must be flagged.
    writeFile(mirror / "stale.txt", "aaaa");
    writeFile(wt / "stale.txt", "bbbb");
    fs::last_write_time(wt / "stale.txt",
                        fs::last_write_time(mirror / "stale.txt"));

    // Same stats, same bytes: a healthy skipped file.
    writeFile(mirror / "same.txt", "data");
    writeFile(wt / "same.txt", "data");
    fs::last_write_time(wt / "same.txt",
                        fs::last_write_time(mirror / "same.txt"));

    // Different mtime (a branch edit, a fresh sync): legitimate divergence -
    // import recopies it anyway, so it is not flagged.
    writeFile(mirror / "fresh.txt", "xxxx");
    writeFile(wt / "fresh.txt", "yyyy");
    fs::last_write_time(wt / "fresh.txt",
                        fs::last_write_time(mirror / "fresh.txt") +
                            std::chrono::seconds{5});

    // Missing from the working tree: import will copy it - self-healing.
    writeFile(mirror / "sub/new.txt", "n");

    const auto stale = p4gw::mirror::findStaleFastPathFiles(
        {"stale.txt", "same.txt", "fresh.txt", "sub/new.txt"},
        mirror.string(), wt.string());
    CHECK(stale == (std::vector<std::string>{"stale.txt"}));
}

TEST(apply_sync_actions_full_copy_overwrites_stat_matching_files) {
    TempDir dir("p4gw_test_fullcopy");
    const fs::path mirror = dir.path / "mirror";
    const fs::path wt = dir.path / "wt";
    writeFile(mirror / "a.txt", "good");
    writeFile(wt / "a.txt", "bad!");  // same size...
    fs::last_write_time(wt / "a.txt",
                        fs::last_write_time(mirror / "a.txt"));  // ...and mtime

    p4gw::mirror::SyncActions actions;
    actions.copies = {"a.txt"};

    // Trusting the stats, the stale file is skipped - exactly the hole a torn
    // import can leave.
    auto copied = p4gw::mirror::applySyncActions(actions, mirror.string(),
                                                 wt.string());
    CHECK(copied && *copied == 0);
    CHECK(readFile(wt / "a.txt") == "bad!");

    // Full-copy mode ignores the stats and restores the mirror content, then
    // re-stamps the mirror mtime so the next trusting run can skip it again.
    copied = p4gw::mirror::applySyncActions(actions, mirror.string(),
                                            wt.string(), /*trustStats=*/false);
    CHECK(copied && *copied == 1);
    CHECK(readFile(wt / "a.txt") == "good");
    CHECK(fs::last_write_time(wt / "a.txt") ==
          fs::last_write_time(mirror / "a.txt"));
}

TEST(import_pending_marker_lives_in_the_git_dir) {
    const fs::path p =
        p4gw::mirror::importPendingMarkerPath("/repo/.git");
    CHECK(p.filename() == "p4gw-import-pending");
    CHECK(p.parent_path() == fs::path("/repo/.git"));
}
