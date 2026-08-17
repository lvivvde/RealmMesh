#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/network/codec/length_field_codec.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace realm::game::common {
namespace {

class ChildProcess final {
public:
    explicit ChildProcess(const char* executable) {
        pid_ = ::fork();
        if (pid_ < 0) throw std::runtime_error("fork failed");
        if (pid_ == 0) {
            ::execl(executable, executable, static_cast<char*>(nullptr));
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

class SocketGuard final {
public:
    explicit SocketGuard(int descriptor) : descriptor_(descriptor) {}
    ~SocketGuard() { if (descriptor_ >= 0) ::close(descriptor_); }
    SocketGuard(const SocketGuard&) = delete;
    SocketGuard& operator=(const SocketGuard&) = delete;
    SocketGuard(SocketGuard&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)) {}
    [[nodiscard]] int get() const noexcept { return descriptor_; }

private:
    int descriptor_;
};

std::optional<SocketGuard> try_connect(std::uint16_t port) {
    const int descriptor = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (descriptor < 0) return std::nullopt;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(descriptor, reinterpret_cast<const sockaddr*>(&address),
                  sizeof(address)) < 0) {
        ::close(descriptor);
        return std::nullopt;
    }
    timeval timeout{};
    timeout.tv_sec = 2;
    static_cast<void>(::setsockopt(
        descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    return SocketGuard(descriptor);
}

SocketGuard connect_when_ready(std::uint16_t port) {
    using namespace std::chrono_literals;
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (auto socket = try_connect(port); socket.has_value()) {
            return std::move(*socket);
        }
        std::this_thread::sleep_for(10ms);
    }
    throw std::runtime_error("service did not open expected port");
}

void send_all(int descriptor, std::span<const std::byte> bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto sent = ::send(
            descriptor, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
        if (sent <= 0) throw std::runtime_error("send failed");
        offset += static_cast<std::size_t>(sent);
    }
}

std::vector<std::byte> receive_exactly(int descriptor, std::size_t size) {
    std::vector<std::byte> result(size);
    std::size_t offset = 0;
    while (offset < size) {
        const auto received = ::recv(
            descriptor, result.data() + offset, result.size() - offset, 0);
        if (received <= 0) throw std::runtime_error("receive failed");
        offset += static_cast<std::size_t>(received);
    }
    return result;
}

void send_message(int descriptor, std::span<const std::byte> payload) {
    const network::LengthFieldCodec codec(65536);
    send_all(descriptor, codec.encode(payload));
}

std::vector<std::byte> receive_message(int descriptor) {
    const auto header = receive_exactly(descriptor, 4);
    std::uint32_t size = 0;
    for (const auto value : header) {
        size = (size << 8U) | std::to_integer<std::uint8_t>(value);
    }
    if (size > 65536) throw std::runtime_error("response frame too large");
    return receive_exactly(descriptor, size);
}

TEST(ThreeStageFlowTest, LogsInSelectsACharacterAndEntersTheGateway) {
    constexpr auto key =
        "0102030405060708090a0b0c0d0e0f10"
        "1112131415161718191a1b1c1d1e1f20";
    ASSERT_EQ(::setenv("REALMMESH_SESSION_TICKET_KEY", key, 1), 0);

    ChildProcess gateway(REALMMESH_GATEWAY_EXECUTABLE);
    ChildProcess realm(REALMMESH_REALM_EXECUTABLE);
    ChildProcess login(REALMMESH_LOGIN_EXECUTABLE);

    auto login_socket = connect_when_ready(7000);
    LoginRequest login_request;
    login_request.set_account("alice");
    login_request.set_credential("dev");
    send_message(login_socket.get(), encode(login_request, 1));
    const auto login_wire = receive_message(login_socket.get());
    EXPECT_EQ(edge_request_id(login_wire), 1);
    const auto login_response = decode_login_succeeded(login_wire);
    ASSERT_TRUE(login_response.has_value());
    EXPECT_EQ(login_response->realm_endpoint().port(), 7100);

    auto realm_socket = connect_when_ready(
        static_cast<std::uint16_t>(login_response->realm_endpoint().port()));
    RealmAuthenticate authenticate;
    authenticate.set_login_ticket(login_response->login_ticket());
    send_message(realm_socket.get(), encode(authenticate, 2));
    const auto character_wire = receive_message(realm_socket.get());
    EXPECT_EQ(edge_request_id(character_wire), 2);
    const auto characters = decode_character_list(character_wire);
    ASSERT_TRUE(characters.has_value());
    ASSERT_EQ(characters->characters_size(), 1);

    SelectCharacter select_character;
    select_character.set_character_id(characters->characters(0).id());
    send_message(realm_socket.get(), encode(select_character, 3));
    const auto enter_wire = receive_message(realm_socket.get());
    EXPECT_EQ(edge_request_id(enter_wire), 3);
    const auto enter = decode_enter_game_issued(enter_wire);
    ASSERT_TRUE(enter.has_value());
    EXPECT_EQ(enter->gateway_endpoint().port(), 8000);

    auto gateway_socket = connect_when_ready(
        static_cast<std::uint16_t>(enter->gateway_endpoint().port()));
    EnterGame enter_game;
    enter_game.set_enter_game_ticket(enter->enter_game_ticket());
    send_message(gateway_socket.get(), encode(enter_game, 4));
    const auto accepted_wire = receive_message(gateway_socket.get());
    EXPECT_EQ(edge_request_id(accepted_wire), 4);
    const auto accepted = decode_enter_game_accepted(accepted_wire);
    ASSERT_TRUE(accepted.has_value());
    EXPECT_EQ(accepted->account_id(), login_response->account_id());
    EXPECT_EQ(accepted->character_id(), characters->characters(0).id());

    login.stop();
    realm.stop();
    gateway.stop();
    static_cast<void>(::unsetenv("REALMMESH_SESSION_TICKET_KEY"));
}

}  // namespace
}  // namespace realm::game::common
