#include "git.h"

#include "process.h"

namespace p4gw::git {

namespace {

std::string trimTrailing(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }
    return s;
}

}  // namespace

std::expected<std::string, std::string> run(const std::vector<std::string>& args,
                                            const std::string& cwd) {
    auto result = p4gw::run("git", args, cwd);
    if (!result) {
        return std::unexpected(result.error());
    }
    if (result->exitCode != 0) {
        std::string cmd = "git";
        for (const auto& arg : args) cmd += ' ' + arg;
        return std::unexpected(cmd + " failed:\n" + result->output);
    }
    return trimTrailing(result->output);
}

std::expected<std::string, std::string> currentBranch(const std::string& cwd) {
    return run({"rev-parse", "--abbrev-ref", "HEAD"}, cwd);
}

std::expected<std::string, std::string> revParse(const std::string& ref,
                                                 const std::string& cwd) {
    return run({"rev-parse", "--verify", ref}, cwd);
}

std::expected<bool, std::string> isDirty(const std::string& cwd) {
    auto output = run({"status", "--porcelain"}, cwd);
    if (!output) {
        return std::unexpected(output.error());
    }
    return !output->empty();
}

std::expected<std::vector<FileChange>, std::string> diffNameStatus(
    const std::string& fromRef, const std::string& toRef, const std::string& cwd) {
    auto output = run({"diff", "--name-status", "-M", fromRef, toRef}, cwd);
    if (!output) {
        return std::unexpected(output.error());
    }

    std::vector<FileChange> changes;
    size_t pos = 0;
    while (pos < output->size()) {
        size_t end = output->find('\n', pos);
        if (end == std::string::npos) end = output->size();
        const std::string line = output->substr(pos, end - pos);
        pos = end + 1;
        if (line.empty()) continue;

        FileChange change;
        change.status = line[0];
        const auto firstTab = line.find('\t');
        if (firstTab == std::string::npos) {
            return std::unexpected("unexpected diff --name-status line: " + line);
        }
        const auto secondTab = line.find('\t', firstTab + 1);
        if (secondTab == std::string::npos) {
            change.path = line.substr(firstTab + 1);
        } else {
            // Rename: STATUS<TAB>old<TAB>new
            change.path = line.substr(firstTab + 1, secondTab - firstTab - 1);
            change.newPath = line.substr(secondTab + 1);
        }
        changes.push_back(std::move(change));
    }
    return changes;
}

std::expected<std::string, std::string> commitMessages(const std::string& fromRef,
                                                       const std::string& toRef,
                                                       const std::string& cwd) {
    return run({"log", "--reverse", "--format=%B", fromRef + ".." + toRef}, cwd);
}

}  // namespace p4gw::git
