#include "mirror.h"

#include <algorithm>
#include <filesystem>
#include <unordered_set>

namespace fs = std::filesystem;

namespace p4gw::mirror {

bool isGwMetadataPath(const std::string& repoRelativePath) {
    return repoRelativePath == ".p4gw" || repoRelativePath == ".gitignore";
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

std::expected<std::vector<std::string>, std::string> listFiles(
    const std::string& dir) {
    std::vector<std::string> files;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec)) {
            files.push_back(fs::relative(it->path(), dir, ec).generic_string());
        }
    }
    if (ec) {
        return std::unexpected("failed to list files under " + dir + ": " +
                               ec.message());
    }
    return files;
}

std::expected<void, std::string> applySyncActions(const SyncActions& actions,
                                                  const std::string& mirrorDir,
                                                  const std::string& worktreeDir) {
    std::error_code ec;
    for (const auto& rel : actions.deletes) {
        const fs::path target = fs::path(worktreeDir) / rel;
        fs::remove(target, ec);
        if (ec) {
            return std::unexpected("failed to delete " + target.string() +
                                   ": " + ec.message());
        }
    }
    for (const auto& rel : actions.copies) {
        const fs::path source = fs::path(mirrorDir) / rel;
        const fs::path target = fs::path(worktreeDir) / rel;
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
        fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            return std::unexpected("failed to copy " + source.string() +
                                   " to " + target.string() + ": " +
                                   ec.message());
        }
        // Mirror files are typically read-only (p4 owns them); the working
        // tree copy must be editable.
        fs::permissions(target, fs::perms::owner_write, fs::perm_options::add,
                        ec);
    }
    return {};
}

}  // namespace p4gw::mirror
