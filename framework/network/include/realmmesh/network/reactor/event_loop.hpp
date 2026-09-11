#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
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

// Platform-neutral registration key for an IEventLoop. POSIX backends carry a
// file descriptor; Windows backends will carry a SOCKET. Both widen losslessly.
using EventLoopHandle = std::uintptr_t;
inline constexpr EventLoopHandle invalid_event_loop_handle =
    static_cast<EventLoopHandle>(-1);

[[nodiscard]] constexpr EventLoopHandle to_event_loop_handle(
    int descriptor) noexcept {
    return static_cast<EventLoopHandle>(descriptor);
}

struct ReadyEvent {
    EventLoopHandle handle;
    bool readable;
    bool writable;
    bool peer_closed;
    bool error;
};

// Platform seam for the Event Loop (see ADR-0001): exactly one backend is
// compiled into the library and exposed through make_default_event_loop().
class IEventLoop {
public:
    virtual ~IEventLoop() = default;

    virtual void add(EventLoopHandle handle, EventInterest interest) = 0;
    virtual void modify(EventLoopHandle handle, EventInterest interest) = 0;
    virtual void remove(EventLoopHandle handle) = 0;

    // A negative timeout blocks until at least one event is ready.
    [[nodiscard]] virtual std::vector<ReadyEvent> wait(
        std::chrono::milliseconds timeout) = 0;
};

[[nodiscard]] std::unique_ptr<IEventLoop> make_default_event_loop();

}  // namespace realm::network
