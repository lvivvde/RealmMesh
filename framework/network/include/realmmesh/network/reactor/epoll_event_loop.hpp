#pragma once

#include "realmmesh/network/reactor/event_loop.hpp"

namespace realm::network {

class EpollEventLoop final : public IEventLoop {
public:
    EpollEventLoop();
    ~EpollEventLoop() override;

    EpollEventLoop(const EpollEventLoop&) = delete;
    EpollEventLoop& operator=(const EpollEventLoop&) = delete;

    EpollEventLoop(EpollEventLoop&& other) noexcept;
    EpollEventLoop& operator=(EpollEventLoop&& other) noexcept;

    void add(EventLoopHandle handle, EventInterest interest) override;
    void modify(EventLoopHandle handle, EventInterest interest) override;
    void remove(EventLoopHandle handle) override;

    [[nodiscard]] std::vector<ReadyEvent> wait(
        std::chrono::milliseconds timeout) override;

private:
    void close() noexcept;

    int descriptor_{-1};
};

}  // namespace realm::network
