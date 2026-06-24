#include "mirror.h"
#include "p4.h"
#include "test_framework.h"

// ---- mirror::planImport (import opened-files preflight) ----

TEST(import_plan_edit_reads_depot_not_mirror) {
    p4gw::mirror::SyncActions base;
    base.copies = {"a.cpp", "b.cpp"};  // both present in the mirror
    std::vector<p4gw::mirror::OpenedMirrorFile> opened{{"a.cpp", true}};
    const auto plan = p4gw::mirror::planImport(base, opened);
    CHECK(plan.actions.copies == (std::vector<std::string>{"b.cpp"}));
    CHECK(plan.depotReads == (std::vector<std::string>{"a.cpp"}));
}

TEST(import_plan_add_is_omitted) {
    p4gw::mirror::SyncActions base;
    base.copies = {"keep.cpp", "new.cpp"};
    std::vector<p4gw::mirror::OpenedMirrorFile> opened{{"new.cpp", false}};
    const auto plan = p4gw::mirror::planImport(base, opened);
    CHECK(plan.actions.copies == (std::vector<std::string>{"keep.cpp"}));
    CHECK(plan.depotReads.empty());
}

TEST(import_plan_delete_keeps_depot_file) {
    p4gw::mirror::SyncActions base;
    base.copies = {"keep.cpp"};
    base.deletes = {"gone.cpp"};  // open for delete: removed from the mirror
    std::vector<p4gw::mirror::OpenedMirrorFile> opened{{"gone.cpp", true}};
    const auto plan = p4gw::mirror::planImport(base, opened);
    CHECK(plan.actions.deletes.empty());
    CHECK(plan.depotReads == (std::vector<std::string>{"gone.cpp"}));
}

TEST(import_plan_passes_through_unopened) {
    p4gw::mirror::SyncActions base;
    base.copies = {"a.cpp", "b.cpp"};
    base.deletes = {"c.cpp"};
    const auto plan = p4gw::mirror::planImport(base, {});
    CHECK(plan.actions.copies == base.copies);
    CHECK(plan.actions.deletes == base.deletes);
    CHECK(plan.depotReads.empty());
}

// ---- p4 opened/print preflight helpers ----

TEST(parse_tagged_opened_reads_action_and_path) {
    const std::string out =
        "... depotFile //depot/project/src/a.cpp\n"
        "... clientFile /ws/a.cpp\n"
        "... rev 3\n"
        "... action edit\n"
        "... change 123\n"
        "\n"
        "... depotFile //depot/project/src/b.cpp\n"
        "... action move/add\n"
        "\n";
    const auto files = p4gw::p4::parseTaggedOpened(out);
    CHECK(files.size() == 2);
    CHECK(files[0].depotFile == "//depot/project/src/a.cpp");
    CHECK(files[0].action == "edit");
    CHECK(files[1].depotFile == "//depot/project/src/b.cpp");
    CHECK(files[1].action == "move/add");
}

TEST(parse_tagged_opened_empty_is_empty) {
    CHECK(p4gw::p4::parseTaggedOpened("").empty());
}

TEST(parse_tagged_depot_files_reads_have_listing) {
    const std::string out =
        "... depotFile //depot/project/src/a.cpp\n"
        "... clientFile /ws/a.cpp\n"
        "... haveRev 5\n"
        "\n"
        "... depotFile //depot/project/src/sub/b.h\n"
        "... haveRev 2\n"
        "\n";
    const auto files = p4gw::p4::parseTaggedDepotFiles(out);
    CHECK(files == (std::vector<std::string>{"//depot/project/src/a.cpp",
                                             "//depot/project/src/sub/b.h"}));
}

TEST(parse_tagged_depot_files_empty_is_empty) {
    CHECK(p4gw::p4::parseTaggedDepotFiles("").empty());
}

TEST(depot_relative_path_strips_subtree) {
    CHECK(p4gw::p4::depotRelativePath("//depot/project/src/...",
                                      "//depot/project/src/sub/a.cpp") ==
          "sub/a.cpp");
    CHECK(p4gw::p4::depotRelativePath("//depot/project/src/...",
                                      "//depot/project/src/a.cpp") == "a.cpp");
    CHECK(p4gw::p4::depotRelativePath("//depot/project/src/...",
                                      "//other/x.cpp") == "");
}

TEST(is_add_action_classifies_opens) {
    CHECK(p4gw::p4::isAddAction("add"));
    CHECK(p4gw::p4::isAddAction("move/add"));
    CHECK(p4gw::p4::isAddAction("branch"));
    CHECK(!p4gw::p4::isAddAction("edit"));
    CHECK(!p4gw::p4::isAddAction("delete"));
    CHECK(!p4gw::p4::isAddAction("move/delete"));
}
