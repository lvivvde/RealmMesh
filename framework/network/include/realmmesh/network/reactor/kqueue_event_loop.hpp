#pragma once

#include "realmmesh/network/reactor/event_loop.hpp"

#include <unordered_map>

namespace realm::network {

class KqueueEventLoop final : public IEventLoop {
public:
    KqueueEventLoop();
    ~KqueueEventLoop() override;

    KqueueEventLoop(const KqueueEventLoop&) = delete;
    KqueueEventLoop& operator=(const KqueueEventLoop&) = delete;

    KqueueEventLoop(KqueueEventLoop&& other) noexcept;
    KqueueEventLoop& operator=(KqueueEventLoop&& other) noexcept;

    void add(EventLoopHandle handle, EventInterest interest) override;
    void modify(EventLoopHandle handle, EventInterest interest) override;
    void remove(EventLoopHandle handle) override;

    [[nodiscard]] std::vector<ReadyEvent> wait(
        std::chrono::milliseconds timeout) override;

private:
    void close() noexcept;

    int kqueue_descriptor_{-1};
    // kqueue keeps one filter per direction, so the requested interest has to be
    // remembered to enable/disable the right filters on modify().
    std::unordered_map<EventLoopHandle, EventInterest> interests_;
};

}  // namespace realm::network
