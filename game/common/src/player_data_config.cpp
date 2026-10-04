#include "realmmesh/game/common/player_data_config.hpp"

#include <stdexcept>

namespace realm::game::common {
namespace {

void resolve(std::filesystem::path& path, const std::filesystem::path& root) {
    if (!path.empty() && path.is_relative()) path = root / path;
}

}  // namespace

CredentialHashCost parse_credential_hash_cost(std::string_view value) {
    if (value == "interactive") return CredentialHashCost::Interactive;
    if (value == "minimum") return CredentialHashCost::Minimum;
    throw std::invalid_argument(
        "player_data credential_hash_cost must be interactive or minimum");
}

void resolve_player_data_paths(
    PlayerDataConfig& config, const std::filesystem::path& config_root) {
    if (config.options.bootstrap_accounts_file.has_value()) {
        resolve(*config.options.bootstrap_accounts_file, config_root);
    }
}

}  // namespace realm::game::common
