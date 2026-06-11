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
        {"a.cpp"}, {"a.cpp", ".gitignore", ".p4gw"});
    CHECK(actions.deletes.empty());
}

TEST(mirror_sync_handles_fresh_repo) {
    const auto actions =
        p4gw::mirror::computeSyncActions({"x.cpp", "y.cpp"}, {});
    CHECK(actions.copies.size() == 2);
    CHECK(actions.deletes.empty());
}

TEST(gw_metadata_paths_are_top_level_only) {
    CHECK(p4gw::mirror::isGwMetadataPath(".p4gw"));
    CHECK(p4gw::mirror::isGwMetadataPath(".gitignore"));
    CHECK(!p4gw::mirror::isGwMetadataPath("sub/.gitignore"));
    CHECK(!p4gw::mirror::isGwMetadataPath("main.cpp"));
}
