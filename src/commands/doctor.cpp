// SPDX-License-Identifier: MIT

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iterator>
#include <string>
#include <unordered_set>

#include "commands.h"
#include "config.h"
#include "git.h"
#include "mirror.h"
#include "p4.h"
#include "subprocess.h"

namespace fs = std::filesystem;

namespace p4gw {

namespace {

constexpr const char* kDoctorUsage =
    "usage: gw doctor [options]\n"
    "\n"
    "Check that the environment is sane for the mirror workflow: git and p4 on\n"
    "PATH, a readable p4gw.cfg, the P4 connection, repo-directory ownership, and\n"
    "- the central check - that the client view maps each configured depot path\n"
    "into the mirror and nothing into the Git repo outside it. Reports ok/WARN/\n"
    "FAIL per check and exits non-zero if any check failed.\n"
    "\n"
    "options:\n"
    "      --verify  Also compare every mirror file byte-for-byte against its\n"
    "                working-tree copy and flag files whose content differs\n"
    "                while size+mtime match - the files 'gw import' would\n"
    "                wrongly skip (fix with 'gw import --full'). Reads the\n"
    "                whole mirror; can take a while on a big subtree.\n"
    "  -h, --help    Show this help\n"
    "\n";

}  // namespace

// Checks that the environment is sane for the mirror workflow. The central
// check is the client view: the configured depot path must map into the
// mirror, and nothing may map into the Git repo directory - if the spec
// ever loses the remap line, this is where it gets caught.
int cmdDoctor(const Args& args) {
    bool verify = false;
    for (const auto& arg : args) {
        if (arg == "--help" || arg == "-h") {
            std::printf("%s", kDoctorUsage);
            return 0;
        }
        if (arg == "--verify") {
            verify = true;
            continue;
        }
        std::fprintf(stderr, "gw doctor: unexpected argument '%s'\n",
                     arg.c_str());
        std::fprintf(stderr, "%s", kDoctorUsage);
        return 1;
    }

    int failures = 0;
    int warnings = 0;

    auto gitVersion = run("git", {"--version"});
    if (gitVersion && gitVersion->exitCode == 0) {
        std::printf("ok    git found: %s", gitVersion->stdoutText.c_str());
    } else {
        std::printf("FAIL  git not found on PATH\n");
        ++failures;
    }

    const bool p4Found = [] {
        auto p4Version = run("p4", {"-V"});
        return p4Version && p4Version->exitCode == 0;
    }();
    if (p4Found) {
        std::printf("ok    p4 found\n");
    } else {
        std::printf("FAIL  p4 not found on PATH\n");
        ++failures;
    }

    std::string root;
    auto config = findAndLoadConfig(root);
    if (!config) {
        std::printf("note  no p4gw.cfg config found - run 'gw init' to set up "
                    "an overlay; skipping the workspace checks\n");
        if (failures == 0) {
            std::printf("\nAll checks passed.\n");
        } else {
            std::printf("\n%d check(s) failed.\n", failures);
        }
        return failures == 0 ? 0 : 1;
    }
    std::printf("ok    p4gw.cfg found at %s (%zu include(s))\n", root.c_str(),
                includeRules(config->rules).size());
    for (const auto& rule : config->rules) {
        if (rule.exclude) {
            std::printf("        excluded (in place / dropped, gitignored): "
                        "%s\n", rule.depotPath.c_str());
        } else {
            std::printf("      %s -> %s\n", rule.depotPath.c_str(),
                        rule.mirrorPath.c_str());
        }
    }

    for (const auto* rule : includeRules(config->rules)) {
        const std::string mirrorDir =
            resolveMirrorPath(rule->mirrorPath, root);
        if (fs::exists(mirrorDir)) {
            std::printf("ok    mirror directory exists: %s\n",
                        mirrorDir.c_str());
        } else {
            std::printf("WARN  mirror directory %s does not exist yet - it "
                        "appears on the first sync after the view line is "
                        "added\n", mirrorDir.c_str());
            ++warnings;
        }
    }

    // A torn import (crash, kill, a file locked past the retries) leaves a
    // pending marker in the git dir; while it exists, import distrusts its
    // size+mtime fast path and recopies everything. Surface it here so a
    // weird working-tree state has a visible cause. No git repo yet (doctor
    // runs before 'gw init' too) simply means no marker to check.
    if (auto gitDirPath = git::gitDir(root)) {
        const std::string marker =
            mirror::importPendingMarkerPath(*gitDirPath);
        if (fs::exists(marker)) {
            std::printf("WARN  an earlier 'gw import' was interrupted (%s "
                        "exists) - the working tree may not match the mirror; "
                        "rerun 'gw import'\n      (it will recopy every mirror "
                        "file instead of trusting sizes and timestamps)\n",
                        marker.c_str());
            ++warnings;
        } else {
            std::printf("ok    no interrupted import pending\n");
        }
    }

    // The have manifest is a pure cache (a stale or missing one only costs
    // the next import a full mirror walk), so this is informational either
    // way - but it tells the user whether imports are taking the fast path.
    if (auto gitDirPath = git::gitDir(root)) {
        const std::string depotRef = depotTrackingRef(*config);
        const std::string manifestPath =
            mirror::haveManifestPath(*gitDirPath, config->baselineBranch);
        auto depotTip = git::revParse(depotRef, root);
        std::ifstream manifestFile(manifestPath, std::ios::binary);
        if (!depotTip || !manifestFile) {
            std::printf("note  no have manifest yet - the next 'gw import' "
                        "does a full mirror walk and writes one\n");
        } else {
            std::ostringstream text;
            text << manifestFile.rdbuf();
            std::string snapshot;
            const auto entries =
                mirror::parseHaveManifest(std::move(text).str(), snapshot);
            if (snapshot == *depotTip) {
                std::printf("ok    have manifest bound to the depot baseline "
                            "(%zu file(s); imports take the fast path)\n",
                            entries.size());
            } else {
                std::printf("note  have manifest is stale - the next "
                            "'gw import' falls back to a full mirror walk and "
                            "rewrites it\n");
            }
        }
    }

    // Worktree import mode: the hidden snapshot worktree should exist, be
    // registered, and sit detached at the depot baseline. A broken or stale
    // one just means the next import recreates/resets it, so these degrade to
    // notes and warnings, never failures.
    if (config->importMode == ImportMode::kWorktree) {
        if (auto gitDirPath = git::gitDir(root)) {
            const std::string depotRef = depotTrackingRef(*config);
            const std::string wtPath =
                mirror::snapshotWorktreePath(*gitDirPath);
            auto depotTip = git::revParse(depotRef, root);
            if (!depotTip) {
                std::printf("note  import worktree mode configured; the first "
                            "import runs in your checkout\n");
            } else if (!fs::exists(wtPath)) {
                std::printf("note  snapshot worktree not created yet - the next "
                            "'gw import' creates it\n");
            } else {
                auto wtHead = git::revParse("HEAD", wtPath);
                auto wtDirty = git::isDirty(wtPath);
                if (!wtHead) {
                    std::printf("WARN  snapshot worktree at %s is broken "
                                "(deleted registration or a moved repo) - the "
                                "next 'gw import' recreates it and recopies "
                                "every mirror file\n",
                                wtPath.c_str());
                    ++warnings;
                } else if (*wtHead != *depotTip || (wtDirty && *wtDirty)) {
                    std::printf("WARN  snapshot worktree at %s is stale or "
                                "modified - the next 'gw import' resets it and "
                                "recopies every mirror file\n",
                                wtPath.c_str());
                    ++warnings;
                } else {
                    std::printf("ok    snapshot worktree healthy (detached at "
                                "%s)\n",
                                depotRef.c_str());
                }
            }
        }
    }

    // Repo-directory ownership. gw works through the git CLI, which tolerates a
    // repo owned by another user; libgit2-based tools (git-branchless) do not -
    // they crash with a "not owned by current user" error that is easy to
    // misattribute. Flag it here so it is not a mystery.
    auto owned = isOwnedByCurrentUser(root);
    if (!owned) {
        std::printf("WARN  could not check ownership of %s: %s\n", root.c_str(),
                    owned.error().c_str());
        ++warnings;
    } else if (!*owned) {
        const std::string gitPath = fs::path(root).generic_string();
        std::printf("WARN  %s is not owned by the current user - gw is fine, "
                    "but libgit2 tools like git-branchless will refuse to open "
                    "it.\n      Fix with: git config --global --add "
                    "safe.directory %s\n      (or take ownership of the "
                    "directory).\n",
                    root.c_str(), gitPath.c_str());
        ++warnings;
    } else {
        std::printf("ok    repo directory is owned by the current user\n");
    }

    // Line-ending policy. A committed .gitattributes that pins EOL for every
    // path makes git store the mirror's bytes deterministically, independent of
    // each machine's core.autocrlf - the reliable cure for the "every line
    // changed / rebase conflicts on CRLF vs LF" footgun. Check it up front; the
    // LineEnd/autocrlf check below defers to it when it is present.
    bool eolPinned = false;
    {
        const fs::path gitattributes = fs::path(root) / ".gitattributes";
        if (fs::exists(gitattributes)) {
            std::ifstream in(gitattributes);
            std::string content((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
            eolPinned = gitattributesPinsEol(content);
        }
        if (eolPinned) {
            std::printf("ok    .gitattributes pins line endings for all paths\n");
        } else {
            std::printf("WARN  no .gitattributes pinning line endings - git "
                        "will follow core.autocrlf, which can drift between "
                        "commits and\n      make 'gw import --rebase' conflict "
                        "on CRLF/LF. Add '* -text' (or rerun 'gw init')\n");
            ++warnings;
        }
    }

    // Allowlist coverage. The .gitignore gw writes is an allowlist that
    // re-includes exactly the mapped subtrees; everything else stays ignored.
    // An `include` added to p4gw.cfg without its `!/<subtree>/` re-include
    // leaves that subtree ignored, so 'gw import' copies the mirror in but
    // 'git add' drops it and the baseline ships nothing through it - silently,
    // with no error anywhere. Flag any mapped subtree the allowlist misses.
    // Only meaningful for the allowlist style (a hand-kept denylist tracks by
    // default); an absent/empty .gitignore is simply not an allowlist yet.
    {
        auto readAll = [](const fs::path& p) -> std::string {
            std::ifstream in(p, std::ios::binary);
            if (!in) return {};
            std::string content((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
            return content;
        };
        const std::string gitignore = readAll(fs::path(root) / ".gitignore");
        if (gitignoreIsAllowlist(gitignore)) {
            const auto missing =
                missingAllowlistTrackingLines(config->rules, gitignore);
            if (missing.empty()) {
                std::printf("ok    .gitignore allowlist tracks every mapped "
                            "subtree\n");
            } else {
                std::printf("FAIL  .gitignore allowlist does not track %zu "
                            "mapped subtree(s) - 'gw import' ships nothing "
                            "through them.\n      Add these line(s) to "
                            ".gitignore (or rerun 'gw init'):\n",
                            missing.size());
                for (const auto& line : missing) {
                    std::printf("        %s\n", line.c_str());
                }
                ++failures;
            }
        }
    }

    // Ripgrep blind spot. rg honors .gitignore by default, and the allowlist
    // .gitignore gw writes ignores everything unmapped - so unmapped depot
    // content synced in place (bin/, content/) silently vanishes from every
    // rg search. Covered when the managed .rgignore block (or a hand-rolled
    // root reopen) is present, or when a RIPGREP_CONFIG_PATH config passes
    // --no-ignore-vcs. A shell alias or wrapper is invisible to this check,
    // so `rgignore = off` in p4gw.cfg silences it. Only relevant when rg is
    // installed and the repo's .gitignore really is the allowlist style.
    if (config->manageRgignore) {
        auto readAll = [](const fs::path& p) -> std::string {
            std::ifstream in(p, std::ios::binary);
            if (!in) return {};
            std::string content((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
            return content;
        };
        const bool allowlist =
            gitignoreIsAllowlist(readAll(fs::path(root) / ".gitignore"));
        const bool rgFound = [&] {
            if (!allowlist) return false;  // don't probe for rg needlessly
            auto rgVersion = run("rg", {"--version"});
            return rgVersion && rgVersion->exitCode == 0;
        }();
        if (allowlist && rgFound) {
            bool covered =
                rgignoreReopensRoot(readAll(fs::path(root) / ".rgignore"));
            if (!covered) {
                const std::string rcPath = envValue("RIPGREP_CONFIG_PATH");
                if (!rcPath.empty()) {
                    covered = ripgrepConfigDisablesVcsIgnore(readAll(rcPath));
                }
            }
            if (covered) {
                std::printf("ok    ripgrep reopens what the allowlist "
                            ".gitignore hides (unmapped depot content stays "
                            "searchable)\n");
            } else {
                std::printf(
                    "WARN  ripgrep is installed but nothing reopens what the "
                    "allowlist .gitignore\n      hides - unmapped depot "
                    "content synced in place (bin/, content/) is\n      "
                    "invisible to rg searches. Run 'gw import' (or 'gw init') "
                    "to write the\n      managed .rgignore block, or put "
                    "'--no-ignore-vcs' in a config file that\n      "
                    "RIPGREP_CONFIG_PATH points at. Handled another way (a "
                    "shell alias), or\n      unwanted? Set 'rgignore = off' "
                    "in p4gw.cfg.\n");
                ++warnings;
            }
        }
    }

    if (p4Found) {
        auto info = p4::info(*config);
        if (info) {
            std::printf("ok    p4 connection works\n");
        } else {
            std::printf("FAIL  p4 connection: %s\n", info.error().c_str());
            ++failures;
        }

        std::expected<std::string, std::string> spec =
            std::unexpected(std::string("skipped: no p4 connection"));
        if (info) {
            spec = p4::clientSpec(*config);
            if (!spec) {
                std::printf("FAIL  cannot read client spec: %s\n",
                            spec.error().c_str());
                ++failures;
            }
        }
        if (spec) {
            const std::string lineEnd = p4::specField(*spec, "LineEnd");

            const auto includes = includeRules(config->rules);
            const auto allExcludes = excludeDepotPaths(config->rules);
            for (const auto* rule : includes) {
                const std::string mirrorDir =
                    resolveMirrorPath(rule->mirrorPath, root);
                std::vector<std::string> otherMirrors;
                for (const auto* other : includes) {
                    if (other != rule) {
                        otherMirrors.push_back(
                            resolveMirrorPath(other->mirrorPath, root));
                    }
                }
                const auto problems = p4::checkSpecMapping(
                    *spec, rule->depotPath, root, mirrorDir, allExcludes,
                    otherMirrors);
                if (problems.empty()) {
                    std::printf("ok    client view maps %s to the mirror\n",
                                rule->depotPath.c_str());
                }
                for (const auto& problem : problems) {
                    std::printf("FAIL  %s\n", problem.message.c_str());
                    ++failures;
                }
            }

            // Line endings: the mirror is written with the client LineEnd
            // and committed by git; mismatched translation here is the
            // classic "every line changed" footgun. A .gitattributes that pins
            // EOL overrides core.autocrlf entirely, so when one is present the
            // autocrlf setting is moot - report that and skip the comparison.
            auto autocrlf = git::configValue("core.autocrlf", root);
            const std::string crlf =
                autocrlf && !autocrlf->empty() ? *autocrlf : "false";
            const bool winLineEnd = lineEnd == "win" || lineEnd == "local";
            if (eolPinned) {
                std::printf("ok    line endings pinned by .gitattributes "
                            "(core.autocrlf=%s is not consulted)\n",
                            crlf.c_str());
            } else if (winLineEnd && crlf != "true") {
                std::printf("WARN  client LineEnd '%s' with core.autocrlf=%s "
                            "- git will commit CRLF; consider autocrlf=true "
                            "or LineEnd unix\n",
                            lineEnd.c_str(), crlf.c_str());
                ++warnings;
            } else if (!winLineEnd && crlf == "true") {
                std::printf("WARN  client LineEnd '%s' with core.autocrlf="
                            "true - git will rewrite line endings the depot "
                            "doesn't have; consider autocrlf=input/false\n",
                            lineEnd.c_str());
                ++warnings;
            } else {
                std::printf("ok    LineEnd '%s' and core.autocrlf=%s agree\n",
                            lineEnd.empty() ? "(default)" : lineEnd.c_str(),
                            crlf.c_str());
            }
        }

        auto opened = p4::openedFiles(*config);
        if (opened && opened->empty()) {
            std::printf("ok    no files opened under the configured mappings\n");
        } else if (opened) {
            std::printf("WARN  files are opened under the configured mappings - "
                        "a pending gw changelist, or someone ran p4 edit:\n%s",
                        opened->c_str());
            ++warnings;
        } else {
            std::printf("WARN  could not query opened files: %s\n",
                        opened.error().c_str());
            ++warnings;
        }

        // --verify: byte-compare every p4-tracked mirror file against its
        // working-tree copy, flagging only files import's size+mtime fast path
        // would *skip* while their bytes differ. Legitimate divergence (a
        // branch edit, a fresh sync, an opened file's depot-head restore)
        // rewrites mtime or size and is never flagged, so a hit means the
        // stamped stats lie - a torn import cleaned up by hand - and the next
        // plain import would silently keep stale content.
        if (verify) {
            // The stamped copies live wherever import stages them: the user's
            // checkout (checkout mode) or the hidden snapshot worktree
            // (worktree mode). Compare against that base.
            std::string compareBase = root;
            bool skipVerify = false;
            if (config->importMode == ImportMode::kWorktree) {
                auto gitDirPath = git::gitDir(root);
                if (gitDirPath) {
                    const std::string wtPath =
                        mirror::snapshotWorktreePath(*gitDirPath);
                    if (fs::exists(wtPath)) {
                        compareBase = wtPath;
                    } else {
                        std::printf("note  --verify: snapshot worktree not "
                                    "created yet - nothing to compare\n");
                        skipVerify = true;
                    }
                }
            }
            for (const auto* rule : includeRules(config->rules)) {
                if (skipVerify) break;
                const std::string mirrorDir =
                    resolveMirrorPath(rule->mirrorPath, root);
                if (!fs::exists(mirrorDir)) continue;  // warned above
                auto mirrorFiles = mirror::listFiles(mirrorDir);
                if (!mirrorFiles) {
                    std::printf("FAIL  --verify: %s\n",
                                mirrorFiles.error().c_str());
                    ++failures;
                    continue;
                }
                // Same stray filter as import: only files p4 reports as synced
                // ever get copied, so only those can hide a stale skip.
                auto haveDepot = p4::haveFiles(*config, rule->depotPath);
                if (!haveDepot) {
                    std::printf("FAIL  --verify: %s\n",
                                haveDepot.error().c_str());
                    ++failures;
                    continue;
                }
                std::unordered_set<std::string> haveRel;
                for (const auto& entry : *haveDepot) {
                    std::string rel =
                        p4::depotRelativePath(rule->depotPath, entry.depotFile);
                    if (!rel.empty()) haveRel.insert(std::move(rel));
                }
                std::vector<std::string> mirrorTracked;
                for (auto& path : *mirrorFiles) {
                    if (haveRel.contains(path)) {
                        mirrorTracked.push_back(std::move(path));
                    }
                }
                const std::string worktreeDir =
                    rule->repoSubtree.empty()
                        ? compareBase
                        : (fs::path(compareBase) / rule->repoSubtree).string();
                const auto stale = mirror::findStaleFastPathFiles(
                    mirrorTracked, mirrorDir, worktreeDir);
                if (stale.empty()) {
                    std::printf("ok    verified %zu mirror file(s) against the "
                                "working tree for %s\n",
                                mirrorTracked.size(), rule->depotPath.c_str());
                    continue;
                }
                std::printf("FAIL  %zu file(s) under %s differ from the mirror "
                            "while matching its size+mtime - 'gw import' would "
                            "wrongly skip them; run 'gw import --full':\n",
                            stale.size(), rule->depotPath.c_str());
                constexpr size_t kMaxListed = 20;
                for (size_t i = 0; i < stale.size() && i < kMaxListed; ++i) {
                    std::printf("        %s%s%s\n",
                                rule->repoSubtree.empty()
                                    ? ""
                                    : rule->repoSubtree.c_str(),
                                rule->repoSubtree.empty() ? "" : "/",
                                stale[i].c_str());
                }
                if (stale.size() > kMaxListed) {
                    std::printf("        ... and %zu more\n",
                                stale.size() - kMaxListed);
                }
                ++failures;
            }
        }
    } else {
        std::printf("note  skipping P4 connection, client view, and opened-"
                    "file checks (no p4)\n");
        if (verify) {
            std::printf("note  skipping --verify (needs p4 to tell tracked "
                        "mirror files from strays)\n");
        }
    }

    if (failures == 0 && warnings == 0) {
        std::printf("\nAll checks passed.\n");
    } else {
        std::printf("\n%d check(s) failed, %d warning(s).\n", failures,
                    warnings);
    }
    return failures == 0 ? 0 : 1;
}

}  // namespace p4gw