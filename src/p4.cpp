#include "p4.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "process.h"

namespace fs = std::filesystem;

namespace p4gw::p4 {

namespace {

// Keep each spawned p4 command line comfortably under the ~8k cmd.exe limit.
constexpr size_t kMaxFilesPerCall = 16;

std::vector<std::string> clientArgs(const Config& config) {
    std::vector<std::string> args;
    if (!config.client.empty()) {
        args.push_back("-c");
        args.push_back(config.client);
    }
    return args;
}

std::string commandLine(const std::vector<std::string>& args) {
    std::string cmd = "p4";
    for (const auto& arg : args) cmd += ' ' + arg;
    return cmd;
}

std::expected<std::string, std::string> runWithOptions(
    const Config& config, const std::vector<std::string>& args,
    const RunOptions& options) {
    std::vector<std::string> fullArgs = clientArgs(config);
    fullArgs.insert(fullArgs.end(), args.begin(), args.end());

    auto result = p4gw::run("p4", fullArgs, options);
    if (!result) {
        return std::unexpected(result.error());
    }
    if (result->exitCode != 0) {
        return std::unexpected(commandLine(fullArgs) + " failed:\n" +
                               result->output);
    }
    return result->output;
}

// Opens `files` with `p4 <verb> -c <cl>`, chunked.
std::expected<std::string, std::string> openFiles(
    const Config& config, const std::string& verb, const std::string& cl,
    const std::vector<std::string>& files) {
    std::string output;
    for (size_t i = 0; i < files.size(); i += kMaxFilesPerCall) {
        std::vector<std::string> args{verb, "-c", cl};
        for (size_t j = i; j < files.size() && j < i + kMaxFilesPerCall; ++j) {
            args.push_back(files[j]);
        }
        auto result = run(config, args);
        if (!result) {
            return std::unexpected(result.error());
        }
        output += *result;
    }
    return output;
}

std::string trimWhitespace(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

// Splits a view mapping line into whitespace-separated tokens, honoring
// double quotes around paths with spaces.
std::vector<std::string> tokenizeViewLine(const std::string& line) {
    std::vector<std::string> tokens;
    size_t pos = 0;
    while (pos < line.size()) {
        while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
            ++pos;
        }
        if (pos >= line.size()) break;
        std::string token;
        if (line[pos] == '"') {
            const auto close = line.find('"', pos + 1);
            if (close == std::string::npos) {
                token = line.substr(pos + 1);
                pos = line.size();
            } else {
                token = line.substr(pos + 1, close - pos - 1);
                pos = close + 1;
            }
        } else {
            const auto end = line.find_first_of(" \t", pos);
            token = line.substr(pos, (end == std::string::npos ? line.size() : end) - pos);
            pos = (end == std::string::npos) ? line.size() : end;
        }
        tokens.push_back(std::move(token));
    }
    return tokens;
}

// "//depot/x/..." -> "//depot/x/" so prefix comparison works.
std::string stripWildcard(const std::string& path) {
    if (path.ends_with("...")) {
        return path.substr(0, path.size() - 3);
    }
    return path;
}

}  // namespace

std::expected<std::string, std::string> run(const Config& config,
                                            const std::vector<std::string>& args) {
    return runWithOptions(config, args, RunOptions{});
}

std::string specField(const std::string& spec, const std::string& field) {
    const std::string prefix = field + ":";
    size_t pos = 0;
    while (pos < spec.size()) {
        size_t end = spec.find('\n', pos);
        if (end == std::string::npos) end = spec.size();
        const std::string line = spec.substr(pos, end - pos);
        pos = end + 1;
        if (line.starts_with(prefix)) {
            return trimWhitespace(line.substr(prefix.size()));
        }
    }
    return {};
}

std::vector<ViewLine> parseClientView(const std::string& spec) {
    std::vector<ViewLine> view;
    bool inView = false;
    size_t pos = 0;
    while (pos < spec.size()) {
        size_t end = spec.find('\n', pos);
        if (end == std::string::npos) end = spec.size();
        std::string line = spec.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line.starts_with("View:")) {
            inView = true;
            continue;
        }
        if (!inView) continue;
        // The View: section is the indented lines that follow it; the first
        // non-indented, non-empty line ends it.
        if (!line.empty() && line[0] != ' ' && line[0] != '\t') break;

        auto tokens = tokenizeViewLine(line);
        if (tokens.size() != 2) continue;

        ViewLine mapping;
        std::string& depot = tokens[0];
        if (depot.starts_with("-")) {
            mapping.exclude = true;
            depot.erase(0, 1);
        } else if (depot.starts_with("+")) {
            mapping.overlay = true;
            depot.erase(0, 1);
        }
        mapping.depot = depot;
        mapping.client = tokens[1];
        view.push_back(std::move(mapping));
    }
    return view;
}

std::vector<ViewProblem> checkViewMapping(
    const std::vector<ViewLine>& view, const std::string& depotPath,
    const std::string& expectedClientPath,
    const std::string& repoClientPrefix,
    const std::vector<std::string>& excludedDepotPaths,
    const std::vector<std::string>& extraMirrorPrefixes) {
    std::vector<ViewProblem> problems;

    // The mirror typically lives inside the repo (the recommended `.p4gw`),
    // so its client path is itself under the repo prefix. That mapping is the
    // whole point, so exempt anything under the mirror from the "maps into the
    // repo" check below while still catching every other line.
    const std::string mirrorPrefix = stripWildcard(expectedClientPath);
    const std::string depotBase = stripWildcard(depotPath);  // ends with '/'

    // A client path that lands in this include's mirror or any sibling mirror
    // (another mapped subtree, or a re-include whose mirror nests inside this
    // one) is a mirror mapping, not a repo leak.
    auto intoAnyMirror = [&](const std::string& clientBase) {
        if (!mirrorPrefix.empty() && clientBase.starts_with(mirrorPrefix)) {
            return true;
        }
        for (const auto& prefix : extraMirrorPrefixes) {
            if (!prefix.empty() && clientBase.starts_with(prefix)) return true;
        }
        return false;
    };

    // A depot path lies under one of the carved-out exclude subtrees, so its
    // in-place client mapping is intentional (the config gitignores it).
    auto underExclude = [&](const std::string& lineDepotBase) {
        for (const auto& ex : excludedDepotPaths) {
            if (lineDepotBase.starts_with(stripWildcard(ex))) return true;
        }
        return false;
    };

    // Later view lines win per depot file. Where the *bulk* of depotPath lands
    // is set by the last line whose scope covers depotPath (it or broader);
    // narrower lines only carve up sub-paths and must not shadow the bulk remap.
    const ViewLine* effective = nullptr;
    for (const auto& line : view) {
        const std::string lineBase = stripWildcard(line.depot);
        const std::string clientBase = stripWildcard(line.client);
        if (depotBase.starts_with(lineBase)) {
            effective = &line;  // this line's scope covers all of depotPath
        }

        // A `-` line removes content (never writes into the repo), a line into
        // the mirror is the whole point, and a declared exclude is intentional:
        // none of these need flagging.
        if (line.exclude || intoAnyMirror(clientBase) || underExclude(lineBase))
            continue;

        // A line strictly under the mapped subtree that lands anywhere but the
        // mirror diverts part of depotPath out of it - typically an in-place
        // sync into the repo working tree. gw must be told (an `exclude` line)
        // so it gitignores the path and ships nothing through it; otherwise p4
        // would write into a Git-tracked directory. This keys off the depot
        // subtree, so it fires even when the repo is the client root and the
        // repo-prefix test below is disabled (every sync lands under the root).
        const bool underDepot =
            lineBase.starts_with(depotBase) && lineBase != depotBase;
        if (underDepot) {
            problems.push_back(
                {"'" + line.depot + " " + line.client + "' diverts part of " +
                     depotPath + " out of the mirror - add 'exclude = " +
                     line.depot + "' to p4gw.cfg so gw gitignores it",
                 line.depot});
        } else if (!repoClientPrefix.empty() &&
                   clientBase.starts_with(repoClientPrefix)) {
            // Content outside the subtree that still lands in the repo dir.
            problems.push_back(
                {"view line '" + line.depot + " " + line.client +
                     "' maps depot files into the Git repo directory; "
                     "P4 must never write there (declare it with an "
                     "'exclude' line if it should sync in place)",
                 ""});
        }
    }

    if (effective == nullptr) {
        problems.push_back(
            {"depot path " + depotPath + " is not mapped in the client view",
             ""});
    } else if (effective->exclude) {
        problems.push_back(
            {"depot path " + depotPath + " is excluded by view line '-" +
                 effective->depot + " " + effective->client +
                 "' - the mirror would be empty",
             ""});
    } else if (effective->depot != depotPath ||
               effective->client != expectedClientPath) {
        problems.push_back(
            {"the effective mapping for " + depotPath + " is '" +
                 effective->depot + " " + effective->client + "'; expected '" +
                 depotPath + " " + expectedClientPath +
                 "' (place it after any view line it overlaps - later lines "
                 "win)",
             ""});
    }
    return problems;
}

std::vector<std::string> minimalExcludePaths(
    const std::vector<std::string>& excludePaths) {
    std::vector<std::string> unique;
    for (const auto& p : excludePaths) {
        if (std::find(unique.begin(), unique.end(), p) == unique.end()) {
            unique.push_back(p);
        }
    }
    std::vector<std::string> result;
    for (const auto& p : unique) {
        const std::string pBase = stripWildcard(p);  // ends with '/'
        bool nested = false;
        for (const auto& q : unique) {
            if (q == p) continue;
            const std::string qBase = stripWildcard(q);
            // p strictly under q: excluding q already covers p.
            if (pBase.size() > qBase.size() && pBase.starts_with(qBase)) {
                nested = true;
                break;
            }
        }
        if (!nested) result.push_back(p);
    }
    return result;
}

std::string clientViewPath(const std::string& clientName,
                           const std::string& clientRoot,
                           const std::string& localDir,
                           const std::string& suffix) {
    std::error_code ec;
    const fs::path rel = fs::relative(localDir, clientRoot, ec);
    if (ec || rel.empty()) return {};
    const std::string relStr = rel.generic_string();
    if (relStr == "." || relStr == ".." || relStr.starts_with("../")) {
        return {};
    }
    return "//" + clientName + "/" + relStr + suffix;
}

std::vector<ViewProblem> checkSpecMapping(
    const std::string& spec, const std::string& depotPath,
    const std::string& repoDir, const std::string& mirrorDir,
    const std::vector<std::string>& excludedDepotPaths,
    const std::vector<std::string>& otherMirrorDirs) {
    const std::string clientName = specField(spec, "Client");
    const std::string clientRoot = specField(spec, "Root");
    if (clientName.empty() || clientRoot.empty()) {
        return {{"client spec has no Client:/Root: field", ""}};
    }
    const std::string expectedClientPath =
        clientViewPath(clientName, clientRoot, mirrorDir, "/...");
    if (expectedClientPath.empty()) {
        return {{"mirror " + mirrorDir + " is not inside the client root " +
                     clientRoot + " - p4 cannot map it",
                 ""}};
    }
    const std::string repoPrefix =
        clientViewPath(clientName, clientRoot, repoDir, "/");
    // Client-side prefixes of the config's other mirrors, so sibling and nested
    // (re-include) mirrors are not mistaken for repo leaks. A mirror outside the
    // client root has no client path; skip it.
    std::vector<std::string> extraMirrorPrefixes;
    for (const auto& other : otherMirrorDirs) {
        const std::string prefix =
            clientViewPath(clientName, clientRoot, other, "/");
        if (!prefix.empty()) extraMirrorPrefixes.push_back(prefix);
    }
    return checkViewMapping(parseClientView(spec), depotPath,
                            expectedClientPath, repoPrefix, excludedDepotPaths,
                            extraMirrorPrefixes);
}

std::expected<std::string, std::string> info(const Config& config) {
    return run(config, {"info"});
}

std::expected<std::string, std::string> clientSpec(const Config& config) {
    return run(config, {"client", "-o"});
}

std::expected<std::string, std::string> clientRoot(const Config& config) {
    auto spec = clientSpec(config);
    if (!spec) {
        return std::unexpected(spec.error());
    }
    std::string root = specField(*spec, "Root");
    if (root.empty()) {
        return std::unexpected("p4 client -o output has no Root: field");
    }
    return root;
}

std::expected<std::string, std::string> createChangelist(
    const Config& config, const std::string& description) {
    // Build a changelist spec for `p4 change -i`. Description lines must be
    // tab-indented; p4 fills in the remaining fields.
    std::string spec = "Change:\tnew\n\nDescription:\n";
    size_t pos = 0;
    bool wroteLine = false;
    while (pos <= description.size()) {
        size_t end = description.find('\n', pos);
        if (end == std::string::npos) end = description.size();
        std::string line = description.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        spec += '\t' + line + '\n';
        wroteLine = true;
        if (end == description.size()) break;
        pos = end + 1;
    }
    if (!wroteLine) {
        spec += "\t(no description)\n";
    }

    const fs::path specPath = uniqueTempFile("p4gw_change_spec", ".txt");
    {
        std::ofstream file(specPath, std::ios::binary);
        if (!file) {
            return std::unexpected("cannot write temp file: " +
                                   specPath.string());
        }
        file << spec;
    }
    RunOptions options;
    options.stdinFile = specPath.string();
    auto result = runWithOptions(config, {"change", "-i"}, options);
    std::error_code ec;
    fs::remove(specPath, ec);
    if (!result) {
        return std::unexpected(result.error());
    }

    // Expected response: "Change 12345 created."
    const auto changePos = result->find("Change ");
    if (changePos == std::string::npos) {
        return std::unexpected("unexpected p4 change -i output:\n" + *result);
    }
    const auto numberStart = changePos + 7;
    auto numberEnd = numberStart;
    while (numberEnd < result->size() && std::isdigit((*result)[numberEnd])) {
        ++numberEnd;
    }
    if (numberEnd == numberStart) {
        return std::unexpected("unexpected p4 change -i output:\n" + *result);
    }
    return result->substr(numberStart, numberEnd - numberStart);
}

std::expected<std::string, std::string> editFiles(
    const Config& config, const std::string& cl,
    const std::vector<std::string>& files) {
    return openFiles(config, "edit", cl, files);
}

std::expected<std::string, std::string> addFiles(
    const Config& config, const std::string& cl,
    const std::vector<std::string>& files) {
    return openFiles(config, "add", cl, files);
}

std::expected<std::string, std::string> deleteFiles(
    const Config& config, const std::string& cl,
    const std::vector<std::string>& files) {
    return openFiles(config, "delete", cl, files);
}

std::expected<std::string, std::string> moveFile(const Config& config,
                                                 const std::string& cl,
                                                 const std::string& from,
                                                 const std::string& to) {
    return run(config, {"move", "-c", cl, from, to});
}

bool reconcileReportsClean(const std::string& output) {
    std::string lower = output;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return lower.find("no file(s) to reconcile") != std::string::npos;
}

std::expected<std::string, std::string> reconcilePreview(const Config& config) {
    std::vector<std::string> args = clientArgs(config);
    args.insert(args.end(), {"reconcile", "-n"});
    for (const auto* rule : includeRules(config.rules)) {
        args.push_back(rule->depotPath);
    }
    auto result = p4gw::run("p4", args);
    if (!result) {
        return std::unexpected(result.error());
    }
    // With nothing to do, p4 prints "... - no file(s) to reconcile." and may
    // exit non-zero; that's the clean case, not an error.
    if (reconcileReportsClean(result->output)) {
        return std::string{};
    }
    if (result->exitCode != 0) {
        return std::unexpected(commandLine(args) + " failed:\n" +
                               result->output);
    }
    return result->output;
}

std::expected<std::string, std::string> reconcilePreviewFiles(
    const Config& config, const std::vector<std::string>& files) {
    std::string combined;
    for (size_t i = 0; i < files.size(); i += kMaxFilesPerCall) {
        std::vector<std::string> args = clientArgs(config);
        args.insert(args.end(), {"reconcile", "-n"});
        for (size_t j = i; j < files.size() && j < i + kMaxFilesPerCall; ++j) {
            args.push_back(files[j]);
        }
        auto result = p4gw::run("p4", args);
        if (!result) {
            return std::unexpected(result.error());
        }
        // A clean chunk reports "No file(s) to reconcile." (capitalized for an
        // explicit file list, and possibly with a non-zero exit); skip it.
        // Anything else is either real preview output or a failure.
        if (reconcileReportsClean(result->output)) {
            continue;
        }
        if (result->exitCode != 0) {
            return std::unexpected(commandLine(args) + " failed:\n" +
                                   result->output);
        }
        combined += result->output;
    }
    return combined;
}

std::expected<std::string, std::string> latestSubmittedCl(const Config& config) {
    std::vector<std::string> args{"changes", "-m1", "-s", "submitted"};
    for (const auto* rule : includeRules(config.rules)) {
        args.push_back(rule->depotPath + "#have");
    }
    auto result = run(config, args);
    if (!result) {
        return std::unexpected(result.error());
    }
    // Expected: "Change 481467 on 2026/06/10 by user@client '...'"
    if (!result->starts_with("Change ")) {
        return std::string{};
    }
    const auto numberStart = 7;
    auto numberEnd = result->find(' ', numberStart);
    if (numberEnd == std::string::npos) {
        return std::string{};
    }
    return result->substr(numberStart, numberEnd - numberStart);
}

std::expected<std::string, std::string> openedFiles(const Config& config) {
    // Delegate to the tagged query so excluded-subtree opens are filtered out
    // the same way prepare/import see them - otherwise status would over-count a
    // "pending CL" and doctor would warn on the user's own (excluded) P4 edits.
    // One line per kept file.
    auto tagged = openedFilesTagged(config);
    if (!tagged) {
        return std::unexpected(tagged.error());
    }
    std::string out;
    for (const auto& file : *tagged) {
        out += file.depotFile + " - " + file.action + "\n";
    }
    return out;
}

std::expected<std::string, std::string> currentUser(const Config& config) {
    auto info = run(config, {"info"});
    if (!info) {
        return std::unexpected(info.error());
    }
    std::string user = specField(*info, "User name");
    if (user.empty()) {
        return std::unexpected("p4 info output has no 'User name' field");
    }
    return user;
}

std::expected<std::string, std::string> currentClient(const Config& config) {
    auto info = run(config, {"info"});
    if (!info) {
        return std::unexpected(info.error());
    }
    std::string client = specField(*info, "Client name");
    if (client.empty()) {
        return std::unexpected("p4 info output has no 'Client name' field");
    }
    return client;
}

std::expected<std::string, std::string> changes(const Config& config,
                                                const std::string& status,
                                                const std::string& user,
                                                const std::string& client) {
    std::vector<std::string> args{"-ztag", "changes", "-s", status};
    if (!user.empty()) {
        args.push_back("-u");
        args.push_back(user);
    }
    if (!client.empty()) {
        args.push_back("-c");
        args.push_back(client);
    }
    for (const auto* rule : includeRules(config.rules)) {
        args.push_back(rule->depotPath);
    }
    return run(config, args);
}

std::expected<std::string, std::string> describeShelved(const Config& config,
                                                        const std::string& cl) {
    return run(config, {"-ztag", "describe", "-S", cl});
}

std::expected<std::string, std::string> printDepotFile(
    const Config& config, const std::string& fileSpec,
    const std::string& destFile) {
    // -q drops the "//depot/foo#rev - ..." header line; -o writes the file
    // content directly, so binaries round-trip without shell redirection.
    return run(config, {"print", "-q", "-o", destFile, fileSpec});
}

std::vector<OpenedFile> parseTaggedOpened(const std::string& ztagOutput) {
    std::vector<OpenedFile> files;
    OpenedFile current;
    bool have = false;
    auto flush = [&]() {
        if (have && !current.depotFile.empty()) files.push_back(current);
        current = OpenedFile{};
        have = false;
    };
    // -ztag records are blank-line separated; a sentinel ends the last one.
    std::istringstream lines(ztagOutput + "\n\n");
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) {
            flush();
            continue;
        }
        if (line.starts_with("... depotFile ")) {
            current.depotFile = line.substr(14);
            have = true;
        } else if (line.starts_with("... action ")) {
            current.action = line.substr(11);
            have = true;
        }
    }
    flush();
    return files;
}

std::vector<std::string> parseTaggedDepotFiles(const std::string& ztagOutput) {
    std::vector<std::string> files;
    std::istringstream lines(ztagOutput);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.starts_with("... depotFile ")) {
            files.push_back(line.substr(14));
        }
    }
    return files;
}

std::string depotRelativePath(const std::string& depotPath,
                              const std::string& depotFile) {
    const std::string base = stripWildcard(depotPath);  // ends with '/'
    if (!base.empty() && depotFile.starts_with(base)) {
        return depotFile.substr(base.size());
    }
    return {};
}

bool isAddAction(const std::string& action) {
    return action == "add" || action == "move/add" || action == "branch";
}

std::vector<OpenedFile> filterExcludedOpens(
    const std::vector<OpenedFile>& opened,
    const std::vector<ViewRule>& rules) {
    std::vector<OpenedFile> kept;
    for (const auto& file : opened) {
        // The effective rule resolves the ordered include/exclude lines
        // later-wins, so a re-included subtree under an excluded parent is kept
        // while the excluded parent itself is dropped.
        const ViewRule* rule = effectiveRuleForDepot(rules, file.depotFile);
        if (rule != nullptr && !rule->exclude) kept.push_back(file);
    }
    return kept;
}

std::expected<std::vector<OpenedFile>, std::string> openedFilesTagged(
    const Config& config) {
    std::vector<std::string> args = clientArgs(config);
    args.insert(args.end(), {"-ztag", "opened"});
    for (const auto* rule : includeRules(config.rules)) {
        args.push_back(rule->depotPath);
    }
    auto result = p4gw::run("p4", args);
    if (!result) {
        return std::unexpected(result.error());
    }
    if (result->output.find("not opened") != std::string::npos) {
        return std::vector<OpenedFile>{};
    }
    if (result->exitCode != 0) {
        return std::unexpected(commandLine(args) + " failed:\n" +
                               result->output);
    }
    // `p4 opened` is scoped to the include depot paths, so it also reports files
    // open under excluded subtrees (vendored libs the user edits directly in
    // P4). Those are not part of any gw changelist, so drop them (via the
    // ordered rules) - otherwise prepare's opened-files preflight would refuse
    // to run on account of the user's own unrelated P4 work.
    return filterExcludedOpens(parseTaggedOpened(result->output), config.rules);
}

std::expected<std::vector<std::string>, std::string> haveFiles(
    const Config& config, const std::string& depotPath) {
    std::vector<std::string> args = clientArgs(config);
    args.insert(args.end(), {"-ztag", "have", depotPath});
    auto result = p4gw::run("p4", args);
    if (!result) {
        return std::unexpected(result.error());
    }
    // Nothing synced under the subtree is a normal, empty result - not an
    // error - so it must not be confused with a failed call (which would
    // otherwise make import treat every mirror file as a stray).
    if (result->output.find("file(s) not on client") != std::string::npos ||
        result->output.find("no such file") != std::string::npos) {
        return std::vector<std::string>{};
    }
    if (result->exitCode != 0) {
        return std::unexpected(commandLine(args) + " failed:\n" +
                               result->output);
    }
    return parseTaggedDepotFiles(result->output);
}

std::expected<void, std::string> printHeadToFile(const Config& config,
                                                 const std::string& depotFile,
                                                 const std::string& dest) {
    auto result = printDepotFile(config, depotFile + "#head", dest);
    if (!result) {
        return std::unexpected(result.error());
    }
    return {};
}

std::expected<std::string, std::string> whereDepotDir(const Config& config,
                                                      const std::string& localDir) {
    auto result = run(config, {"-ztag", "where", localDir + "/..."});
    if (!result) {
        return std::unexpected(result.error());
    }
    // -ztag records are blank-line separated; records for excluded view
    // lines carry an "unmap" tag. Use the first mapped record's depotFile.
    std::string depot;
    bool unmapped = false;
    std::istringstream lines(*result + "\n\n");  // sentinel ends the record
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) {
            if (!depot.empty() && !unmapped) break;
            depot.clear();
            unmapped = false;
            continue;
        }
        if (line.starts_with("... unmap")) unmapped = true;
        if (line.starts_with("... depotFile ")) depot = line.substr(14);
    }
    if (depot.empty()) {
        return std::unexpected("p4 where returned no mapping for " +
                               localDir + ":\n" + *result);
    }
    if (depot.ends_with("/...")) {
        depot.resize(depot.size() - 4);
    }
    return depot;
}

std::expected<std::string, std::string> sync(const Config& config,
                                             const std::string& pathSpec) {
    auto result = run(config, {"sync", pathSpec});
    if (!result &&
        result.error().find("up-to-date") != std::string::npos) {
        return std::string{};
    }
    return result;
}

std::expected<std::string, std::string> syncForce(const Config& config,
                                                  const std::string& pathSpec) {
    auto result = run(config, {"sync", "-f", pathSpec});
    if (!result &&
        result.error().find("up-to-date") != std::string::npos) {
        return std::string{};
    }
    return result;
}

std::expected<std::string, std::string> revert(const Config& config,
                                               const std::string& pathSpec) {
    auto result = run(config, {"revert", pathSpec});
    if (!result && result.error().find("not opened") != std::string::npos) {
        return std::string{};
    }
    return result;
}

std::expected<std::string, std::string> revertChangelist(const Config& config,
                                                         const std::string& cl) {
    // -w so files opened for add are deleted from the mirror on revert (a plain
    // revert leaves them behind as strays); edits and deletes are restored from
    // the depot head. Scope to the configured depot paths so we revert only this
    // CL's files under the mirror, never an unscoped sweep of the workspace.
    std::vector<std::string> args{"revert", "-w", "-c", cl};
    for (const auto* rule : includeRules(config.rules)) {
        args.push_back(rule->depotPath);
    }
    auto result = run(config, args);
    if (!result && result.error().find("not opened") != std::string::npos) {
        return std::string{};
    }
    return result;
}

std::expected<std::string, std::string> reconcileToCl(const Config& config,
                                                      const std::string& cl,
                                                      const std::string& pathSpec) {
    auto result = run(config, {"reconcile", "-c", cl, pathSpec});
    if (!result && reconcileReportsClean(result.error())) {
        return std::string{};
    }
    return result;
}

std::expected<std::string, std::string> openedInCl(const Config& config,
                                                   const std::string& cl) {
    auto result = run(config, {"opened", "-c", cl});
    // p4 reports "not opened" in various forms, sometimes via exit 0 (stdout)
    // and sometimes via exit non-zero (stderr/error).
    auto contains = [](const std::string& s, const char* sub) {
        return s.find(sub) != std::string::npos;
    };
    if (result && contains(*result, "not opened anywhere")) return std::string{};
    if (!result && (contains(result.error(), "not opened") ||
                    contains(result.error(), "no file(s) opened"))) {
        return std::string{};
    }
    return result;
}

std::expected<std::string, std::string> submit(const Config& config,
                                               const std::string& cl) {
    return run(config, {"submit", "-c", cl});
}

std::expected<std::string, std::string> shelve(const Config& config,
                                               const std::string& cl) {
    return run(config, {"shelve", "-c", cl});
}

std::expected<std::string, std::string> deleteShelve(const Config& config,
                                                     const std::string& cl) {
    return run(config, {"shelve", "-d", "-c", cl});
}

std::expected<std::string, std::string> deleteChangelist(const Config& config,
                                                         const std::string& cl) {
    return run(config, {"change", "-d", cl});
}

std::expected<std::string, std::string> obliterate(const Config& config,
                                                   const std::string& depotFile) {
    auto result = run(config, {"obliterate", "-y", depotFile});
    if (!result &&
        result.error().find("no file(s) to obliterate") != std::string::npos) {
        return std::string{};
    }
    return result;
}

std::expected<std::string, std::string> pendingChangelistsForUser(
    const Config& config, const std::string& user) {
    return run(config, {"-ztag", "changes", "-s", "pending", "-u", user});
}

std::string serverIdFromInfo(const std::string& info) {
    std::string id = specField(info, "ServerID");
    if (id.empty()) id = specField(info, "Server ID");
    return id;
}

int securityLevelFromShow(const std::string& configureShowOutput) {
    // Lines look like "security=1" or "security=1 (configure)"; an unset
    // configurable yields no such line, which means level 0.
    const std::string key = "security=";
    const auto pos = configureShowOutput.find(key);
    if (pos == std::string::npos) return 0;
    int value = 0;
    bool any = false;
    for (size_t i = pos + key.size();
         i < configureShowOutput.size() &&
         std::isdigit(static_cast<unsigned char>(configureShowOutput[i]));
         ++i) {
        value = value * 10 + (configureShowOutput[i] - '0');
        any = true;
    }
    return any ? value : 0;
}

std::expected<std::string, std::string> serverId(const Config& config) {
    auto info = run(config, {"info"});
    if (!info) {
        return std::unexpected(info.error());
    }
    return serverIdFromInfo(*info);
}

std::expected<int, std::string> securityLevel(const Config& config) {
    // Read the raw result so an unset configurable (which p4 may report via a
    // non-zero exit and/or "No configurables have been set") still parses as 0
    // rather than surfacing as an error.
    std::vector<std::string> args = clientArgs(config);
    args.insert(args.end(), {"configure", "show", "security"});
    auto result = p4gw::run("p4", args);
    if (!result) {
        return std::unexpected(result.error());
    }
    return securityLevelFromShow(result->output);
}

}  // namespace p4gw::p4
