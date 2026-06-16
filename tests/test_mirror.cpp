#include "mirror.h"
#include "test_framework.h"

TEST(mirror_sync_copies_everything_and_deletes_vanished) {
    const std::vector<std::string> mirrorFiles{"a.cpp", "sub/b.h"};
    const std::vector<std::string> tracked{"a.cpp", "sub/b.h", "gone.cpp"};
    const auto actions = p4gw::mirror::computeSyncActions(mirrorFiles, tracked);
    CHECK(actions.copies == (std::vector<std::string>{"a.cpp", "sub/b.h"}));
    CHECK(actions.deletes == (std::vector<std::string>{"gone.cpp"}));
}

TEST(mirror_sync_never_deletes_gw_metadata) {
    const auto actions = p4gw::mirror::computeSyncActions(
        {"a.cpp"}, {"a.cpp", ".gitignore", "p4gw.cfg"});
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
