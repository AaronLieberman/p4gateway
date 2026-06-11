#include "p4ops.h"

#include "mirror.h"

namespace p4gw {

std::expected<std::vector<P4Op>, std::string> planP4Operations(
    const std::vector<git::FileChange>& changes) {
    std::vector<P4Op> deletes;
    std::vector<P4Op> edits;
    std::vector<P4Op> moves;
    std::vector<P4Op> adds;

    for (const auto& change : changes) {
        if (mirror::isGwMetadataPath(change.path)) continue;

        switch (change.status) {
        case 'M':  // modified
        case 'T':  // type change (e.g. file <-> symlink); content rewrite
            edits.push_back({P4Op::Kind::Edit, change.path, {}});
            break;
        case 'A':
            adds.push_back({P4Op::Kind::Add, change.path, {}});
            break;
        case 'D':
            deletes.push_back({P4Op::Kind::Delete, change.path, {}});
            break;
        case 'R':
            if (change.newPath.empty()) {
                return std::unexpected("rename without destination: " +
                                       change.path);
            }
            moves.push_back({P4Op::Kind::Move, change.path, change.newPath});
            break;
        case 'C':  // copied: the source is untouched, so this is a plain add
            if (change.newPath.empty()) {
                return std::unexpected("copy without destination: " +
                                       change.path);
            }
            adds.push_back({P4Op::Kind::Add, change.newPath, {}});
            break;
        default:
            return std::unexpected(std::string("unsupported git change '") +
                                   change.status + "' for " + change.path);
        }
    }

    // Deletes first so renames into a freed path see it gone; adds last so
    // nothing collides with a pending move destination.
    std::vector<P4Op> ops;
    ops.reserve(deletes.size() + edits.size() + moves.size() + adds.size());
    auto append = [&ops](std::vector<P4Op>& group) {
        for (auto& op : group) ops.push_back(std::move(op));
    };
    append(deletes);
    append(edits);
    append(moves);
    append(adds);
    return ops;
}

}  // namespace p4gw
