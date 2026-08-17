#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

namespace realm::network {

enum class EventInterest : std::uint8_t {
    Read = 1U << 0U,
    Write = 1U << 1U,
};

[[nodiscard]] constexpr EventInterest operator|(
    EventInterest left,
    EventInterest right) noexcept {
    return static_cast<EventInterest>(
        static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
}

struct ReadyEvent {
    int descriptor;
    bool readable;
    bool writable;
    bool peer_closed;
    bool error;
};

class EpollEventLoop final {
public:
    EpollEventLoop();
    ~EpollEventLoop();

    EpollEventLoop(const EpollEventLoop&) = delete;
    EpollEventLoop& operator=(const EpollEventLoop&) = delete;

    EpollEventLoop(EpollEventLoop&& other) noexcept;
    EpollEventLoop& operator=(EpollEventLoop&& other) noexcept;

    void add(int descriptor, EventInterest interest);
    void modify(int descriptor, EventInterest interest);
    void remove(int descriptor);

    [[nodiscard]] std::vector<ReadyEvent> wait(std::chrono::milliseconds timeout);

private:
    void close() noexcept;

    int descriptor_{-1};
};

}  // namespace realm::network
