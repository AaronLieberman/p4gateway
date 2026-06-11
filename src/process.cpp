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

// NOTE: popen-based implementation merges stderr into stdout via shell
// redirection and routes through the shell. PLAN.md milestone M1 replaces
// this with CreateProcess/posix_spawn for separate streams and no shell.
std::expected<RunResult, std::string> run(const std::string& exe,
                                          const std::vector<std::string>& args,
                                          const std::string& cwd) {
    std::string cmdline;
    if (!cwd.empty()) {
#ifdef _WIN32
        cmdline += "cd /d " + quoteArg(cwd) + " && ";
#else
        cmdline += "cd " + quoteArg(cwd) + " && ";
#endif
    }
    cmdline += quoteArg(exe);
    for (const auto& arg : args) {
        cmdline += ' ';
        cmdline += quoteArg(arg);
    }
    cmdline += " 2>&1";

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
