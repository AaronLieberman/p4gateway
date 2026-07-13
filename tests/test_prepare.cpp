// SPDX-License-Identifier: MIT

#include "p4ops.h"
#include "test_framework.h"

using p4gw::P4Op;
using p4gw::git::FileChange;

TEST(ops_map_statuses_in_execution_order) {
    const std::vector<FileChange> changes{
        {'M', "anim/Blend.cpp", ""},
        {'A', "anim/tests/BlendZero.cpp", ""},
        {'D', "anim/Legacy.cpp", ""},
        {'R', "core/Old.h", "core/New.h"},
        {'T', "tools/link", ""},
    };
    auto ops = p4gw::planP4Operations(changes);
    CHECK(ops.has_value());
    if (!ops) return;
    CHECK(ops->size() == 5);
    // Deletes, then edits (M and T), then moves, then adds.
    CHECK((*ops)[0].kind == P4Op::Kind::Delete);
    CHECK((*ops)[0].path == "anim/Legacy.cpp");
    CHECK((*ops)[1].kind == P4Op::Kind::Edit);
    CHECK((*ops)[1].path == "anim/Blend.cpp");
    CHECK((*ops)[2].kind == P4Op::Kind::Edit);
    CHECK((*ops)[2].path == "tools/link");
    CHECK((*ops)[3].kind == P4Op::Kind::Move);
    CHECK((*ops)[3].path == "core/Old.h");
    CHECK((*ops)[3].newPath == "core/New.h");
    CHECK((*ops)[4].kind == P4Op::Kind::Add);
    CHECK((*ops)[4].path == "anim/tests/BlendZero.cpp");
}

TEST(ops_treat_copies_as_adds) {
    auto ops = p4gw::planP4Operations({{'C', "a.h", "b.h"}});
    CHECK(ops.has_value());
    if (ops) {
        CHECK(ops->size() == 1);
        CHECK((*ops)[0].kind == P4Op::Kind::Add);
        CHECK((*ops)[0].path == "b.h");
    }
}

TEST(ops_skip_gw_metadata_files) {
    auto ops = p4gw::planP4Operations({
        {'A', ".gitignore", ""},
        {'M', "p4gw.cfg", ""},
        {'M', "real/file.cpp", ""},
    });
    CHECK(ops.has_value());
    if (ops) {
        CHECK(ops->size() == 1);
        CHECK((*ops)[0].path == "real/file.cpp");
    }
}

TEST(ops_reject_unknown_status) {
    auto ops = p4gw::planP4Operations({{'U', "weird.cpp", ""}});
    CHECK(!ops.has_value());
}

TEST(ops_reject_rename_without_destination) {
    auto ops = p4gw::planP4Operations({{'R', "old.cpp", ""}});
    CHECK(!ops.has_value());
}

// ---- resolveSliceRange (prepare's positional -> commit range) ----

using p4gw::resolveSliceRange;

TEST(slice_default_is_whole_stack) {
    auto r = resolveSliceRange("", /*stack=*/false, "refs/p4gw/main");
    CHECK(r.has_value());
    if (r) {
        CHECK(r->base == "refs/p4gw/main");
        CHECK(r->target == "HEAD");
    }
}

TEST(slice_single_commit_ships_just_it) {
    auto r = resolveSliceRange("abc123", false, "refs/p4gw/main");
    CHECK(r.has_value());
    if (r) {
        CHECK(r->base == "abc123^");
        CHECK(r->target == "abc123");
    }
}

TEST(slice_stack_widens_a_single_commit_to_the_baseline) {
    auto r = resolveSliceRange("abc123", /*stack=*/true, "refs/p4gw/main");
    CHECK(r.has_value());
    if (r) {
        CHECK(r->base == "refs/p4gw/main");
        CHECK(r->target == "abc123");
    }
}

TEST(slice_explicit_range_is_verbatim) {
    auto r = resolveSliceRange("a1..b2", false, "refs/p4gw/main");
    CHECK(r.has_value());
    if (r) {
        CHECK(r->base == "a1");
        CHECK(r->target == "b2");
    }
}

TEST(slice_range_omitted_sides_default) {
    auto left = resolveSliceRange("..b2", false, "refs/p4gw/main");
    CHECK(left.has_value());
    if (left) {
        CHECK(left->base == "refs/p4gw/main");
        CHECK(left->target == "b2");
    }
    auto right = resolveSliceRange("a1..", false, "refs/p4gw/main");
    CHECK(right.has_value());
    if (right) {
        CHECK(right->base == "a1");
        CHECK(right->target == "HEAD");
    }
}

TEST(slice_rejects_symmetric_range) {
    auto r = resolveSliceRange("a...b", false, "refs/p4gw/main");
    CHECK(!r.has_value());
}

TEST(slice_rejects_stack_with_a_range) {
    auto r = resolveSliceRange("a..b", /*stack=*/true, "refs/p4gw/main");
    CHECK(!r.has_value());
}