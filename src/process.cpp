#include "process.h"

#include <array>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#define POPEN _popen
#define PCLOSE _pclose
#else
#define POPEN popen
#define PCLOSE pclose
#endif

namespace p4gw {

namespace {

bool g_verbose = false;

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

// Reads an environment variable, returning empty if it is unset.
std::string envValue(const char* name) {
#ifdef _WIN32
    // getenv_s avoids MSVC's C4996 deprecation of std::getenv.
    std::size_t len = 0;
    if (getenv_s(&len, nullptr, 0, name) != 0 || len == 0) return {};
    std::vector<char> buffer(len);
    if (getenv_s(&len, buffer.data(), buffer.size(), name) != 0) return {};
    return std::string(buffer.data());  // stops at the null terminator
#else
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string{};
#endif
}

}  // namespace

void setVerbose(bool on) { g_verbose = on; }

std::string currentUser() {
    // USERNAME is the Windows login name; USER/LOGNAME cover the POSIX builds
    // used in CI and dev.
    for (const char* name : {"USERNAME", "USER", "LOGNAME"}) {
        std::string value = envValue(name);
        if (!value.empty()) return value;
    }
    return {};
}

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

    if (g_verbose) {
        // Echo the meaningful command, not the shell wrapping (cd/redirects);
        // quoted so it can be copy-pasted to rerun by hand.
        std::string display = quoteArg(exe);
        for (const auto& arg : args) {
            display += ' ';
            display += quoteArg(arg);
        }
        if (!options.cwd.empty()) {
            display += "   (in " + options.cwd + ")";
        }
        std::fprintf(stderr, "+ %s\n", display.c_str());
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
