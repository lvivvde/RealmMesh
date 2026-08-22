#include "realmmesh/network/client/preferred_transport_connector.hpp"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace realm::network::client {
namespace {

struct AttemptResult {
    TransportProtocol protocol;
    ConnectAttempt result;
};

struct RaceState {
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<AttemptResult> results;
};

[[nodiscard]] const EndpointCandidate* candidate_for(
    std::span<const EndpointCandidate> candidates,
    TransportProtocol protocol) {
    const EndpointCandidate* selected = nullptr;
    for (const auto& candidate : candidates) {
        if (candidate.protocol == protocol &&
            (selected == nullptr || candidate.priority < selected->priority)) {
            selected = &candidate;
        }
    }
    return selected;
}

}  // namespace

PreferredTransportConnector::PreferredTransportConnector(
    ITransportDialer& dialer,
    ConnectorOptions options)
    : dialer_(dialer), options_(options) {
    if (options_.tls_tcp_delay < std::chrono::milliseconds::zero() ||
        options_.handshake_timeout <= std::chrono::milliseconds::zero() ||
        options_.round_timeout <= std::chrono::milliseconds::zero() ||
        options_.quic_negative_cache_ttl <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("connector timeouts are invalid");
    }
}

ConnectAttempt PreferredTransportConnector::connect(
    std::span<const EndpointCandidate> candidates,
    std::string_view network_id) {
    const auto* quic = candidate_for(candidates, TransportProtocol::Quic);
    const auto* tls_tcp = candidate_for(candidates, TransportProtocol::TlsTcp);
    if (quic == nullptr && tls_tcp == nullptr) {
        return ConnectFailure::NoCandidate;
    }

    const auto round_deadline =
        std::chrono::steady_clock::now() + options_.round_timeout;
    auto state = std::make_shared<RaceState>();
    std::vector<std::jthread> workers;
    auto start = [&](const EndpointCandidate& endpoint) {
        workers.emplace_back(
            [this, state, endpoint](std::stop_token stop_token) {
                auto result = dialer_.connect(
                    endpoint, options_.handshake_timeout, stop_token);
                {
                    std::lock_guard lock(state->mutex);
                    state->results.push_back({endpoint.protocol, std::move(result)});
                }
                state->ready.notify_one();
            });
    };

    bool quic_started = false;
    bool tls_started = false;
    if (quic != nullptr && !quic_is_suppressed(network_id)) {
        start(*quic);
        quic_started = true;
    } else if (tls_tcp != nullptr) {
        start(*tls_tcp);
        tls_started = true;
    }

    const auto tls_start_at =
        std::chrono::steady_clock::now() + options_.tls_tcp_delay;
    std::size_t handled = 0;
    ConnectFailure last_failure = ConnectFailure::NoCandidate;

    while (std::chrono::steady_clock::now() < round_deadline) {
        std::unique_lock lock(state->mutex);
        const auto wake_at = !tls_started && tls_tcp != nullptr
                                 ? std::min(tls_start_at, round_deadline)
                                 : round_deadline;
        state->ready.wait_until(lock, wake_at, [&] {
            return state->results.size() > handled;
        });

        while (handled < state->results.size()) {
            auto result = std::move(state->results[handled++]);
            if (const auto* connection =
                    std::get_if<std::shared_ptr<ISecureConnection>>(&result.result)) {
                auto winner = *connection;
                lock.unlock();
                for (auto& worker : workers) worker.request_stop();
                return winner;
            }

            const auto failure = std::get<ConnectFailure>(result.result);
            last_failure = failure;
            if (result.protocol == TransportProtocol::Quic) {
                remember_quic_failure(network_id, failure);
            }
            if (!permits_transport_fallback(failure) &&
                failure != ConnectFailure::Cancelled) {
                lock.unlock();
                for (auto& worker : workers) worker.request_stop();
                return failure;
            }
            if (result.protocol == TransportProtocol::Quic &&
                !tls_started && tls_tcp != nullptr) {
                lock.unlock();
                start(*tls_tcp);
                tls_started = true;
                lock.lock();
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (!tls_started && tls_tcp != nullptr && now >= tls_start_at) {
            lock.unlock();
            start(*tls_tcp);
            tls_started = true;
            continue;
        }

        const auto expected_results =
            static_cast<std::size_t>(quic_started) +
            static_cast<std::size_t>(tls_started);
        if (expected_results != 0U && handled >= expected_results) {
            break;
        }
    }

    for (auto& worker : workers) worker.request_stop();
    return last_failure == ConnectFailure::NoCandidate
               ? ConnectFailure::HandshakeTimeout
               : last_failure;
}

void PreferredTransportConnector::network_changed() {
    quic_negative_cache_.reset();
}

bool PreferredTransportConnector::quic_is_suppressed(
    std::string_view network_id) const {
    return quic_negative_cache_.has_value() &&
           quic_negative_cache_->network_id == network_id &&
           quic_negative_cache_->expires_at > std::chrono::steady_clock::now();
}

void PreferredTransportConnector::remember_quic_failure(
    std::string_view network_id,
    ConnectFailure failure) {
    if (failure != ConnectFailure::NetworkUnreachable &&
        failure != ConnectFailure::HandshakeTimeout) {
        return;
    }
    quic_negative_cache_ = NegativeCacheEntry{
        .network_id = std::string(network_id),
        .expires_at = std::chrono::steady_clock::now() +
                      options_.quic_negative_cache_ttl,
    };
}

}  // namespace realm::network::client
