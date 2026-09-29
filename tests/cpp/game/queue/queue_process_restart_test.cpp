#include "realmmesh/game/common/identity_token.hpp"
#include "realmmesh/game/common/json.hpp"
#include "realmmesh/cluster/etcd_service_registry.hpp"
#include "realmmesh/test_support/etcd_process.hpp"

#include "queue_test_https.hpp"

#include <gtest/gtest.h>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace realm::game::queue {
namespace {

using common::IdentityClaims;
using common::IdentityTokenCodec;
using common::JsonCodec;
using test_https::https_exchange;

constexpr std::string_view identity_seed_hex =
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
constexpr std::string_view queue_number_seed_hex =
    "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb";
constexpr std::string_view admission_grant_seed_hex =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

[[nodiscard]] std::string base64_encode(std::string_view input) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    for (std::size_t offset = 0; offset < input.size(); offset += 3U) {
        const auto first = static_cast<unsigned char>(input[offset]);
        const auto second = offset + 1U < input.size()
                                ? static_cast<unsigned char>(input[offset + 1U])
                                : 0U;
        const auto third = offset + 2U < input.size()
                               ? static_cast<unsigned char>(input[offset + 2U])
                               : 0U;
        const std::uint32_t value = (static_cast<std::uint32_t>(first) << 16U) |
                                    (static_cast<std::uint32_t>(second) << 8U) |
                                    static_cast<std::uint32_t>(third);
        output.push_back(alphabet[(value >> 18U) & 0x3FU]);
        output.push_back(alphabet[(value >> 12U) & 0x3FU]);
        output.push_back(
            offset + 1U < input.size() ? alphabet[(value >> 6U) & 0x3FU] : '=');
        output.push_back(
            offset + 2U < input.size() ? alphabet[value & 0x3FU] : '=');
    }
    return output;
}

void put_etcd(
    cluster::IEtcdHttpClient& client,
    std::string_view key,
    std::string_view value) {
    std::string error;
    const std::string body =
        "{\"key\":\"" + base64_encode(key) + "\",\"value\":\"" +
        base64_encode(value) + "\"}";
    const auto response = client.post(
        "/v3/kv/put", body, &error);
    if (!response.has_value()) {
        throw std::runtime_error("cannot seed queue budgets: " + error);
    }
}

void seed_budgets(std::string_view endpoint) {
    const auto client = cluster::make_etcd_http_client(
        std::string{endpoint}, std::chrono::seconds{2});
    put_etcd(
        *client,
        "/realmmesh/budgets/service/gateway/gateway-test/budget",
        R"({"conn_free":100,"fetch_free":100})");
    put_etcd(
        *client,
        "/realmmesh/budgets/service/realm/realm-test/budget",
        R"({"conn_free":100})");
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

void write_file(const std::filesystem::path& path, std::string_view contents) {
    std::ofstream output(path, std::ios::trunc);
    output << contents;
    if (!output) throw std::runtime_error("cannot rewrite queue config");
}

[[nodiscard]] std::string replace_all(
    std::string contents, std::string_view from, std::string_view to) {
    for (auto position = contents.find(from); position != std::string::npos;
         position = contents.find(from, position + to.size())) {
        contents.replace(position, from.size(), to);
    }
    return contents;
}

class ScratchConfigRoot final {
public:
    ScratchConfigRoot() {
        path_ = std::filesystem::temp_directory_path() /
                ("queue-process-restart-" +
                 std::to_string(static_cast<long long>(::getpid())));
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        std::filesystem::create_directories(path_, error);
        if (error) throw std::runtime_error("cannot create config scratch dir");
        std::filesystem::copy(
            std::filesystem::path(REALMMESH_TEST_SOURCE_DIR) / "configs",
            path_,
            std::filesystem::copy_options::recursive |
                std::filesystem::copy_options::overwrite_existing,
            error);
        if (error) throw std::runtime_error("cannot copy config tree");
    }

    ~ScratchConfigRoot() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

class ChildProcess final {
public:
    explicit ChildProcess(const std::filesystem::path& config_root) {
        pid_ = ::fork();
        if (pid_ < 0) throw std::runtime_error("fork failed");
        if (pid_ == 0) {
            ::execl(
                REALMMESH_MESH_EXECUTABLE,
                REALMMESH_MESH_EXECUTABLE,
                "--config",
                config_root.c_str(),
                "--service",
                "queue",
                static_cast<char*>(nullptr));
            _exit(127);
        }
    }

    ~ChildProcess() { stop(); }
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    void stop() noexcept {
        if (pid_ <= 0) return;
        static_cast<void>(::kill(pid_, SIGINT));
        int status = 0;
        static_cast<void>(::waitpid(pid_, &status, 0));
        pid_ = -1;
    }

private:
    pid_t pid_{-1};
};

void wait_for_queue(std::uint16_t port) {
    using namespace std::chrono_literals;
    for (int attempt = 0; attempt < 500; ++attempt) {
        const auto response = https_exchange(
            port,
            "GET /healthz HTTP/1.1\r\nHost: localhost\r\n"
            "Connection: close\r\n\r\n");
        if (response.has_value() &&
            response->find(" 200 ") != std::string::npos) {
            return;
        }
        std::this_thread::sleep_for(10ms);
    }
    throw std::runtime_error("queue did not become ready");
}

[[nodiscard]] std::pair<std::uint64_t, std::string> issue(
    std::uint16_t port, std::string_view identity_token) {
    const std::string request =
        "POST /v1/queue/tickets HTTP/1.1\r\nHost: localhost\r\n"
        "Authorization: Bearer " + std::string(identity_token) +
        "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    const auto response = https_exchange(port, request);
    if (!response.has_value() || response->find(" 202 ") == std::string::npos) {
        throw std::runtime_error("queue ticket request failed");
    }
    const auto body_start = response->find("\r\n\r\n");
    const auto payload = JsonCodec::decode(
        std::string_view(*response).substr(body_start + 4U));
    if (!payload.has_value()) {
        throw std::runtime_error("invalid queue response");
    }
    return {
        static_cast<std::uint64_t>(
            std::get<std::int64_t>(payload->at("number"))),
        std::get<std::string>(payload->at("queue_number_token")),
    };
}

void wait_until_admitted(
    std::uint16_t port, std::string_view queue_number_token) {
    using namespace std::chrono_literals;
    const std::string request =
        "GET /v1/queue/tickets/me HTTP/1.1\r\nHost: localhost\r\n"
        "Authorization: Bearer " + std::string{queue_number_token} +
        "\r\nConnection: close\r\n\r\n";
    for (int attempt = 0; attempt < 120; ++attempt) {
        const auto response = https_exchange(port, request);
        if (response.has_value() &&
            response->find(R"("status":"admitted")") !=
                std::string::npos) {
            return;
        }
        std::this_thread::sleep_for(50ms);
    }
    throw std::runtime_error("queue number was not released after restart");
}

void set_environment(std::string_view name, std::string_view value) {
    if (::setenv(
            std::string(name).c_str(), std::string(value).c_str(), 1) != 0) {
        throw std::runtime_error("cannot set test environment");
    }
}

[[nodiscard]] std::string identity_token(std::string jti) {
    const auto now = std::chrono::system_clock::now();
    return IdentityTokenCodec(
               common::parse_identity_seed_hex(identity_seed_hex),
               "login-verify-v1")
        .issue(IdentityClaims{
            .issuer = "realmmesh/login-verify",
            .account_id = 42,
            .jti = std::move(jti),
            .issued_at = now,
            .expires_at = now + std::chrono::minutes{30},
        });
}

TEST(QueueProcessRestartTest, RecoversAcknowledgedIssueFromAuthoritativeStore) {
    test_support::EtcdProcess etcd;
    etcd.wait_ready();
    seed_budgets(etcd.endpoint());
    ScratchConfigRoot config;
    const auto port = test_support::unused_loopback_ports(1).front();
    const auto queue_config = config.path() / "services" / "queue.lua";
    auto contents = read_file(queue_config);
    contents = replace_all(
        contents, "listen_port = 0",
        "listen_port = " + std::to_string(port));
    contents = replace_all(contents, "metrics_port = 9105", "metrics_port = 0");
    contents = replace_all(
        contents,
        "etcd_endpoint = \"http://127.0.0.1:2379\"",
        "etcd_endpoint = \"" + etcd.endpoint() + "\"");
    write_file(queue_config, contents);

    set_environment("REALMMESH_IDENTITY_KEY_SEED", identity_seed_hex);
    set_environment("REALMMESH_QUEUE_NUMBER_KEY_SEED", queue_number_seed_hex);
    set_environment(
        "REALMMESH_ADMISSION_GRANT_KEY_SEED", admission_grant_seed_hex);
    set_environment(
        "REALMMESH_TLS_CERTIFICATE_FILE", REALMMESH_TEST_TLS_CERTIFICATE);
    set_environment(
        "REALMMESH_TLS_PRIVATE_KEY_FILE", REALMMESH_TEST_TLS_PRIVATE_KEY);

    const auto first_identity =
        identity_token("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    std::pair<std::uint64_t, std::string> acknowledged;
    {
        ChildProcess primary(config.path());
        wait_for_queue(port);
        acknowledged = issue(port, first_identity);
        EXPECT_EQ(acknowledged.first, 1U);
    }

    {
        ChildProcess cold_backup(config.path());
        wait_for_queue(port);
        const auto recovered = issue(port, first_identity);
        EXPECT_EQ(recovered, acknowledged);
        wait_until_admitted(port, recovered.second);
        const auto distinct = issue(
            port,
            identity_token("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
        EXPECT_EQ(distinct.first, 2U);
    }
}

}  // namespace
}  // namespace realm::game::queue
