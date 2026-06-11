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
        "mirror_path = ../p4gw-mirror\n"
        "client = aaron-dev\n"
        "baseline_branch = p4-base\n");
    CHECK(config.has_value());
    if (config) {
        CHECK(config->depotPath == "//depot/yourgame/src/...");
        CHECK(config->mirrorPath == "../p4gw-mirror");
        CHECK(config->client == "aaron-dev");
        CHECK(config->baselineBranch == "p4-base");
    }
}

TEST(config_resolves_relative_mirror_path) {
    p4gw::Config config;
    config.mirrorPath = "../p4gw-mirror";
    const fs::path resolved =
        p4gw::resolveMirrorPath(config, (fs::path("work") / "game" / "src").string());
    CHECK(resolved == fs::path("work") / "game" / "p4gw-mirror");
}

TEST(config_keeps_absolute_mirror_path) {
    p4gw::Config config;
    const fs::path absolute = fs::temp_directory_path() / "mirror";
    config.mirrorPath = absolute.string();
    CHECK(fs::path(p4gw::resolveMirrorPath(config, "elsewhere")) == absolute);
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
