#include "process.h"

#include <array>
#include <cstdio>

#ifdef _WIN32
#define POPEN _popen
#define PCLOSE _pclose
#else
#define POPEN popen
#define PCLOSE pclose
#endif

namespace p4gw {

namespace {

// Quotes a single argument for the platform shell if it contains characters
// that would otherwise be misinterpreted.
std::string quoteArg(const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\"'&|<>^%") == std::string::npos) {
        return arg;
    }
#ifdef _WIN32
    // cmd.exe style: wrap in double quotes, double up embedded quotes.
    std::string quoted = "\"";
    for (char c : arg) {
        if (c == '"') quoted += '"';
        quoted += c;
    }
    quoted += '"';
    return quoted;
#else
    // POSIX shell style: wrap in single quotes, escape embedded single quotes.
    std::string quoted = "'";
    for (char c : arg) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted += c;
        }
    }
    quoted += '\'';
    return quoted;
#endif
}

}  // namespace

std::expected<RunResult, std::string> run(const std::string& exe,
                                          const std::vector<std::string>& args,
                                          const std::string& cwd) {
    RunOptions options;
    options.cwd = cwd;
    return run(exe, args, options);
}

// NOTE: popen-based implementation merges stderr into stdout via shell
// redirection and routes through the shell, which is also how the stdin and
// stdout file redirections are done. PLAN.md replaces this with
// CreateProcess/posix_spawn for separate streams and no shell.
std::expected<RunResult, std::string> run(const std::string& exe,
                                          const std::vector<std::string>& args,
                                          const RunOptions& options) {
    std::string cmdline;
    if (!options.cwd.empty()) {
#ifdef _WIN32
        cmdline += "cd /d " + quoteArg(options.cwd) + " && ";
#else
        cmdline += "cd " + quoteArg(options.cwd) + " && ";
#endif
    }
    cmdline += quoteArg(exe);
    for (const auto& arg : args) {
        cmdline += ' ';
        cmdline += quoteArg(arg);
    }
    if (!options.stdinFile.empty()) {
        cmdline += " < " + quoteArg(options.stdinFile);
    }
    // `2>&1` first so stderr is duplicated onto the capture pipe *before* a
    // stdout file redirection takes stdout away from it.
    cmdline += " 2>&1";
    if (!options.stdoutFile.empty()) {
        cmdline += " > " + quoteArg(options.stdoutFile);
    }

    FILE* pipe = POPEN(cmdline.c_str(), "r");
    if (!pipe) {
        return std::unexpected("failed to start process: " + exe);
    }

    RunResult result;
    std::array<char, 4096> buffer{};
    size_t bytesRead = 0;
    while ((bytesRead = std::fread(buffer.data(), 1, buffer.size(), pipe)) > 0) {
        result.output.append(buffer.data(), bytesRead);
    }
    result.exitCode = PCLOSE(pipe);
    return result;
}

}  // namespace p4gw
