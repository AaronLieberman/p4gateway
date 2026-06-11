#include <cstdio>
#include <filesystem>
#include <fstream>

#include "config.h"
#include "test_framework.h"

namespace fs = std::filesystem;

namespace {

// Writes `content` to a temp file and loads it as a config.
std::expected<p4gw::Config, std::string> loadFromString(const std::string& content) {
    const fs::path path = fs::temp_directory_path() / "p4gw_test_config";
    {
        std::ofstream file(path);
        file << content;
    }
    auto result = p4gw::loadConfig(path.string());
    fs::remove(path);
    return result;
}

}  // namespace

TEST(config_parses_all_keys) {
    auto config = loadFromString(
        "# overlay for the engine source tree\n"
        "depot_path = //depot/yourgame/src/...\n"
        "client = aaron-dev\n"
        "baseline_branch = p4-base\n");
    CHECK(config.has_value());
    if (config) {
        CHECK(config->depotPath == "//depot/yourgame/src/...");
        CHECK(config->client == "aaron-dev");
        CHECK(config->baselineBranch == "p4-base");
    }
}

TEST(config_defaults_baseline_branch) {
    auto config = loadFromString("depot_path = //depot/yourgame/src/...\n");
    CHECK(config.has_value());
    if (config) {
        CHECK(config->client.empty());
        CHECK(config->baselineBranch == "p4-main");
    }
}

TEST(config_requires_depot_path) {
    auto config = loadFromString("client = aaron-dev\n");
    CHECK(!config.has_value());
}

TEST(config_rejects_unknown_keys) {
    auto config = loadFromString(
        "depot_path = //depot/x/...\n"
        "depot_paht = typo\n");
    CHECK(!config.has_value());
}

TEST(config_rejects_malformed_lines) {
    auto config = loadFromString("depot_path //depot/x/...\n");
    CHECK(!config.has_value());
}
