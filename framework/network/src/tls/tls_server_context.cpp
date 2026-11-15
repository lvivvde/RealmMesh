#include "realmmesh/network/tls/tls_server_context.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <stdexcept>
#include <string>

namespace realm::network {
namespace {

[[nodiscard]] std::string latest_error(const char* operation) {
    const auto error = ERR_get_error();
    if (error == 0) {
        return operation;
    }
    char buffer[256]{};
    ERR_error_string_n(error, buffer, sizeof(buffer));
    return std::string(operation) + ": " + buffer;
}

int select_alpn(
    SSL*,
    const unsigned char** selected,
    unsigned char* selected_length,
    const unsigned char* offered,
    unsigned int offered_length,
    void* argument) {
    const auto* expected = static_cast<const std::string*>(argument);
    const auto status = SSL_select_next_proto(
        const_cast<unsigned char**>(selected),
        selected_length,
        reinterpret_cast<const unsigned char*>(expected->data()),
        static_cast<unsigned int>(expected->size()),
        offered,
        offered_length);
    return status == OPENSSL_NPN_NEGOTIATED
               ? SSL_TLSEXT_ERR_OK
               : SSL_TLSEXT_ERR_ALERT_FATAL;
}

int verify_client(X509_STORE_CTX* store, void* argument) {
    if (X509_verify_cert(store) != 1) return 0;
    const auto& identities =
        *static_cast<const std::vector<std::string>*>(argument);
    auto* certificate = X509_STORE_CTX_get0_cert(store);
    for (const auto& identity : identities) {
        if (X509_check_host(
                certificate,
                identity.c_str(),
                identity.size(),
                X509_CHECK_FLAG_NEVER_CHECK_SUBJECT |
                    X509_CHECK_FLAG_NO_WILDCARDS,
                nullptr) == 1)
            return 1;
    }
    X509_STORE_CTX_set_error(store, X509_V_ERR_APPLICATION_VERIFICATION);
    return 0;
}

}  // namespace

TlsServerContext::TlsServerContext(
    const TransportConfig::TlsServerIdentity& identity) {
    if (identity.certificate_chain_file.empty()) {
        throw std::invalid_argument("TLS certificate chain file cannot be empty");
    }
    if (identity.private_key_file.empty()) {
        throw std::invalid_argument("TLS private key file cannot be empty");
    }
    if (identity.alpn.empty() || identity.alpn.size() > 255U) {
        throw std::invalid_argument("TLS ALPN must contain between 1 and 255 bytes");
    }

    context_.reset(SSL_CTX_new(TLS_server_method()));
    if (!context_) {
        throw std::runtime_error(latest_error("SSL_CTX_new"));
    }
    if (SSL_CTX_set_min_proto_version(context_.get(), TLS1_3_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(context_.get(), TLS1_3_VERSION) != 1) {
        throw std::runtime_error(latest_error("configure TLS 1.3"));
    }
    SSL_CTX_set_options(context_.get(), SSL_OP_NO_RENEGOTIATION);
    SSL_CTX_set_max_early_data(context_.get(), 0);

    if (SSL_CTX_use_certificate_chain_file(
            context_.get(), identity.certificate_chain_file.c_str()) != 1) {
        throw std::runtime_error(latest_error("load TLS certificate chain"));
    }
    if (SSL_CTX_use_PrivateKey_file(
            context_.get(),
            identity.private_key_file.c_str(),
            SSL_FILETYPE_PEM) != 1) {
        throw std::runtime_error(latest_error("load TLS private key"));
    }
    if (SSL_CTX_check_private_key(context_.get()) != 1) {
        throw std::runtime_error(latest_error("validate TLS private key"));
    }

    if (identity.client_ca_file.empty() !=
        identity.allowed_client_identities.empty()) {
        throw std::invalid_argument("mTLS requires CA and client identities");
    }
    if (!identity.client_ca_file.empty()) {
        if (SSL_CTX_load_verify_locations(
                context_.get(), identity.client_ca_file.c_str(), nullptr) !=
            1) {
            throw std::runtime_error(latest_error("load client CA"));
        }
        allowed_client_identities_ = identity.allowed_client_identities;
        SSL_CTX_set_verify(
            context_.get(),
            SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
            nullptr);
        SSL_CTX_set_cert_verify_callback(
            context_.get(), verify_client, &allowed_client_identities_);
    }

    // OpenSSL 的服务端选择回调要求协议列表使用“1 字节长度 + 内容”的 wire 格式；
    // alpn_wire_ 作为成员必须与 SSL_CTX 一同存活，因为回调参数保存了它的地址。
    alpn_wire_.reserve(identity.alpn.size() + 1U);
    alpn_wire_.push_back(static_cast<char>(identity.alpn.size()));
    alpn_wire_.append(identity.alpn);
    SSL_CTX_set_alpn_select_cb(context_.get(), select_alpn, &alpn_wire_);
}

void TlsServerContext::Deleter::operator()(SSL_CTX* context) const noexcept {
    if (context == nullptr) {
        return;
    }
    SSL_CTX_free(context);
}

SSL_CTX* TlsServerContext::native_handle() const noexcept {
    return context_.get();
}

}  // namespace realm::network
