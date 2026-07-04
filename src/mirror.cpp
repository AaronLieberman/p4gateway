#include "mirror.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <unordered_set>

namespace fs = std::filesystem;

namespace p4gw::mirror {

namespace {

// On Windows, antivirus and the Search indexer transiently open files the
// moment git or p4 writes them, so a remove or overwrite that lands in that
// window fails with a sharing violation ("being used by another process").
// Most such locks clear within milliseconds, but a scanner can hold a file
// for a second or more, so the schedule escalates: a burst of quick attempts
// for the common case, then 100ms pauses, then up to 1s - about 3.5s in total
// before surfacing the error. On a genuine, persistent failure this only
// delays the error path; on Linux the op succeeds on the first attempt and
// the retries never run.
template <typename Op>
void withRetry(std::error_code& ec, Op&& op) {
    constexpr int kDelaysMs[] = {25,  25,  25,  25,  25,   25,  25,  25,
                                 100, 100, 100, 250, 500, 1000, 1000};
    size_t attempt = 0;
    for (;;) {
        ec.clear();
        op(ec);
        if (!ec) return;
        if (attempt >= std::size(kDelaysMs)) return;
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kDelaysMs[attempt++]));
    }
}

}  // namespace

bool isGwMetadataPath(const std::string& repoRelativePath) {
    return repoRelativePath == "p4gw.cfg" || repoRelativePath == ".gitignore" ||
           repoRelativePath == ".gitattributes";
}

SyncActions computeSyncActions(const std::vector<std::string>& mirrorFiles,
                               const std::vector<std::string>& trackedFiles) {
    SyncActions actions;
    actions.copies = mirrorFiles;
    std::sort(actions.copies.begin(), actions.copies.end());

    const std::unordered_set<std::string> inMirror(mirrorFiles.begin(),
                                                   mirrorFiles.end());
    for (const auto& tracked : trackedFiles) {
        if (!inMirror.contains(tracked) && !isGwMetadataPath(tracked)) {
            actions.deletes.push_back(tracked);
        }
    }
    std::sort(actions.deletes.begin(), actions.deletes.end());
    return actions;
}

ImportPlan planImport(const SyncActions& base,
                      const std::vector<OpenedMirrorFile>& opened) {
    std::unordered_set<std::string> openedAll;
    std::unordered_set<std::string> depotReads;
    for (const auto& o : opened) {
        openedAll.insert(o.path);
        if (o.hasDepotRev) depotReads.insert(o.path);
    }

    ImportPlan plan;
    // Opened files are never copied straight from the mirror.
    for (const auto& copy : base.copies) {
        if (!openedAll.contains(copy)) plan.actions.copies.push_back(copy);
    }
    // A file open for delete/move-delete still exists in the depot, so it must
    // not be removed from the baseline; we restore the head revision instead.
    for (const auto& del : base.deletes) {
        if (!depotReads.contains(del)) plan.actions.deletes.push_back(del);
    }
    plan.depotReads.assign(depotReads.begin(), depotReads.end());
    std::sort(plan.depotReads.begin(), plan.depotReads.end());
    return plan;
}

std::expected<std::vector<std::string>, std::string> listFiles(
    const std::string& dir) {
    std::vector<std::string> files;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec)) {
            // lexically_relative is pure string work. fs::relative would
            // weakly_canonical both paths - a filesystem hit per file (opening
            // each one on Windows) - which dominates the walk on a big mirror.
            // The iterator yields paths already rooted at `dir`, so a lexical
            // strip is correct and far cheaper.
            files.push_back(it->path().lexically_relative(dir).generic_string());
        }
    }
    if (ec) {
        return std::unexpected("failed to list files under " + dir + ": " +
                               ec.message());
    }
    return files;
}

bool copyNeeded(std::uintmax_t srcSize, fs::file_time_type srcMtime,
                bool dstExists, std::uintmax_t dstSize,
                fs::file_time_type dstMtime) {
    if (!dstExists) return true;
    return srcSize != dstSize || srcMtime != dstMtime;
}

std::expected<std::size_t, std::string> applySyncActions(
    const SyncActions& actions, const std::string& mirrorDir,
    const std::string& worktreeDir, bool trustStats) {
    std::error_code ec;
    std::size_t copied = 0;
    for (const auto& rel : actions.deletes) {
        const fs::path target = fs::path(worktreeDir) / rel;
        withRetry(ec, [&](std::error_code& e) { fs::remove(target, e); });
        if (ec) {
            return std::unexpected("failed to delete " + target.string() +
                                   ": " + ec.message());
        }
    }
    for (const auto& rel : actions.copies) {
        const fs::path source = fs::path(mirrorDir) / rel;
        const fs::path target = fs::path(worktreeDir) / rel;

        // Skip files whose working-tree copy already matches the mirror in size
        // and mtime; import stamps the mirror mtime onto the copy below so an
        // untouched file stays detectable as unchanged on the next run. Any
        // stat error just falls through to a normal copy. In full-copy mode
        // (`trustStats` false) the stamps are suspect - a torn import may have
        // left them on files whose content was since restored by `git reset` -
        // so nothing is skipped.
        if (trustStats) {
            const auto srcSize = fs::file_size(source, ec);
            const auto srcMtime = fs::last_write_time(source, ec);
            if (!ec) {
                std::error_code dstEc;
                const bool dstExists = fs::exists(target, dstEc);
                const auto dstSize =
                    dstExists ? fs::file_size(target, dstEc) : std::uintmax_t{0};
                const auto dstMtime = dstExists
                                          ? fs::last_write_time(target, dstEc)
                                          : fs::file_time_type{};
                if (!dstEc && !copyNeeded(srcSize, srcMtime, dstExists, dstSize,
                                          dstMtime)) {
                    continue;
                }
            }
            ec.clear();
        }

        fs::create_directories(target.parent_path(), ec);
        if (ec) {
            return std::unexpected("failed to create directory " +
                                   target.parent_path().string() + ": " +
                                   ec.message());
        }
        // An existing read-only target (e.g. copied from the mirror by an
        // earlier import) would make copy_file fail; make it writable first.
        if (fs::exists(target, ec)) {
            fs::permissions(target, fs::perms::owner_write,
                            fs::perm_options::add, ec);
        }
        withRetry(ec, [&](std::error_code& e) {
            fs::copy_file(source, target, fs::copy_options::overwrite_existing,
                          e);
        });
        if (ec) {
            return std::unexpected("failed to copy " + source.string() +
                                   " to " + target.string() + ": " +
                                   ec.message());
        }
        // Mirror files are typically read-only (p4 owns them); the working
        // tree copy must be editable.
        fs::permissions(target, fs::perms::owner_write, fs::perm_options::add,
                        ec);
        // Stamp the mirror's mtime onto the copy so the next import can skip it
        // when the mirror file is unchanged (see copyNeeded). Done last:
        // permission changes don't touch mtime, but the copy itself sets a
        // fresh one. Re-read the source rather than trusting the value gathered
        // for the skip check, which may not have been read if that stat failed.
        const auto stampMtime = fs::last_write_time(source, ec);
        if (ec) {
            return std::unexpected("failed to read mtime of " + source.string() +
                                   ": " + ec.message());
        }
        withRetry(ec, [&](std::error_code& e) {
            fs::last_write_time(target, stampMtime, e);
        });
        if (ec) {
            return std::unexpected("failed to set mtime on " + target.string() +
                                   ": " + ec.message());
        }
        ++copied;  // reached only when the file was actually (re)copied
    }
    return copied;
}

bool filesIdentical(const fs::path& a, const fs::path& b) {
    std::ifstream fileA(a, std::ios::binary);
    std::ifstream fileB(b, std::ios::binary);
    if (!fileA || !fileB) return false;
    constexpr std::streamsize kChunk = 64 * 1024;
    std::vector<char> bufA(kChunk);
    std::vector<char> bufB(kChunk);
    for (;;) {
        fileA.read(bufA.data(), kChunk);
        fileB.read(bufB.data(), kChunk);
        const std::streamsize gotA = fileA.gcount();
        const std::streamsize gotB = fileB.gcount();
        if (gotA != gotB) return false;
        if (gotA == 0) return true;
        if (std::memcmp(bufA.data(), bufB.data(),
                        static_cast<size_t>(gotA)) != 0) {
            return false;
        }
        if (fileA.eof() || fileB.eof()) return fileA.eof() == fileB.eof();
    }
}

std::vector<std::string> findStaleFastPathFiles(
    const std::vector<std::string>& files, const std::string& mirrorDir,
    const std::string& worktreeDir) {
    std::vector<std::string> stale;
    for (const auto& rel : files) {
        const fs::path source = fs::path(mirrorDir) / rel;
        const fs::path target = fs::path(worktreeDir) / rel;
        std::error_code ec;
        if (!fs::exists(target, ec) || ec) continue;  // import would copy it
        const auto srcSize = fs::file_size(source, ec);
        if (ec) continue;
        const auto srcMtime = fs::last_write_time(source, ec);
        if (ec) continue;
        const auto dstSize = fs::file_size(target, ec);
        if (ec) continue;
        const auto dstMtime = fs::last_write_time(target, ec);
        if (ec) continue;
        // Only files the fast path would skip can hide stale content; a file
        // it would recopy anyway is self-healing and not worth flagging.
        if (copyNeeded(srcSize, srcMtime, /*dstExists=*/true, dstSize,
                       dstMtime)) {
            continue;
        }
        if (!filesIdentical(source, target)) stale.push_back(rel);
    }
    return stale;
}

std::string importPendingMarkerPath(const std::string& gitDir) {
    return (fs::path(gitDir) / "p4gw-import-pending").string();
}

std::string snapshotWorktreePath(const std::string& gitDir) {
    return (fs::path(gitDir) / "p4gw" / "worktree").string();
}

}  // namespace p4gw::mirror
