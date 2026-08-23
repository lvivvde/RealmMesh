#include "realmmesh/service_host/startup_topology.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace realm::service_host {
namespace {

/// 校验服务名非空且唯一,依赖均在集合内。
void validate(const std::vector<ServiceSpec>& specs) {
    std::unordered_set<std::string> names;
    for (const auto& spec : specs) {
        if (spec.name.empty() || !names.insert(spec.name).second) {
            throw std::invalid_argument(
                "service names must be non-empty and unique");
        }
    }
    for (const auto& spec : specs) {
        for (const auto& dependency : spec.depends_on) {
            if (!names.contains(dependency)) {
                throw std::invalid_argument(
                    "unknown dependency: " + dependency);
            }
        }
    }
}

}  // namespace

StartupTopology::StartupTopology(std::vector<ServiceSpec> specs) {
    validate(specs);

    const std::size_t count = specs.size();

    // 名字 → 声明下标(名字已校验唯一)。
    std::unordered_map<std::string, std::size_t> index_of;
    for (std::size_t i = 0; i < count; ++i) {
        index_of.emplace(specs[i].name, i);
    }

    // Kahn 环检测:依赖全部消解的节点出队,出队序即拓扑序;
    // 出队数量少于节点总数说明存在环。
    std::vector<std::size_t> indegree(count);
    std::vector<std::vector<std::size_t>> dependents(count);
    for (std::size_t i = 0; i < count; ++i) {
        indegree[i] = specs[i].depends_on.size();
        for (const auto& dependency : specs[i].depends_on) {
            dependents[index_of.at(dependency)].push_back(i);
        }
    }

    std::vector<std::size_t> ready;
    for (std::size_t i = 0; i < count; ++i) {
        if (indegree[i] == 0) {
            ready.push_back(i);
        }
    }

    std::vector<std::size_t> topological_order;
    topological_order.reserve(count);
    for (std::size_t head = 0; head < ready.size(); ++head) {
        const std::size_t node = ready[head];
        topological_order.push_back(node);
        for (const std::size_t dependent : dependents[node]) {
            if (--indegree[dependent] == 0) {
                ready.push_back(dependent);
            }
        }
    }
    if (topological_order.size() != count) {
        throw std::invalid_argument("dependency cycle detected");
    }

    // 深度分层:节点深度 = 1 + 最深依赖(无依赖为 0)。
    std::vector<std::size_t> depth(count, 0);
    for (const std::size_t node : topological_order) {
        for (const auto& dependency : specs[node].depends_on) {
            depth[node] =
                std::max(depth[node], depth[index_of.at(dependency)] + 1);
        }
    }

    // 波次装配:同深度节点归入同一波,波内保持声明序。
    std::size_t wave_count = 0;
    for (std::size_t i = 0; i < count; ++i) {
        wave_count = std::max(wave_count, depth[i] + 1);
    }
    waves_.assign(wave_count, {});
    all_names_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        waves_[depth[i]].push_back(specs[i].name);
        all_names_.push_back(specs[i].name);
    }
}

const std::vector<std::vector<std::string>>& StartupTopology::waves()
    const noexcept {
    return waves_;
}

std::vector<std::string> StartupTopology::shutdown_order() const {
    std::vector<std::string> order;
    order.reserve(all_names_.size());
    for (const auto& wave : waves_) {
        for (const auto& name : wave) {
            order.push_back(name);
        }
    }
    std::reverse(order.begin(), order.end());
    return order;
}

const std::vector<std::string>& StartupTopology::all_names() const noexcept {
    return all_names_;
}

}  // namespace realm::service_host
