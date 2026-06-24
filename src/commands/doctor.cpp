#include <cstdio>
#include <filesystem>
#include <string>

#include "commands.h"
#include "config.h"
#include "git.h"
#include "p4.h"
#include "process.h"

namespace fs = std::filesystem;

namespace p4gw {

// Checks that the environment is sane for the mirror workflow. The central
// check is the client view: the configured depot path must map into the
// mirror, and nothing may map into the Git repo directory - if the spec
// ever loses the remap line, this is where it gets caught.
int cmdDoctor(const Args& args) {
    (void)args;

    int failures = 0;
    int warnings = 0;

    auto gitVersion = run("git", {"--version"});
    if (gitVersion && gitVersion->exitCode == 0) {
        std::printf("ok    git found: %s", gitVersion->output.c_str());
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
                config->mappings.size());
    for (const auto& mapping : config->mappings) {
        std::printf("      %s -> %s\n", mapping.depotPath.c_str(),
                    mapping.mirrorPath.c_str());
        for (const auto& ex : mapping.excludedDepotPaths) {
            std::printf("        excluded (in place, gitignored): %s\n",
                        ex.c_str());
        }
    }

    for (const auto& mapping : config->mappings) {
        const std::string mirrorDir =
            resolveMirrorPath(mapping.mirrorPath, root);
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

            for (const auto& mapping : config->mappings) {
                const std::string mirrorDir =
                    resolveMirrorPath(mapping.mirrorPath, root);
                const auto problems = p4::checkSpecMapping(
                    *spec, mapping.depotPath, root, mirrorDir,
                    mapping.excludedDepotPaths);
                if (problems.empty()) {
                    std::printf("ok    client view maps %s to the mirror\n",
                                mapping.depotPath.c_str());
                }
                for (const auto& problem : problems) {
                    std::printf("FAIL  %s\n", problem.message.c_str());
                    ++failures;
                }
            }

            // Line endings: the mirror is written with the client LineEnd
            // and committed by git; mismatched translation here is the
            // classic "every line changed" footgun.
            auto autocrlf = git::configValue("core.autocrlf", root);
            const std::string crlf =
                autocrlf && !autocrlf->empty() ? *autocrlf : "false";
            const bool winLineEnd = lineEnd == "win" || lineEnd == "local";
            if (winLineEnd && crlf != "true") {
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
    } else {
        std::printf("note  skipping P4 connection, client view, and opened-"
                    "file checks (no p4)\n");
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
