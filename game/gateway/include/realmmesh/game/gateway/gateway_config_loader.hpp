#pragma once

#include "realmmesh/game/gateway/gateway_server.hpp"

#include <filesystem>

namespace realm::game::gateway {

class GatewayConfigLoader final {
public:
    [[nodiscard]] static GatewayConfig load(const std::filesystem::path& path);
};

}  // namespace realm::game::gateway
