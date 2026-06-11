#include "p4.h"

#include "process.h"

namespace p4gw::p4 {

std::expected<std::string, std::string> run(const Config& config,
                                            const std::vector<std::string>& args) {
    std::vector<std::string> fullArgs;
    if (!config.client.empty()) {
        fullArgs.push_back("-c");
        fullArgs.push_back(config.client);
    }
    fullArgs.insert(fullArgs.end(), args.begin(), args.end());

    auto result = p4gw::run("p4", fullArgs);
    if (!result) {
        return std::unexpected(result.error());
    }
    if (result->exitCode != 0) {
        std::string cmd = "p4";
        for (const auto& arg : fullArgs) cmd += ' ' + arg;
        return std::unexpected(cmd + " failed:\n" + result->output);
    }
    return result->output;
}

std::expected<std::string, std::string> clientRoot(const Config& config) {
    // TODO(M1): parse `p4 -Ztag client -o` output for the Root field.
    (void)config;
    return std::unexpected("not implemented yet (see PLAN.md milestone M1)");
}

std::expected<std::string, std::string> createChangelist(
    const Config& config, const std::string& description) {
    // TODO(M4): pipe a changelist spec into `p4 change -i` and parse the
    // "Change NNN created." response.
    (void)config;
    (void)description;
    return std::unexpected("not implemented yet (see PLAN.md milestone M4)");
}

std::expected<std::string, std::string> reconcile(const Config& config,
                                                  const std::string& changelist) {
    // TODO(M4): `p4 reconcile -c <changelist> <depotPath>`, tolerating the
    // "no file(s) to reconcile" exit status.
    (void)config;
    (void)changelist;
    return std::unexpected("not implemented yet (see PLAN.md milestone M4)");
}

}  // namespace p4gw::p4
