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

// Returns the p4 client root directory for the configured client.
std::expected<std::string, std::string> clientRoot(const Config& config);

// Creates a new numbered pending changelist with `description` and returns
// its number.
std::expected<std::string, std::string> createChangelist(
    const Config& config, const std::string& description);

// Runs `p4 reconcile -c <changelist>` scoped to the configured depot path,
// opening adds/edits/deletes for everything that differs from the depot.
std::expected<std::string, std::string> reconcile(const Config& config,
                                                  const std::string& changelist);

}  // namespace p4gw::p4
