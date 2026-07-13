// SPDX-License-Identifier: MIT

#include <filesystem>
#include <string>

#include "subprocess.h"
#include "test_framework.h"

namespace fs = std::filesystem;

// Two calls must never return the same path (the whole point: concurrent gw
// runs or repeated scratch files within a run must not collide), and the name
// must still carry the caller's prefix and suffix.
TEST(unique_temp_file_is_unique_and_shaped) {
    const std::string a = p4gw::uniqueTempFile("p4gw_test", ".tmp");
    const std::string b = p4gw::uniqueTempFile("p4gw_test", ".tmp");
    CHECK(a != b);

    const std::string parent = fs::temp_directory_path().string();
    CHECK(a.rfind(parent, 0) == 0);  // lives in the temp directory

    const std::string nameA = fs::path(a).filename().string();
    CHECK(nameA.rfind("p4gw_test", 0) == 0);  // keeps the prefix
    CHECK(nameA.size() >= 4 &&
          nameA.compare(nameA.size() - 4, 4, ".tmp") == 0);  // keeps the suffix
}

// An empty suffix is allowed and produces no trailing separator.
TEST(unique_temp_file_empty_suffix) {
    const std::string p = p4gw::uniqueTempFile("p4gw_test");
    const std::string name = fs::path(p).filename().string();
    CHECK(name.rfind("p4gw_test", 0) == 0);
    CHECK(name.back() != '.');
}