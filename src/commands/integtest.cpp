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
#include "process.h"
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

// The ServerID a throwaway p4d must carry before this destructive command will
// touch it; the README setup writes this exact value to server.id.
constexpr const char* kThrowawayServerId = "p4gw-integtest-throwaway";

// Every depot file (relative to depotRoot) that init or the run can submit;
// cleanup obliterates exactly these - explicit paths, never a wildcard.
constexpr const char* kObliterateFiles[] = {
    "readme.txt", "notes.txt",
    "bin/tool.txt", "bin/helper.txt",
    "src/main.cpp", "src/util.cpp", "src/util.h",
    "src/utils.h", "src/newfile.cpp", "src/docs/overview.md",
};

// Top-level names integtest itself creates under testRoot (== repoDir). The
// local-wipe guard treats anything else as a stray it must not blindly delete.
// p4.ini/.p4config are the personal connection config and are kept, not wiped.
constexpr const char* kKnownLocalEntries[] = {
    "p4.ini", ".p4config",
    "p4gw.cfg", ".gitignore", ".git",
    ".p4gw", "src", "bin",
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
    vlog(it, result->output);
    if (result->exitCode != 0) {
        return std::unexpected(display + " exited " +
                               std::to_string(result->exitCode) + ":\n" +
                               result->output);
    }
    return result->output;
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
        steps.emplace_back("feature branch: edit/add then delete/rename",
                           [&] { return itFeatureBranch(it); });
        steps.emplace_back("gw prepare --shelf shelves and leaves no opens",
                           [&] { return itPrepareShelf(it); });
        steps.emplace_back("gw prepare opens the exact expected files",
                           [&] { return itPrepare(it); });
        steps.emplace_back("submit, then gw import --rebase melts the branch",
                           [&] { return itSubmitAndAbsorb(it); });
        steps.emplace_back("teammate change absorbed with import --rebase",
                           [&] { return itTeammateChange(it); });
        steps.emplace_back("opened mirror files: import reads depot, prepare "
                           "refuses",
                           [&] { return itOpenedFilesPreflight(it); });
        steps.emplace_back("gw shelf import reconstructs a shelved CL as a "
                           "branch",
                           [&] { return itShelfImport(it); });
        steps.emplace_back("doctor clean, nothing opened, no stray writes",
                           [&] { return itFinalChecks(it); });
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
