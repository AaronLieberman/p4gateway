#include "subprocess.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <aclapi.h>

#include <thread>
#pragma comment(lib, "advapi32.lib")
#else
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

extern char** environ;
#endif

namespace p4gw {

namespace {

bool g_verbose = false;

// Quotes a single argument for *display* (the --verbose echo and error
// messages), so the line can be copy-pasted into a shell to rerun by hand.
// The spawn itself passes arguments to the child natively - no shell is
// involved - so this quoting never affects what the child receives.
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

#ifdef _WIN32

// Appends one argument to a CreateProcessW command line, quoted the way the
// MSVC CRT and CommandLineToArgvW parse it back into argv: backslashes are
// literal except when they precede a double quote, where N backslashes plus a
// quote must be written as 2N+1 backslashes plus the quote (and a quoted
// argument's trailing backslashes are doubled so they don't escape the closing
// quote). This is argv quoting for the child itself - no cmd.exe - so shell
// metacharacters (%, ^, &) need no treatment.
void appendQuotedArg(std::wstring& cmdline, const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        cmdline += arg;
        return;
    }
    cmdline += L'"';
    size_t backslashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            cmdline.append(backslashes * 2 + 1, L'\\');
        } else {
            cmdline.append(backslashes, L'\\');
        }
        backslashes = 0;
        cmdline += c;
    }
    cmdline.append(backslashes * 2, L'\\');
    cmdline += L'"';
}

// Narrow to UTF-16 for the W-series APIs, via the active code page - the
// encoding gw's narrow strings (argv, fs::path::string()) actually carry
// today. The M3 "UTF-8 output" polish item revisits the encoding story.
std::wstring widen(const std::string& narrow) {
    if (narrow.empty()) return {};
    const int size = static_cast<int>(narrow.size());
    const int len =
        MultiByteToWideChar(CP_ACP, 0, narrow.data(), size, nullptr, 0);
    std::wstring wide(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_ACP, 0, narrow.data(), size, wide.data(), len);
    return wide;
}

// Reads a pipe to EOF (ReadFile fails with ERROR_BROKEN_PIPE once the child
// closes its end). Raw bytes - no text-mode CRLF translation, unlike _popen.
std::string readPipe(HANDLE pipe) {
    std::string text;
    char buffer[4096];
    DWORD got = 0;
    while (ReadFile(pipe, buffer, sizeof buffer, &got, nullptr) && got > 0) {
        text.append(buffer, got);
    }
    return text;
}

std::expected<RunResult, std::string> spawnChild(
    const std::string& exe, const std::vector<std::string>& args,
    const RunOptions& options) {
    std::wstring cmdline;
    appendQuotedArg(cmdline, widen(exe));
    for (const auto& arg : args) {
        cmdline += L' ';
        appendQuotedArg(cmdline, widen(arg));
    }

    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof inheritable;
    inheritable.bInheritHandle = TRUE;

    HANDLE outRead = nullptr;
    HANDLE outWrite = nullptr;
    HANDLE errRead = nullptr;
    HANDLE errWrite = nullptr;
    HANDLE stdinFile = nullptr;   // owned handle for options.stdinFile
    HANDLE stdoutFile = nullptr;  // owned handle for options.stdoutFile
    auto closeAll = [&] {
        for (HANDLE h : {outRead, outWrite, errRead, errWrite, stdinFile,
                         stdoutFile}) {
            if (h != nullptr) CloseHandle(h);
        }
    };

    // The child inherits the write ends; the parent's read ends must not leak
    // into the child or the pipes would never signal EOF.
    if (!CreatePipe(&outRead, &outWrite, &inheritable, 0) ||
        !CreatePipe(&errRead, &errWrite, &inheritable, 0)) {
        closeAll();
        return std::unexpected("failed to start process: " + exe +
                               ": cannot create pipes");
    }
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);

    // Without a stdin redirect the child shares gw's own stdin (as popen did),
    // so an interactive prompt (p4 login) still reaches the console.
    HANDLE childStdin = GetStdHandle(STD_INPUT_HANDLE);
    if (!options.stdinFile.empty()) {
        stdinFile = CreateFileW(widen(options.stdinFile).c_str(), GENERIC_READ,
                                FILE_SHARE_READ, &inheritable, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        if (stdinFile == INVALID_HANDLE_VALUE) {
            stdinFile = nullptr;
            closeAll();
            return std::unexpected("failed to start process: " + exe +
                                   ": cannot open " + options.stdinFile);
        }
        childStdin = stdinFile;
    }
    HANDLE childStdout = outWrite;
    if (!options.stdoutFile.empty()) {
        stdoutFile = CreateFileW(widen(options.stdoutFile).c_str(),
                                 GENERIC_WRITE, 0, &inheritable, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        if (stdoutFile == INVALID_HANDLE_VALUE) {
            stdoutFile = nullptr;
            closeAll();
            return std::unexpected("failed to start process: " + exe +
                                   ": cannot write " + options.stdoutFile);
        }
        childStdout = stdoutFile;
    }

    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = childStdin;
    si.hStdOutput = childStdout;
    si.hStdError = errWrite;
    PROCESS_INFORMATION pi{};
    // No application name: CreateProcessW resolves the command line's first
    // token against PATH (appending .exe), like the shell used to.
    const std::wstring cwd = widen(options.cwd);
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr,
                        /*bInheritHandles=*/TRUE, 0, nullptr,
                        options.cwd.empty() ? nullptr : cwd.c_str(), &si,
                        &pi)) {
        const DWORD error = GetLastError();
        closeAll();
        return std::unexpected("failed to start process: " + exe + " (error " +
                               std::to_string(error) + ")");
    }

    // Close the child's ends (and the redirect files) in the parent, or the
    // pipe reads below would never see EOF.
    CloseHandle(outWrite);
    outWrite = nullptr;
    CloseHandle(errWrite);
    errWrite = nullptr;
    if (stdinFile != nullptr) {
        CloseHandle(stdinFile);
        stdinFile = nullptr;
    }
    if (stdoutFile != nullptr) {
        CloseHandle(stdoutFile);
        stdoutFile = nullptr;
    }

    // Drain both pipes concurrently: a child that fills one while gw reads
    // only the other would deadlock once the ~64 KiB pipe buffer is full.
    RunResult result;
    std::thread errReader([&] { result.stderrText = readPipe(errRead); });
    result.stdoutText = readPipe(outRead);
    errReader.join();
    CloseHandle(outRead);
    outRead = nullptr;
    CloseHandle(errRead);
    errRead = nullptr;

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    result.exitCode = static_cast<int>(exitCode);
    return result;
}

#else  // POSIX

// Reads both pipes to EOF concurrently via poll: a child that fills one pipe
// while gw drains only the other would deadlock once the ~64 KiB pipe buffer
// is full.
void drainPipes(int outFd, int errFd, std::string& outText,
                std::string& errText) {
    struct Stream {
        int fd;
        std::string* dest;
        bool open = true;
    } streams[2] = {{outFd, &outText, true}, {errFd, &errText, true}};

    char buffer[4096];
    while (streams[0].open || streams[1].open) {
        pollfd fds[2];
        nfds_t count = 0;
        for (const auto& s : streams) {
            if (s.open) fds[count++] = {s.fd, POLLIN, 0};
        }
        if (poll(fds, count, -1) < 0) {
            if (errno == EINTR) continue;
            return;  // give up on the output; waitpid still reaps the child
        }
        nfds_t next = 0;
        for (auto& s : streams) {
            if (!s.open) continue;
            const short revents = fds[next++].revents;
            if (revents == 0) continue;  // POLLIN or POLLHUP: try a read
            const ssize_t got = read(s.fd, buffer, sizeof buffer);
            if (got > 0) {
                s.dest->append(buffer, static_cast<size_t>(got));
            } else if (got == 0 || errno != EINTR) {
                s.open = false;  // EOF, or a real error
            }
        }
    }
}

std::expected<RunResult, std::string> spawnChild(
    const std::string& exe, const std::vector<std::string>& args,
    const RunOptions& options) {
    int outPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};
    auto closeAll = [&] {
        for (int fd : {outPipe[0], outPipe[1], errPipe[0], errPipe[1]}) {
            if (fd >= 0) ::close(fd);
        }
    };
    if (::pipe(outPipe) != 0 || ::pipe(errPipe) != 0) {
        closeAll();
        return std::unexpected("failed to start process: " + exe +
                               ": cannot create pipes");
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    // File actions run in the child, in order, before exec; an action that
    // fails (missing cwd, unreadable stdin file) surfaces as posix_spawnp's
    // return value below. addchdir_np is the one non-standard call: glibc,
    // musl, and macOS all provide it, and it is what makes a fork/exec
    // fallback unnecessary.
    if (!options.cwd.empty()) {
        posix_spawn_file_actions_addchdir_np(&actions, options.cwd.c_str());
    }
    if (!options.stdinFile.empty()) {
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO,
                                         options.stdinFile.c_str(), O_RDONLY,
                                         0);
    }
    if (!options.stdoutFile.empty()) {
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
                                         options.stdoutFile.c_str(),
                                         O_WRONLY | O_CREAT | O_TRUNC, 0666);
    } else {
        posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
    }
    posix_spawn_file_actions_adddup2(&actions, errPipe[1], STDERR_FILENO);
    for (int fd : {outPipe[0], outPipe[1], errPipe[0], errPipe[1]}) {
        posix_spawn_file_actions_addclose(&actions, fd);
    }

    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(exe.c_str()));
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    pid_t pid = -1;
    // posix_spawnp resolves `exe` against PATH and reports the child's exec
    // failure (e.g. a missing binary) as its own return value.
    const int rc =
        posix_spawnp(&pid, exe.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(outPipe[1]);
    outPipe[1] = -1;
    ::close(errPipe[1]);
    errPipe[1] = -1;
    if (rc != 0) {
        closeAll();
        return std::unexpected("failed to start process: " + exe + ": " +
                               std::strerror(rc));
    }

    RunResult result;
    drainPipes(outPipe[0], errPipe[0], result.stdoutText, result.stderrText);
    closeAll();

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    // The exit code is packed in the wait status: without decoding, every
    // tool that legitimately exits 1 - `git merge-base --is-ancestor`
    // answering "no", `git rev-parse --verify --quiet` on a missing ref -
    // would read as a hard failure.
    result.exitCode =
        WIFEXITED(status) ? WEXITSTATUS(status) : -1;  // killed by a signal
    return result;
}

#endif

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

std::string uniqueTempFile(const std::string& prefix,
                           const std::string& suffix) {
    // PID separates concurrent gw runs; the atomic counter separates multiple
    // scratch files within one run (and avoids a same-name reuse race after a
    // caller removes an earlier one).
    static std::atomic<unsigned long long> counter{0};
#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
    const std::string name = prefix + "_" + std::to_string(pid) + "_" +
                             std::to_string(counter.fetch_add(1)) + suffix;
    return (std::filesystem::temp_directory_path() / name).string();
}

std::expected<RunResult, std::string> run(const std::string& exe,
                                          const std::vector<std::string>& args,
                                          const RunOptions& options) {
    if (g_verbose) {
        // Echo the command quoted so it can be copy-pasted to rerun by hand.
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
    return spawnChild(exe, args, options);
}

#ifdef _WIN32
std::expected<bool, std::string> isOwnedByCurrentUser(const std::string& path) {
    const std::wstring wpath = std::filesystem::path(path).wstring();

    PSID ownerSid = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    DWORD rc = GetNamedSecurityInfoW(
        wpath.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &ownerSid,
        nullptr, nullptr, nullptr, &sd);
    if (rc != ERROR_SUCCESS) {
        return std::unexpected("could not read the owner of " + path +
                               " (error " + std::to_string(rc) + ")");
    }

    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        LocalFree(sd);
        return std::unexpected("OpenProcessToken failed");
    }
    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    std::vector<unsigned char> buffer(needed);
    const bool got =
        GetTokenInformation(token, TokenUser, buffer.data(), needed, &needed);
    CloseHandle(token);
    if (!got) {
        LocalFree(sd);
        return std::unexpected("GetTokenInformation failed");
    }

    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    const bool owned = EqualSid(ownerSid, user->User.Sid) != FALSE;
    LocalFree(sd);
    return owned;
}
#else
std::expected<bool, std::string> isOwnedByCurrentUser(const std::string& path) {
    struct stat info{};
    if (::stat(path.c_str(), &info) != 0) {
        return std::unexpected("could not stat " + path);
    }
    return info.st_uid == ::geteuid();
}
#endif

}  // namespace p4gw
