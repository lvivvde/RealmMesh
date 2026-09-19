#include "realmmesh/loadgen/login_chain_adapter.hpp"

#include <utility>

namespace realm::loadgen {
namespace {

[[nodiscard]] bool needs_gateway(LoadgenLoginTarget target) {
    return target == LoadgenLoginTarget::Gateway ||
        target == LoadgenLoginTarget::GatewaySoak ||
        target == LoadgenLoginTarget::Full ||
        target == LoadgenLoginTarget::All;
}

[[nodiscard]] bool is_soak(LoadgenLoginTarget target) {
    return target == LoadgenLoginTarget::GatewaySoak ||
        target == LoadgenLoginTarget::All;
}

[[nodiscard]] client::PollingProfile client_polling_profile(
    LoadgenPollingProfile polling) {
    switch (polling) {
        case LoadgenPollingProfile::ClientRealistic:
            return client::PollingProfile::ClientRealistic;
        case LoadgenPollingProfile::Pressure:
            return client::PollingProfile::Pressure;
    }
    return client::PollingProfile::Pressure;
}

}  // namespace

LoginRunAdaptation adapt_login_run(const LoadgenLoginOptions& options) {
    if (options.polling == LoadgenPollingProfile::Pressure &&
        options.poll_interval.count() <= 0) {
        return LoginRunConfigError::NonPositivePollInterval;
    }
    if (needs_gateway(options.target) && options.endpoints.gateway.port == 0) {
        return LoginRunConfigError::MissingGatewayEndpoint;
    }
    if (is_soak(options.target)) {
        if (!options.hold_until.has_value()) {
            return LoginRunConfigError::MissingSoakHorizon;
        }
        if (*options.hold_until < client::Clock::now()) {
            return LoginRunConfigError::SoakHorizonInPast;
        }
    } else if (options.hold_until.has_value()) {
        return LoginRunConfigError::UnexpectedSoakHorizon;
    }

    client::LoginChainConfig chain;
    chain.pressure_poll_interval = options.poll_interval;
    if (needs_gateway(options.target)) {
        chain.gateway_endpoints.push_back(
            network::client::EndpointCandidate{
                .protocol = network::TransportProtocol::TlsTcp,
                .host = options.endpoints.gateway.host,
                .port = options.endpoints.gateway.port,
                .priority = 1,
            });
    }

    client::WireEndpoints wire;
    wire.login_verify_host = options.endpoints.login_verify.host;
    wire.login_verify_port = options.endpoints.login_verify.port;
    wire.queue_host = options.endpoints.queue.host;
    wire.queue_port = options.endpoints.queue.port;
    wire.verify_peer = false;

    client::WireTransportOptions transport;
    transport.network_id = "loadgen";
    transport.reset_close_on_release = true;

    auto finish = [&](client::LoginRun run) -> LoginRunAdaptation {
        return AdaptedLoginRun{std::move(run), std::move(chain),
                               std::move(wire), std::move(transport)};
    };
    const auto polling = client_polling_profile(options.polling);

    switch (options.target) {
        case LoadgenLoginTarget::Verify:
            return finish(client::LoginRun::verify(
                options.account, options.credential, options.deadline));
        case LoadgenLoginTarget::Tickets:
            return finish(client::LoginRun::tickets(
                options.account, options.credential, options.deadline));
        case LoadgenLoginTarget::Poll:
            return finish(client::LoginRun::poll(
                options.account, options.credential, options.deadline,
                polling));
        case LoadgenLoginTarget::Gateway:
            return finish(client::LoginRun::gateway(
                options.account, options.credential, options.deadline,
                polling));
        case LoadgenLoginTarget::GatewaySoak:
        case LoadgenLoginTarget::All: {
            auto run = client::LoginRun::gateway_soak(
                options.account, options.credential, options.deadline,
                polling, *options.hold_until);
            if (!run.has_value()) {
                return LoginRunConfigError::SoakHorizonInPast;
            }
            return finish(std::move(*run));
        }
        case LoadgenLoginTarget::Full:
            return finish(options.polling ==
                                  LoadgenPollingProfile::ClientRealistic
                              ? client::LoginRun::full(
                                    options.account, options.credential,
                                    options.deadline)
                              : client::LoginRun::full_for_loadgen(
                                    options.account, options.credential,
                                    options.deadline, polling));
    }
    return LoginRunConfigError::UnexpectedSoakHorizon;
}

}  // namespace realm::loadgen
