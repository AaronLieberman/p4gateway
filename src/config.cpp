#include "config.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace p4gw {

namespace {

std::string trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

}  // namespace

std::expected<Config, std::string> loadConfig(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        return std::unexpected("cannot open config file: " + path);
    }

    Config config;
    std::string line;
    int lineNumber = 0;
    while (std::getline(file, line)) {
        ++lineNumber;
        const std::string stripped = trim(line);
        if (stripped.empty() || stripped[0] == '#') continue;

        const auto eq = stripped.find('=');
        if (eq == std::string::npos) {
            return std::unexpected(path + ":" + std::to_string(lineNumber) +
                                   ": expected 'key = value'");
        }
        const std::string key = trim(stripped.substr(0, eq));
        const std::string value = trim(stripped.substr(eq + 1));

        if (key == "depot_path") {
            config.depotPath = value;
        } else if (key == "client") {
            config.client = value;
        } else if (key == "baseline_branch") {
            config.baselineBranch = value;
        } else {
            return std::unexpected(path + ":" + std::to_string(lineNumber) +
                                   ": unknown key '" + key + "'");
        }
    }

    if (config.depotPath.empty()) {
        return std::unexpected(path + ": missing required key 'depot_path'");
    }
    return config;
}

std::expected<Config, std::string> findAndLoadConfig(std::string& rootDir) {
    fs::path dir = fs::current_path();
    while (true) {
        const fs::path candidate = dir / ".p4gw";
        if (fs::exists(candidate)) {
            rootDir = dir.string();
            return loadConfig(candidate.string());
        }
        if (!dir.has_parent_path() || dir.parent_path() == dir) {
            return std::unexpected(
                "no .p4gw config found in this directory or any parent; "
                "run 'gw init' first");
        }
        dir = dir.parent_path();
    }
}

}  // namespace p4gw
