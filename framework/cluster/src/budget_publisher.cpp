#include "realmmesh/cluster/budget_publisher.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <utility>

namespace realm::cluster {
namespace {

using Json = nlohmann::json;

/// 单维度是否超阈值:相对上次发布值的变化 ≥ 阈值;从零出发或落到零
/// 都是 100% 变化,必超阈值。
bool beyond_threshold(
    std::uint64_t previous, std::uint64_t next, double threshold) {
    if (previous == next) return false;
    if (previous == 0U || next == 0U) return true;
    const auto difference =
        previous > next ? previous - next : next - previous;
    return static_cast<double>(difference) >=
           threshold * static_cast<double>(previous);
}

std::string encode_budget_value(
    const InstanceBudgetSnapshot& snapshot,
    std::chrono::system_clock::time_point now) {
    Json value{{"conn_free", snapshot.conn_free}};
    if (snapshot.has_fetch) {
        value["fetch_free"] = snapshot.fetch_free;
    }
    value["updated_at"] =
        std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch())
            .count();
    return value.dump();
}

}  // namespace

std::string budget_key(ServiceType type, std::string_view instance_id) {
    return std::string(budget_key_prefix) + "/" +
           std::string(service_type_name(type)) + "/" +
           std::string(instance_id) + "/budget";
}

bool BudgetPublishPolicy::should_publish(
    const std::optional<InstanceBudgetSnapshot>& confirmed,
    const InstanceBudgetSnapshot& next,
    std::chrono::system_clock::time_point confirmed_at,
    std::chrono::system_clock::time_point now) const {
    if (!confirmed.has_value()) return true;

    const bool conn_changed = confirmed->conn_free != next.conn_free;
    const bool fetch_changed =
        confirmed->has_fetch != next.has_fetch ||
        (confirmed->has_fetch && confirmed->fetch_free != next.fetch_free);
    if (!conn_changed && !fetch_changed) return false;

    if (beyond_threshold(confirmed->conn_free, next.conn_free,
                         change_threshold)) {
        return true;
    }
    if (confirmed->has_fetch &&
        beyond_threshold(confirmed->fetch_free, next.fetch_free,
                         change_threshold)) {
        return true;
    }
    if (conn_capacity != 0U && next.conn_free == conn_capacity &&
        confirmed->conn_free != conn_capacity) {
        return true;  // 回满:实例恢复全额接纳,放行阀门需立即知晓。
    }
    if (confirmed->has_fetch && fetch_capacity != 0U &&
        next.fetch_free == fetch_capacity &&
        confirmed->fetch_free != fetch_capacity) {
        return true;
    }
    if (confirmed->has_fetch != next.has_fetch) return true;
    return now - confirmed_at >= min_interval;
}

InstanceBudgetReporter::InstanceBudgetReporter(
    IServiceRegistry& registry,
    RegistrationId registration_id,
    ServiceType type,
    std::string instance_id,
    BudgetPublishPolicy policy)
    : registry_(&registry),
      registration_id_(registration_id),
      type_(type),
      instance_id_(std::move(instance_id)),
      policy_(policy) {}

void InstanceBudgetReporter::set_failure_sink(
    std::function<void(const std::string&)> sink) {
    failure_sink_ = std::move(sink);
}

bool InstanceBudgetReporter::publish(
    const InstanceBudgetSnapshot& snapshot,
    std::chrono::system_clock::time_point now) {
    if (attempted_.has_value() && *attempted_ == snapshot) {
        if (attempted_ok_) return false;  // 已在 etcd,无变化
        if (now - *attempted_at_ < policy_.min_interval) {
            return false;  // 失败重试节流
        }
    } else if (!policy_.should_publish(
                   confirmed_, snapshot, confirmed_at_.value_or(now), now)) {
        return false;
    }

    const auto key = budget_key(type_, instance_id_);
    const bool published =
        registry_->put_leased(registration_id_, key, encode_budget_value(snapshot, now));
    attempted_ = snapshot;
    attempted_at_ = now;
    attempted_ok_ = published;
    if (published) {
        confirmed_ = snapshot;
        confirmed_at_ = now;
    } else if (failure_sink_) {
        failure_sink_(key);
    }
    return published;
}

}  // namespace realm::cluster
