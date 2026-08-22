#include "realmmesh/cluster/etcd_service_registry.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace realm::cluster {
namespace {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

class EtcdHttpClient final : public IEtcdHttpClient {
public:
    EtcdHttpClient(std::string endpoint, std::chrono::milliseconds timeout)
        : endpoint_(std::move(endpoint)), timeout_(timeout) {}

    std::optional<std::string> post(
        std::string_view path,
        std::string_view json_body,
        std::string* error) override {
        httplib::Client client(endpoint_);
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(timeout_);
        const auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(
            timeout_ - seconds);
        client.set_connection_timeout(
            seconds.count(), static_cast<time_t>(microseconds.count()));
        client.set_read_timeout(
            seconds.count(), static_cast<time_t>(microseconds.count()));
        client.set_write_timeout(
            seconds.count(), static_cast<time_t>(microseconds.count()));
        const auto response = client.Post(
            std::string(path), std::string(json_body), "application/json");
        if (!response) {
            if (error != nullptr) {
                *error = "etcd request failed: " + httplib::to_string(response.error());
            }
            return std::nullopt;
        }
        if (response->status < 200 || response->status >= 300) {
            if (error != nullptr) {
                *error = "etcd returned HTTP " + std::to_string(response->status) +
                         ": " + response->body;
            }
            return std::nullopt;
        }
        return response->body;
    }

private:
    std::string endpoint_;
    std::chrono::milliseconds timeout_;
};

std::string base64_encode(std::string_view input) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((input.size() + 2U) / 3U) * 4U);
    for (std::size_t offset = 0; offset < input.size(); offset += 3U) {
        const auto first = static_cast<unsigned char>(input[offset]);
        const auto second = offset + 1U < input.size()
            ? static_cast<unsigned char>(input[offset + 1U])
            : 0U;
        const auto third = offset + 2U < input.size()
            ? static_cast<unsigned char>(input[offset + 2U])
            : 0U;
        const std::uint32_t value =
            (static_cast<std::uint32_t>(first) << 16U) |
            (static_cast<std::uint32_t>(second) << 8U) |
            static_cast<std::uint32_t>(third);
        output.push_back(alphabet[(value >> 18U) & 0x3FU]);
        output.push_back(alphabet[(value >> 12U) & 0x3FU]);
        output.push_back(offset + 1U < input.size()
                             ? alphabet[(value >> 6U) & 0x3FU]
                             : '=');
        output.push_back(offset + 2U < input.size()
                             ? alphabet[value & 0x3FU]
                             : '=');
    }
    return output;
}

std::optional<std::string> base64_decode(std::string_view input) {
    auto decode = [](char character) -> int {
        if (character >= 'A' && character <= 'Z') return character - 'A';
        if (character >= 'a' && character <= 'z') return character - 'a' + 26;
        if (character >= '0' && character <= '9') return character - '0' + 52;
        if (character == '+') return 62;
        if (character == '/') return 63;
        return -1;
    };
    if (input.size() % 4U != 0U) return std::nullopt;
    std::string output;
    output.reserve((input.size() / 4U) * 3U);
    for (std::size_t offset = 0; offset < input.size(); offset += 4U) {
        const int first = decode(input[offset]);
        const int second = decode(input[offset + 1U]);
        const int third = input[offset + 2U] == '=' ? 0 : decode(input[offset + 2U]);
        const int fourth = input[offset + 3U] == '=' ? 0 : decode(input[offset + 3U]);
        if (first < 0 || second < 0 || third < 0 || fourth < 0) {
            return std::nullopt;
        }
        const auto value =
            (static_cast<std::uint32_t>(first) << 18U) |
            (static_cast<std::uint32_t>(second) << 12U) |
            (static_cast<std::uint32_t>(third) << 6U) |
            static_cast<std::uint32_t>(fourth);
        output.push_back(static_cast<char>((value >> 16U) & 0xFFU));
        if (input[offset + 2U] != '=') {
            output.push_back(static_cast<char>((value >> 8U) & 0xFFU));
        }
        if (input[offset + 3U] != '=') {
            output.push_back(static_cast<char>(value & 0xFFU));
        }
    }
    return output;
}

std::string_view service_type_name(ServiceType type) {
    switch (type) {
        case ServiceType::Coordinator: return "coordinator";
        case ServiceType::Gateway: return "gateway";
        case ServiceType::Login: return "login";
        case ServiceType::Realm: return "realm";
        case ServiceType::Lobby: return "lobby";
        case ServiceType::Scene: return "scene";
        case ServiceType::Friend: return "friend";
        case ServiceType::Chat: return "chat";
        case ServiceType::Storage: return "storage";
    }
    throw std::invalid_argument("unknown service type");
}

std::optional<ServiceType> parse_service_type(std::string_view value) {
    if (value == "coordinator") return ServiceType::Coordinator;
    if (value == "gateway") return ServiceType::Gateway;
    if (value == "login") return ServiceType::Login;
    if (value == "realm") return ServiceType::Realm;
    if (value == "lobby") return ServiceType::Lobby;
    if (value == "scene") return ServiceType::Scene;
    if (value == "friend") return ServiceType::Friend;
    if (value == "chat") return ServiceType::Chat;
    if (value == "storage") return ServiceType::Storage;
    return std::nullopt;
}

Json encode_instance(const ServiceInstance& instance) {
    Json endpoints = Json::array();
    for (const auto& endpoint : instance.endpoints) {
        endpoints.push_back({
            {"name", endpoint.name},
            {"protocol", network::to_string(endpoint.protocol)},
            {"address", endpoint.address},
            {"port", endpoint.port},
        });
    }
    return {
        {"type", service_type_name(instance.type)},
        {"instance_id", instance.instance_id},
        {"node_id", instance.node_id},
        {"zone", instance.zone},
        {"endpoints", std::move(endpoints)},
        {"weight", instance.weight},
        {"version", instance.version},
    };
}

std::optional<network::TransportProtocol> parse_protocol(std::string_view value) {
    if (value == "quic") return network::TransportProtocol::Quic;
    if (value == "tls_tcp") return network::TransportProtocol::TlsTcp;
    return std::nullopt;
}

std::optional<ServiceInstance> decode_instance(const Json& value) {
    try {
        const auto type = parse_service_type(value.at("type").get<std::string>());
        if (!type.has_value()) return std::nullopt;
        ServiceInstance instance{
            .type = *type,
            .instance_id = value.at("instance_id").get<std::string>(),
            .node_id = value.at("node_id").get<std::string>(),
            .zone = value.at("zone").get<std::string>(),
            .endpoints = {},
            .weight = value.value("weight", 100U),
            .version = value.value("version", std::string{}),
        };
        for (const auto& endpoint_value : value.at("endpoints")) {
            const auto protocol = parse_protocol(
                endpoint_value.at("protocol").get<std::string>());
            if (!protocol.has_value()) return std::nullopt;
            instance.endpoints.push_back({
                .name = endpoint_value.at("name").get<std::string>(),
                .protocol = *protocol,
                .address = endpoint_value.at("address").get<std::string>(),
                .port = endpoint_value.at("port").get<std::uint16_t>(),
            });
        }
        if (instance.instance_id.empty() || instance.node_id.empty() ||
            instance.endpoints.empty()) {
            return std::nullopt;
        }
        return instance;
    } catch (const Json::exception&) {
        return std::nullopt;
    }
}

std::optional<std::int64_t> json_integer(const Json& value) {
    if (value.is_number_integer()) return value.get<std::int64_t>();
    if (!value.is_string()) return std::nullopt;
    const auto text = value.get<std::string>();
    std::int64_t result = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return result;
}

bool valid_instance(const ServiceInstance& instance) {
    if (instance.instance_id.empty() || instance.node_id.empty() ||
        instance.zone.empty() || instance.endpoints.empty()) {
        return false;
    }
    if (!std::ranges::all_of(instance.instance_id, [](unsigned char character) {
            return std::isalnum(character) != 0 || character == '-' ||
                   character == '_' || character == '.';
        })) {
        return false;
    }
    return std::ranges::all_of(
        instance.endpoints, [](const network::TransportEndpoint& endpoint) {
            return !endpoint.name.empty() && !endpoint.address.empty() &&
                   endpoint.port != 0;
        });
}

std::string prefix_range_end(std::string prefix) {
    for (auto iterator = prefix.rbegin(); iterator != prefix.rend(); ++iterator) {
        const auto value = static_cast<unsigned char>(*iterator);
        if (value != 0xFFU) {
            *iterator = static_cast<char>(value + 1U);
            prefix.erase(iterator.base(), prefix.end());
            return prefix;
        }
    }
    return std::string(1, '\0');
}

}  // namespace

class EtcdServiceRegistry::Impl final {
public:
    Impl(
        EtcdRegistryOptions options,
        std::shared_ptr<IEtcdHttpClient> http_client)
        : options_(std::move(options)), http_client_(std::move(http_client)) {
        while (options_.key_prefix.size() > 1U &&
               options_.key_prefix.back() == '/') {
            options_.key_prefix.pop_back();
        }
        if (options_.key_prefix.empty() || options_.key_prefix.front() != '/' ||
            options_.request_timeout <= std::chrono::milliseconds::zero() ||
            options_.watch_interval <= std::chrono::milliseconds::zero() ||
            http_client_ == nullptr) {
            throw std::invalid_argument("invalid etcd registry options");
        }
        if (options_.background_maintenance) {
            worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
        }
    }

    ~Impl() {
        if (worker_.joinable()) {
            worker_.request_stop();
            wakeup_.notify_all();
            worker_.join();
        }
        std::vector<RegistrationId> registrations;
        {
            const std::scoped_lock lock(mutex_);
            registrations.reserve(registrations_.size());
            for (const auto& [id, registration] : registrations_) {
                static_cast<void>(registration);
                registrations.push_back(id);
            }
        }
        for (const auto id : registrations) {
            static_cast<void>(unregister_instance(id));
        }
    }

    RegistrationResult register_instance(
        const ServiceInstance& instance,
        std::chrono::seconds lease_ttl) {
        if (!valid_instance(instance) || lease_ttl <= std::chrono::seconds::zero()) {
            return {RegistryStatus::InvalidArgument};
        }
        const auto lease = grant_lease(lease_ttl);
        if (!lease.has_value()) return {RegistryStatus::Unavailable};

        const auto key = instance_key(instance);
        Json response;
        const Json request = {
            {"compare", Json::array({{
                {"key", base64_encode(key)},
                {"target", "VERSION"},
                {"result", "EQUAL"},
                {"version", "0"},
            }})},
            {"success", Json::array({{
                {"requestPut", {
                    {"key", base64_encode(key)},
                    {"value", base64_encode(encode_instance(instance).dump())},
                    {"lease", std::to_string(*lease)},
                }},
            }})},
        };
        if (!call("/v3/kv/txn", request, response)) {
            static_cast<void>(revoke_lease(*lease));
            return {RegistryStatus::Unavailable};
        }
        if (!response.value("succeeded", false)) {
            static_cast<void>(revoke_lease(*lease));
            return {RegistryStatus::AlreadyExists};
        }

        const std::scoped_lock lock(mutex_);
        const auto id = next_registration_id_++;
        registrations_.emplace(id, Registration{
            instance,
            key,
            *lease,
            lease_ttl,
            Clock::now() + refresh_delay(lease_ttl),
        });
        return {RegistryStatus::Success, id};
    }

    bool refresh_registration(RegistrationId registration_id) {
        Registration registration;
        {
            const std::scoped_lock lock(mutex_);
            const auto found = registrations_.find(registration_id);
            if (found == registrations_.end()) return false;
            registration = found->second;
        }

        const auto new_lease = grant_lease(registration.lease_ttl);
        if (!new_lease.has_value()) return false;
        Json response;
        const Json request = {
            {"key", base64_encode(registration.key)},
            {"value", base64_encode(encode_instance(registration.instance).dump())},
            {"lease", std::to_string(*new_lease)},
        };
        if (!call("/v3/kv/put", request, response)) {
            static_cast<void>(revoke_lease(*new_lease));
            return false;
        }

        std::int64_t old_lease = 0;
        {
            const std::scoped_lock lock(mutex_);
            const auto found = registrations_.find(registration_id);
            if (found == registrations_.end()) {
                static_cast<void>(revoke_lease(*new_lease));
                return false;
            }
            old_lease = found->second.lease_id;
            found->second.lease_id = *new_lease;
            found->second.next_refresh =
                Clock::now() + refresh_delay(found->second.lease_ttl);
        }
        static_cast<void>(revoke_lease(old_lease));
        return true;
    }

    bool unregister_instance(RegistrationId registration_id) {
        std::int64_t lease_id = 0;
        {
            const std::scoped_lock lock(mutex_);
            const auto found = registrations_.find(registration_id);
            if (found == registrations_.end()) return false;
            lease_id = found->second.lease_id;
            registrations_.erase(found);
        }
        return revoke_lease(lease_id);
    }

    std::vector<ServiceInstance> discover(ServiceType type) const {
        const auto result = try_discover(type);
        return result.value_or(std::vector<ServiceInstance>{});
    }

    WatchId watch(ServiceType type, ServiceEventHandler handler) {
        if (!handler) return invalid_watch_id;
        const auto initial = try_discover(type);
        WatchId id = invalid_watch_id;
        {
            const std::scoped_lock lock(mutex_);
            id = next_watch_id_++;
            Watch watch_value{type, handler, {}, initial.has_value()};
            if (initial.has_value()) {
                for (const auto& instance : *initial) {
                    watch_value.instances.emplace(instance.instance_id, instance);
                }
            }
            watches_.emplace(id, std::move(watch_value));
        }
        if (initial.has_value()) {
            for (const auto& instance : *initial) {
                handler({ServiceEventKind::Added, instance});
            }
        }
        wakeup_.notify_all();
        return id;
    }

    bool cancel_watch(WatchId watch_id) {
        const std::scoped_lock lock(mutex_);
        return watches_.erase(watch_id) == 1U;
    }

    void poll_once() {
        std::vector<WatchId> watch_ids;
        {
            const std::scoped_lock lock(mutex_);
            watch_ids.reserve(watches_.size());
            for (const auto& [id, watch_value] : watches_) {
                static_cast<void>(watch_value);
                watch_ids.push_back(id);
            }
        }

        for (const auto id : watch_ids) {
            ServiceType type{};
            {
                const std::scoped_lock lock(mutex_);
                const auto found = watches_.find(id);
                if (found == watches_.end()) continue;
                type = found->second.type;
            }
            const auto discovered = try_discover(type);
            if (!discovered.has_value()) continue;

            std::map<std::string, ServiceInstance> next;
            for (const auto& instance : *discovered) {
                next.emplace(instance.instance_id, instance);
            }
            ServiceEventHandler handler;
            std::vector<ServiceEvent> events;
            {
                const std::scoped_lock lock(mutex_);
                const auto found = watches_.find(id);
                if (found == watches_.end()) continue;
                handler = found->second.handler;
                if (found->second.initialized) {
                    for (const auto& [instance_id, instance] :
                         found->second.instances) {
                        const auto current = next.find(instance_id);
                        if (current == next.end() || current->second != instance) {
                            events.push_back({ServiceEventKind::Removed, instance});
                        }
                    }
                }
                for (const auto& [instance_id, instance] : next) {
                    const auto previous = found->second.instances.find(instance_id);
                    if (!found->second.initialized ||
                        previous == found->second.instances.end() ||
                        previous->second != instance) {
                        events.push_back({ServiceEventKind::Added, instance});
                    }
                }
                found->second.instances = std::move(next);
                found->second.initialized = true;
            }
            for (const auto& event : events) handler(event);
        }
    }

    std::string last_error() const {
        const std::scoped_lock lock(error_mutex_);
        return last_error_;
    }

private:
    struct Registration {
        ServiceInstance instance;
        std::string key;
        std::int64_t lease_id;
        std::chrono::seconds lease_ttl;
        Clock::time_point next_refresh;
    };

    struct Watch {
        ServiceType type;
        ServiceEventHandler handler;
        std::map<std::string, ServiceInstance> instances;
        bool initialized;
    };

    static std::chrono::seconds refresh_delay(std::chrono::seconds ttl) {
        return std::max(std::chrono::seconds(1), ttl / 3);
    }

    std::string type_prefix(ServiceType type) const {
        return options_.key_prefix + "/" + std::string(service_type_name(type)) +
               "/";
    }

    std::string instance_key(const ServiceInstance& instance) const {
        return type_prefix(instance.type) + instance.instance_id;
    }

    bool call(std::string_view path, const Json& request, Json& response) const {
        std::string error;
        const auto body = http_client_->post(path, request.dump(), &error);
        if (!body.has_value()) {
            set_error(std::move(error));
            return false;
        }
        try {
            response = Json::parse(*body);
            return true;
        } catch (const Json::exception& exception) {
            set_error("invalid etcd JSON response: " + std::string(exception.what()));
            return false;
        }
    }

    std::optional<std::int64_t> grant_lease(std::chrono::seconds ttl) const {
        Json response;
        if (!call(
                "/v3/lease/grant",
                {{"TTL", std::to_string(ttl.count())}},
                response)) {
            return std::nullopt;
        }
        const auto id = response.contains("ID")
            ? json_integer(response.at("ID"))
            : std::nullopt;
        if (!id.has_value() || *id == 0) {
            set_error("etcd lease grant response did not contain a valid ID");
            return std::nullopt;
        }
        return id;
    }

    bool revoke_lease(std::int64_t lease_id) const {
        Json response;
        return call(
            "/v3/lease/revoke", {{"ID", std::to_string(lease_id)}}, response);
    }

    std::optional<std::vector<ServiceInstance>> try_discover(
        ServiceType type) const {
        const auto prefix = type_prefix(type);
        Json response;
        if (!call(
                "/v3/kv/range",
                {
                    {"key", base64_encode(prefix)},
                    {"range_end", base64_encode(prefix_range_end(prefix))},
                },
                response)) {
            return std::nullopt;
        }

        std::vector<ServiceInstance> instances;
        try {
            for (const auto& key_value : response.value("kvs", Json::array())) {
                const auto decoded = base64_decode(
                    key_value.at("value").get<std::string>());
                if (!decoded.has_value()) continue;
                const auto instance = decode_instance(Json::parse(*decoded));
                if (instance.has_value() && instance->type == type) {
                    instances.push_back(std::move(*instance));
                }
            }
        } catch (const Json::exception& exception) {
            set_error("invalid etcd range response: " + std::string(exception.what()));
            return std::nullopt;
        }
        std::ranges::sort(instances, {}, &ServiceInstance::instance_id);
        return instances;
    }

    void set_error(std::string error) const {
        const std::scoped_lock lock(error_mutex_);
        last_error_ = std::move(error);
    }

    void run(std::stop_token stop) {
        while (!stop.stop_requested()) {
            std::vector<RegistrationId> due;
            {
                const std::scoped_lock lock(mutex_);
                const auto now = Clock::now();
                for (const auto& [id, registration] : registrations_) {
                    if (registration.next_refresh <= now) due.push_back(id);
                }
            }
            for (const auto id : due) {
                if (stop.stop_requested()) break;
                static_cast<void>(refresh_registration(id));
            }
            if (!stop.stop_requested()) poll_once();

            std::unique_lock lock(wait_mutex_);
            wakeup_.wait_for(lock, options_.watch_interval, [&stop] {
                return stop.stop_requested();
            });
        }
    }

    EtcdRegistryOptions options_;
    std::shared_ptr<IEtcdHttpClient> http_client_;
    mutable std::mutex mutex_;
    RegistrationId next_registration_id_{1};
    WatchId next_watch_id_{1};
    std::unordered_map<RegistrationId, Registration> registrations_;
    std::unordered_map<WatchId, Watch> watches_;
    mutable std::mutex error_mutex_;
    mutable std::string last_error_;
    std::mutex wait_mutex_;
    std::condition_variable wakeup_;
    std::jthread worker_;
};

EtcdServiceRegistry::EtcdServiceRegistry(EtcdRegistryOptions options)
    : EtcdServiceRegistry(
          options,
          std::make_shared<EtcdHttpClient>(
              options.endpoint, options.request_timeout)) {}

EtcdServiceRegistry::EtcdServiceRegistry(
    EtcdRegistryOptions options,
    std::shared_ptr<IEtcdHttpClient> http_client)
    : impl_(std::make_unique<Impl>(std::move(options), std::move(http_client))) {}

EtcdServiceRegistry::~EtcdServiceRegistry() = default;

RegistrationResult EtcdServiceRegistry::register_instance(
    const ServiceInstance& instance,
    std::chrono::seconds lease_ttl) {
    return impl_->register_instance(instance, lease_ttl);
}

bool EtcdServiceRegistry::refresh_registration(RegistrationId registration_id) {
    return impl_->refresh_registration(registration_id);
}

bool EtcdServiceRegistry::unregister_instance(RegistrationId registration_id) {
    return impl_->unregister_instance(registration_id);
}

std::vector<ServiceInstance> EtcdServiceRegistry::discover(
    ServiceType type) const {
    return impl_->discover(type);
}

WatchId EtcdServiceRegistry::watch(
    ServiceType type,
    ServiceEventHandler handler) {
    return impl_->watch(type, std::move(handler));
}

bool EtcdServiceRegistry::cancel_watch(WatchId watch_id) {
    return impl_->cancel_watch(watch_id);
}

void EtcdServiceRegistry::poll_once() { impl_->poll_once(); }

std::string EtcdServiceRegistry::last_error() const { return impl_->last_error(); }

}  // namespace realm::cluster
