#pragma once

#include "realmmesh/network/transport/transport_config.hpp"

#include <memory>
#include <string>

typedef struct ssl_ctx_st SSL_CTX;

namespace realm::network {

class TlsServerContext final {
public:
    explicit TlsServerContext(
        const TransportConfig::TlsServerIdentity& identity);

    TlsServerContext(const TlsServerContext&) = delete;
    TlsServerContext& operator=(const TlsServerContext&) = delete;
    TlsServerContext(TlsServerContext&&) = delete;
    TlsServerContext& operator=(TlsServerContext&&) = delete;

    [[nodiscard]] SSL_CTX* native_handle() const noexcept;

private:
    struct Deleter {
        void operator()(SSL_CTX* context) const noexcept;
    };

    std::string alpn_wire_;
    std::unique_ptr<SSL_CTX, Deleter> context_;
};

}  // namespace realm::network
