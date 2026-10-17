#include "realmmesh/game/gateway/gateway_source_address.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <utility>

namespace realm::game::gateway {
namespace {

struct IpAddress final {
    int family{AF_UNSPEC};
    std::array<unsigned char, 16> bytes{};
};

struct Cidr final {
    IpAddress network;
    unsigned prefix{0};
};

std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t");
    return std::string{value.substr(first, last - first + 1U)};
}

std::optional<IpAddress> parse_ip(std::string_view value) {
    const auto text = trim(value);
    if (text.empty()) return std::nullopt;
    IpAddress address;
    if (::inet_pton(AF_INET, text.c_str(), address.bytes.data()) == 1) {
        address.family = AF_INET;
        return address;
    }
    if (::inet_pton(AF_INET6, text.c_str(), address.bytes.data()) == 1) {
        address.family = AF_INET6;
        return address;
    }
    return std::nullopt;
}

std::optional<std::string> canonical_ip(std::string_view value) {
    const auto address = parse_ip(value);
    if (!address.has_value()) return std::nullopt;
    char output[INET6_ADDRSTRLEN]{};
    if (::inet_ntop(
            address->family,
            address->bytes.data(),
            output,
            sizeof(output)) == nullptr) {
        return std::nullopt;
    }
    return std::string{output};
}

std::optional<Cidr> parse_cidr(std::string_view value) {
    const auto slash = value.find('/');
    if (slash == std::string_view::npos ||
        value.find('/', slash + 1U) != std::string_view::npos) {
        return std::nullopt;
    }
    const auto address = parse_ip(value.substr(0, slash));
    if (!address.has_value()) return std::nullopt;
    const auto prefix_text = value.substr(slash + 1U);
    if (prefix_text.empty()) return std::nullopt;
    unsigned prefix = 0;
    for (const char character : prefix_text) {
        if (character < '0' || character > '9') return std::nullopt;
        const auto digit = static_cast<unsigned>(character - '0');
        if (prefix > (std::numeric_limits<unsigned>::max() - digit) / 10U) {
            return std::nullopt;
        }
        prefix = prefix * 10U + digit;
    }
    const auto maximum = address->family == AF_INET ? 32U : 128U;
    if (prefix > maximum) return std::nullopt;
    return Cidr{*address, prefix};
}

bool contains(const Cidr& cidr, const IpAddress& address) {
    if (cidr.network.family != address.family) return false;
    const auto whole_bytes = cidr.prefix / 8U;
    const auto remaining_bits = cidr.prefix % 8U;
    if (!std::equal(
            cidr.network.bytes.begin(),
            cidr.network.bytes.begin() + whole_bytes,
            address.bytes.begin())) {
        return false;
    }
    if (remaining_bits == 0U) return true;
    const auto mask = static_cast<unsigned char>(0xFFU << (8U - remaining_bits));
    return (cidr.network.bytes[whole_bytes] & mask) ==
        (address.bytes[whole_bytes] & mask);
}

std::vector<std::string> split(std::string_view value, char delimiter) {
    std::vector<std::string> result;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto end = value.find(delimiter, begin);
        result.push_back(trim(value.substr(
            begin,
            end == std::string_view::npos ? value.size() - begin
                                          : end - begin)));
        if (end == std::string_view::npos) break;
        begin = end + 1U;
    }
    return result;
}

std::optional<std::string> forwarded_for_value(std::string_view element) {
    for (const auto& parameter : split(element, ';')) {
        const auto equals = parameter.find('=');
        if (equals == std::string::npos) continue;
        auto name = trim(std::string_view{parameter}.substr(0, equals));
        std::ranges::transform(name, name.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        if (name != "for") continue;
        auto value = trim(std::string_view{parameter}.substr(equals + 1U));
        if (value.size() >= 2U && value.front() == '"' &&
            value.back() == '"') {
            value = value.substr(1U, value.size() - 2U);
        }
        if (value.empty() || value == "unknown" || value.front() == '_') {
            return std::nullopt;
        }
        if (value.front() == '[') {
            const auto close = value.find(']');
            if (close == std::string::npos) return std::nullopt;
            return canonical_ip(value.substr(1U, close - 1U));
        }
        if (const auto direct = canonical_ip(value); direct.has_value()) {
            return direct;
        }
        const auto colon = value.rfind(':');
        if (colon == std::string::npos) return std::nullopt;
        return canonical_ip(value.substr(0, colon));
    }
    return std::nullopt;
}

}  // namespace

GatewaySourceMode parse_gateway_source_mode(std::string_view value) {
    if (value == "direct_peer") return GatewaySourceMode::DirectPeer;
    if (value == "trusted_x_forwarded_for") {
        return GatewaySourceMode::TrustedXForwardedFor;
    }
    if (value == "trusted_forwarded") {
        return GatewaySourceMode::TrustedForwarded;
    }
    throw std::invalid_argument("unsupported gateway source mode");
}

std::string_view to_string(GatewaySourceMode mode) noexcept {
    switch (mode) {
    case GatewaySourceMode::DirectPeer:
        return "direct_peer";
    case GatewaySourceMode::TrustedXForwardedFor:
        return "trusted_x_forwarded_for";
    case GatewaySourceMode::TrustedForwarded:
        return "trusted_forwarded";
    }
    return "unknown";
}

void GatewaySourceConfig::validate() const {
    if (trusted_proxy_cidrs.size() > gateway_trusted_proxy_hard_max) {
        throw std::invalid_argument("too many trusted proxy CIDRs");
    }
    if (mode == GatewaySourceMode::DirectPeer &&
        !trusted_proxy_cidrs.empty()) {
        throw std::invalid_argument(
            "direct peer mode cannot configure trusted proxy CIDRs");
    }
    if (mode != GatewaySourceMode::DirectPeer &&
        trusted_proxy_cidrs.empty()) {
        throw std::invalid_argument(
            "forwarded source mode requires trusted proxy CIDRs");
    }
    if (std::ranges::any_of(trusted_proxy_cidrs, [](const auto& cidr) {
            return !parse_cidr(cidr).has_value();
        })) {
        throw std::invalid_argument("invalid trusted proxy CIDR");
    }
}

class GatewaySourceNormalizer::Impl final {
public:
    explicit Impl(GatewaySourceConfig config) : config_(std::move(config)) {
        config_.validate();
        trusted_.reserve(config_.trusted_proxy_cidrs.size());
        for (const auto& text : config_.trusted_proxy_cidrs) {
            trusted_.push_back(*parse_cidr(text));
        }
    }

    std::optional<std::string> normalize(
        const network::TransportIngressSource& source) const {
        const auto direct = parse_ip(source.direct_peer);
        const auto canonical_direct = canonical_ip(source.direct_peer);
        if (!direct.has_value() || !canonical_direct.has_value()) {
            return std::nullopt;
        }
        if (config_.mode == GatewaySourceMode::DirectPeer ||
            !is_trusted(*direct)) {
            return canonical_direct;
        }

        std::vector<std::string> chain;
        if (config_.mode == GatewaySourceMode::TrustedXForwardedFor) {
            if (source.x_forwarded_for.empty()) return std::nullopt;
            for (const auto& entry : split(source.x_forwarded_for, ',')) {
                const auto parsed = canonical_ip(entry);
                if (!parsed.has_value()) return std::nullopt;
                chain.push_back(*parsed);
            }
        } else {
            if (source.forwarded.empty()) return std::nullopt;
            for (const auto& entry : split(source.forwarded, ',')) {
                const auto parsed = forwarded_for_value(entry);
                if (!parsed.has_value()) return std::nullopt;
                chain.push_back(*parsed);
            }
        }
        chain.push_back(*canonical_direct);
        for (auto iterator = chain.rbegin(); iterator != chain.rend();
             ++iterator) {
            const auto address = parse_ip(*iterator);
            if (!address.has_value()) return std::nullopt;
            if (!is_trusted(*address)) return *iterator;
        }
        return chain.front();
    }

private:
    bool is_trusted(const IpAddress& address) const {
        return std::ranges::any_of(trusted_, [&address](const auto& cidr) {
            return contains(cidr, address);
        });
    }

    GatewaySourceConfig config_;
    std::vector<Cidr> trusted_;
};

GatewaySourceNormalizer::GatewaySourceNormalizer(GatewaySourceConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
GatewaySourceNormalizer::~GatewaySourceNormalizer() = default;
GatewaySourceNormalizer::GatewaySourceNormalizer(
    GatewaySourceNormalizer&&) noexcept = default;
GatewaySourceNormalizer& GatewaySourceNormalizer::operator=(
    GatewaySourceNormalizer&&) noexcept = default;
std::optional<std::string> GatewaySourceNormalizer::normalize(
    const network::TransportIngressSource& source) const {
    return impl_->normalize(source);
}

}  // namespace realm::game::gateway
