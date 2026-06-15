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

namespace fs = std::filesystem;

namespace p4gw {

namespace {

// Live-P4 integration tests (see PLAN-integrationtests.md). Everything runs
// inside a `p4gw-test` directory under the client root, whose depot path is
// discovered via `p4 where`; the user has pre-mapped its `src` subtree to
// `.p4gw/mirror/src` in the client view. `integtest init` (re)builds a
// known fixture in the depot; `integtest run` drives the real workflow
// through child `gw` processes and asserts state at each step.

struct ItContext {
    std::string gw;            // gw executable to spawn for commands under test
    bool verbose = false;
    std::string testRoot;      // the p4gw-test directory (cwd)
    std::string depotRoot;     // its depot path, no trailing /...
    std::string srcDepotPath;  // depotRoot + "/src/..."
    std::string repoDir;       // testRoot/src - the Git overlay repo
    std::string mirrorDir;     // testRoot/.p4gw/mirror/src
    Config p4;                 // ambient client; depotPath = depotRoot + "/..."
    std::string pendingCl;     // CL produced by the prepare step
};

struct FixtureFile {
    const char* rel;
    const char* content;
};

// Root and bin files live outside the overlay: gw must never touch them.
constexpr FixtureFile kRootFixture[] = {
    {"readme.txt", "p4gw-test fixture: root file, outside the overlay\n"},
    {"notes.txt", "p4gw-test fixture: more root content\n"},
};
constexpr FixtureFile kBinFixture[] = {
    {"bin/tool.txt", "p4gw-test fixture: bin file, outside the overlay\n"},
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
        "     remaps the test's src subtree into the mirror, for example:\n"
        "       //depot/.../p4gw-test/src/...   "
        "//CLIENT/.../p4gw-test/.p4gw/mirror/src/...\n"
        "     Later view lines win, so keep it last. (init verifies this and\n"
        "     prints the exact line to use if it's off.)\n"
        "  5. From inside p4gw-test, run:  gw integtest init\n"
        "Full details: PLAN-integrationtests.md.\n";
}

std::expected<void, std::string> discover(ItContext& it) {
    it.testRoot = fs::current_path().string();
    if (fs::path(it.testRoot).filename() != "p4gw-test") {
        return std::unexpected(
            "integtest must be run from inside a directory named "
            "'p4gw-test' (integtest init DELETES everything under the "
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
    it.p4.depotPath = it.depotRoot + "/...";
    it.srcDepotPath = it.depotRoot + "/src/...";
    it.repoDir = (fs::path(it.testRoot) / "src").string();
    it.mirrorDir =
        (fs::path(it.testRoot) / ".p4gw" / "mirror" / "src").string();
    return {};
}

// ---- integtest init steps ----

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
        message += problem + "\n";
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
    auto reverted = trace(it, "p4 revert " + it.p4.depotPath,
                          p4::revert(it.p4, it.p4.depotPath));
    if (!reverted) return std::unexpected(reverted.error());
    auto synced = trace(it, "p4 sync " + it.p4.depotPath,
                        p4::sync(it.p4, it.p4.depotPath));
    if (!synced) return std::unexpected(synced.error());

    std::error_code ec;
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
    auto reconciled = trace(it, "p4 reconcile -c " + *cl + " " + it.p4.depotPath,
                            p4::reconcileToCl(it.p4, *cl, it.p4.depotPath));
    if (!reconciled) return std::unexpected(reconciled.error());
    auto opened = p4::openedInCl(it.p4, *cl);
    if (!opened) return std::unexpected(opened.error());
    if (opened->empty()) {
        auto deleted = p4::deleteChangelist(it.p4, *cl);
        if (!deleted) return std::unexpected(deleted.error());
        std::printf("note  fixture already matches the depot - nothing "
                    "submitted\n");
        return {};
    }
    auto submitted = trace(it, "p4 submit -c " + *cl, p4::submit(it.p4, *cl));
    if (!submitted) return std::unexpected(submitted.error());
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
                      "--mirror-path", "../.p4gw/mirror/src"});
    if (!out) return std::unexpected(out.error());
    if (!fs::is_regular_file(fs::path(it.repoDir) / "p4gw.cfg")) {
        return std::unexpected("gw setup did not write " + it.repoDir +
                               "/p4gw.cfg");
    }
    return {};
}

// ---- integtest run steps ----

std::expected<void, std::string> itGwInit(ItContext& it) {
    auto out = runGw(it, it.repoDir, {"init"});
    if (!out) return std::unexpected(out.error());
    auto branch = git::currentBranch(it.repoDir);
    if (!branch) return std::unexpected(branch.error());
    if (*branch != "p4-main") {
        return std::unexpected("expected to be on p4-main after init, got '" +
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
    auto synced = trace(it, "p4 sync " + it.p4.depotPath,
                        p4::sync(it.p4, it.p4.depotPath));
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
    std::sort(trackedContent.begin(), trackedContent.end());
    std::sort(mirrorFiles->begin(), mirrorFiles->end());
    if (trackedContent != *mirrorFiles) {
        std::string message = "tracked files do not match the mirror after "
                              "import\n  tracked:";
        for (const auto& file : trackedContent) message += " " + file;
        message += "\n  mirror: ";
        for (const auto& file : *mirrorFiles) message += " " + file;
        return std::unexpected(message);
    }

    auto repoMain = readFile(fs::path(it.repoDir) / "main.cpp");
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
    auto edited = appendFile(fs::path(it.repoDir) / "util.cpp",
                             "// integtest edit\n");
    if (!edited) return edited;
    auto added = writeFile(fs::path(it.repoDir) / "newfile.cpp",
                           "// integtest new file\nint newFn() { return 2; }\n");
    if (!added) return added;
    auto staged = git::addAll(it.repoDir);
    if (!staged) return std::unexpected(staged.error());
    auto committed =
        git::commit("integtest: edit util.cpp and add newfile.cpp",
                    it.repoDir);
    if (!committed) return std::unexpected(committed.error());

    // Commit 2: delete + rename.
    auto removed = trace(it, "git rm docs/overview.md",
                         git::run({"rm", "-q", "docs/overview.md"}, it.repoDir));
    if (!removed) return std::unexpected(removed.error());
    auto renamed = trace(it, "git mv util.h utils.h",
                         git::run({"mv", "util.h", "utils.h"}, it.repoDir));
    if (!renamed) return std::unexpected(renamed.error());
    committed = git::commit("integtest: delete overview.md, rename util.h",
                            it.repoDir);
    if (!committed) return std::unexpected(committed.error());
    return {};
}

std::expected<void, std::string> itPrepare(ItContext& it) {
    auto out = runGw(it, it.repoDir, {"prepare"});
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
    auto synced = trace(it, "p4 sync " + it.p4.depotPath,
                        p4::sync(it.p4, it.p4.depotPath));
    if (!synced) return std::unexpected(synced.error());

    auto out = runGw(it, it.repoDir, {"import", "--rebase"});
    if (!out) return std::unexpected(out.error());

    auto featureTip = git::revParse("it-feature", it.repoDir);
    auto baselineTip = git::revParse("p4-main", it.repoDir);
    if (!featureTip) return std::unexpected(featureTip.error());
    if (!baselineTip) return std::unexpected(baselineTip.error());
    if (*featureTip != *baselineTip) {
        return std::unexpected(
            "after submitting and importing, the branch commits should melt "
            "away (it-feature " + *featureTip + " != p4-main " +
            *baselineTip + ")");
    }
    if (fs::exists(fs::path(it.repoDir) / "docs" / "overview.md")) {
        return std::unexpected("docs/overview.md still exists after its "
                               "delete was submitted and imported");
    }
    if (!fs::exists(fs::path(it.repoDir) / "utils.h")) {
        return std::unexpected("utils.h (renamed from util.h) missing after "
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
    auto synced = trace(it, "p4 sync " + it.p4.depotPath,
                        p4::sync(it.p4, it.p4.depotPath));
    if (!synced) return std::unexpected(synced.error());

    auto switched = trace(it, "git switch -c it-feature2",
                          git::run({"switch", "-c", "it-feature2"}, it.repoDir));
    if (!switched) return std::unexpected(switched.error());
    auto changed = appendFile(fs::path(it.repoDir) / "util.cpp",
                              "// second feature edit\n");
    if (!changed) return changed;
    auto staged = git::addAll(it.repoDir);
    if (!staged) return std::unexpected(staged.error());
    auto committed = git::commit("integtest: feature2 local change",
                                 it.repoDir);
    if (!committed) return std::unexpected(committed.error());

    auto out = runGw(it, it.repoDir, {"import", "--rebase"});
    if (!out) return std::unexpected(out.error());

    auto repoMain = readFile(fs::path(it.repoDir) / "main.cpp");
    if (!repoMain) return std::unexpected(repoMain.error());
    if (repoMain->find("// teammate change") == std::string::npos) {
        return std::unexpected("the teammate's submitted change is missing "
                               "from the repo after import --rebase");
    }
    auto ahead = git::run({"rev-list", "--count", "p4-main..HEAD"},
                          it.repoDir);
    if (!ahead) return std::unexpected(ahead.error());
    if (*ahead != "1") {
        return std::unexpected("expected exactly 1 commit ahead of p4-main "
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
    auto baselineUtil = git::run({"show", "p4-main:util.cpp"}, it.repoDir);
    if (!baselineUtil) return std::unexpected(baselineUtil.error());
    if (baselineUtil->find("UNSUBMITTED stray edit") != std::string::npos) {
        return std::unexpected("import absorbed an un-submitted mirror edit "
                               "into p4-main:\n" + *baselineUtil);
    }

    // Clean up: revert the open, drop the empty CL, restore the mirror.
    auto reverted = trace(it, "p4 revert " + it.p4.depotPath,
                          p4::revert(it.p4, it.p4.depotPath));
    if (!reverted) return std::unexpected(reverted.error());
    auto deleted = p4::deleteChangelist(it.p4, *cl);
    if (!deleted) return std::unexpected(deleted.error());
    auto synced = trace(it, "p4 sync " + it.p4.depotPath,
                        p4::sync(it.p4, it.p4.depotPath));
    if (!synced) return std::unexpected(synced.error());
    return {};
}

std::expected<void, std::string> itFinalChecks(ItContext& it) {
    auto out = runGw(it, it.repoDir, {"doctor"});
    if (!out) return std::unexpected(out.error());

    auto opened = p4::openedFiles(it.p4);
    if (!opened) return std::unexpected(opened.error());
    if (!opened->empty()) {
        return std::unexpected("files are still opened under " +
                               it.p4.depotPath + ":\n" + *opened);
    }

    // gw must never have touched anything outside the overlay.
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
    auto usage = [] {
        std::fprintf(stderr,
                     "usage: gw integtest <init|run> [--verbose] [--gw <path>]\n"
                     "  init  reset the p4gw-test fixture (DELETES everything "
                     "under the current\n        directory except p4.ini) and "
                     "submit it to the depot\n"
                     "  run   drive the full workflow against the fixture "
                     "(run init first)\n"
                     "Needs p4 and a live server; see "
                     "PLAN-integrationtests.md.\n");
        return 1;
    };
    if (args.empty() || (args[0] != "init" && args[0] != "run")) {
        return usage();
    }
    const std::string mode = args[0];

    ItContext it;
    it.gw = resolveGwExe(gwExe);
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--verbose") {
            it.verbose = true;
        } else if (args[i] == "--gw" && i + 1 < args.size()) {
            it.gw = args[++i];
        } else {
            std::fprintf(stderr, "gw integtest: unknown option '%s'\n",
                         args[i].c_str());
            return usage();
        }
    }

    std::vector<Step> steps;
    steps.emplace_back("discover the test depot path (p4 where)",
                       [&] { return discover(it); });
    if (mode == "init") {
        steps.emplace_back("verify the src -> .p4gw/mirror/src view mapping",
                           [&] { return itVerifyMapping(it); });
        steps.emplace_back("revert, sync, and wipe the local test directory",
                           [&] { return itResetLocal(it); });
        steps.emplace_back("write the fixture files",
                           [&] { return itWriteFixture(it); });
        steps.emplace_back("reconcile and submit the fixture",
                           [&] { return itSubmitFixture(it); });
        steps.emplace_back("gw setup in src/",
                           [&] { return itGwSetup(it); });
    } else {
        steps.emplace_back("require the fixture (gw integtest init first)",
                           [&]() -> std::expected<void, std::string> {
            if (!fs::is_regular_file(fs::path(it.repoDir) / "p4gw.cfg")) {
                return std::unexpected(it.repoDir + "/p4gw.cfg not found - run "
                                       "'gw integtest init' first");
            }
            return {};
        });
        steps.emplace_back("gw init verifies the mapping and creates the repo",
                           [&] { return itGwInit(it); });
        steps.emplace_back("sync + first gw import builds p4-main",
                           [&] { return itFirstImport(it); });
        steps.emplace_back("feature branch: edit/add then delete/rename",
                           [&] { return itFeatureBranch(it); });
        steps.emplace_back("gw prepare opens the exact expected files",
                           [&] { return itPrepare(it); });
        steps.emplace_back("submit, then gw import --rebase melts the branch",
                           [&] { return itSubmitAndAbsorb(it); });
        steps.emplace_back("teammate change absorbed with import --rebase",
                           [&] { return itTeammateChange(it); });
        steps.emplace_back("opened mirror files: import reads depot, prepare "
                           "refuses",
                           [&] { return itOpenedFilesPreflight(it); });
        steps.emplace_back("doctor clean, nothing opened, no stray writes",
                           [&] { return itFinalChecks(it); });
    }

    const int rc = runSteps(steps);
    if (rc == 0) {
        std::printf("\nintegtest %s: all %zu steps passed.\n", mode.c_str(),
                    steps.size());
        if (mode == "init") {
            std::printf("Now run: gw integtest run\n");
        }
    } else {
        std::printf("\nintegtest %s failed. Rerun with --verbose for every "
                    "command and its output;\n'gw integtest init' resets the "
                    "fixture.\n", mode.c_str());
    }
    return rc;
}

}  // namespace p4gw
