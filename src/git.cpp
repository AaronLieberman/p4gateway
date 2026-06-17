#include "git.h"

#include <sstream>

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

std::expected<bool, std::string> branchExists(const std::string& branch,
                                              const std::string& cwd) {
    auto result = p4gw::run("git",
                            {"rev-parse", "--verify", "--quiet",
                             "refs/heads/" + branch},
                            cwd);
    if (!result) {
        return std::unexpected(result.error());
    }
    return result->exitCode == 0;
}

std::expected<std::string, std::string> switchBranch(const std::string& branch,
                                                     const std::string& cwd) {
    return run({"switch", branch}, cwd);
}

std::expected<std::string, std::string> switchDetached(const std::string& ref,
                                                       const std::string& cwd) {
    return run({"switch", "--detach", ref}, cwd);
}

std::expected<void, std::string> updateRef(const std::string& ref,
                                           const std::string& target,
                                           const std::string& cwd) {
    auto result = run({"update-ref", ref, target}, cwd);
    if (!result) return std::unexpected(result.error());
    return {};
}

std::expected<std::string, std::string> mergeFastForward(const std::string& ref,
                                                         const std::string& cwd) {
    return run({"merge", "--ff-only", ref}, cwd);
}

std::expected<std::string, std::string> latestCommitMatching(
    const std::string& pattern, const std::string& ref, const std::string& cwd) {
    // --grep with --extended-regexp; --max-count=1 keeps only the newest match
    // (rev-list lists newest first). Empty output means no commit matched.
    return run({"rev-list", "--max-count=1", "--extended-regexp",
                "--grep=" + pattern, ref},
               cwd);
}

std::expected<std::string, std::string> switchOrphanBranch(
    const std::string& branch, const std::string& cwd) {
    return run({"switch", "--orphan", branch}, cwd);
}

std::expected<std::string, std::string> createBranch(const std::string& branch,
                                                     const std::string& startRef,
                                                     const std::string& cwd) {
    return run({"switch", "-c", branch, startRef}, cwd);
}

std::expected<bool, std::string> mergeFile(const std::string& ours,
                                           const std::string& base,
                                           const std::string& theirs,
                                           const std::string& cwd) {
    auto result = p4gw::run("git", {"merge-file", ours, base, theirs}, cwd);
    if (!result) {
        return std::unexpected(result.error());
    }
    // git merge-file: 0 = clean, N>0 = that many conflicts, <0 (255) = error.
    if (result->exitCode < 0 || result->exitCode == 255) {
        return std::unexpected("git merge-file " + ours + " " + base + " " +
                               theirs + " failed:\n" + result->output);
    }
    return result->exitCode != 0;
}

std::expected<bool, std::string> isAncestor(const std::string& ancestor,
                                            const std::string& descendant,
                                            const std::string& cwd) {
    auto result = p4gw::run(
        "git", {"merge-base", "--is-ancestor", ancestor, descendant}, cwd);
    if (!result) {
        return std::unexpected(result.error());
    }
    if (result->exitCode == 0) return true;
    if (result->exitCode == 1) return false;
    return std::unexpected("git merge-base --is-ancestor " + ancestor + " " +
                           descendant + " failed:\n" + result->output);
}

std::expected<AheadBehind, std::string> aheadBehind(const std::string& base,
                                                    const std::string& ref,
                                                    const std::string& cwd) {
    auto output = run({"rev-list", "--left-right", "--count",
                       base + "..." + ref},
                      cwd);
    if (!output) {
        return std::unexpected(output.error());
    }
    // "<behind>\t<ahead>": left side is base-only, right side is ref-only.
    // operator>> skips the tab/whitespace and parses both counts.
    AheadBehind counts;
    std::istringstream stream(*output);
    if (!(stream >> counts.behind >> counts.ahead)) {
        return std::unexpected("unexpected git rev-list output: " + *output);
    }
    return counts;
}

std::expected<std::vector<std::string>, std::string> statusLines(
    const std::string& cwd) {
    auto output = run({"status", "--porcelain"}, cwd);
    if (!output) {
        return std::unexpected(output.error());
    }
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < output->size()) {
        size_t end = output->find('\n', pos);
        if (end == std::string::npos) end = output->size();
        std::string line = output->substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(std::move(line));
    }
    return lines;
}

std::expected<std::string, std::string> commitSubject(const std::string& ref,
                                                      const std::string& cwd) {
    return run({"log", "-1", "--format=%s", ref}, cwd);
}

std::expected<std::vector<std::string>, std::string> lsFiles(
    const std::string& cwd) {
    auto output = run({"ls-files"}, cwd);
    if (!output) {
        return std::unexpected(output.error());
    }
    std::vector<std::string> files;
    size_t pos = 0;
    while (pos < output->size()) {
        size_t end = output->find('\n', pos);
        if (end == std::string::npos) end = output->size();
        std::string line = output->substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) files.push_back(std::move(line));
    }
    return files;
}

std::expected<std::string, std::string> addAll(const std::string& cwd) {
    return run({"add", "-A"}, cwd);
}

std::expected<bool, std::string> indexMatchesHead(const std::string& cwd) {
    auto output = run({"status", "--porcelain"}, cwd);
    if (!output) {
        return std::unexpected(output.error());
    }
    return output->empty();
}

std::expected<std::string, std::string> commit(const std::string& message,
                                               const std::string& cwd) {
    return run({"commit", "-m", message}, cwd);
}

std::expected<std::string, std::string> rebase(const std::string& onto,
                                               const std::string& cwd) {
    return run({"rebase", onto}, cwd);
}

std::expected<bool, std::string> isBranchless(const std::string& cwd) {
    // `git branchless init` sets this key; its presence is our signal that the
    // repo is managed by branchless (and tells us its main-branch name).
    auto value = configValue("branchless.core.mainBranch", cwd);
    if (!value) return std::unexpected(value.error());
    return !value->empty();
}

std::expected<std::string, std::string> branchlessSync(const std::string& cwd) {
    // Plain `sync` restacks onto the local main branch without pulling a
    // remote, which is exactly what we want: the depot baseline is local-only.
    return run({"branchless", "sync"}, cwd);
}

std::expected<void, std::string> setConfig(const std::string& key,
                                           const std::string& value,
                                           const std::string& cwd) {
    auto result = run({"config", key, value}, cwd);
    if (!result) return std::unexpected(result.error());
    return {};
}

std::expected<std::string, std::string> catBlobToFile(const std::string& ref,
                                                      const std::string& path,
                                                      const std::string& destFile,
                                                      const std::string& cwd) {
    RunOptions options;
    options.cwd = cwd;
    options.stdoutFile = destFile;
    auto result = p4gw::run("git", {"cat-file", "blob", ref + ":" + path},
                            options);
    if (!result) {
        return std::unexpected(result.error());
    }
    if (result->exitCode != 0) {
        return std::unexpected("git cat-file blob " + ref + ":" + path +
                               " failed:\n" + result->output);
    }
    return result->output;
}

std::expected<std::string, std::string> configValue(const std::string& key,
                                                    const std::string& cwd) {
    auto result = p4gw::run("git", {"config", "--get", key}, cwd);
    if (!result) {
        return std::unexpected(result.error());
    }
    if (result->exitCode != 0) {
        return std::string{};  // unset key
    }
    return trimTrailing(result->output);
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
