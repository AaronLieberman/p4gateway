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

// Splits a `mapping` value into whitespace-separated tokens, honoring double
// quotes around paths that contain spaces.
std::vector<std::string> tokenize(const std::string& value) {
    std::vector<std::string> tokens;
    size_t pos = 0;
    while (pos < value.size()) {
        while (pos < value.size() && (value[pos] == ' ' || value[pos] == '\t')) {
            ++pos;
        }
        if (pos >= value.size()) break;
        std::string token;
        if (value[pos] == '"') {
            const auto close = value.find('"', pos + 1);
            if (close == std::string::npos) {
                token = value.substr(pos + 1);
                pos = value.size();
            } else {
                token = value.substr(pos + 1, close - pos - 1);
                pos = close + 1;
            }
        } else {
            const auto end = value.find_first_of(" \t", pos);
            token = value.substr(
                pos, (end == std::string::npos ? value.size() : end) - pos);
            pos = (end == std::string::npos) ? value.size() : end;
        }
        tokens.push_back(std::move(token));
    }
    return tokens;
}

}  // namespace

std::string mirrorRepoSubtree(const std::string& mirrorPath) {
    fs::path normalized = fs::path(mirrorPath).lexically_normal();
    auto it = normalized.begin();
    if (it == normalized.end()) return {};
    ++it;  // drop the leading `.p4gw` container component
    fs::path subtree;
    for (; it != normalized.end(); ++it) {
        if (it->string() == ".") continue;
        subtree /= *it;
    }
    return subtree.generic_string();
}

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
        const std::string where = path + ":" + std::to_string(lineNumber);

        if (key == "mapping") {
            const auto tokens = tokenize(value);
            if (tokens.size() != 2) {
                return std::unexpected(
                    where + ": 'mapping' takes two values: "
                    "<depot_path> <mirror_path>");
            }
            Mapping mapping;
            mapping.depotPath = tokens[0];
            mapping.mirrorPath = tokens[1];
            mapping.repoSubtree = mirrorRepoSubtree(tokens[1]);
            if (!mapping.depotPath.ends_with("/...")) {
                return std::unexpected(
                    where + ": depot path '" + mapping.depotPath +
                    "' must end with '/...'");
            }
            for (const auto& existing : config.mappings) {
                if (existing.depotPath == mapping.depotPath) {
                    return std::unexpected(where + ": depot path '" +
                                           mapping.depotPath +
                                           "' is mapped twice");
                }
                if (existing.mirrorPath == mapping.mirrorPath) {
                    return std::unexpected(where + ": mirror path '" +
                                           mapping.mirrorPath +
                                           "' is used by two mappings");
                }
            }
            config.mappings.push_back(std::move(mapping));
        } else if (key == "client") {
            config.client = value;
        } else if (key == "baseline_branch") {
            config.baselineBranch = value;
        } else {
            return std::unexpected(where + ": unknown key '" + key + "'");
        }
    }

    if (config.mappings.empty()) {
        return std::unexpected(path + ": no 'mapping' lines - add at least one "
                               "('gw setup' writes the template). Format: "
                               "mapping = //depot/yourgame/src/... .p4gw/src");
    }
    return config;
}

std::string findConfigFile(const std::string& startDir) {
    fs::path dir = fs::absolute(startDir);
    while (true) {
        const fs::path candidate = dir / "p4gw.cfg";
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) {
            return candidate.string();
        }
        if (!dir.has_parent_path() || dir.parent_path() == dir) {
            return {};
        }
        dir = dir.parent_path();
    }
}

std::expected<Config, std::string> findAndLoadConfig(std::string& rootDir) {
    const std::string file = findConfigFile(fs::current_path().string());
    if (file.empty()) {
        return std::unexpected(
            "no p4gw.cfg config found in this directory or any parent; "
            "run 'gw setup' first");
    }
    rootDir = fs::path(file).parent_path().string();
    return loadConfig(file);
}

std::string resolveMirrorPath(const std::string& mirrorPath,
                              const std::string& rootDir) {
    fs::path mirror(mirrorPath);
    if (mirror.is_relative()) {
        mirror = fs::path(rootDir) / mirror;
    }
    return mirror.lexically_normal().string();
}

}  // namespace p4gw
