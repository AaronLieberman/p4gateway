#include "shelf.h"
#include "test_framework.h"

using namespace p4gw;

namespace {

// A representative `p4 -ztag describe -S` record: globals, a multi-line
// description, and three files (edit, add, delete) with their fields.
const char* kZtag =
    "... change 4821\n"
    "... user bob\n"
    "... desc Fix anim blend\n"
    "More detail on the second line\n"
    "... status pending\n"
    "... depotFile0 //depot/project/src/anim/Blend.cpp\n"
    "... action0 edit\n"
    "... type0 text\n"
    "... rev0 5\n"
    "... depotFile1 //depot/project/src/anim/New.cpp\n"
    "... action1 add\n"
    "... type1 text\n"
    "... rev1 1\n"
    "... depotFile2 //depot/project/src/anim/Old.cpp\n"
    "... action2 delete\n"
    "... type2 text\n"
    "... rev2 9\n";

}  // namespace

TEST(shelf_parses_change_and_multiline_desc) {
    auto info = parseShelveDescribe(kZtag);
    CHECK(info.has_value());
    if (!info) return;
    CHECK(info->change == "4821");
    CHECK(info->description == "Fix anim blend\nMore detail on the second line");
    CHECK(info->files.size() == 3);
}

TEST(shelf_parses_file_fields_in_index_order) {
    auto info = parseShelveDescribe(kZtag);
    CHECK(info.has_value());
    if (!info) return;
    CHECK(info->files[0].depotFile == "//depot/project/src/anim/Blend.cpp");
    CHECK(info->files[0].action == ShelveAction::Edit);
    CHECK(info->files[0].rev == "5");
    CHECK(info->files[1].action == ShelveAction::Add);
    CHECK(info->files[2].action == ShelveAction::Delete);
    CHECK(info->files[2].rev == "9");
}

TEST(shelf_parses_move_actions) {
    CHECK(parseShelveAction("move/add") == ShelveAction::MoveAdd);
    CHECK(parseShelveAction("move/delete") == ShelveAction::MoveDelete);
    CHECK(parseShelveAction("integrate") == ShelveAction::Other);
}

TEST(shelf_empty_describe_yields_no_files) {
    auto info = parseShelveDescribe("");
    CHECK(info.has_value());
    if (info) CHECK(info->files.empty());
}

TEST(shelf_binary_type_detection) {
    CHECK(!isBinaryType("text"));
    CHECK(!isBinaryType("xtext"));
    CHECK(!isBinaryType("ktext+w"));
    CHECK(!isBinaryType("symlink"));
    CHECK(isBinaryType("binary"));
    CHECK(isBinaryType("ubinary"));
    CHECK(isBinaryType("apple"));
}

TEST(shelf_parse_changes_reads_records) {
    const char* out =
        "... change 4830\n"
        "... time 1718000000\n"
        "... user bob\n"
        "... client bob-dev\n"
        "... status pending\n"
        "... desc WIP refactor\n"
        "\n"
        "... change 4821\n"
        "... user bob\n"
        "... status pending\n"
        "... desc Fix anim blend\n"
        "second line of the description\n"
        "\n";
    const auto changes = parseChanges(out);
    CHECK(changes.size() == 2);
    if (changes.size() == 2) {
        CHECK(changes[0].change == "4830");
        CHECK(changes[0].description == "WIP refactor");
        CHECK(changes[0].shelved == false);
        CHECK(changes[1].change == "4821");
        // Multi-line descriptions are captured whole; the command shows line 1.
        CHECK(changes[1].description ==
              "Fix anim blend\nsecond line of the description");
    }
}

TEST(shelf_parse_changes_empty_is_empty) {
    CHECK(parseChanges("").empty());
}
