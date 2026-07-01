#include "mirror.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>
#include <unordered_set>

namespace fs = std::filesystem;

namespace p4gw::mirror {

namespace {

// On Windows, antivirus and the Search indexer transiently open files the
// moment git or p4 writes them, so a remove or overwrite that lands in that
// window fails with a sharing violation ("being used by another process")
// that clears within milliseconds. Retry the filesystem op a handful of times
// with a short backoff before surfacing the error. On a genuine, persistent
// failure this only adds a fraction of a second to the error path; on Linux
// the op succeeds on the first attempt and the retries never run.
template <typename Op>
void withRetry(std::error_code& ec, Op&& op) {
    constexpr int kMaxAttempts = 20;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        ec.clear();
        op(ec);
        if (!ec) return;
        if (attempt + 1 < kMaxAttempts) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
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
    const std::string& worktreeDir) {
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
        // stat error just falls through to a normal copy.
        const auto srcSize = fs::file_size(source, ec);
        const auto srcMtime = fs::last_write_time(source, ec);
        if (!ec) {
            std::error_code dstEc;
            const bool dstExists = fs::exists(target, dstEc);
            const auto dstSize =
                dstExists ? fs::file_size(target, dstEc) : std::uintmax_t{0};
            const auto dstMtime = dstExists ? fs::last_write_time(target, dstEc)
                                            : fs::file_time_type{};
            if (!dstEc && !copyNeeded(srcSize, srcMtime, dstExists, dstSize,
                                      dstMtime)) {
                continue;
            }
        }
        ec.clear();

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
        fs::last_write_time(target, stampMtime, ec);
        if (ec) {
            return std::unexpected("failed to set mtime on " + target.string() +
                                   ": " + ec.message());
        }
        ++copied;  // reached only when the file was actually (re)copied
    }
    return copied;
}

}  // namespace p4gw::mirror
