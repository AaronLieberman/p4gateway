// SPDX-License-Identifier: MIT

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "commands.h"
#include "config.h"
#include "git.h"
#include "mirror.h"
#include "p4.h"
#include "subprocess.h"
#include "shelf.h"

namespace fs = std::filesystem;

namespace p4gw {

namespace {

// Live-P4 integration tests (see PLAN-integrationtests.md). Everything runs
// inside a `p4gw-test` directory under the client root, whose depot path is
// discovered via `p4 where`; the user has pre-mapped its `src` subtree into
// the repo's mirror container at `.p4gw/src` in the client view (the repo root
// is `p4gw-test` itself; bin/ and root files stay unmapped).
// `gw integtest run` (re)builds a known fixture in the depot, drives the real
// workflow through child `gw` processes asserting state at each step, then
// cleans up. It is self-resetting, so it can be run repeatedly; the sibling
// `gw integtest clean` subcommand runs only the cleanup, and `run --leave`
// skips it. The whole command is gated on a throwaway-server check because it
// obliterates depot files and wipes the local tree.

struct ItContext {
    std::string gw;            // gw executable to spawn for commands under test
    bool verbose = false;
    bool force = false;        // wipe past the stray-file guard
    bool leave = false;        // skip the post-run depot/local cleanup
    bool clean = false;        // run only the cleanup (recovery), then exit
    std::string testRoot;      // the p4gw-test directory (cwd)
    std::string depotRoot;     // its depot path, no trailing /...
    std::string srcDepotPath;  // depotRoot + "/src/..."
    std::string repoDir;       // testRoot - the Git overlay repo root
    std::string srcWork;       // repoDir/src - the mapped subtree's working dir
    std::string mirrorDir;     // repoDir/.p4gw/src - the src mapping's mirror
    Config p4;                 // ambient client; one mapping spanning the depot
    std::string p4DepotPath;   // depotRoot + "/..." - scope for raw p4 calls
    std::string pendingCl;     // CL produced by the prepare step
};

struct FixtureFile {
    const char* rel;
    const char* content;
};

// Root and bin files are unmapped: they sync into the repo in place and are
// gitignored, so gw never tracks or touches them.
constexpr FixtureFile kRootFixture[] = {
    {"readme.txt", "p4gw-test fixture: root file, unmapped\n"},
    {"notes.txt", "p4gw-test fixture: more root content\n"},
};
constexpr FixtureFile kBinFixture[] = {
    {"bin/tool.txt", "p4gw-test fixture: bin file, unmapped\n"},
    {"bin/helper.txt", "p4gw-test fixture: another bin file\n"},
};
// The src set is created physically inside the mirror - with the remap
// active that is where the src depot paths live on disk.
constexpr FixtureFile kSrcFixture[] = {
    {"main.cpp", "// fixture main.cpp\nint main() { return 0; }\n"},
    {"util.cpp", "// fixture util.cpp\nint util() { return 1; }\n"},
    {"util.h", "// fixture util.h\nint util();\n"},
    {"docs/overview.md", "# fixture overview\n"},
};

// Content of the binary fixture src/blob.bin, built with an explicit length so
// the embedded NULs survive (which is also why it can't live in kSrcFixture's
// C-string table). The NULs make p4 auto-type the file binary when the fixture
// is reconciled; the mixed CRLF / lone-LF / lone-CR runs make any line-ending
// translation - which must never happen to a binary file - corrupt it
// detectably.
std::string binaryBlobContent() {
    static constexpr char kBytes[] =
        "p4gw-test binary fixture\x00\x01\x02\xff\r\nmid\nlone\rend\r\n";
    return std::string(kBytes, sizeof kBytes - 1);
}

// The ServerID a throwaway p4d must carry before this destructive command will
// touch it; the README setup writes this exact value to server.id.
constexpr const char* kThrowawayServerId = "p4gw-integtest-throwaway";

// Every depot file (relative to depotRoot) that init or the run can submit;
// cleanup obliterates exactly these - explicit paths, never a wildcard.
constexpr const char* kObliterateFiles[] = {
    "readme.txt", "notes.txt",
    "bin/tool.txt", "bin/helper.txt",
    "src/main.cpp", "src/util.cpp", "src/util.h", "src/blob.bin",
    "src/utils.h", "src/newfile.cpp", "src/docs/overview.md",
    // itHaveManifestIgnored's and itWorktreeGitignore's build-output files.
    "src/generated/out.txt", "src/generated/w.txt",
    // itHaveManifestExclude's carve-out; obliterated in-step, listed here as a
    // safety net so an aborted run's cleanup still removes it.
    "src/devtools/tool.txt",
};

// Top-level names integtest itself creates under testRoot (== repoDir). The
// local-wipe guard treats anything else as a stray it must not blindly delete.
// p4.ini/.p4config are the personal connection config and are kept, not wiped.
constexpr const char* kKnownLocalEntries[] = {
    "p4.ini", ".p4config",
    "p4gw.cfg", ".gitignore", ".gitattributes", ".git",
    ".p4gw", "src", "bin", "devtools",
    "readme.txt", "notes.txt",
};

void vlog(const ItContext& it, const std::string& text) {
    if (it.verbose && !text.empty()) {
        std::printf("%s\n", text.c_str());
    }
}

// Verbose tap for an already-made wrapper call: echoes what ran and what
// came back, then passes the result through.
std::expected<std::string, std::string> trace(
    const ItContext& it, const std::string& what,
    std::expected<std::string, std::string> result) {
    vlog(it, "$ " + what);
    if (result) vlog(it, *result);
    return result;
}

// Spawns the gw binary under test (the one integtest itself runs in, by
// default). This is the system-under-test boundary, so the driver spawns it
// directly rather than through a git/p4 wrapper. Non-zero exit is an error
// carrying the full output.
std::expected<std::string, std::string> runGw(const ItContext& it,
                                              const std::string& cwd,
                                              const std::vector<std::string>& args) {
    std::string display = it.gw;
    for (const auto& arg : args) display += " " + arg;
    vlog(it, "$ " + display + "   (in " + cwd + ")");
    auto result = p4gw::run(it.gw, args, cwd);
    if (!result) {
        return std::unexpected(result.error());
    }
    // The tests assert against everything gw said, so keep both streams
    // together here (gw prints its notes to stdout and errors to stderr).
    vlog(it, result->combined());
    if (result->exitCode != 0) {
        return std::unexpected(display + " exited " +
                               std::to_string(result->exitCode) + ":\n" +
                               result->combined());
    }
    return result->combined();
}

std::expected<void, std::string> writeFile(const fs::path& path,
                                           const std::string& content) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        return std::unexpected("cannot create " + path.parent_path().string() +
                               ": " + ec.message());
    }
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected("cannot write " + path.string());
    }
    file << content;
    return {};
}

std::expected<std::string, std::string> readFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected("cannot read " + path.string());
    }
    std::ostringstream content;
    content << file.rdbuf();
    return std::move(content).str();
}

std::expected<void, std::string> appendFile(const fs::path& path,
                                            const std::string& content) {
    std::ofstream file(path, std::ios::binary | std::ios::app);
    if (!file) {
        return std::unexpected("cannot append to " + path.string());
    }
    file << content;
    return {};
}

// p4-synced files are read-only; clear that before deleting a tree.
void makeWritableRecursive(const fs::path& path) {
    std::error_code ec;
    fs::permissions(path, fs::perms::owner_write, fs::perm_options::add, ec);
    if (!fs::is_directory(path, ec)) return;
    for (const auto& entry : fs::recursive_directory_iterator(
             path, fs::directory_options::skip_permission_denied, ec)) {
        std::error_code ec2;
        fs::permissions(entry.path(), fs::perms::owner_write,
                        fs::perm_options::add, ec2);
    }
}

bool isKnownLocalEntry(const std::string& name) {
    for (const char* known : kKnownLocalEntries) {
        if (name == known) return true;
    }
    return false;
}

// Deletes everything under testRoot except the personal connection config
// (p4.ini/.p4config). Guarded: if any entry isn't one integtest creates and
// --force wasn't given, it refuses without deleting anything, so an accidental
// run in the wrong directory can't destroy real files. Shared by the reset at
// the start of a run and the cleanup at the end.
std::expected<void, std::string> wipeLocal(ItContext& it) {
    std::error_code ec;
    std::vector<std::string> strays;
    for (const auto& entry : fs::directory_iterator(it.testRoot, ec)) {
        const std::string name = entry.path().filename().string();
        if (!isKnownLocalEntry(name)) strays.push_back(name);
    }
    if (ec) {
        return std::unexpected("cannot list " + it.testRoot + ": " +
                               ec.message());
    }
    if (!strays.empty() && !it.force) {
        std::string list;
        for (const auto& name : strays) list += "\n  " + name;
        return std::unexpected(
            "refusing to wipe " + it.testRoot + ": it contains files integtest "
            "did not create:" + list +
            "\nRemove them, or pass --force to delete everything here anyway.");
    }
    for (const auto& entry : fs::directory_iterator(it.testRoot, ec)) {
        const std::string name = entry.path().filename().string();
        // The p4 connection config must survive the wipe.
        if (name == "p4.ini" || name == ".p4config") continue;
        vlog(it, "deleting " + entry.path().string());
        makeWritableRecursive(entry.path());
        std::error_code removeEc;
        fs::remove_all(entry.path(), removeEc);
        if (removeEc) {
            return std::unexpected("cannot delete " + entry.path().string() +
                                   ": " + removeEc.message());
        }
    }
    if (ec) {
        return std::unexpected("cannot list " + it.testRoot + ": " +
                               ec.message());
    }
    return {};
}

// ---- shared discovery ----

// The one-time prerequisites the tests cannot set up themselves; appended to
// every precondition failure so the fix is right there.
std::string setupInstructions() {
    return
        "\nSetting up the integration test (one time):\n"
        "  1. Pick or create a P4 client whose view you can edit.\n"
        "  2. Under that client's root, create a directory named 'p4gw-test'.\n"
        "  3. Make sure 'p4' works from inside it - rely on ambient\n"
        "     P4PORT/P4USER/P4CLIENT, or drop a p4.ini / .p4config there.\n"
        "  4. Add a line at the END of the client view ('p4 client') that\n"
        "     remaps the test's src subtree into the repo's mirror container,\n"
        "     e.g.:\n"
        "       //depot/.../p4gw-test/src/...   "
        "//CLIENT/.../p4gw-test/.p4gw/src/...\n"
        "     Later view lines win, so keep it last. (run verifies this and\n"
        "     prints the exact line to use if it's off.)\n"
        "  5. From inside p4gw-test, run:  gw integtest run\n"
        "Full details: README-integtest.md.\n";
}

std::expected<void, std::string> discover(ItContext& it) {
    it.testRoot = fs::current_path().string();
    if (fs::path(it.testRoot).filename() != "p4gw-test") {
        return std::unexpected(
            "integtest must be run from inside a directory named "
            "'p4gw-test' (gw integtest DELETES everything under the "
            "current directory); this is " + it.testRoot +
            "\n" + setupInstructions());
    }
    auto depot = trace(it, "p4 where " + it.testRoot,
                       p4::whereDepotDir(it.p4, it.testRoot));
    if (!depot) {
        return std::unexpected("cannot discover the depot path of " +
                               it.testRoot + ": " + depot.error() +
                               "\n" + setupInstructions());
    }
    it.depotRoot = *depot;
    it.p4DepotPath = it.depotRoot + "/...";
    it.srcDepotPath = it.depotRoot + "/src/...";
    // The whole p4gw-test directory is the Git repo root; the `src` subtree is
    // the mapped one (remapped into the repo's .p4gw container at .p4gw/src),
    // while bin/ and the root files stay unmapped (synced in place, gitignored).
    it.repoDir = it.testRoot;
    it.srcWork = (fs::path(it.testRoot) / "src").string();
    it.mirrorDir = (fs::path(it.testRoot) / ".p4gw" / "src").string();
    // The raw-p4 helper config carries one whole-depot include so calls like
    // p4::openedFiles scope to the test depot; its mirror path is unused here.
    it.p4.rules = {{false, it.p4DepotPath, ".p4gw", ""}};
    return {};
}

// The single gate that makes the obliterate/wipe safe: this command only runs
// against a dedicated throwaway p4d, identified by a sentinel ServerID and a
// security level of 0 (a fresh, unsecured server). Run at preflight before
// anything destructive, and re-run just before the obliterate in itCleanup.
std::expected<void, std::string> itVerifyThrowaway(ItContext& it) {
    auto id = trace(it, "p4 info (ServerID)", p4::serverId(it.p4));
    if (!id) return std::unexpected(id.error());
    if (*id != kThrowawayServerId) {
        return std::unexpected(
            "refusing to run: this command OBLITERATES depot files and wipes "
            "the local tree, so it only runs against a dedicated throwaway "
            "p4d.\nThe server's ServerID is '" +
            (id->empty() ? std::string("(unset)") : *id) +
            "', but it must be '" + kThrowawayServerId +
            "'.\nSet it on your test server and restart p4d, e.g.:\n"
            "  echo " + kThrowawayServerId + " > ~/p4root/server.id\n"
            "See README-integtest.md.");
    }
    auto level = p4::securityLevel(it.p4);
    if (!level) return std::unexpected(level.error());
    if (*level != 0) {
        return std::unexpected(
            "refusing to run: server security level is " +
            std::to_string(*level) +
            ", expected 0 (a fresh, unsecured throwaway p4d). This command is "
            "destructive and must not touch a real server.\n"
            "See README-integtest.md.");
    }
    return {};
}

// ---- integtest fixture-build steps ----

std::expected<void, std::string> itVerifyMapping(ItContext& it) {
    auto spec = p4::clientSpec(it.p4);
    if (!spec) {
        return std::unexpected(spec.error());
    }
    const auto problems =
        p4::checkSpecMapping(*spec, it.srcDepotPath, it.repoDir, it.mirrorDir);
    if (problems.empty()) return {};

    std::string message;
    for (const auto& problem : problems) {
        message += problem.message + "\n";
    }
    const std::string expected =
        p4::clientViewPath(p4::specField(*spec, "Client"),
                           p4::specField(*spec, "Root"), it.mirrorDir, "/...");
    if (!expected.empty()) {
        message += "add this line at the END of the client view "
                   "('p4 client'):\n  " + it.srcDepotPath + " " + expected;
    }
    message += "\n" + setupInstructions();
    return std::unexpected(message);
}

std::expected<void, std::string> itResetLocal(ItContext& it) {
    auto reverted = trace(it, "p4 revert " + it.p4DepotPath,
                          p4::revert(it.p4, it.p4DepotPath));
    if (!reverted) return std::unexpected(reverted.error());
    // Force-sync so writable fixture files written by a previous run don't
    // block the sync (p4 won't clobber writable files without -f).
    auto synced = trace(it, "p4 sync -f " + it.p4DepotPath,
                        p4::syncForce(it.p4, it.p4DepotPath));
    if (!synced) return std::unexpected(synced.error());

    return wipeLocal(it);
}

std::expected<void, std::string> itWriteFixture(ItContext& it) {
    for (const auto& file : kRootFixture) {
        auto written = writeFile(fs::path(it.testRoot) / file.rel, file.content);
        if (!written) return written;
    }
    for (const auto& file : kBinFixture) {
        auto written = writeFile(fs::path(it.testRoot) / file.rel, file.content);
        if (!written) return written;
    }
    for (const auto& file : kSrcFixture) {
        auto written = writeFile(fs::path(it.mirrorDir) / file.rel, file.content);
        if (!written) return written;
    }
    auto blob = writeFile(fs::path(it.mirrorDir) / "blob.bin",
                          binaryBlobContent());
    if (!blob) return blob;
    return {};
}

std::expected<void, std::string> itSubmitFixture(ItContext& it) {
    auto cl = p4::createChangelist(it.p4, "gw integtest: fixture");
    if (!cl) return std::unexpected(cl.error());
    auto reconciled = trace(it, "p4 reconcile -c " + *cl + " " + it.p4DepotPath,
                            p4::reconcileToCl(it.p4, *cl, it.p4DepotPath));
    if (!reconciled) return std::unexpected(reconciled.error());
    auto opened = p4::openedInCl(it.p4, *cl);
    if (!opened) return std::unexpected(opened.error());
    if (opened->empty()) {
        auto deleted = p4::deleteChangelist(it.p4, *cl);
        if (!deleted) return std::unexpected(deleted.error());
        // itWriteFixture wrote mirror (src) files as writable; without a
        // submit p4 never resets them to read-only, which causes `p4 delete`
        // to fail in gw prepare later.  Scope to srcDepotPath so we don't
        // force-sync the unmapped root/bin files (LineEnd:local would
        // rewrite their line endings from LF to CRLF, breaking itFinalChecks).
        auto synced = trace(it, "p4 sync -f " + it.srcDepotPath,
                            p4::syncForce(it.p4, it.srcDepotPath));
        if (!synced) return std::unexpected(synced.error());
        std::printf("note  fixture already matches the depot - nothing "
                    "submitted\n");
        return {};
    }
    auto submitted = trace(it, "p4 submit -c " + *cl, p4::submit(it.p4, *cl));
    if (!submitted) return std::unexpected(submitted.error());
    // Submit leaves the just-written mirror files on disk with the LF endings
    // writeFile gave them, but a real workspace only ever sees mirror content
    // via p4 sync, which applies the client's LineEnd (CRLF here). Force-sync
    // the src subtree so the mirror matches what sync produces - exactly as the
    // nothing-to-submit branch above does. Without it the first gw import reads
    // LF from the mirror while git checks out CRLF into the working tree, and
    // itFirstImport flags them as differing on every run that actually submits.
    // Scope to srcDepotPath so the unmapped root/bin files keep their LF endings
    // (a force-sync there would rewrite them and break itFinalChecks).
    auto synced = trace(it, "p4 sync -f " + it.srcDepotPath,
                        p4::syncForce(it.p4, it.srcDepotPath));
    if (!synced) return std::unexpected(synced.error());
    return {};
}

std::expected<void, std::string> itGwSetup(ItContext& it) {
    std::error_code ec;
    fs::create_directories(it.repoDir, ec);
    if (ec) {
        return std::unexpected("cannot create " + it.repoDir + ": " +
                               ec.message());
    }
    auto out = runGw(it, it.repoDir,
                     {"setup", "--depot-path", it.srcDepotPath,
                      "--mirror-path", ".p4gw/src"});
    if (!out) return std::unexpected(out.error());
    if (!fs::is_regular_file(fs::path(it.repoDir) / "p4gw.cfg")) {
        return std::unexpected("gw setup did not write " + it.repoDir +
                               "/p4gw.cfg");
    }
    // Carry one `ignore` rule so the fixture has build-output-style content that
    // p4 syncs into the mirror but Git must not track (real repos always do).
    // itHaveManifestIgnored relies on this: `gw init` bakes it into the
    // allowlist below, so a file synced to src/generated is gitignored.
    auto ignored = appendFile(fs::path(it.repoDir) / "p4gw.cfg",
                              "\nignore = /src/generated/\n");
    if (!ignored) return ignored;
    // No hand-written .gitignore: `gw init` generates the allowlist that keeps
    // the only Git-tracked content the mapped src subtree, ignoring the
    // unmapped dirs (bin/, root files) that sync into the repo in place. That
    // is exactly the guarantee itFirstImport asserts, so let init produce it.
    return {};
}

// ---- integtest run steps ----

std::expected<void, std::string> itGwInit(ItContext& it) {
    auto out = runGw(it, it.repoDir, {"init", "--allow-in-repo"});
    if (!out) return std::unexpected(out.error());
    auto branch = git::currentBranch(it.repoDir);
    if (!branch) return std::unexpected(branch.error());
    if (*branch != "main") {
        return std::unexpected("expected to be on main after init, got '" +
                               *branch + "'");
    }
    if (!git::revParse("HEAD", it.repoDir)) {
        return std::unexpected("init left the repo without a commit "
                               "(.gitignore should be committed)");
    }
    if (!fs::exists(fs::path(it.repoDir) / ".gitignore")) {
        return std::unexpected("init did not write a .gitignore");
    }
    if (!fs::exists(fs::path(it.repoDir) / ".gitattributes")) {
        return std::unexpected("init did not write a .gitattributes");
    }
    auto tracked = git::lsFiles(it.repoDir);
    if (!tracked) return std::unexpected(tracked.error());
    if (std::find(tracked->begin(), tracked->end(), ".gitattributes") ==
        tracked->end()) {
        return std::unexpected(".gitattributes was written but not committed");
    }
    return {};
}

std::expected<void, std::string> itFirstImport(ItContext& it) {
    auto synced = trace(it, "p4 sync " + it.p4DepotPath,
                        p4::sync(it.p4, it.p4DepotPath));
    if (!synced) return std::unexpected(synced.error());
    auto out = runGw(it, it.repoDir, {"import"});
    if (!out) return std::unexpected(out.error());

    auto tracked = git::lsFiles(it.repoDir);
    if (!tracked) return std::unexpected(tracked.error());
    auto mirrorFiles = mirror::listFiles(it.mirrorDir);
    if (!mirrorFiles) return std::unexpected(mirrorFiles.error());

    std::vector<std::string> trackedContent;
    for (const auto& file : *tracked) {
        if (!mirror::isGwMetadataPath(file)) trackedContent.push_back(file);
    }
    // The mirror holds the subtree at its own root; the repo holds it under
    // src/, so compare the mirror listing prefixed with the subtree.
    std::vector<std::string> expectedTracked;
    for (const auto& file : *mirrorFiles) expectedTracked.push_back("src/" + file);
    std::sort(trackedContent.begin(), trackedContent.end());
    std::sort(expectedTracked.begin(), expectedTracked.end());
    if (trackedContent != expectedTracked) {
        std::string message = "tracked files do not match the mirror after "
                              "import\n  tracked:";
        for (const auto& file : trackedContent) message += " " + file;
        message += "\n  expected:";
        for (const auto& file : expectedTracked) message += " " + file;
        return std::unexpected(message);
    }

    auto repoMain = readFile(fs::path(it.srcWork) / "main.cpp");
    auto mirrorMain = readFile(fs::path(it.mirrorDir) / "main.cpp");
    if (!repoMain) return std::unexpected(repoMain.error());
    if (!mirrorMain) return std::unexpected(mirrorMain.error());
    if (*repoMain != *mirrorMain) {
        return std::unexpected("main.cpp content differs between the repo "
                               "and the mirror after import");
    }
    return {};
}

// `p4 print -q -o` (printHeadToFile) is how import reads depot-head content
// for opened files and how shelf import reads every file, so its bytes must be
// exactly what `p4 sync` writes into the mirror. That is not a given: sync
// applies the client's LineEnd translation to text files (CRLF for win/local
// on a Windows client) while print may hand back the server's stored form
// (LF). If the two differ, an imported opened file churns on every line and a
// shelf-import merge conflicts throughout. Byte-compare print output against
// the freshly synced mirror copy for a text file and for the binary fixture;
// for text, first confirm the mirror copy really carries the client's line
// endings so the comparison cannot pass vacuously, and for binary confirm
// sync preserved the fixture's exact bytes.
std::expected<void, std::string> itPrintFidelity(ItContext& it) {
    auto spec = p4::clientSpec(it.p4);
    if (!spec) return std::unexpected(spec.error());
    const std::string lineEnd = p4::specField(*spec, "LineEnd");
#ifdef _WIN32
    // 'local' (and an unset field, which defaults to local) means the
    // platform convention - CRLF here.
    const bool expectCrlf =
        lineEnd.empty() || lineEnd == "local" || lineEnd == "win";
#else
    const bool expectCrlf = lineEnd == "win";
#endif

    struct Case {
        const char* rel;
        bool binary;
    };
    constexpr Case kCases[] = {{"main.cpp", false}, {"blob.bin", true}};
    for (const auto& c : kCases) {
        auto mirrorBytes = readFile(fs::path(it.mirrorDir) / c.rel);
        if (!mirrorBytes) return std::unexpected(mirrorBytes.error());

        if (c.binary) {
            // Sync must never translate a binary file: the mirror copy must be
            // the fixture's exact bytes (NULs, lone CR/LF runs and all).
            if (*mirrorBytes != binaryBlobContent()) {
                return std::unexpected(
                    std::string(c.rel) +
                    ": the synced mirror bytes differ from the fixture - "
                    "line-ending translation on a binary file?");
            }
        } else if (expectCrlf &&
                   mirrorBytes->find("\r\n") == std::string::npos) {
            return std::unexpected(
                std::string(c.rel) +
                ": the synced mirror copy has no CRLF despite client LineEnd '" +
                (lineEnd.empty() ? "local" : lineEnd) +
                "' - the print comparison would be vacuous");
        }

        const std::string depotFile = it.depotRoot + "/src/" + c.rel;
        const fs::path printed = uniqueTempFile(
            std::string("p4gw_it_print_") + (c.binary ? "bin" : "txt"), ".tmp");
        vlog(it, "$ p4 print -q -o " + printed.string() + " " + depotFile +
                 "#head");
        auto fetched = p4::printHeadToFile(it.p4, depotFile, printed.string());
        if (!fetched) return std::unexpected(fetched.error());
        auto printedBytes = readFile(printed);
        std::error_code ec;
        fs::remove(printed, ec);
        if (!printedBytes) return std::unexpected(printedBytes.error());

        if (*printedBytes != *mirrorBytes) {
            std::string message =
                std::string(c.rel) + ": p4 print bytes differ from the synced "
                "mirror copy (" + std::to_string(printedBytes->size()) +
                " vs " + std::to_string(mirrorBytes->size()) + " bytes)";
            if (!c.binary &&
                printedBytes->find("\r\n") == std::string::npos &&
                mirrorBytes->find("\r\n") != std::string::npos) {
                message += " - print returned LF where sync wrote CRLF: "
                           "p4 print skips the client LineEnd translation, so "
                           "printHeadToFile needs a per-filetype translation "
                           "step (see the have-manifest note in PLAN.md)";
            }
            return std::unexpected(message);
        }
    }
    return {};
}

// `gw prepare` slicing: a bare <commit> ships only that commit, --stack widens
// it to the whole stack, and A..B ships a range. Verified through --dry-run
// (side-effect-free), which prints the final, reclassified op list. Builds a
// two-commit branch off main and restores main clean. The key cases: a file an
// earlier commit created and the selected commit edited must ship as an ADD (P4
// has no such file yet), and a file only the earlier commit touched must NOT
// ship at all. Runs on 'main', clean, after itFirstImport.
std::expected<void, std::string> itPrepareSlice(ItContext& it) {
    const fs::path mainCpp = fs::path(it.srcWork) / "main.cpp";
    const fs::path utilCpp = fs::path(it.srcWork) / "util.cpp";
    const fs::path sliceA = fs::path(it.srcWork) / "slice_a.cpp";
    const fs::path sliceB = fs::path(it.srcWork) / "slice_b.cpp";

    auto switched = trace(it, "git switch -c it-slice",
                          git::run({"switch", "-c", "it-slice"}, it.repoDir));
    if (!switched) return std::unexpected(switched.error());

    // Commit A: touch main.cpp and util.cpp, add slice_a.cpp.
    if (auto r = appendFile(mainCpp, "// slice A\n"); !r) return r;
    if (auto r = appendFile(utilCpp, "// slice A only\n"); !r) return r;
    if (auto r = writeFile(sliceA, "// slice a, rev A\n"); !r) return r;
    if (auto r = git::addAll(it.repoDir); !r)
        return std::unexpected(r.error());
    if (auto r = git::commit("integtest slice: commit A", it.repoDir); !r)
        return std::unexpected(r.error());
    auto shaA = git::revParse("HEAD", it.repoDir);
    if (!shaA) return std::unexpected(shaA.error());

    // Commit B: touch main.cpp again, edit slice_a.cpp, add slice_b.cpp. It does
    // NOT touch util.cpp.
    if (auto r = appendFile(mainCpp, "// slice B\n"); !r) return r;
    if (auto r = writeFile(sliceA, "// slice a, rev B\n"); !r) return r;
    if (auto r = writeFile(sliceB, "// slice b, rev B\n"); !r) return r;
    if (auto r = git::addAll(it.repoDir); !r)
        return std::unexpected(r.error());
    if (auto r = git::commit("integtest slice: commit B", it.repoDir); !r)
        return std::unexpected(r.error());
    auto shaB = git::revParse("HEAD", it.repoDir);
    if (!shaB) return std::unexpected(shaB.error());

    auto restore = [&]() {
        (void)git::run({"switch", "main"}, it.repoDir);
        (void)git::run({"branch", "-D", "it-slice"}, it.repoDir);
    };
    auto bail = [&](std::string msg) -> std::expected<void, std::string> {
        restore();
        return std::unexpected(std::move(msg));
    };

    // Just commit B: slice_a.cpp is a git-modify but the depot has no such file
    // (commit A is not being shipped), so it must reclassify to an add; util.cpp
    // (touched only by A) must be absent.
    auto only = runGw(it, it.repoDir, {"prepare", *shaB, "--dry-run"});
    if (!only) return bail(only.error());
    if (only->find("add     src/slice_a.cpp") == std::string::npos) {
        return bail("prepare <commit> did not reclassify the earlier-created "
                    "file to an add:\n" + *only);
    }
    if (only->find("edit    src/slice_a.cpp") != std::string::npos) {
        return bail("prepare <commit> staged slice_a.cpp as an edit though the "
                    "depot lacks it:\n" + *only);
    }
    if (only->find("add     src/slice_b.cpp") == std::string::npos ||
        only->find("edit    src/main.cpp") == std::string::npos) {
        return bail("prepare <commit> missed the commit's own files:\n" + *only);
    }
    if (only->find("src/util.cpp") != std::string::npos) {
        return bail("prepare <commit> shipped a file only an earlier commit "
                    "touched:\n" + *only);
    }

    // --stack widens to the whole stack: util.cpp (from commit A) is now in.
    auto stacked = runGw(it, it.repoDir, {"prepare", *shaB, "--stack", "-n"});
    if (!stacked) return bail(stacked.error());
    if (stacked->find("edit    src/util.cpp") == std::string::npos) {
        return bail("prepare --stack did not include the earlier commit's "
                    "file:\n" + *stacked);
    }

    // An explicit A..B range equals "just B": util.cpp out, slice_b.cpp in.
    auto range =
        runGw(it, it.repoDir, {"prepare", *shaA + ".." + *shaB, "--dry-run"});
    if (!range) return bail(range.error());
    if (range->find("src/util.cpp") != std::string::npos ||
        range->find("src/slice_b.cpp") == std::string::npos) {
        return bail("prepare A..B did not match the expected slice:\n" + *range);
    }

    restore();
    return {};
}

std::expected<void, std::string> itFeatureBranch(ItContext& it) {
    auto switched = trace(it, "git switch -c it-feature",
                          git::run({"switch", "-c", "it-feature"}, it.repoDir));
    if (!switched) return std::unexpected(switched.error());

    // Commit 1: edit + add.
    auto edited = appendFile(fs::path(it.srcWork) / "util.cpp",
                             "// integtest edit\n");
    if (!edited) return edited;
    auto added = writeFile(fs::path(it.srcWork) / "newfile.cpp",
                           "// integtest new file\nint newFn() { return 2; }\n");
    if (!added) return added;
    auto staged = git::addAll(it.repoDir);
    if (!staged) return std::unexpected(staged.error());
    auto committed =
        git::commit("integtest: edit util.cpp and add newfile.cpp",
                    it.repoDir);
    if (!committed) return std::unexpected(committed.error());

    // Commit 2: delete + rename.
    auto removed = trace(it, "git rm src/docs/overview.md",
                         git::run({"rm", "-q", "src/docs/overview.md"},
                                  it.repoDir));
    if (!removed) return std::unexpected(removed.error());
    auto renamed = trace(it, "git mv src/util.h src/utils.h",
                         git::run({"mv", "src/util.h", "src/utils.h"},
                                  it.repoDir));
    if (!renamed) return std::unexpected(renamed.error());
    committed = git::commit("integtest: delete overview.md, rename util.h",
                            it.repoDir);
    if (!committed) return std::unexpected(committed.error());
    return {};
}

// `gw prepare --shelf` on the feature branch must produce a shelf, leave no
// files open, and restore the mirror to the depot head (added file gone, rename
// undone). It runs before itPrepare on the same branch, so it cleans up the
// shelf and its changelist afterwards to leave a clean slate.
std::expected<void, std::string> itPrepareShelf(ItContext& it) {
    auto out = runGw(it, it.repoDir, {"prepare", "--shelf"});
    if (!out) return std::unexpected(out.error());

    // The shelf lives on a changelist; grab its number.
    const std::string marker = "Created pending changelist ";
    const auto markerPos = out->find(marker);
    if (markerPos == std::string::npos) {
        return std::unexpected("prepare --shelf output has no changelist "
                               "number:\n" + *out);
    }
    auto numberStart = markerPos + marker.size();
    auto numberEnd = numberStart;
    while (numberEnd < out->size() && std::isdigit((*out)[numberEnd])) {
        ++numberEnd;
    }
    const std::string cl = out->substr(numberStart, numberEnd - numberStart);
    if (cl.empty()) {
        return std::unexpected("prepare --shelf output has no changelist "
                               "number:\n" + *out);
    }
    if (out->find("Shelved changelist " + cl + " is ready") ==
        std::string::npos) {
        return std::unexpected("prepare --shelf did not report a shelved "
                               "changelist:\n" + *out);
    }

    // The opens must be gone: --shelf reverts them after shelving.
    auto opened = p4::openedFilesTagged(it.p4);
    if (!opened) return std::unexpected(opened.error());
    if (!opened->empty()) {
        std::string message = "prepare --shelf left files open:\n";
        for (const auto& o : *opened) {
            message += "  " + o.action + " " + o.depotFile + "\n";
        }
        return std::unexpected(message);
    }

    // The shelf carries the branch's five changes (edit/add/delete/move pair).
    auto described = trace(it, "p4 describe -S " + cl,
                           p4::describeShelved(it.p4, cl));
    if (!described) return std::unexpected(described.error());
    auto shelf = parseShelveDescribe(*described);
    if (!shelf) return std::unexpected(shelf.error());
    if (shelf->files.size() != 5) {
        return std::unexpected("expected 5 shelved files, got " +
                               std::to_string(shelf->files.size()));
    }

    // The mirror is back at the depot head: the added file is gone, the rename
    // is undone (util.h present, utils.h absent).
    if (fs::exists(fs::path(it.mirrorDir) / "newfile.cpp")) {
        return std::unexpected("prepare --shelf left the added newfile.cpp in "
                               "the mirror");
    }
    if (!fs::exists(fs::path(it.mirrorDir) / "util.h")) {
        return std::unexpected("prepare --shelf did not restore util.h in the "
                               "mirror");
    }
    if (fs::exists(fs::path(it.mirrorDir) / "utils.h")) {
        return std::unexpected("prepare --shelf left the renamed utils.h in the "
                               "mirror");
    }

    // Discard the shelf and its now-empty changelist; itPrepare re-prepares the
    // same branch as a normal pending CL next.
    auto discarded = trace(it, "p4 shelve -d -c " + cl,
                           p4::deleteShelve(it.p4, cl));
    if (!discarded) return std::unexpected(discarded.error());
    auto deleted = trace(it, "p4 change -d " + cl,
                         p4::deleteChangelist(it.p4, cl));
    if (!deleted) return std::unexpected(deleted.error());
    return {};
}

// `gw prepare --shelf --update <CL>` refreshes an existing shelf in place: it
// re-stages the branch and replaces the CL's shelved files (p4 shelve -r),
// keeping the same changelist. Builds a one-file shelf, grows the branch to two
// changed files, updates the shelf, and asserts it was replaced (two files, no
// new CL, no leftover opens). Self-contained on a throwaway branch off main.
std::expected<void, std::string> itPrepareShelfUpdate(ItContext& it) {
    const fs::path util = fs::path(it.srcWork) / "util.cpp";
    const fs::path extra = fs::path(it.srcWork) / "shelfupd.cpp";

    auto onMain = git::run({"switch", "-f", "main"}, it.repoDir);
    if (!onMain) return std::unexpected(onMain.error());
    auto branch = git::run({"switch", "-c", "it-shelfupd"}, it.repoDir);
    if (!branch) return std::unexpected(branch.error());

    auto extractCl = [](const std::string& out) -> std::string {
        const std::string marker = "Created pending changelist ";
        auto pos = out.find(marker);
        if (pos == std::string::npos) return {};
        pos += marker.size();
        auto end = pos;
        while (end < out.size() &&
               std::isdigit(static_cast<unsigned char>(out[end]))) {
            ++end;
        }
        return out.substr(pos, end - pos);
    };

    // v1: one changed file -> a one-file shelf.
    if (auto r = appendFile(util, "// shelf-update v1\n"); !r) return r;
    if (auto r = git::addAll(it.repoDir); !r) return std::unexpected(r.error());
    if (auto r = git::commit("integtest: shelf-update v1", it.repoDir); !r) {
        return std::unexpected(r.error());
    }
    auto created = runGw(it, it.repoDir, {"prepare", "--shelf"});
    if (!created) return std::unexpected(created.error());
    const std::string cl = extractCl(*created);
    if (cl.empty()) {
        return std::unexpected("prepare --shelf output has no changelist "
                               "number:\n" + *created);
    }
    auto d1 = trace(it, "p4 describe -S " + cl, p4::describeShelved(it.p4, cl));
    if (!d1) return std::unexpected(d1.error());
    auto s1 = parseShelveDescribe(*d1);
    if (!s1) return std::unexpected(s1.error());
    if (s1->files.size() != 1) {
        return std::unexpected("expected 1 shelved file initially, got " +
                               std::to_string(s1->files.size()));
    }

    // Grow the branch: edit util.cpp again and add a second file.
    if (auto r = appendFile(util, "// shelf-update v2\n"); !r) return r;
    if (auto r = writeFile(extra, "// added on shelf update\n"); !r) return r;
    if (auto r = git::addAll(it.repoDir); !r) return std::unexpected(r.error());
    if (auto r = git::commit("integtest: shelf-update v2", it.repoDir); !r) {
        return std::unexpected(r.error());
    }

    auto updated = runGw(it, it.repoDir, {"prepare", "--shelf", "--update", cl});
    if (!updated) return std::unexpected(updated.error());
    if (updated->find("Updated shelf on changelist " + cl) ==
        std::string::npos) {
        return std::unexpected("prepare --shelf --update did not report an "
                               "updated shelf:\n" + *updated);
    }
    if (updated->find("Created pending changelist") != std::string::npos) {
        return std::unexpected("prepare --shelf --update created a new "
                               "changelist instead of reusing " + cl + ":\n" +
                               *updated);
    }

    auto opened = p4::openedFilesTagged(it.p4);
    if (!opened) return std::unexpected(opened.error());
    if (!opened->empty()) {
        return std::unexpected("prepare --shelf --update left files open");
    }
    // The shelf was replaced: it now holds both files (not the stale single one).
    auto d2 = trace(it, "p4 describe -S " + cl, p4::describeShelved(it.p4, cl));
    if (!d2) return std::unexpected(d2.error());
    auto s2 = parseShelveDescribe(*d2);
    if (!s2) return std::unexpected(s2.error());
    if (s2->files.size() != 2) {
        return std::unexpected("expected the updated shelf to hold 2 files, "
                               "got " + std::to_string(s2->files.size()));
    }
    bool hasUtil = false;
    bool hasExtra = false;
    for (const auto& f : s2->files) {
        if (f.depotFile.find("util.cpp") != std::string::npos) hasUtil = true;
        if (f.depotFile.find("shelfupd.cpp") != std::string::npos) {
            hasExtra = true;
        }
    }
    if (!hasUtil || !hasExtra) {
        return std::unexpected("the updated shelf is missing an expected "
                               "file:\n" + *d2);
    }

    // Clean up: discard the shelf and its changelist, back to main, drop branch.
    auto discarded = trace(it, "p4 shelve -d -c " + cl,
                           p4::deleteShelve(it.p4, cl));
    if (!discarded) return std::unexpected(discarded.error());
    auto deletedCl = p4::deleteChangelist(it.p4, cl);
    if (!deletedCl) return std::unexpected(deletedCl.error());
    auto back = git::run({"switch", "-f", "main"}, it.repoDir);
    if (!back) return std::unexpected(back.error());
    (void)git::run({"branch", "-D", "it-shelfupd"}, it.repoDir);
    return {};
}

// `gw prepare --abandon <CL>` throws a changelist away: reverts its opens
// (restoring the mirror to the depot head), drops any shelf, and deletes it.
// Covers both a plain pending CL and a shelved CL, on a throwaway branch.
std::expected<void, std::string> itPrepareAbandon(ItContext& it) {
    const fs::path util = fs::path(it.srcWork) / "util.cpp";
    const fs::path mirrorUtil = fs::path(it.mirrorDir) / "util.cpp";

    auto extractCl = [](const std::string& out) -> std::string {
        const std::string marker = "Created pending changelist ";
        auto pos = out.find(marker);
        if (pos == std::string::npos) return {};
        pos += marker.size();
        auto end = pos;
        while (end < out.size() &&
               std::isdigit(static_cast<unsigned char>(out[end]))) {
            ++end;
        }
        return out.substr(pos, end - pos);
    };

    auto onMain = git::run({"switch", "-f", "main"}, it.repoDir);
    if (!onMain) return std::unexpected(onMain.error());
    auto branch = git::run({"switch", "-c", "it-abandon"}, it.repoDir);
    if (!branch) return std::unexpected(branch.error());
    if (auto r = appendFile(util, "// abandon-me\n"); !r) return r;
    if (auto r = git::addAll(it.repoDir); !r) return std::unexpected(r.error());
    if (auto r = git::commit("integtest: to be abandoned", it.repoDir); !r) {
        return std::unexpected(r.error());
    }

    // --- Abandon a plain pending changelist (with opens). ---
    auto prepared = runGw(it, it.repoDir, {"prepare"});
    if (!prepared) return std::unexpected(prepared.error());
    const std::string cl = extractCl(*prepared);
    if (cl.empty()) {
        return std::unexpected("prepare output has no changelist number:\n" +
                               *prepared);
    }
    auto opened = p4::openedFilesTagged(it.p4);
    if (!opened) return std::unexpected(opened.error());
    if (opened->empty()) {
        return std::unexpected("prepare opened no files to abandon");
    }
    auto mirrorBefore = readFile(mirrorUtil);
    if (!mirrorBefore) return std::unexpected(mirrorBefore.error());
    if (mirrorBefore->find("// abandon-me") == std::string::npos) {
        return std::unexpected("prepare did not stage the edit into the mirror");
    }

    auto abandoned = runGw(it, it.repoDir, {"prepare", "--abandon", cl});
    if (!abandoned) return std::unexpected(abandoned.error());
    if (abandoned->find("Abandoned changelist " + cl) == std::string::npos) {
        return std::unexpected("--abandon did not report abandoning the "
                               "changelist:\n" + *abandoned);
    }
    auto openedAfter = p4::openedFilesTagged(it.p4);
    if (!openedAfter) return std::unexpected(openedAfter.error());
    if (!openedAfter->empty()) {
        return std::unexpected("--abandon left files open");
    }
    auto mirrorAfter = readFile(mirrorUtil);
    if (!mirrorAfter) return std::unexpected(mirrorAfter.error());
    if (mirrorAfter->find("// abandon-me") != std::string::npos) {
        return std::unexpected("--abandon did not restore the mirror to the "
                               "depot head");
    }
    // The changelist is gone: gw only reports success after `p4 change -d`,
    // which refuses a changelist that still has opens - so the delete
    // succeeding is itself the proof the opens were reverted first.

    // --- Abandon a shelved changelist (shelf gets dropped too). ---
    auto shelfPrep = runGw(it, it.repoDir, {"prepare", "--shelf"});
    if (!shelfPrep) return std::unexpected(shelfPrep.error());
    const std::string shelfCl = extractCl(*shelfPrep);
    if (shelfCl.empty()) {
        return std::unexpected("prepare --shelf output has no changelist "
                               "number:\n" + *shelfPrep);
    }
    // Confirm it really carries a shelf before we abandon it.
    auto describe = trace(it, "p4 describe -S " + shelfCl,
                          p4::describeShelved(it.p4, shelfCl));
    if (!describe) return std::unexpected(describe.error());
    auto shelfInfo = parseShelveDescribe(*describe);
    if (!shelfInfo) return std::unexpected(shelfInfo.error());
    if (shelfInfo->files.empty()) {
        return std::unexpected("prepare --shelf produced no shelved files to "
                               "abandon");
    }
    // Abandon succeeds only if `p4 change -d` deleted the CL, which p4 refuses
    // while a shelf remains - so success proves the shelf was dropped too.
    auto shelfAbandon = runGw(it, it.repoDir, {"prepare", "--abandon", shelfCl});
    if (!shelfAbandon) return std::unexpected(shelfAbandon.error());
    if (shelfAbandon->find("Abandoned changelist " + shelfCl) ==
        std::string::npos) {
        return std::unexpected("--abandon did not report abandoning the "
                               "shelved changelist:\n" + *shelfAbandon);
    }
    auto openedShelf = p4::openedFilesTagged(it.p4);
    if (!openedShelf) return std::unexpected(openedShelf.error());
    if (!openedShelf->empty()) {
        return std::unexpected("--abandon of a shelf left files open");
    }

    auto back = git::run({"switch", "-f", "main"}, it.repoDir);
    if (!back) return std::unexpected(back.error());
    (void)git::run({"branch", "-D", "it-abandon"}, it.repoDir);
    return {};
}

std::expected<void, std::string> itPrepare(ItContext& it) {
    auto out = runGw(it, it.repoDir, {"prepare", "--verify"});
    if (!out) return std::unexpected(out.error());

    const std::string marker = "Created pending changelist ";
    const auto markerPos = out->find(marker);
    if (markerPos == std::string::npos) {
        return std::unexpected("prepare output has no changelist number:\n" +
                               *out);
    }
    auto numberStart = markerPos + marker.size();
    auto numberEnd = numberStart;
    while (numberEnd < out->size() && std::isdigit((*out)[numberEnd])) {
        ++numberEnd;
    }
    it.pendingCl = out->substr(numberStart, numberEnd - numberStart);
    if (it.pendingCl.empty()) {
        return std::unexpected("prepare output has no changelist number:\n" +
                               *out);
    }
    if (out->find("Verified: mirror matches the branch exactly.") ==
        std::string::npos) {
        return std::unexpected("prepare did not report a clean reconcile "
                               "verification:\n" + *out);
    }

    auto opened = trace(it, "p4 opened -c " + it.pendingCl,
                        p4::openedInCl(it.p4, it.pendingCl));
    if (!opened) return std::unexpected(opened.error());
    const std::pair<std::string, std::string> expected[] = {
        {it.depotRoot + "/src/util.cpp#", "edit"},
        {it.depotRoot + "/src/newfile.cpp#", "add"},
        {it.depotRoot + "/src/docs/overview.md#", "delete"},
        {it.depotRoot + "/src/utils.h#", "move/add"},
        {it.depotRoot + "/src/util.h#", "move/delete"},
    };
    std::istringstream lines(*opened);
    std::vector<std::string> openedLines;
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty()) openedLines.push_back(line);
    }
    std::string message;
    for (const auto& [path, action] : expected) {
        const bool found = std::any_of(
            openedLines.begin(), openedLines.end(), [&](const std::string& l) {
                return l.find(path) != std::string::npos &&
                       l.find(action) != std::string::npos;
            });
        if (!found) {
            message += "expected '" + action + "' of " + path + "...\n";
        }
    }
    if (openedLines.size() != std::size(expected)) {
        message += "expected " + std::to_string(std::size(expected)) +
                   " opened files, got " +
                   std::to_string(openedLines.size()) + "\n";
    }
    if (!message.empty()) {
        return std::unexpected(message + "p4 opened -c " + it.pendingCl +
                               ":\n" + *opened);
    }
    return {};
}

// After itPrepare built the pending CL, a review tweak arrives: a new commit
// adds src/extra.cpp on top of the branch. `gw prepare --update <CL>` must
// refresh that same CL in place (same number, no new changelist) rather than
// hit the opened-files guard, and the added file must show up among its opens.
// Then the tweak is dropped and the CL refreshed again, proving --update also
// *removes* what left the branch - and leaving the CL exactly as itPrepare
// produced it (five opens) so the submit/import steps that follow are
// unaffected.
std::expected<void, std::string> itPrepareUpdate(ItContext& it) {
    // 1. Add a tweak commit, then refresh the existing CL to match.
    auto added = writeFile(
        fs::path(it.srcWork) / "extra.cpp",
        "// integtest update tweak\nint extraFn() { return 9; }\n");
    if (!added) return added;
    auto staged = git::addAll(it.repoDir);
    if (!staged) return std::unexpected(staged.error());
    auto committed = git::commit("integtest: review tweak, add extra.cpp",
                                 it.repoDir);
    if (!committed) return std::unexpected(committed.error());

    auto out = runGw(it, it.repoDir, {"prepare", "--update", it.pendingCl});
    if (!out) return std::unexpected(out.error());
    if (out->find("Refreshing pending changelist " + it.pendingCl) ==
        std::string::npos) {
        return std::unexpected("prepare --update did not refresh CL " +
                               it.pendingCl + ":\n" + *out);
    }
    if (out->find("Created pending changelist") != std::string::npos) {
        return std::unexpected("prepare --update created a new changelist "
                               "instead of refreshing " + it.pendingCl + ":\n" +
                               *out);
    }

    auto opened = trace(it, "p4 opened -c " + it.pendingCl,
                        p4::openedInCl(it.p4, it.pendingCl));
    if (!opened) return std::unexpected(opened.error());
    if (opened->find(it.depotRoot + "/src/extra.cpp") == std::string::npos) {
        return std::unexpected("prepare --update did not open the new extra.cpp "
                               "in CL " + it.pendingCl + ":\n" + *opened);
    }

    // 2. Drop the tweak commit and refresh again: the CL must return to exactly
    // the five opens itPrepare produced (extra.cpp gone), so the following
    // submit and import steps see the same state.
    auto reset = trace(it, "git reset --hard HEAD~1",
                       git::run({"reset", "--hard", "HEAD~1"}, it.repoDir));
    if (!reset) return std::unexpected(reset.error());

    auto out2 = runGw(it, it.repoDir, {"prepare", "--update", it.pendingCl});
    if (!out2) return std::unexpected(out2.error());

    auto opened2 = trace(it, "p4 opened -c " + it.pendingCl,
                         p4::openedInCl(it.p4, it.pendingCl));
    if (!opened2) return std::unexpected(opened2.error());
    if (opened2->find("/src/extra.cpp") != std::string::npos) {
        return std::unexpected("prepare --update left extra.cpp open after it "
                               "was dropped from the branch:\n" + *opened2);
    }
    std::istringstream lines(*opened2);
    std::string line;
    size_t count = 0;
    while (std::getline(lines, line)) {
        if (!line.empty()) ++count;
    }
    if (count != 5) {
        return std::unexpected("prepare --update should have restored CL " +
                               it.pendingCl + " to 5 opens, got " +
                               std::to_string(count) + ":\n" + *opened2);
    }
    return {};
}

std::expected<void, std::string> itSubmitAndAbsorb(ItContext& it) {
    auto submitted = trace(it, "p4 submit -c " + it.pendingCl,
                           p4::submit(it.p4, it.pendingCl));
    if (!submitted) return std::unexpected(submitted.error());
    auto synced = trace(it, "p4 sync " + it.p4DepotPath,
                        p4::sync(it.p4, it.p4DepotPath));
    if (!synced) return std::unexpected(synced.error());

    auto out = runGw(it, it.repoDir, {"import", "--rebase"});
    if (!out) return std::unexpected(out.error());

    auto featureTip = git::revParse("it-feature", it.repoDir);
    auto baselineTip = git::revParse("main", it.repoDir);
    if (!featureTip) return std::unexpected(featureTip.error());
    if (!baselineTip) return std::unexpected(baselineTip.error());
    if (*featureTip != *baselineTip) {
        return std::unexpected(
            "after submitting and importing, the branch commits should melt "
            "away (it-feature " + *featureTip + " != main " +
            *baselineTip + ")");
    }
    if (fs::exists(fs::path(it.srcWork) / "docs" / "overview.md")) {
        return std::unexpected("src/docs/overview.md still exists after its "
                               "delete was submitted and imported");
    }
    if (!fs::exists(fs::path(it.srcWork) / "utils.h")) {
        return std::unexpected("src/utils.h (renamed from util.h) missing after "
                               "import");
    }
    return {};
}

std::expected<void, std::string> itTeammateChange(ItContext& it) {
    auto cl = p4::createChangelist(it.p4, "gw integtest: teammate change");
    if (!cl) return std::unexpected(cl.error());
    const std::string mirrorMain =
        (fs::path(it.mirrorDir) / "main.cpp").string();
    auto opened = trace(it, "p4 edit " + mirrorMain,
                        p4::editFiles(it.p4, *cl, {mirrorMain}));
    if (!opened) return std::unexpected(opened.error());
    auto edited = appendFile(mirrorMain, "// teammate change\n");
    if (!edited) return edited;
    auto submitted = trace(it, "p4 submit -c " + *cl, p4::submit(it.p4, *cl));
    if (!submitted) return std::unexpected(submitted.error());
    auto synced = trace(it, "p4 sync " + it.p4DepotPath,
                        p4::sync(it.p4, it.p4DepotPath));
    if (!synced) return std::unexpected(synced.error());

    auto switched = trace(it, "git switch -c it-feature2",
                          git::run({"switch", "-c", "it-feature2"}, it.repoDir));
    if (!switched) return std::unexpected(switched.error());
    auto changed = appendFile(fs::path(it.srcWork) / "util.cpp",
                              "// second feature edit\n");
    if (!changed) return changed;
    auto staged = git::addAll(it.repoDir);
    if (!staged) return std::unexpected(staged.error());
    auto committed = git::commit("integtest: feature2 local change",
                                 it.repoDir);
    if (!committed) return std::unexpected(committed.error());

    auto out = runGw(it, it.repoDir, {"import", "--rebase"});
    if (!out) return std::unexpected(out.error());

    auto repoMain = readFile(fs::path(it.srcWork) / "main.cpp");
    if (!repoMain) return std::unexpected(repoMain.error());
    if (repoMain->find("// teammate change") == std::string::npos) {
        return std::unexpected("the teammate's submitted change is missing "
                               "from the repo after import --rebase");
    }
    auto ahead = git::run({"rev-list", "--count", "main..HEAD"},
                          it.repoDir);
    if (!ahead) return std::unexpected(ahead.error());
    if (*ahead != "1") {
        return std::unexpected("expected exactly 1 commit ahead of main "
                               "after the rebase, got " + *ahead);
    }
    return {};
}

std::expected<void, std::string> itOpenedFilesPreflight(ItContext& it) {
    // Simulate a stray / mid-prepare open: open util.cpp in the mirror for
    // edit and give its working copy un-submitted content. import must ignore
    // that copy (reading the depot head), and prepare must refuse to run.
    auto cl = p4::createChangelist(it.p4, "gw integtest: stray open");
    if (!cl) return std::unexpected(cl.error());
    const std::string mirrorUtil =
        (fs::path(it.mirrorDir) / "util.cpp").string();
    auto opened = trace(it, "p4 edit " + mirrorUtil,
                        p4::editFiles(it.p4, *cl, {mirrorUtil}));
    if (!opened) return std::unexpected(opened.error());
    auto edited = appendFile(mirrorUtil, "// UNSUBMITTED stray edit\n");
    if (!edited) return edited;

    // prepare would double-open these files, so it must refuse.
    auto prepare = runGw(it, it.repoDir, {"prepare"});
    if (prepare) {
        return std::unexpected("gw prepare should have failed with files open "
                               "in the mirror, but it succeeded:\n" + *prepare);
    }
    if (prepare.error().find("already open") == std::string::npos) {
        return std::unexpected("gw prepare failed for the wrong reason:\n" +
                               prepare.error());
    }

    // import must commit the depot head, not the un-submitted working copy.
    auto imported = runGw(it, it.repoDir, {"import"});
    if (!imported) return std::unexpected(imported.error());
    auto baselineUtil = git::run({"show", "main:src/util.cpp"}, it.repoDir);
    if (!baselineUtil) return std::unexpected(baselineUtil.error());
    if (baselineUtil->find("UNSUBMITTED stray edit") != std::string::npos) {
        return std::unexpected("import absorbed an un-submitted mirror edit "
                               "into main:\n" + *baselineUtil);
    }

    // Clean up: revert the open, drop the empty CL, restore the mirror.
    auto reverted = trace(it, "p4 revert " + it.p4DepotPath,
                          p4::revert(it.p4, it.p4DepotPath));
    if (!reverted) return std::unexpected(reverted.error());
    auto deleted = p4::deleteChangelist(it.p4, *cl);
    if (!deleted) return std::unexpected(deleted.error());
    auto synced = trace(it, "p4 sync " + it.p4DepotPath,
                        p4::sync(it.p4, it.p4DepotPath));
    if (!synced) return std::unexpected(synced.error());
    return {};
}

std::expected<void, std::string> itShelfImport(ItContext& it) {
    // Build a shelved CL on the server (an edit plus an add), then revert the
    // workspace so the mirror is clean and only the shelf remains. The shelf
    // is based on the current depot head, which main already tracks, so
    // `gw shelf import` should reconstruct it as a clean one-commit branch.
    auto cl = p4::createChangelist(it.p4, "gw integtest: shelved change");
    if (!cl) return std::unexpected(cl.error());

    const std::string mirrorUtil =
        (fs::path(it.mirrorDir) / "util.cpp").string();
    auto opened = trace(it, "p4 edit " + mirrorUtil,
                        p4::editFiles(it.p4, *cl, {mirrorUtil}));
    if (!opened) return std::unexpected(opened.error());
    auto edited = appendFile(mirrorUtil, "// shelf edit\n");
    if (!edited) return edited;

    const fs::path mirrorNew = fs::path(it.mirrorDir) / "shelfnew.cpp";
    auto wroteNew = writeFile(
        mirrorNew, "// shelved new file\nint shelfFn() { return 7; }\n");
    if (!wroteNew) return wroteNew;
    auto added = trace(it, "p4 add " + mirrorNew.string(),
                       p4::addFiles(it.p4, *cl, {mirrorNew.string()}));
    if (!added) return std::unexpected(added.error());

    auto shelved = trace(it, "p4 shelve -c " + *cl, p4::shelve(it.p4, *cl));
    if (!shelved) return std::unexpected(shelved.error());

    // Revert the workspace opens so the mirror returns to depot state; the
    // shelf lives on the server. Reverting an add unopens but leaves the file
    // on disk, so remove it by hand, then re-sync.
    auto reverted = trace(it, "p4 revert " + it.p4DepotPath,
                          p4::revert(it.p4, it.p4DepotPath));
    if (!reverted) return std::unexpected(reverted.error());
    std::error_code ec;
    fs::remove(mirrorNew, ec);

    // The shelf (now reverted from the workspace) must still show up in
    // `gw shelf list`, marked as shelved, so it can be picked for import.
    auto listed = runGw(it, it.repoDir, {"shelf", "list"});
    if (!listed) return std::unexpected(listed.error());
    if (listed->find(*cl) == std::string::npos ||
        listed->find("[shelved]") == std::string::npos) {
        return std::unexpected("gw shelf list did not show shelved CL " + *cl +
                               ":\n" + *listed);
    }

    auto synced = trace(it, "p4 sync " + it.p4DepotPath,
                        p4::sync(it.p4, it.p4DepotPath));
    if (!synced) return std::unexpected(synced.error());

    // Import the shelf: a new branch off main carrying the shelved delta.
    auto out = runGw(it, it.repoDir, {"shelf", "import", *cl});
    if (!out) return std::unexpected(out.error());

    const std::string branch = "shelf-" + *cl;
    auto current = git::currentBranch(it.repoDir);
    if (!current) return std::unexpected(current.error());
    if (*current != branch) {
        return std::unexpected("expected to be on '" + branch +
                               "' after shelf import, got '" + *current + "'");
    }
    auto ahead = git::run({"rev-list", "--count", "main..HEAD"}, it.repoDir);
    if (!ahead) return std::unexpected(ahead.error());
    if (*ahead != "1") {
        return std::unexpected("expected exactly 1 commit on the shelf branch, "
                               "got " + *ahead);
    }
    auto utilContent = readFile(fs::path(it.srcWork) / "util.cpp");
    if (!utilContent) return std::unexpected(utilContent.error());
    if (utilContent->find("// shelf edit") == std::string::npos) {
        return std::unexpected("shelf import did not bring in the edited "
                               "util.cpp content");
    }
    if (!fs::exists(fs::path(it.srcWork) / "shelfnew.cpp")) {
        return std::unexpected("shelf import did not add shelfnew.cpp");
    }

    // Clean up: back to main, drop the shelf branch, delete shelf + CL.
    auto back = git::switchBranch("main", it.repoDir);
    if (!back) return std::unexpected(back.error());
    auto dropped = git::run({"branch", "-D", branch}, it.repoDir);
    if (!dropped) return std::unexpected(dropped.error());
    auto unshelved = trace(it, "p4 shelve -d -c " + *cl,
                           p4::deleteShelve(it.p4, *cl));
    if (!unshelved) return std::unexpected(unshelved.error());
    auto deletedCl = p4::deleteChangelist(it.p4, *cl);
    if (!deletedCl) return std::unexpected(deletedCl.error());
    return {};
}

// Exercises `import_mode = worktree`: import must succeed with a dirty working
// tree (checkout mode refuses), advance the depot baseline without touching the
// user's checkout or their branch, then bring the branch up once the tree is
// clean, all while the persisted snapshot worktree's stamps survive across
// runs. Runs on 'main' (clean) after itShelfImport and restores that state.
std::expected<void, std::string> itWorktreeImport(ItContext& it) {
    const fs::path cfg = fs::path(it.repoDir) / "p4gw.cfg";
    const fs::path util = fs::path(it.srcWork) / "util.cpp";
    const fs::path main = fs::path(it.srcWork) / "main.cpp";

    // Save the pristine config to restore at the end (and to rebuild from
    // between the two modes we exercise).
    auto savedCfg = readFile(cfg);
    if (!savedCfg) return std::unexpected(savedCfg.error());

    // (a) Checkout mode still refuses a dirty tree. Worktree is now the
    // default, so opt into checkout explicitly. Dirty util.cpp, then import.
    auto checkoutCfg = appendFile(cfg, "\nimport_mode = checkout\n");
    if (!checkoutCfg) return checkoutCfg;
    auto dirtied = appendFile(util, "// local uncommitted edit\n");
    if (!dirtied) return dirtied;
    auto refused = runGw(it, it.repoDir, {"import"});
    if (refused) {
        return std::unexpected("checkout-mode import should refuse a dirty "
                               "tree, but it succeeded:\n" + *refused);
    }
    if (refused.error().find("not clean") == std::string::npos) {
        return std::unexpected("checkout-mode import failed for the wrong "
                               "reason:\n" + refused.error());
    }

    // (b) Switch to worktree mode (rewrite from the pristine config so only the
    // one import_mode line is present).
    auto appended = writeFile(cfg, *savedCfg + "\nimport_mode = worktree\n");
    if (!appended) return appended;

    // (c) A teammate-style depot change to import.
    auto cl = p4::createChangelist(it.p4, "gw integtest: worktree-mode change");
    if (!cl) return std::unexpected(cl.error());
    const std::string mirrorMain =
        (fs::path(it.mirrorDir) / "main.cpp").string();
    auto edited = trace(it, "p4 edit " + mirrorMain,
                        p4::editFiles(it.p4, *cl, {mirrorMain}));
    if (!edited) return std::unexpected(edited.error());
    auto appendedDepot = appendFile(mirrorMain, "// worktree-mode change\n");
    if (!appendedDepot) return appendedDepot;
    auto submitted = trace(it, "p4 submit -c " + *cl, p4::submit(it.p4, *cl));
    if (!submitted) return std::unexpected(submitted.error());
    auto synced = trace(it, "p4 sync " + it.p4DepotPath,
                        p4::sync(it.p4, it.p4DepotPath));
    if (!synced) return std::unexpected(synced.error());

    // (d) Import while the tree is still dirty: must succeed, advance the
    // baseline, and leave the user's checkout and branch untouched.
    auto oldBaseline = git::revParse("refs/p4gw/main", it.repoDir);
    if (!oldBaseline) return std::unexpected(oldBaseline.error());
    auto oldHead = git::revParse("HEAD", it.repoDir);
    if (!oldHead) return std::unexpected(oldHead.error());

    auto dirtyImport = runGw(it, it.repoDir, {"import"});
    if (!dirtyImport) return std::unexpected(dirtyImport.error());
    if (dirtyImport->find("working tree is not clean") == std::string::npos) {
        return std::unexpected("worktree-mode dirty import did not report the "
                               "skipped branch update:\n" + *dirtyImport);
    }
    auto newBaseline = git::revParse("refs/p4gw/main", it.repoDir);
    if (!newBaseline) return std::unexpected(newBaseline.error());
    if (*newBaseline == *oldBaseline) {
        return std::unexpected("worktree-mode import did not advance the depot "
                               "baseline");
    }
    auto baselineMain =
        git::run({"show", "refs/p4gw/main:src/main.cpp"}, it.repoDir);
    if (!baselineMain) return std::unexpected(baselineMain.error());
    if (baselineMain->find("// worktree-mode change") == std::string::npos) {
        return std::unexpected("the depot change is missing from the imported "
                               "baseline");
    }
    // The user's branch and HEAD must not have moved (on main + dirty).
    auto branchMain = git::revParse("refs/heads/main", it.repoDir);
    if (!branchMain) return std::unexpected(branchMain.error());
    if (*branchMain != *oldHead) {
        return std::unexpected("worktree-mode import advanced 'main' despite "
                               "the dirty tree");
    }
    auto nowHead = git::revParse("HEAD", it.repoDir);
    if (!nowHead || *nowHead != *oldHead) {
        return std::unexpected("worktree-mode import moved HEAD");
    }
    // The local edit survives; the depot change has NOT reached the checkout.
    auto utilNow = readFile(util);
    if (!utilNow) return std::unexpected(utilNow.error());
    if (utilNow->find("// local uncommitted edit") == std::string::npos) {
        return std::unexpected("worktree-mode import discarded the user's local "
                               "edit");
    }
    auto mainNow = readFile(main);
    if (!mainNow) return std::unexpected(mainNow.error());
    if (mainNow->find("// worktree-mode change") != std::string::npos) {
        return std::unexpected("worktree-mode import wrote the depot change "
                               "into the checkout (it should not touch it)");
    }
    const fs::path wtDir =
        fs::path(it.repoDir) / ".git" / "p4gw" / "worktree";
    if (!fs::exists(wtDir)) {
        return std::unexpected("the snapshot worktree was not created");
    }

    // (e) Clean the tree, import again: now the branch fast-forwards.
    auto restored = git::run({"restore", "src/util.cpp"}, it.repoDir);
    if (!restored) return std::unexpected(restored.error());
    auto cleanImport = runGw(it, it.repoDir, {"import"});
    if (!cleanImport) return std::unexpected(cleanImport.error());
    auto branchAfter = git::revParse("refs/heads/main", it.repoDir);
    if (!branchAfter) return std::unexpected(branchAfter.error());
    if (*branchAfter != *newBaseline) {
        return std::unexpected("clean worktree-mode import did not fast-forward "
                               "'main' onto the depot baseline");
    }
    auto mainAfter = readFile(main);
    if (!mainAfter) return std::unexpected(mainAfter.error());
    if (mainAfter->find("// worktree-mode change") == std::string::npos) {
        return std::unexpected("the depot change did not reach the checkout "
                               "after the clean import");
    }

    // (f) A no-change import is "Already up to date" (stamps survived unreset),
    // and doctor --verify confirms the worktree is healthy and byte-honest.
    auto noop = runGw(it, it.repoDir, {"import"});
    if (!noop) return std::unexpected(noop.error());
    if (noop->find("Already up to date") == std::string::npos) {
        return std::unexpected("a no-change worktree-mode import was not "
                               "reported as up to date:\n" + *noop);
    }
    auto verify = runGw(it, it.repoDir, {"doctor", "--verify"});
    if (!verify) return std::unexpected(verify.error());
    if (verify->find("snapshot worktree healthy") == std::string::npos) {
        return std::unexpected("doctor did not report the snapshot worktree "
                               "healthy:\n" + *verify);
    }
    if (verify->find("verified") == std::string::npos) {
        return std::unexpected("doctor --verify did not confirm the mirror "
                               "matches in worktree mode:\n" + *verify);
    }

    // (g) Restore the pristine config; end on 'main', clean, for itFinalChecks.
    auto wrote = writeFile(cfg, *savedCfg);
    if (!wrote) return wrote;
    return {};
}

// Renders one client-view line the way `p4 client -o` prints it (leading tab,
// -/+ prefix, spaces quoted). Shared by the steps that rewrite the spec.
std::string renderViewLine(const p4::ViewLine& line) {
    auto quoted = [](const std::string& path) {
        return path.find(' ') == std::string::npos ? path : '"' + path + '"';
    };
    std::string s = "\t";
    if (line.exclude) s += "-";
    if (line.overlay) s += "+";
    s += quoted(line.depot) + " " + quoted(line.client) + "\n";
    return s;
}

// Rebuilds a client spec from a header (everything through the "View:" line) and
// an ordered line list.
std::string buildClientSpec(const std::string& header,
                            const std::vector<p4::ViewLine>& lines) {
    std::string spec = header + "View:\n";
    for (const auto& line : lines) spec += renderViewLine(line);
    return spec;
}

// Exercises the have-manifest fast path and its fallback: a depot change is
// imported via the manifest diff (no mirror walk), a deleted manifest falls
// back to the full walk and is rewritten, a corrupted binding is ignored the
// same way, and the path is fast again afterwards. Runs on 'main', clean,
// with a manifest already written by the earlier import steps.
std::expected<void, std::string> itHaveManifest(ItContext& it) {
    const fs::path manifest =
        fs::path(it.repoDir) / ".git" / "p4gw" / "have-main";
    if (!fs::exists(manifest)) {
        return std::unexpected("no have manifest after the earlier imports (" +
                               manifest.string() + " missing)");
    }

    // A depot change, imported through the manifest diff.
    auto cl = p4::createChangelist(it.p4, "gw integtest: manifest change");
    if (!cl) return std::unexpected(cl.error());
    const std::string mirrorMain =
        (fs::path(it.mirrorDir) / "main.cpp").string();
    auto edited = trace(it, "p4 edit " + mirrorMain,
                        p4::editFiles(it.p4, *cl, {mirrorMain}));
    if (!edited) return std::unexpected(edited.error());
    auto appended = appendFile(mirrorMain, "// manifest change\n");
    if (!appended) return appended;
    auto submitted = trace(it, "p4 submit -c " + *cl, p4::submit(it.p4, *cl));
    if (!submitted) return std::unexpected(submitted.error());
    auto synced = trace(it, "p4 sync " + it.p4DepotPath,
                        p4::sync(it.p4, it.p4DepotPath));
    if (!synced) return std::unexpected(synced.error());

    auto fast = runGw(it, it.repoDir, {"import"});
    if (!fast) return std::unexpected(fast.error());
    if (fast->find("Have manifest for") == std::string::npos ||
        fast->find("1 changed, 0 deleted") == std::string::npos) {
        return std::unexpected("import did not take the have-manifest fast "
                               "path:\n" + *fast);
    }
    if (fast->find("Listing mirror files") != std::string::npos) {
        return std::unexpected("fast-path import still walked the mirror:\n" +
                               *fast);
    }
    auto mainNow = readFile(fs::path(it.srcWork) / "main.cpp");
    if (!mainNow) return std::unexpected(mainNow.error());
    if (mainNow->find("// manifest change") == std::string::npos) {
        return std::unexpected("the fast-path import did not deliver the "
                               "depot change");
    }

    // Delete the manifest: the next import must fall back to the full walk,
    // stay correct, and rewrite the manifest.
    std::error_code ec;
    fs::remove(manifest, ec);
    auto fallback = runGw(it, it.repoDir, {"import"});
    if (!fallback) return std::unexpected(fallback.error());
    if (fallback->find("Listing mirror files") == std::string::npos) {
        return std::unexpected("import without a manifest did not fall back "
                               "to the mirror walk:\n" + *fallback);
    }
    if (fallback->find("Already up to date") == std::string::npos) {
        return std::unexpected("the fallback import was not a clean no-op:\n" +
                               *fallback);
    }
    if (!fs::exists(manifest)) {
        return std::unexpected("the fallback import did not rewrite the have "
                               "manifest");
    }

    // Corrupt the binding: a manifest for the wrong snapshot must be ignored
    // (full walk again), then replaced with a correctly bound one.
    auto corrupted = writeFile(manifest, "snapshot 0000bogus\n");
    if (!corrupted) return corrupted;
    auto rebound = runGw(it, it.repoDir, {"import"});
    if (!rebound) return std::unexpected(rebound.error());
    if (rebound->find("Listing mirror files") == std::string::npos) {
        return std::unexpected("import with a stale manifest binding did not "
                               "fall back:\n" + *rebound);
    }

    // And with the rewritten manifest, the fast path is back.
    auto fastAgain = runGw(it, it.repoDir, {"import"});
    if (!fastAgain) return std::unexpected(fastAgain.error());
    if (fastAgain->find("Have manifest for") == std::string::npos ||
        fastAgain->find("Already up to date") == std::string::npos) {
        return std::unexpected("the rewritten manifest did not restore the "
                               "fast path:\n" + *fastAgain);
    }
    return {};
}

// Regression guard: build output that p4 syncs into the mirror but an `ignore`
// rule keeps out of Git must not break the fast path. The manifest diff still
// surfaces such a file when its p4 revision changes (a real have entry), and
// staging it by explicit pathspec would make `git add` refuse the whole batch
// ("paths are ignored ... use -f"); the fast path must drop it, exactly as the
// full walk's blanket `git add -A` does. The fixture config carries
// `ignore = /src/generated/` (baked at setup), so a file synced there is
// gitignored. Runs on 'main', clean, after itHaveManifest.
std::expected<void, std::string> itHaveManifestIgnored(ItContext& it) {
    const fs::path manifest =
        fs::path(it.repoDir) / ".git" / "p4gw" / "have-main";
    const std::string trackedRel = "src/generated/out.txt";
    const std::string mirrorGen =
        (fs::path(it.mirrorDir) / "generated" / "out.txt").string();
    auto isTracked = [&](const std::vector<std::string>& files) {
        return std::find(files.begin(), files.end(), trackedRel) != files.end();
    };

    // Create the ignored build-output file in the mirror and submit it so it
    // becomes a real have entry under the src include.
    auto wrote =
        writeFile(fs::path(mirrorGen), "generated output, ignored by Git\n");
    if (!wrote) return wrote;
    auto addCl = p4::createChangelist(it.p4, "gw integtest: generated output");
    if (!addCl) return std::unexpected(addCl.error());
    auto added = trace(it, "p4 add " + mirrorGen,
                       p4::addFiles(it.p4, *addCl, {mirrorGen}));
    if (!added) return std::unexpected(added.error());
    auto addSubmit = trace(it, "p4 submit -c " + *addCl,
                           p4::submit(it.p4, *addCl));
    if (!addSubmit) return std::unexpected(addSubmit.error());
    auto synced = trace(it, "p4 sync " + it.p4DepotPath,
                        p4::sync(it.p4, it.p4DepotPath));
    if (!synced) return std::unexpected(synced.error());

    // A full walk rewrites the manifest with the file present as a have entry -
    // but Git must not track it (the allowlist gitignores src/generated).
    std::error_code ec;
    fs::remove(manifest, ec);
    auto full = runGw(it, it.repoDir, {"import"});
    if (!full) return std::unexpected(full.error());
    auto tracked = git::lsFiles(it.repoDir);
    if (!tracked) return std::unexpected(tracked.error());
    if (isTracked(*tracked)) {
        return std::unexpected("gw tracked an ignored build-output file: " +
                               trackedRel);
    }

    // Bump the ignored file's revision and take the fast path. Pre-fix, the
    // changed ignored file entered the explicit `git add` pathspec and import
    // failed outright; now it is filtered out and import succeeds, still
    // tracking nothing.
    auto editCl =
        p4::createChangelist(it.p4, "gw integtest: bump generated output");
    if (!editCl) return std::unexpected(editCl.error());
    auto edited = trace(it, "p4 edit " + mirrorGen,
                        p4::editFiles(it.p4, *editCl, {mirrorGen}));
    if (!edited) return std::unexpected(edited.error());
    auto bumped = appendFile(fs::path(mirrorGen), "more output\n");
    if (!bumped) return bumped;
    auto editSubmit = trace(it, "p4 submit -c " + *editCl,
                            p4::submit(it.p4, *editCl));
    if (!editSubmit) return std::unexpected(editSubmit.error());
    auto synced2 = trace(it, "p4 sync " + it.p4DepotPath,
                         p4::sync(it.p4, it.p4DepotPath));
    if (!synced2) return std::unexpected(synced2.error());

    auto fast = runGw(it, it.repoDir, {"import"});
    if (!fast) {
        return std::unexpected("fast-path import failed on a changed ignored "
                               "file (the explicit-add bug):\n" + fast.error());
    }
    if (fast->find("Have manifest for") == std::string::npos ||
        fast->find("Listing mirror files") != std::string::npos) {
        return std::unexpected("import did not take the have-manifest fast "
                               "path:\n" + *fast);
    }
    auto trackedAfter = git::lsFiles(it.repoDir);
    if (!trackedAfter) return std::unexpected(trackedAfter.error());
    if (isTracked(*trackedAfter)) {
        return std::unexpected("the fast path tracked an ignored build-output "
                               "file");
    }
    return {};
}

// Regression guard for the manifest / excluded-carve-out bug: `p4 have` on an
// include's depot path also lists files the client view diverts elsewhere (an
// `exclude` subtree synced outside the mirror), and the fast path must resolve
// every entry through the ordered rules before touching the mirror - otherwise
// a changed carve-out file makes import try to copy it from a mirror path that
// never existed. Adds a `src/devtools` carve-out diverted to a *root-level*
// (auto-gitignored) client dir so the checkout stays clean, then proves the
// entry never reaches the manifest or a copy. Restores config, spec, and depot
// before returning. Runs on 'main', clean, after itHaveManifest.
std::expected<void, std::string> itHaveManifestExclude(ItContext& it) {
    const fs::path cfg = fs::path(it.repoDir) / "p4gw.cfg";
    const fs::path manifest =
        fs::path(it.repoDir) / ".git" / "p4gw" / "have-main";

    // "//.../src/..." -> "//.../src/", then the carve-out's depot paths.
    std::string srcBase = it.srcDepotPath;
    srcBase.resize(srcBase.size() - 3);
    const std::string devtoolsDepot = srcBase + "devtools/...";
    const std::string devtoolsDepotFile = srcBase + "devtools/tool.txt";
    // Diverted to a root-level client dir (a sibling of src/bin): the allowlist
    // gitignore hides everything at the root, so this synced-in-place file
    // leaves the working tree clean - yet its depot path is still under src, so
    // `p4 have //.../src/...` lists it. That is exactly the bug's shape.
    const fs::path inPlaceDir = fs::path(it.repoDir) / "devtools";
    const std::string inPlaceFile = (inPlaceDir / "tool.txt").string();

    auto savedCfg = readFile(cfg);
    if (!savedCfg) return std::unexpected(savedCfg.error());
    auto originalSpec = p4::clientSpec(it.p4);
    if (!originalSpec) return std::unexpected(originalSpec.error());

    // (1) Add the carve-out view line (later-wins diverts devtools out of the
    // mirror) and declare the matching exclude in p4gw.cfg.
    const std::string clientName = p4::specField(*originalSpec, "Client");
    const std::string clientRoot = p4::specField(*originalSpec, "Root");
    const std::string divertClient =
        p4::clientViewPath(clientName, clientRoot, inPlaceDir.string(), "/...");
    if (divertClient.empty()) {
        return std::unexpected("cannot compute the carve-out's client path");
    }
    const auto viewPos = originalSpec->find("\nView:");
    if (viewPos == std::string::npos) {
        return std::unexpected("client spec has no View: section");
    }
    const std::string header = originalSpec->substr(0, viewPos + 1);
    std::vector<p4::ViewLine> view = p4::parseClientView(*originalSpec);
    view.push_back({devtoolsDepot, divertClient, false, false});
    auto diverted =
        p4::writeClientSpec(it.p4, buildClientSpec(header, view));
    if (!diverted) return std::unexpected(diverted.error());
    auto excludedCfg = appendFile(cfg, "\nexclude = " + devtoolsDepot + "\n");
    if (!excludedCfg) return excludedCfg;

    // (2) Create the excluded depot file (synced in place at the root divert)
    // and sync so it appears in the include's have listing.
    auto wroteInPlace =
        writeFile(fs::path(inPlaceFile), "// devtools carve-out, synced in "
                                         "place, shipped by nobody\n");
    if (!wroteInPlace) return wroteInPlace;
    auto addCl = p4::createChangelist(it.p4, "gw integtest: devtools carve-out");
    if (!addCl) return std::unexpected(addCl.error());
    auto added = trace(it, "p4 add " + inPlaceFile,
                       p4::addFiles(it.p4, *addCl, {inPlaceFile}));
    if (!added) return std::unexpected(added.error());
    auto addSubmit = trace(it, "p4 submit -c " + *addCl,
                           p4::submit(it.p4, *addCl));
    if (!addSubmit) return std::unexpected(addSubmit.error());
    auto synced = trace(it, "p4 sync " + it.p4DepotPath,
                        p4::sync(it.p4, it.p4DepotPath));
    if (!synced) return std::unexpected(synced.error());

    // (3) Rewrite the manifest from a full walk now that the carve-out is
    // synced. The written manifest must NOT list the diverted file: it lives
    // outside this mirror, so recording it would make a later fast path try to
    // copy or delete a path that does not exist. (With the bug present, the
    // full walk stored it here and this assertion fails.)
    std::error_code ec;
    fs::remove(manifest, ec);
    auto full = runGw(it, it.repoDir, {"import"});
    if (!full) return std::unexpected(full.error());
    auto manifestText = readFile(manifest);
    if (!manifestText) return std::unexpected(manifestText.error());
    if (manifestText->find(devtoolsDepotFile) != std::string::npos) {
        return std::unexpected("the manifest recorded a file the view diverts "
                               "out of the mirror:\n" + *manifestText);
    }
    // The excluded subtree never entered the imported baseline either.
    auto baselineTree =
        git::run({"ls-tree", "-r", "--name-only", "refs/p4gw/main"},
                 it.repoDir);
    if (!baselineTree) return std::unexpected(baselineTree.error());
    if (baselineTree->find("devtools") != std::string::npos) {
        return std::unexpected("gw shipped a file through the exclude into the "
                               "depot baseline:\n" + *baselineTree);
    }

    // (4) Bump the carve-out's revision (in place) alongside a real owned
    // change, then take the fast path. Pre-fix, the changed carve-out drove a
    // copy from the nonexistent mirror path and import failed; now it is
    // filtered out and only the owned change flows.
    auto bumpCl = p4::createChangelist(it.p4, "gw integtest: bump carve-out "
                                              "and an owned file");
    if (!bumpCl) return std::unexpected(bumpCl.error());
    auto editedCarve = trace(it, "p4 edit " + inPlaceFile,
                             p4::editFiles(it.p4, *bumpCl, {inPlaceFile}));
    if (!editedCarve) return std::unexpected(editedCarve.error());
    auto bumpedCarve = appendFile(fs::path(inPlaceFile), "// bumped\n");
    if (!bumpedCarve) return bumpedCarve;
    const std::string mirrorMain =
        (fs::path(it.mirrorDir) / "main.cpp").string();
    auto editedMain = trace(it, "p4 edit " + mirrorMain,
                            p4::editFiles(it.p4, *bumpCl, {mirrorMain}));
    if (!editedMain) return std::unexpected(editedMain.error());
    auto bumpedMain = appendFile(fs::path(mirrorMain), "// exclude-test owned "
                                                       "change\n");
    if (!bumpedMain) return bumpedMain;
    auto bumpSubmit = trace(it, "p4 submit -c " + *bumpCl,
                            p4::submit(it.p4, *bumpCl));
    if (!bumpSubmit) return std::unexpected(bumpSubmit.error());
    auto synced2 = trace(it, "p4 sync " + it.p4DepotPath,
                         p4::sync(it.p4, it.p4DepotPath));
    if (!synced2) return std::unexpected(synced2.error());

    auto fast = runGw(it, it.repoDir, {"import"});
    if (!fast) {
        return std::unexpected("fast-path import failed on a changed excluded "
                               "carve-out (the manifest bug):\n" + fast.error());
    }
    if (fast->find("Have manifest for") == std::string::npos ||
        fast->find("Listing mirror files") != std::string::npos) {
        return std::unexpected("import did not take the have-manifest fast "
                               "path:\n" + *fast);
    }
    if (fast->find("1 changed, 0 deleted") == std::string::npos) {
        return std::unexpected("the fast path did not report exactly the one "
                               "owned change (the carve-out must be "
                               "invisible):\n" + *fast);
    }
    auto mainNow = readFile(fs::path(it.srcWork) / "main.cpp");
    if (!mainNow) return std::unexpected(mainNow.error());
    if (mainNow->find("// exclude-test owned change") == std::string::npos) {
        return std::unexpected("the fast-path import did not deliver the owned "
                               "change");
    }

    // (5) Restore config, spec, and depot so later steps see the original
    // fixture. Obliterate is guarded by the throwaway sentinel, as in cleanup.
    auto reverted = trace(it, "p4 revert " + it.p4DepotPath,
                          p4::revert(it.p4, it.p4DepotPath));
    if (!reverted) return std::unexpected(reverted.error());
    auto restoredCfg = writeFile(cfg, *savedCfg);
    if (!restoredCfg) return restoredCfg;
    auto restoredSpec = p4::writeClientSpec(it.p4, *originalSpec);
    if (!restoredSpec) return std::unexpected(restoredSpec.error());
    auto guard = itVerifyThrowaway(it);
    if (!guard) return std::unexpected(guard.error());
    auto obliterated = trace(it, "p4 obliterate -y " + devtoolsDepotFile,
                             p4::obliterate(it.p4, devtoolsDepotFile));
    if (!obliterated) return std::unexpected(obliterated.error());
    fs::remove_all(inPlaceDir, ec);
    fs::remove(manifest, ec);
    return {};
}

// Regression guard for the worktree-mode gitignore-staleness bug: the snapshot
// worktree stages detached at the old baseline, so it carries that baseline's
// .gitignore. When the user changes their allowlist - un-ignoring a subtree the
// mirror already holds (adding a re-include and re-running `gw init`, editing an
// ignore rule) - worktree imports (fast AND --full) staged against the stale
// rules, silently tracking nothing. The file's p4 revision never moved, so the
// rev-diff fast path can't see it either; the fix syncs the meta files into the
// worktree and forces the full walk when they change. Flips to worktree mode,
// leaves an ignored mirror file untracked, then un-ignores it and asserts the
// next import commits it. Restores config and .gitignore. Runs on 'main', clean.
std::expected<void, std::string> itWorktreeGitignore(ItContext& it) {
    const fs::path cfg = fs::path(it.repoDir) / "p4gw.cfg";
    const fs::path gitignore = fs::path(it.repoDir) / ".gitignore";
    const std::string trackedRel = "src/generated/w.txt";
    const std::string mirrorGen =
        (fs::path(it.mirrorDir) / "generated" / "w.txt").string();
    auto inBaseline = [&](const std::string& tree, const std::string& rel) {
        return tree.find(rel) != std::string::npos;
    };

    auto savedCfg = readFile(cfg);
    if (!savedCfg) return std::unexpected(savedCfg.error());
    auto savedIgnore = readFile(gitignore);
    if (!savedIgnore) return std::unexpected(savedIgnore.error());
    if (savedIgnore->find("/src/generated/") == std::string::npos) {
        return std::unexpected("fixture .gitignore lacks the '/src/generated/' "
                               "rule this test relies on");
    }

    // Worktree mode, and a mirror file under the currently-ignored subtree.
    auto appended = appendFile(cfg, "\nimport_mode = worktree\n");
    if (!appended) return appended;
    auto wrote = writeFile(fs::path(mirrorGen),
                           "worktree gitignore test payload\n");
    if (!wrote) return wrote;
    auto addCl = p4::createChangelist(it.p4, "gw integtest: worktree gitignore");
    if (!addCl) return std::unexpected(addCl.error());
    auto added = trace(it, "p4 add " + mirrorGen,
                       p4::addFiles(it.p4, *addCl, {mirrorGen}));
    if (!added) return std::unexpected(added.error());
    auto addSubmit = trace(it, "p4 submit -c " + *addCl,
                           p4::submit(it.p4, *addCl));
    if (!addSubmit) return std::unexpected(addSubmit.error());
    auto synced = trace(it, "p4 sync " + it.p4DepotPath,
                        p4::sync(it.p4, it.p4DepotPath));
    if (!synced) return std::unexpected(synced.error());

    // Import while still ignored: the file must stay out of the baseline.
    auto ignoredImport = runGw(it, it.repoDir, {"import"});
    if (!ignoredImport) return std::unexpected(ignoredImport.error());
    auto treeIgnored =
        git::run({"ls-tree", "-r", "--name-only", "refs/p4gw/main"},
                 it.repoDir);
    if (!treeIgnored) return std::unexpected(treeIgnored.error());
    if (inBaseline(*treeIgnored, trackedRel)) {
        return std::unexpected("an ignored mirror file entered the baseline "
                               "before it was un-ignored");
    }

    // Un-ignore the subtree (as re-running `gw init` after a config edit would),
    // then import: the worktree must pick up the new rule and commit the file.
    // Drop the whole "/src/generated/" line, terminator and all, so it works
    // whether the file uses LF or (on Windows) CRLF endings.
    std::string opened = *savedIgnore;
    if (const auto pos = opened.find("/src/generated/");
        pos != std::string::npos) {
        auto end = opened.find('\n', pos);
        end = end == std::string::npos ? opened.size() : end + 1;
        opened.erase(pos, end - pos);
    }
    if (opened.find("/src/generated/") != std::string::npos) {
        return std::unexpected("test bug: failed to strip the generated ignore "
                               "rule from .gitignore");
    }
    auto unignored = writeFile(gitignore, opened);
    if (!unignored) return unignored;

    auto trackImport = runGw(it, it.repoDir, {"import"});
    if (!trackImport) {
        return std::unexpected("worktree import after un-ignoring failed:\n" +
                               trackImport.error());
    }
    auto treeTracked =
        git::run({"ls-tree", "-r", "--name-only", "refs/p4gw/main"},
                 it.repoDir);
    if (!treeTracked) return std::unexpected(treeTracked.error());
    if (!inBaseline(*treeTracked, trackedRel)) {
        return std::unexpected("worktree import did not commit the newly "
                               "un-ignored mirror file (the stale-gitignore "
                               "bug):\n" + *treeTracked);
    }
    // The new baseline records the updated .gitignore, so the next worktree is
    // no longer stale.
    auto baselineIgnore =
        git::run({"show", "refs/p4gw/main:.gitignore"}, it.repoDir);
    if (!baselineIgnore) return std::unexpected(baselineIgnore.error());
    if (baselineIgnore->find("/src/generated/") != std::string::npos) {
        return std::unexpected("the baseline .gitignore was not updated to the "
                               "user's current one");
    }

    // Restore config and .gitignore, drop the depot file.
    auto reverted = trace(it, "p4 revert " + it.p4DepotPath,
                          p4::revert(it.p4, it.p4DepotPath));
    if (!reverted) return std::unexpected(reverted.error());
    auto restoredIgnore = writeFile(gitignore, *savedIgnore);
    if (!restoredIgnore) return restoredIgnore;
    auto restoredCfg = writeFile(cfg, *savedCfg);
    if (!restoredCfg) return restoredCfg;
    auto guard = itVerifyThrowaway(it);
    if (!guard) return std::unexpected(guard.error());
    const std::string genDepotFile = it.depotRoot + "/src/generated/w.txt";
    auto obliterated = trace(it, "p4 obliterate -y " + genDepotFile,
                             p4::obliterate(it.p4, genDepotFile));
    if (!obliterated) return std::unexpected(obliterated.error());
    return {};
}

std::expected<void, std::string> itFinalChecks(ItContext& it) {
    auto out = runGw(it, it.repoDir, {"doctor"});
    if (!out) return std::unexpected(out.error());

    auto opened = p4::openedFiles(it.p4);
    if (!opened) return std::unexpected(opened.error());
    if (!opened->empty()) {
        return std::unexpected("files are still opened under " +
                               it.p4DepotPath + ":\n" + *opened);
    }

    // gw must never have touched the unmapped files (bin/ and root).
    const std::pair<const FixtureFile*, size_t> untouched[] = {
        {kRootFixture, std::size(kRootFixture)},
        {kBinFixture, std::size(kBinFixture)},
    };
    for (const auto& [files, count] : untouched) {
        for (size_t i = 0; i < count; ++i) {
            auto content = readFile(fs::path(it.testRoot) / files[i].rel);
            if (!content) return std::unexpected(content.error());
            if (*content != files[i].content) {
                return std::unexpected(std::string(files[i].rel) +
                                       " was modified during the run - gw "
                                       "touched a file outside the overlay");
            }
        }
    }
    return {};
}

// gw doctor's client-view check is the architecture's safety interlock (the
// "remap line vanished from the spec" risk in PLAN.md), so break the view in
// each distinct way and assert doctor catches it with the right FAIL message.
// The spec is broken only for the duration of a single doctor run - mutate,
// run doctor, restore, then judge - so even a failed assertion leaves the
// client usable for the next run. One doctor message is deliberately not
// covered: the "maps into the Git repo directory" leak can't fire in this
// fixture because the repo IS the client root (everything syncs under it, so
// that generic check is disabled by design; the depot-subtree diversion check
// below is the one that covers the client-root case).
std::expected<void, std::string> itDoctorMisconfigs(ItContext& it) {
    auto originalSpec = p4::clientSpec(it.p4);
    if (!originalSpec) return std::unexpected(originalSpec.error());

    const std::string clientName = p4::specField(*originalSpec, "Client");
    const std::string clientRoot = p4::specField(*originalSpec, "Root");
    const std::string mirrorClient =
        p4::clientViewPath(clientName, clientRoot, it.mirrorDir, "/...");
    if (mirrorClient.empty()) {
        return std::unexpected("cannot compute the mirror's client view path");
    }

    // Split the spec into the header (through the "View:" line) and the parsed
    // view, so scenarios can rebuild the spec with a modified line list.
    const auto viewPos = originalSpec->find("\nView:");
    if (viewPos == std::string::npos) {
        return std::unexpected("client spec has no View: section");
    }
    const std::string header = originalSpec->substr(0, viewPos + 1);
    const auto view = p4::parseClientView(*originalSpec);

    auto buildSpec = [&](const std::vector<p4::ViewLine>& lines) {
        return buildClientSpec(header, lines);
    };

    // The remap line under test, and every other line in original order. The
    // baseline setup always carries the default broad `//depot/... //client/...`
    // line (p4 writes it into a fresh client), which is what makes the
    // missing/shadowed scenarios read as a wrong *effective* mapping rather
    // than no mapping at all.
    p4::ViewLine remap;
    bool foundRemap = false;
    std::vector<p4::ViewLine> others;
    for (const auto& line : view) {
        if (!line.exclude && !line.overlay && line.depot == it.srcDepotPath &&
            line.client == mirrorClient) {
            remap = line;
            foundRemap = true;
        } else {
            others.push_back(line);
        }
    }
    if (!foundRemap) {
        return std::unexpected("the view has no '" + it.srcDepotPath + " " +
                               mirrorClient + "' remap line to break");
    }

    // A depot subtree that covers nothing the config maps (scenario 1), and a
    // line diverting part of the mapped subtree to sync in place (scenario 5).
    std::string srcBase = it.srcDepotPath;  // "//.../src/..." -> "//.../src/"
    srcBase.resize(srcBase.size() - 3);
    const p4::ViewLine unrelated{
        it.depotRoot + "/p4gw-it-unrelated/...",
        "//" + clientName + "/p4gw-it-unrelated/...", false, false};
    const std::string divertClient = p4::clientViewPath(
        clientName, clientRoot, (fs::path(it.srcWork) / "lib").string(),
        "/...");
    if (divertClient.empty()) {
        return std::unexpected("cannot compute the diversion's client path");
    }
    const p4::ViewLine divert{srcBase + "lib/...", divertClient, false, false};
    p4::ViewLine excludeSrc = remap;
    excludeSrc.exclude = true;

    struct Scenario {
        const char* name;
        std::vector<p4::ViewLine> lines;
        const char* expect;  // doctor's FAIL message must contain this
    };
    std::vector<Scenario> scenarios;
    scenarios.push_back({"nothing maps the subtree",
                         {unrelated},
                         "is not mapped in the client view"});
    scenarios.push_back({"remap line removed (default line wins)", others,
                         "the effective mapping for"});
    {
        // Remap present but shadowed: a later, broader line wins per path.
        std::vector<p4::ViewLine> shadowed;
        shadowed.push_back(remap);
        shadowed.insert(shadowed.end(), others.begin(), others.end());
        scenarios.push_back({"remap shadowed by a later broader line",
                             std::move(shadowed), "the effective mapping for"});
    }
    {
        std::vector<p4::ViewLine> excluded = view;
        excluded.push_back(excludeSrc);
        scenarios.push_back({"subtree excluded from the view",
                             std::move(excluded), "the mirror would be empty"});
    }
    {
        std::vector<p4::ViewLine> diverted = view;
        diverted.push_back(divert);
        scenarios.push_back({"sub-path diverted out of the mirror",
                             std::move(diverted), "diverts part of"});
    }

    for (const auto& scenario : scenarios) {
        vlog(it, "-- doctor scenario: " + std::string(scenario.name));
        auto broke = p4::writeClientSpec(it.p4, buildSpec(scenario.lines));
        if (!broke) {
            return std::unexpected(std::string(scenario.name) + ": " +
                                   broke.error());
        }
        auto doctor = runGw(it, it.repoDir, {"doctor"});
        // Restore before judging, so a failed assertion never leaves the
        // client spec broken.
        auto restored = p4::writeClientSpec(it.p4, *originalSpec);
        if (!restored) {
            return std::unexpected("restoring the client spec failed: " +
                                   restored.error());
        }
        if (doctor) {
            return std::unexpected(std::string(scenario.name) +
                                   ": doctor passed on a broken view:\n" +
                                   *doctor);
        }
        if (doctor.error().find(scenario.expect) == std::string::npos) {
            return std::unexpected(
                std::string(scenario.name) +
                ": doctor failed for the wrong reason - expected \"" +
                scenario.expect + "\" in:\n" + doctor.error());
        }
    }

    // Positive control: with the original spec restored, doctor is green.
    auto healthy = runGw(it, it.repoDir, {"doctor"});
    if (!healthy) {
        return std::unexpected(
            "doctor still failing after the spec was restored:\n" +
            healthy.error());
    }
    return {};
}

// Exercises gw's git-branchless-specific import behavior, which the rest of the
// run (a plain-git repo) never touches. Skipped with a note when git-branchless
// is not on PATH, so `gw integtest run` still passes without it; CI installs a
// pinned release so these run there. Initializes branchless in the fixture repo,
// checks three behaviors, then uninstalls - all after the plain-git steps.
//   A) A detached HEAD's local commit is restacked via `git branchless sync`
//      (not a plain rebase) and left detached at the rewrite, not on a branch.
//   B) When the checked-out commit was itself already submitted, branchless
//      obsoletes it and gw leaves HEAD detached at the new depot baseline.
//   C) After `git branchless init --uninstall`, gw detects the repo as plain
//      again and falls back to `git rebase`.
std::expected<void, std::string> itBranchless(ItContext& it) {
    if (!git::run({"branchless", "--version"}, it.repoDir)) {
        std::printf("note  git-branchless is not on PATH - skipping the "
                    "branchless behavior checks\n");
        return {};
    }

    const fs::path util = fs::path(it.srcWork) / "util.cpp";
    const fs::path main = fs::path(it.srcWork) / "main.cpp";
    const std::string mirrorMain =
        (fs::path(it.mirrorDir) / "main.cpp").string();

    auto detached = [&]() {
        // symbolic-ref succeeds (a branch) or fails (detached).
        return !git::run({"symbolic-ref", "-q", "HEAD"}, it.repoDir);
    };
    // Advance the depot baseline the way a teammate's submit would, so an import
    // has something to bring forward.
    auto teammate = [&](const std::string& marker)
        -> std::expected<void, std::string> {
        auto cl = p4::createChangelist(it.p4, "gw integtest: branchless teammate");
        if (!cl) return std::unexpected(cl.error());
        auto edited = trace(it, "p4 edit " + mirrorMain,
                            p4::editFiles(it.p4, *cl, {mirrorMain}));
        if (!edited) return std::unexpected(edited.error());
        auto appended = appendFile(fs::path(mirrorMain), marker);
        if (!appended) return appended;
        auto submitted = trace(it, "p4 submit -c " + *cl, p4::submit(it.p4, *cl));
        if (!submitted) return std::unexpected(submitted.error());
        auto synced = trace(it, "p4 sync " + it.p4DepotPath,
                            p4::sync(it.p4, it.p4DepotPath));
        if (!synced) return std::unexpected(synced.error());
        return {};
    };

    // Start from a clean main with no leftover feature branches, so branchless
    // only ever restacks the commits this step creates.
    auto onMain = git::run({"switch", "-f", "main"}, it.repoDir);
    if (!onMain) return std::unexpected(onMain.error());
    auto branches = git::run(
        {"for-each-ref", "--format=%(refname:short)", "refs/heads"}, it.repoDir);
    if (!branches) return std::unexpected(branches.error());
    std::istringstream branchLines(*branches);
    std::string branch;
    while (std::getline(branchLines, branch)) {
        if (!branch.empty() && branch != "main") {
            auto dropped = git::run({"branch", "-D", branch}, it.repoDir);
            if (!dropped) return std::unexpected(dropped.error());
        }
    }
    auto init = git::run({"branchless", "init", "--main-branch", "main"},
                         it.repoDir);
    if (!init) {
        return std::unexpected("git branchless init failed: " + init.error());
    }

    // --- A: detached local work is restacked (branchless sync) and stays
    // detached at the rewrite. ---
    auto swA = git::run({"switch", "--detach", "main"}, it.repoDir);
    if (!swA) return std::unexpected(swA.error());
    if (auto r = appendFile(util, "// branchless local A\n"); !r) return r;
    if (auto r = git::addAll(it.repoDir); !r)
        return std::unexpected(r.error());
    if (auto r = git::commit("integtest branchless: local work A", it.repoDir);
        !r)
        return std::unexpected(r.error());
    if (auto r = teammate("// branchless teammate A\n"); !r) return r;
    auto importA = runGw(it, it.repoDir, {"import", "--rebase"});
    if (!importA) return std::unexpected(importA.error());
    if (importA->find("Restacked your visible commits") == std::string::npos) {
        return std::unexpected("branchless import did not take the branchless "
                               "sync path:\n" + *importA);
    }
    if (!detached()) {
        return std::unexpected("HEAD is on a branch after a branchless import "
                               "of detached work; expected it to stay detached");
    }
    auto onBaseline = git::isAncestor("refs/p4gw/main", "HEAD", it.repoDir);
    if (!onBaseline) return std::unexpected(onBaseline.error());
    if (!*onBaseline) {
        return std::unexpected("the restacked HEAD is not on the new depot "
                               "baseline");
    }
    auto utilA = readFile(util);
    if (!utilA) return std::unexpected(utilA.error());
    if (utilA->find("// branchless local A") == std::string::npos) {
        return std::unexpected("the restacked commit lost its local change");
    }

    // --- B: the checked-out commit is itself already applied -> HEAD stays
    // detached at the new baseline. ---
    auto swB = git::run({"switch", "--detach", "refs/p4gw/main"}, it.repoDir);
    if (!swB) return std::unexpected(swB.error());
    if (auto r = appendFile(main, "// branchless merged\n"); !r) return r;
    if (auto r = git::addAll(it.repoDir); !r)
        return std::unexpected(r.error());
    if (auto r = git::commit("integtest branchless: to be merged", it.repoDir);
        !r)
        return std::unexpected(r.error());
    // Submit the identical change so the import absorbs it (making the local
    // commit already-applied).
    if (auto r = teammate("// branchless merged\n"); !r) return r;
    auto importB = runGw(it, it.repoDir, {"import", "--rebase"});
    if (!importB) return std::unexpected(importB.error());
    if (importB->find("detached at the new depot baseline") ==
        std::string::npos) {
        return std::unexpected("a merged-away detached HEAD was not reported as "
                               "landing detached at the baseline:\n" + *importB);
    }
    if (!detached()) {
        return std::unexpected("HEAD is on a branch after a merged-away import; "
                               "expected it to stay detached");
    }
    auto headB = git::revParse("HEAD", it.repoDir);
    auto baseB = git::revParse("refs/p4gw/main", it.repoDir);
    if (!headB) return std::unexpected(headB.error());
    if (!baseB) return std::unexpected(baseB.error());
    if (*headB != *baseB) {
        return std::unexpected("HEAD did not land on the new depot baseline "
                               "after the merged-away import");
    }

    // --- D: detached ON the baseline (no work of its own) fast-forwards to the
    // new baseline, rather than being stranded on the prior one. ---
    auto swD = git::run({"switch", "--detach", "refs/p4gw/main"}, it.repoDir);
    if (!swD) return std::unexpected(swD.error());
    auto priorBase = git::revParse("HEAD", it.repoDir);
    if (!priorBase) return std::unexpected(priorBase.error());
    if (auto r = teammate("// branchless fast-forward\n"); !r) return r;
    auto importD = runGw(it, it.repoDir, {"import", "--rebase"});
    if (!importD) return std::unexpected(importD.error());
    auto headD = git::revParse("HEAD", it.repoDir);
    auto baseD = git::revParse("refs/p4gw/main", it.repoDir);
    if (!headD) return std::unexpected(headD.error());
    if (!baseD) return std::unexpected(baseD.error());
    if (*headD == *priorBase) {
        return std::unexpected("import left HEAD on the prior baseline instead "
                               "of fast-forwarding to the new one");
    }
    if (*headD != *baseD) {
        return std::unexpected("a detached-on-baseline import did not "
                               "fast-forward HEAD to the new baseline");
    }
    if (!detached()) {
        return std::unexpected("HEAD is on a branch after a fast-forward "
                               "import; expected it to stay detached");
    }

    // --- C: after uninstall, gw treats the repo as plain git again. ---
    auto uninstall = git::run({"branchless", "init", "--uninstall"}, it.repoDir);
    if (!uninstall) {
        return std::unexpected("git branchless uninstall failed: " +
                               uninstall.error());
    }
    auto swC = git::run({"switch", "--detach", "refs/p4gw/main"}, it.repoDir);
    if (!swC) return std::unexpected(swC.error());
    if (auto r = appendFile(util, "// post-uninstall local\n"); !r) return r;
    if (auto r = git::addAll(it.repoDir); !r)
        return std::unexpected(r.error());
    if (auto r = git::commit("integtest branchless: post-uninstall", it.repoDir);
        !r)
        return std::unexpected(r.error());
    if (auto r = teammate("// post-uninstall teammate\n"); !r) return r;
    auto importC = runGw(it, it.repoDir, {"import", "--rebase"});
    if (!importC) return std::unexpected(importC.error());
    if (importC->find("Rebased your detached work") == std::string::npos ||
        importC->find("Restacked") != std::string::npos) {
        return std::unexpected("after uninstall, import did not fall back to a "
                               "plain rebase:\n" + *importC);
    }

    // Leave a clean main for cleanup (branchless is already uninstalled).
    auto back = git::run({"switch", "-f", "main"}, it.repoDir);
    if (!back) return std::unexpected(back.error());
    auto after = git::run(
        {"for-each-ref", "--format=%(refname:short)", "refs/heads"}, it.repoDir);
    if (!after) return std::unexpected(after.error());
    std::istringstream afterLines(*after);
    while (std::getline(afterLines, branch)) {
        if (!branch.empty() && branch != "main") {
            (void)git::run({"branch", "-D", branch}, it.repoDir);
        }
    }
    return {};
}

// Exercises checkout-mode staging end-to-end now that worktree is the
// default: flips import_mode to checkout on a repo that has been importing
// in worktree mode (a leftover snapshot worktree sits on disk throughout),
// runs a clean fast-forward import and a --rebase one there, then flips back
// to the worktree default and asserts the now-stale snapshot worktree
// self-heals. Starts and ends on 'main', clean.
std::expected<void, std::string> itCheckoutMode(ItContext& it) {
    const fs::path cfg = fs::path(it.repoDir) / "p4gw.cfg";
    const fs::path repoMain = fs::path(it.srcWork) / "main.cpp";
    const std::string mirrorMain =
        (fs::path(it.mirrorDir) / "main.cpp").string();

    // Submit a teammate-style depot edit appending `tag`, then sync.
    auto teammate = [&](const std::string& tag)
        -> std::expected<void, std::string> {
        auto cl = p4::createChangelist(it.p4, "gw integtest: checkout-mode");
        if (!cl) return std::unexpected(cl.error());
        auto opened = trace(it, "p4 edit " + mirrorMain,
                            p4::editFiles(it.p4, *cl, {mirrorMain}));
        if (!opened) return std::unexpected(opened.error());
        if (auto r = appendFile(mirrorMain, tag); !r) return r;
        auto submitted = trace(it, "p4 submit -c " + *cl,
                               p4::submit(it.p4, *cl));
        if (!submitted) return std::unexpected(submitted.error());
        auto synced = trace(it, "p4 sync " + it.p4DepotPath,
                            p4::sync(it.p4, it.p4DepotPath));
        if (!synced) return std::unexpected(synced.error());
        return {};
    };

    // The earlier worktree-mode steps left a snapshot worktree behind; the
    // point of this step is that checkout mode works alongside it and the
    // flip back self-heals it, so make sure it is actually there.
    const fs::path wtDir = fs::path(it.repoDir) / ".git" / "p4gw" / "worktree";
    if (!fs::exists(wtDir)) {
        return std::unexpected("expected a snapshot worktree left over from "
                               "the worktree-mode steps");
    }

    auto savedCfg = readFile(cfg);
    if (!savedCfg) return std::unexpected(savedCfg.error());
    auto flipped = appendFile(cfg, "\nimport_mode = checkout\n");
    if (!flipped) return flipped;

    // (a) Clean-tree fast-forward on 'main': the full checkout staging path
    // (detach onto the old baseline, overlay, commit, switch back) must run
    // and leave the user exactly where they were, brought up to date.
    if (auto r = teammate("// checkout-ff change\n"); !r) return r;
    auto ffOut = runGw(it, it.repoDir, {"import"});
    if (!ffOut) return std::unexpected(ffOut.error());
    if (ffOut->find("You are on 'main'") == std::string::npos) {
        return std::unexpected("checkout-mode ff import did not end on "
                               "'main':\n" + *ffOut);
    }
    auto branch = git::currentBranch(it.repoDir);
    if (!branch) return std::unexpected(branch.error());
    if (*branch != "main") {
        return std::unexpected("checkout-mode import left HEAD on '" +
                               *branch + "', not back on 'main'");
    }
    auto baselineTip = git::revParse("refs/p4gw/main", it.repoDir);
    if (!baselineTip) return std::unexpected(baselineTip.error());
    auto mainTip = git::revParse("refs/heads/main", it.repoDir);
    if (!mainTip) return std::unexpected(mainTip.error());
    if (*mainTip != *baselineTip) {
        return std::unexpected("checkout-mode import did not fast-forward "
                               "'main' to the depot baseline");
    }
    auto ffContent = readFile(repoMain);
    if (!ffContent) return std::unexpected(ffContent.error());
    if (ffContent->find("// checkout-ff change") == std::string::npos) {
        return std::unexpected("the depot change did not reach the checkout "
                               "after the checkout-mode import");
    }
    auto dirty = git::isDirty(it.repoDir);
    if (!dirty) return std::unexpected(dirty.error());
    if (*dirty) {
        return std::unexpected("checkout-mode import left staging residue in "
                               "the working tree");
    }
    // The stale snapshot worktree must not trip doctor in checkout mode.
    auto doc = runGw(it, it.repoDir, {"doctor"});
    if (!doc) return std::unexpected(doc.error());

    // (b) --rebase with a local commit on a feature branch.
    auto sw = git::run({"switch", "-c", "it-checkout"}, it.repoDir);
    if (!sw) return std::unexpected(sw.error());
    if (auto r = appendFile(fs::path(it.srcWork) / "util.cpp",
                            "// checkout-mode local edit\n"); !r) {
        return r;
    }
    if (auto r = git::addAll(it.repoDir); !r) return std::unexpected(r.error());
    if (auto r = git::commit("integtest: checkout-mode local change",
                             it.repoDir); !r) {
        return std::unexpected(r.error());
    }
    if (auto r = teammate("// checkout-rebase change\n"); !r) return r;
    auto rebaseOut = runGw(it, it.repoDir, {"import", "--rebase"});
    if (!rebaseOut) return std::unexpected(rebaseOut.error());
    if (rebaseOut->find("Rebased 'it-checkout' onto the new depot state") ==
        std::string::npos) {
        return std::unexpected("checkout-mode import --rebase did not report "
                               "the rebase:\n" + *rebaseOut);
    }
    branch = git::currentBranch(it.repoDir);
    if (!branch) return std::unexpected(branch.error());
    if (*branch != "it-checkout") {
        return std::unexpected("checkout-mode --rebase left HEAD on '" +
                               *branch + "', not back on 'it-checkout'");
    }
    auto rebased = readFile(repoMain);
    if (!rebased) return std::unexpected(rebased.error());
    if (rebased->find("// checkout-rebase change") == std::string::npos) {
        return std::unexpected("the depot change is missing after the "
                               "checkout-mode rebase");
    }
    auto ahead = git::run({"rev-list", "--count", "main..HEAD"}, it.repoDir);
    if (!ahead) return std::unexpected(ahead.error());
    if (*ahead != "1") {
        return std::unexpected("expected exactly 1 commit ahead of main after "
                               "the checkout-mode rebase, got " + *ahead);
    }
    auto back = git::run({"switch", "-f", "main"}, it.repoDir);
    if (!back) return std::unexpected(back.error());
    (void)git::run({"branch", "-D", "it-checkout"}, it.repoDir);

    // (c) Flip back to the worktree default: the snapshot worktree is now
    // stale (the baseline advanced twice while checkout mode ran), so the
    // next import must self-heal it rather than trust its stamps.
    if (auto r = writeFile(cfg, *savedCfg); !r) return r;
    if (auto r = teammate("// flip-back change\n"); !r) return r;
    auto healOut = runGw(it, it.repoDir, {"import"});
    if (!healOut) return std::unexpected(healOut.error());
    auto healed = readFile(repoMain);
    if (!healed) return std::unexpected(healed.error());
    if (healed->find("// flip-back change") == std::string::npos) {
        return std::unexpected("the depot change did not arrive after "
                               "flipping back to worktree mode");
    }
    // The worktree staged this import: its HEAD tracks the new baseline.
    auto wtHead = git::revParse("HEAD", wtDir.string());
    if (!wtHead) return std::unexpected(wtHead.error());
    baselineTip = git::revParse("refs/p4gw/main", it.repoDir);
    if (!baselineTip) return std::unexpected(baselineTip.error());
    if (*wtHead != *baselineTip) {
        return std::unexpected("the snapshot worktree did not self-heal onto "
                               "the new baseline after the mode flip");
    }
    auto verify = runGw(it, it.repoDir, {"doctor", "--verify"});
    if (!verify) return std::unexpected(verify.error());
    if (verify->find("snapshot worktree healthy") == std::string::npos) {
        return std::unexpected("doctor does not report the snapshot worktree "
                               "healthy after the mode flip:\n" + *verify);
    }
    return {};
}

// Leaves both sides clean: reverts opens, sweeps any leftover changelists,
// obliterates the explicitly-named depot files, and wipes the local tree. Runs
// as the last step of a successful run (unless --leave) and as the whole job
// under the `clean` subcommand. Shares wipeLocal (and its guard) with
// itResetLocal.
std::expected<void, std::string> itCleanup(ItContext& it) {
    // 1. Drop any opened files in the test depot (no-op after a clean run;
    //    recovers an aborted one).
    auto reverted = trace(it, "p4 revert " + it.p4DepotPath,
                          p4::revert(it.p4, it.p4DepotPath));
    if (!reverted) return std::unexpected(reverted.error());

    // 2. Best-effort: delete this user's leftover pending/shelved changelists.
    //    An aborted run can leave a shelf or empty CL behind; a clean run
    //    leaves none. Errors here are cleanup niceties, not failures.
    auto user = p4::currentUser(it.p4);
    if (user) {
        auto listing = p4::pendingChangelistsForUser(it.p4, *user);
        if (listing) {
            for (const auto& cl : parseChanges(*listing)) {
                // Best-effort: a shelf may not exist, and a CL may refuse to
                // delete; either way keep sweeping the rest.
                (void)trace(it, "p4 shelve -d -c " + cl.change,
                            p4::deleteShelve(it.p4, cl.change));
                (void)trace(it, "p4 change -d " + cl.change,
                            p4::deleteChangelist(it.p4, cl.change));
            }
        }
    }

    // 3. Re-verify the throwaway sentinel immediately before the irreversible
    //    obliterate, then remove each explicitly-named depot file (no
    //    wildcards). A file that isn't in the depot is treated as success.
    auto guard = itVerifyThrowaway(it);
    if (!guard) return std::unexpected(guard.error());
    for (const char* rel : kObliterateFiles) {
        const std::string depotFile = it.depotRoot + "/" + rel;
        auto obliterated = trace(it, "p4 obliterate -y " + depotFile,
                                 p4::obliterate(it.p4, depotFile));
        if (!obliterated) return std::unexpected(obliterated.error());
    }

    // 4. Wipe the local tree, leaving only the connection config.
    return wipeLocal(it);
}

using Step = std::pair<const char*,
                       std::function<std::expected<void, std::string>()>>;

int runSteps(const std::vector<Step>& steps) {
    for (const auto& [name, fn] : steps) {
        auto result = fn();
        if (!result) {
            std::printf("FAIL  %s\n%s\n", name, result.error().c_str());
            return 1;
        }
        std::printf("ok    %s\n", name);
    }
    return 0;
}

// argv[0] with a path is made absolute so child spawns survive cwd changes;
// a bare name keeps using PATH lookup.
std::string resolveGwExe(const std::string& argv0) {
    if (argv0.find('/') == std::string::npos &&
        argv0.find('\\') == std::string::npos) {
        return argv0;
    }
    std::error_code ec;
    const fs::path absolute = fs::absolute(argv0, ec);
    return ec ? argv0 : absolute.string();
}

}  // namespace

int cmdIntegtest(const std::string& gwExe, const Args& args) {
    auto printUsage = [](std::FILE* out) {
        std::fprintf(out,
                     "usage: gw integtest <command> [options]\n"
                     "\n"
                     "commands:\n"
                     "  run    Drive the full workflow against a throwaway p4d,\n"
                     "         resetting the fixture (local + depot) first and\n"
                     "         cleaning up afterward.\n"
                     "  clean  Only clean up: revert opens, drop leftover\n"
                     "         changelists, obliterate the depot files, and wipe\n"
                     "         the local tree (recover after a failed run).\n"
                     "\n"
                     "options:\n"
                     "  --leave      (run only) keep the built state instead of\n"
                     "               cleaning up at the end\n"
                     "  --force      wipe even past unrecognized files\n"
                     "  --verbose    echo every command and its output\n"
                     "  --gw <path>  gw binary to drive (default: this one)\n"
                     "\n"
                     "DESTRUCTIVE: obliterates depot files and wipes everything\n"
                     "under the current directory except p4.ini/.p4config.\n"
                     "Refuses unless the server is a throwaway (ServerID '%s',\n"
                     "security 0). Needs p4 and a live server; see "
                     "README-integtest.md.\n"
                     "\n",
                     kThrowawayServerId);
    };
    auto usage = [&] {
        printUsage(stderr);
        return 1;
    };

    for (const auto& arg : args) {
        if (arg == "--help" || arg == "-h") {
            printUsage(stdout);
            return 0;
        }
    }

    ItContext it;
    it.gw = resolveGwExe(gwExe);

    if (args.empty()) {
        std::fprintf(stderr, "gw integtest: a subcommand is required\n\n");
        return usage();
    }
    const std::string& sub = args[0];
    if (sub == "run") {
        it.clean = false;
    } else if (sub == "clean") {
        it.clean = true;
    } else {
        std::fprintf(stderr, "gw integtest: unknown subcommand '%s'\n\n",
                     sub.c_str());
        return usage();
    }

    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--verbose") {
            it.verbose = true;
        } else if (arg == "--force") {
            it.force = true;
        } else if (arg == "--leave") {
            if (it.clean) {
                std::fprintf(stderr,
                             "gw integtest: --leave is not valid with 'clean'\n");
                return usage();
            }
            it.leave = true;
        } else if (arg == "--gw" && i + 1 < args.size()) {
            it.gw = args[++i];
        } else {
            std::fprintf(stderr, "gw integtest: unknown option '%s'\n",
                         arg.c_str());
            return usage();
        }
    }

    std::vector<Step> steps;
    steps.emplace_back("discover the test depot path (p4 where)",
                       [&] { return discover(it); });
    steps.emplace_back("confirm this is a throwaway server",
                       [&] { return itVerifyThrowaway(it); });
    if (it.clean) {
        // Recovery only: clean up and exit (the `clean` subcommand).
        steps.emplace_back("clean up the depot and local repo",
                           [&] { return itCleanup(it); });
    } else {
        steps.emplace_back("verify the src -> .p4gw/src view mapping",
                           [&] { return itVerifyMapping(it); });
        steps.emplace_back("revert, sync, and wipe the local test directory",
                           [&] { return itResetLocal(it); });
        steps.emplace_back("write the fixture files",
                           [&] { return itWriteFixture(it); });
        steps.emplace_back("reconcile and submit the fixture",
                           [&] { return itSubmitFixture(it); });
        steps.emplace_back("gw setup in src/",
                           [&] { return itGwSetup(it); });
        steps.emplace_back("gw init verifies the mapping and creates the repo",
                           [&] { return itGwInit(it); });
        steps.emplace_back("sync + first gw import builds main",
                           [&] { return itFirstImport(it); });
        steps.emplace_back("p4 print matches synced bytes (text and binary)",
                           [&] { return itPrintFidelity(it); });
        steps.emplace_back("gw prepare slices a single commit, range, and stack",
                           [&] { return itPrepareSlice(it); });
        steps.emplace_back("feature branch: edit/add then delete/rename",
                           [&] { return itFeatureBranch(it); });
        steps.emplace_back("gw prepare --shelf shelves and leaves no opens",
                           [&] { return itPrepareShelf(it); });
        steps.emplace_back("gw prepare opens the exact expected files",
                           [&] { return itPrepare(it); });
        steps.emplace_back("gw prepare --update refreshes the CL in place",
                           [&] { return itPrepareUpdate(it); });
        steps.emplace_back("submit, then gw import --rebase melts the branch",
                           [&] { return itSubmitAndAbsorb(it); });
        steps.emplace_back("gw prepare --shelf --update replaces a shelf in "
                           "place",
                           [&] { return itPrepareShelfUpdate(it); });
        steps.emplace_back("gw prepare --abandon reverts and deletes a CL",
                           [&] { return itPrepareAbandon(it); });
        steps.emplace_back("teammate change absorbed with import --rebase",
                           [&] { return itTeammateChange(it); });
        steps.emplace_back("opened mirror files: import reads depot, prepare "
                           "refuses",
                           [&] { return itOpenedFilesPreflight(it); });
        steps.emplace_back("gw shelf import reconstructs a shelved CL as a "
                           "branch",
                           [&] { return itShelfImport(it); });
        steps.emplace_back("worktree-mode import: dirty-tree ok, checkout "
                           "untouched",
                           [&] { return itWorktreeImport(it); });
        steps.emplace_back("have-manifest fast path, fallback, and rebinding",
                           [&] { return itHaveManifest(it); });
        steps.emplace_back("have-manifest fast path skips a gitignored mirror "
                           "file",
                           [&] { return itHaveManifestIgnored(it); });
        steps.emplace_back("have-manifest ignores an excluded, diverted "
                           "carve-out",
                           [&] { return itHaveManifestExclude(it); });
        steps.emplace_back("worktree import picks up an updated .gitignore",
                           [&] { return itWorktreeGitignore(it); });
        steps.emplace_back("doctor clean, nothing opened, no stray writes",
                           [&] { return itFinalChecks(it); });
        steps.emplace_back("doctor catches each deliberate view "
                           "misconfiguration",
                           [&] { return itDoctorMisconfigs(it); });
        steps.emplace_back("git-branchless: sync restack, detached preserved, "
                           "uninstall",
                           [&] { return itBranchless(it); });
        steps.emplace_back("checkout-mode import: ff, rebase, and worktree "
                           "flip-back self-heal",
                           [&] { return itCheckoutMode(it); });
        if (!it.leave) {
            steps.emplace_back("clean up the depot and local repo",
                               [&] { return itCleanup(it); });
        }
    }

    const int rc = runSteps(steps);
    if (rc == 0) {
        std::printf("\nintegtest: all %zu steps passed.\n", steps.size());
        if (it.leave && !it.clean) {
            std::printf("Built state left in place (--leave); run "
                        "'gw integtest clean' to remove it.\n");
        }
    } else {
        std::printf("\nintegtest failed. Rerun with --verbose for every command "
                    "and its output.\nWhen done inspecting, "
                    "'gw integtest clean' resets the depot and local repo.\n");
    }
    return rc;
}

}  // namespace p4gw