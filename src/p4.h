#pragma once

#include <expected>
#include <string>
#include <vector>

#include "config.h"

namespace p4gw::p4 {

// Runs `p4 <args>` (adding `-c <client>` when the config names one) and
// returns stdout, or an error message including p4's output on failure.
std::expected<std::string, std::string> run(const Config& config,
                                            const std::vector<std::string>& args);

// ---- pure client-spec helpers (no process calls; unit-tested) ----

// One mapping line from the View: section of a client spec.
struct ViewLine {
    std::string depot;    // e.g. //depot/game/src/...
    std::string client;   // e.g. //aaron-dev/p4gw-mirror/src/...
    bool exclude = false; // leading '-'
    bool overlay = false; // leading '+'
};

// Value of a single-line field ("Root", "Client", "LineEnd", ...) in
// `p4 client -o` output; empty string if absent.
std::string specField(const std::string& spec, const std::string& field);

// Parses the View: section of `p4 client -o` output. Handles quoted paths
// and -/+ prefixes.
std::vector<ViewLine> parseClientView(const std::string& spec);

// Checks that the view maps `depotPath` to exactly `expectedClientPath`
// (both in `//.../...` form). Mapping lines that don't overlap `depotPath`
// are ignored — other custom mappings are fine — except that any line whose
// client side falls under `repoClientPrefix` (the Git repo's location, e.g.
// "//client/src/"; pass empty to skip) is an error: P4 must never write
// into the repo. Returns human-readable problems; empty means consistent.
std::vector<std::string> checkViewMapping(const std::vector<ViewLine>& view,
                                          const std::string& depotPath,
                                          const std::string& expectedClientPath,
                                          const std::string& repoClientPrefix);

// ---- wrappers over the p4 CLI ----

std::expected<std::string, std::string> info(const Config& config);

// Full `p4 client -o` output for the configured client.
std::expected<std::string, std::string> clientSpec(const Config& config);

// The client root directory, from the spec's Root: field.
std::expected<std::string, std::string> clientRoot(const Config& config);

// Creates a new numbered pending changelist with `description` and returns
// its number.
std::expected<std::string, std::string> createChangelist(
    const Config& config, const std::string& description);

// Open explicit local files in changelist `cl`. Calls are chunked so long
// file lists never overflow the platform command-line limit.
std::expected<std::string, std::string> editFiles(
    const Config& config, const std::string& cl,
    const std::vector<std::string>& files);
std::expected<std::string, std::string> addFiles(
    const Config& config, const std::string& cl,
    const std::vector<std::string>& files);
std::expected<std::string, std::string> deleteFiles(
    const Config& config, const std::string& cl,
    const std::vector<std::string>& files);

// `p4 move` (the source must already be opened for edit).
std::expected<std::string, std::string> moveFile(const Config& config,
                                                 const std::string& cl,
                                                 const std::string& from,
                                                 const std::string& to);

// `p4 reconcile -n` scoped to the configured depot path. Returns p4's
// preview output; an empty string means nothing unexpected to reconcile.
std::expected<std::string, std::string> reconcilePreview(const Config& config);

// Highest submitted changelist among the synced revisions of the subtree
// (`p4 changes -m1 -s submitted <depot_path>#have`); empty if none.
std::expected<std::string, std::string> latestSubmittedCl(const Config& config);

// `p4 opened` scoped to the configured depot path; empty if nothing is open.
std::expected<std::string, std::string> openedFiles(const Config& config);

}  // namespace p4gw::p4
