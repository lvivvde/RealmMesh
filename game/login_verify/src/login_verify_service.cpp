#include "realmmesh/game/login_verify/login_verify_service.hpp"

#include "realmmesh/game/common/account_store.hpp"
#include "realmmesh/game/common/identity_token.hpp"
#include "realmmesh/network/http/http_server.hpp"
#include "realmmesh/observability/logger.hpp"

#include <chrono>
#include <utility>

namespace realm::game::login_verify {

LoginVerifyService::LoginVerifyService(
    LoginVerifyConfig config, observability::MetricsRegistry* metrics)
    : config_(std::move(config)), metrics_(metrics) {}

LoginVerifyService::~LoginVerifyService() = default;

void LoginVerifyService::start(observability::Logger* logger) {
    codec_ = std::make_unique<common::IdentityTokenCodec>(
        common::seed_from_environment("REALMMESH_IDENTITY_KEY_SEED"),
        config_.kid);
    store_ = std::make_unique<common::ConfigAccountStore>(
        common::ConfigAccountStore::load(config_.accounts_file));
    handler_ = std::make_unique<LoginVerifyHandler>(
        *store_, *codec_, [] { return std::chrono::system_clock::now(); },
        identity_token_issuer, identity_token_ttl, metrics_);

    network::HttpServerConfig http_config;
    http_config.tls_identity = config_.tls;
    server_ = std::make_unique<network::HttpServer>(
        config_.listen_address, config_.listen_port, http_config,
        [this](const network::Http1Request& request) {
            return handler_->handle(request.method, request.target, request.body);
        });
    endpoints_ = {network::TransportEndpoint{
        .name = "https",
        .protocol = network::TransportProtocol::TlsTcp,
        .address = config_.listen_address,
        .port = server_->local_port()}};

    if (logger != nullptr) {
        static_cast<void>(logger->info(
            "listener_started",
            "login_verify listener started",
            {observability::field("listen_address", config_.listen_address),
             observability::field("listen_port", server_->local_port()),
             observability::field(
                 "transport", network::to_string(
                                  network::TransportProtocol::TlsTcp)),
             observability::field("transport_name", std::string("https"))}));
        static_cast<void>(logger->info(
            "service_started", "login_verify service started"));
    }
}

void LoginVerifyService::stop() {
    server_.reset();
    handler_.reset();
    codec_.reset();
    store_.reset();
    endpoints_.clear();
}

void LoginVerifyService::tick() {
    if (server_ != nullptr) {
        server_->poll_once(std::chrono::milliseconds(2));
    }
}

bool LoginVerifyService::running() const noexcept {
    return server_ != nullptr;
}

const std::vector<network::TransportEndpoint>&
LoginVerifyService::local_endpoints() const noexcept {
    return endpoints_;
}

}  // namespace realm::game::login_verify
