// SPDX-License-Identifier: MIT

#include <string>
#include <vector>

#include "mirror.h"
#include "test_framework.h"

using p4gw::mirror::ManifestEntry;
using p4gw::mirror::planSyncback;

namespace {

// The baseline have state most cases start from: three files at the revisions
// the last import recorded.
std::vector<ManifestEntry> baselineThreeFiles() {
    return {
        {"//depot/src/a.cpp", "3"},
        {"//depot/src/b.cpp", "7"},
        {"//depot/src/c.cpp", "1"},
    };
}

}  // namespace

TEST(syncback_plans_nothing_when_the_client_is_on_the_baseline) {
    const auto plan =
        planSyncback(baselineThreeFiles(), baselineThreeFiles(), {});
    CHECK(plan.restores.empty());
    CHECK(plan.removes.empty());
    CHECK(plan.skippedOpen.empty());
}

TEST(syncback_restores_a_file_synced_forward_to_resolve_it) {
    // The workflow this exists for: b.cpp was synced to head and resolved, so
    // its have rev moved past the baseline's. Only that file is touched.
    std::vector<ManifestEntry> now = baselineThreeFiles();
    now[1].rev = "9";

    const auto plan = planSyncback(baselineThreeFiles(), now, {});
    CHECK(plan.removes.empty());
    CHECK(plan.skippedOpen.empty());
    CHECK(plan.restores.size() == 1);
    if (plan.restores.size() == 1) {
        CHECK(plan.restores[0].depotFile == "//depot/src/b.cpp");
        CHECK(plan.restores[0].rev == "7");  // the baseline's rev, not head
    }
}

TEST(syncback_restores_a_file_synced_backward_too) {
    // Direction does not matter: the target is always the recorded revision.
    std::vector<ManifestEntry> now = baselineThreeFiles();
    now[0].rev = "1";

    const auto plan = planSyncback(baselineThreeFiles(), now, {});
    CHECK(plan.restores.size() == 1);
    if (plan.restores.size() == 1) {
        CHECK(plan.restores[0].depotFile == "//depot/src/a.cpp");
        CHECK(plan.restores[0].rev == "3");
    }
}

TEST(syncback_restores_a_file_the_client_no_longer_has) {
    // Synced away, or deleted at head and synced: absent from `p4 have`, so the
    // recorded revision has to bring it back.
    std::vector<ManifestEntry> now = baselineThreeFiles();
    now.erase(now.begin() + 2);

    const auto plan = planSyncback(baselineThreeFiles(), now, {});
    CHECK(plan.removes.empty());
    CHECK(plan.restores.size() == 1);
    if (plan.restores.size() == 1) {
        CHECK(plan.restores[0].depotFile == "//depot/src/c.cpp");
        CHECK(plan.restores[0].rev == "1");
    }
}

TEST(syncback_removes_a_file_synced_in_since_the_import) {
    // The baseline never had it, so it is in neither the snapshot nor the
    // working tree; #0 drops p4's copy and restores the recorded state.
    std::vector<ManifestEntry> now = baselineThreeFiles();
    now.push_back({"//depot/src/new.cpp", "1"});

    const auto plan = planSyncback(baselineThreeFiles(), now, {});
    CHECK(plan.restores.empty());
    CHECK(plan.removes.size() == 1);
    if (plan.removes.size() == 1) {
        CHECK(plan.removes[0].depotFile == "//depot/src/new.cpp");
        CHECK(plan.removes[0].rev == "0");
    }
}

TEST(syncback_skips_a_drifted_file_that_is_open_in_p4) {
    // Syncing an opened file back would discard an unsubmitted resolve, so it
    // is reported instead of acted on.
    std::vector<ManifestEntry> now = baselineThreeFiles();
    now[1].rev = "9";

    const auto plan =
        planSyncback(baselineThreeFiles(), now, {"//depot/src/b.cpp"});
    CHECK(plan.restores.empty());
    CHECK(plan.removes.empty());
    CHECK(plan.skippedOpen.size() == 1);
    if (plan.skippedOpen.size() == 1) {
        CHECK(plan.skippedOpen[0] == "//depot/src/b.cpp");
    }
}

TEST(syncback_skips_an_open_file_synced_in_since_the_import) {
    std::vector<ManifestEntry> now = baselineThreeFiles();
    now.push_back({"//depot/src/new.cpp", "1"});

    const auto plan =
        planSyncback(baselineThreeFiles(), now, {"//depot/src/new.cpp"});
    CHECK(plan.removes.empty());
    CHECK(plan.skippedOpen.size() == 1);
    if (plan.skippedOpen.size() == 1) {
        CHECK(plan.skippedOpen[0] == "//depot/src/new.cpp");
    }
}

TEST(syncback_ignores_an_open_file_that_never_drifted) {
    // A pending `gw prepare` CL opens files without moving their have rev.
    // Those need no sync, so they are not reported as skipped either - only
    // drift the user has to resolve gets a warning.
    const auto plan = planSyncback(baselineThreeFiles(), baselineThreeFiles(),
                                   {"//depot/src/a.cpp", "//depot/src/b.cpp"});
    CHECK(plan.restores.empty());
    CHECK(plan.removes.empty());
    CHECK(plan.skippedOpen.empty());
}

TEST(syncback_handles_restores_and_removes_together_sorted) {
    std::vector<ManifestEntry> now = {
        {"//depot/src/c.cpp", "1"},
        {"//depot/src/z.cpp", "4"},   // synced in since the import
        {"//depot/src/b.cpp", "9"},   // moved forward
        {"//depot/src/a.cpp", "5"},   // moved forward
        {"//depot/src/m.cpp", "2"},   // synced in since the import
    };

    const auto plan = planSyncback(baselineThreeFiles(), now, {});
    CHECK(plan.restores.size() == 2);
    if (plan.restores.size() == 2) {
        // Sorted by depot path, so output is stable run to run.
        CHECK(plan.restores[0].depotFile == "//depot/src/a.cpp");
        CHECK(plan.restores[0].rev == "3");
        CHECK(plan.restores[1].depotFile == "//depot/src/b.cpp");
        CHECK(plan.restores[1].rev == "7");
    }
    CHECK(plan.removes.size() == 2);
    if (plan.removes.size() == 2) {
        CHECK(plan.removes[0].depotFile == "//depot/src/m.cpp");
        CHECK(plan.removes[1].depotFile == "//depot/src/z.cpp");
    }
}

TEST(syncback_restores_everything_when_the_client_has_nothing) {
    const auto plan = planSyncback(baselineThreeFiles(), {}, {});
    CHECK(plan.restores.size() == 3);
    CHECK(plan.removes.empty());
}

TEST(syncback_removes_everything_when_the_baseline_was_empty) {
    const auto plan = planSyncback({}, baselineThreeFiles(), {});
    CHECK(plan.restores.empty());
    CHECK(plan.removes.size() == 3);
    for (const auto& action : plan.removes) {
        CHECK(action.rev == "0");
    }
}
