// SPDX-License-Identifier: MIT

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

namespace {

// A rule for the filter tests - only exclude/depotPath drive the effective
// resolution filterExcludedOpens uses.
p4gw::ViewRule rule(bool exclude, const std::string& depot) {
    p4gw::ViewRule r;
    r.exclude = exclude;
    r.depotPath = depot;
    return r;
}

}  // namespace

TEST(filter_excluded_opens_drops_files_under_an_exclude) {
    // The reported case: vendored libs/pdbs the user edits directly in P4 are
    // open under src/lib (an excluded subtree). gw ships nothing through the
    // excludes, so those opens must not block prepare; only the open under the
    // mirrored part of the subtree (src/core) survives.
    const std::vector<p4gw::p4::OpenedFile> opened = {
        {"//depot/project/main/src/lib/public/win64/vc14/render_lib.lib", "edit"},
        {"//depot/project/main/src/lib/public/win64/vc14/render_lib.pdb", "edit"},
        {"//depot/project/main/src/thirdparty/zlib/zlib.h", "edit"},
        {"//depot/project/main/src/core/main.cpp", "edit"},
    };
    const std::vector<p4gw::ViewRule> rules = {
        rule(false, "//depot/project/main/src/..."),
        rule(true, "//depot/project/main/src/lib/..."),
        rule(true, "//depot/project/main/src/thirdparty/..."),
        rule(true, "//depot/project/main/src/devtools/..."),
    };
    const auto kept = p4gw::p4::filterExcludedOpens(opened, rules);
    CHECK(kept.size() == 1);
    if (kept.size() == 1) {
        CHECK(kept[0].depotFile == "//depot/project/main/src/core/main.cpp");
    }
}

TEST(filter_excluded_opens_keeps_reincluded_subtree_under_an_exclude) {
    // win64 is re-included beneath the excluded lib: later-wins keeps its opens
    // shippable, while the rest of lib stays dropped.
    const std::vector<p4gw::p4::OpenedFile> opened = {
        {"//depot/project/main/src/lib/other/a.cpp", "edit"},
        {"//depot/project/main/src/lib/public/win64/keep.cpp", "edit"},
    };
    const std::vector<p4gw::ViewRule> rules = {
        rule(false, "//depot/project/main/src/..."),
        rule(true, "//depot/project/main/src/lib/..."),
        rule(false, "//depot/project/main/src/lib/public/win64/..."),
    };
    const auto kept = p4gw::p4::filterExcludedOpens(opened, rules);
    CHECK(kept.size() == 1);
    if (kept.size() == 1) {
        CHECK(kept[0].depotFile ==
              "//depot/project/main/src/lib/public/win64/keep.cpp");
    }
}

TEST(filter_excluded_opens_with_no_excludes_keeps_all) {
    const std::vector<p4gw::p4::OpenedFile> opened = {
        {"//depot/project/main/src/lib/x.lib", "edit"},
        {"//depot/project/main/src/core/y.cpp", "edit"},
    };
    const std::vector<p4gw::ViewRule> rules = {
        rule(false, "//depot/project/main/src/..."),
    };
    CHECK(p4gw::p4::filterExcludedOpens(opened, rules).size() == 2);
}

TEST(filter_excluded_opens_anchors_at_path_boundary) {
    // src/libutil shares a name prefix with src/lib but is not under it.
    const std::vector<p4gw::p4::OpenedFile> opened = {
        {"//depot/project/main/src/libutil/x.cpp", "edit"},
    };
    const std::vector<p4gw::ViewRule> rules = {
        rule(false, "//depot/project/main/src/..."),
        rule(true, "//depot/project/main/src/lib/..."),
    };
    const auto kept = p4gw::p4::filterExcludedOpens(opened, rules);
    CHECK(kept.size() == 1);
}

TEST(reconcile_reports_clean_handles_both_casings) {
    // p4 capitalizes it for an explicit file list and lowercases it (with a
    // path) for a directory; both must read as "nothing to reconcile".
    CHECK(p4gw::p4::reconcileReportsClean("No file(s) to reconcile.\n"));
    CHECK(p4gw::p4::reconcileReportsClean(
        "//depot/project/main/src/... - no file(s) to reconcile.\n"));
    // Real reconcile output (work to do) must not read as clean.
    CHECK(!p4gw::p4::reconcileReportsClean(
        "//depot/project/main/src/foo.cpp#1 - opened for edit\n"));
    CHECK(!p4gw::p4::reconcileReportsClean(""));
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

TEST(parse_tagged_have_reads_files_and_revs) {
    const std::string out =
        "... depotFile //depot/project/src/a.cpp\n"
        "... clientFile /ws/a.cpp\n"
        "... haveRev 5\n"
        "\n"
        "... depotFile //depot/project/src/sub/b.h\n"
        "... haveRev 2\n"
        "\n";
    const auto entries = p4gw::p4::parseTaggedHave(out);
    CHECK(entries.size() == 2);
    if (entries.size() == 2) {
        CHECK(entries[0].depotFile == "//depot/project/src/a.cpp");
        CHECK(entries[0].rev == "5");
        CHECK(entries[1].depotFile == "//depot/project/src/sub/b.h");
        CHECK(entries[1].rev == "2");
    }
}

TEST(parse_tagged_have_empty_is_empty) {
    CHECK(p4gw::p4::parseTaggedHave("").empty());
}

TEST(filter_have_to_rule_drops_excluded_and_reincluded_entries) {
    // The reported bug: `p4 have //...src/...` also lists files the client
    // view diverts in place (the devtools exclude) - those never exist in the
    // src mirror, so the manifest fast path must not try to copy or delete
    // them. Files under a deeper re-include belong to that mapping instead.
    const std::vector<p4gw::ViewRule> rules = {
        rule(false, "//depot/project/main/src/..."),
        rule(true, "//depot/project/main/src/devtools/..."),
        rule(true, "//depot/project/main/src/lib/..."),
        rule(false, "//depot/project/main/src/lib/public/win64/..."),
    };
    const std::vector<p4gw::p4::HaveEntry> have = {
        {"//depot/project/main/src/core/main.cpp", "3"},
        {"//depot/project/main/src/devtools/codegen/bin/cl", "7"},
        {"//depot/project/main/src/lib/other/a.lib", "2"},
        {"//depot/project/main/src/lib/public/win64/keep.lib", "4"},
    };

    const auto src = p4gw::p4::filterHaveToRule(have, rules, &rules[0]);
    CHECK(src.size() == 1);
    if (src.size() == 1) {
        CHECK(src[0].depotFile == "//depot/project/main/src/core/main.cpp");
        CHECK(src[0].rev == "3");
    }

    const auto win64 = p4gw::p4::filterHaveToRule(have, rules, &rules[3]);
    CHECK(win64.size() == 1);
    if (win64.size() == 1) {
        CHECK(win64[0].depotFile ==
              "//depot/project/main/src/lib/public/win64/keep.lib");
    }
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