#pragma once

#include <chrono>
#include <string>

namespace realm::cluster {

struct ServiceDiscoveryConfig {
    bool enabled{false};
    bool required{false};
    std::string endpoint{"http://127.0.0.1:2379"};
    std::string key_prefix{"/realmmesh/services"};
    std::string instance_id;
    std::string node_id{"development-node"};
    std::string zone{"development"};
    std::string advertise_address{"127.0.0.1"};
    std::chrono::seconds lease_ttl{15};
    std::chrono::milliseconds request_timeout{500};
    std::chrono::milliseconds watch_interval{500};
};

}  // namespace realm::cluster
