#include "realmmesh/network/reactor/epoll_event_loop.hpp"

#include <cerrno>
#include <limits>
#include <stdexcept>
#include <sys/epoll.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace realm::network {
namespace {

std::uint32_t to_epoll_events(EventInterest interest) noexcept {
    const auto bits = static_cast<std::uint8_t>(interest);
    std::uint32_t events = EPOLLRDHUP;
    if ((bits & static_cast<std::uint8_t>(EventInterest::Read)) != 0U) {
        events |= EPOLLIN;
    }
    if ((bits & static_cast<std::uint8_t>(EventInterest::Write)) != 0U) {
        events |= EPOLLOUT;
    }
    return events;
}

void control_epoll(
    int epoll_descriptor,
    int operation,
    int watched_descriptor,
    EventInterest interest) {
    epoll_event event{};
    event.events = to_epoll_events(interest);
    event.data.fd = watched_descriptor;

    if (::epoll_ctl(epoll_descriptor, operation, watched_descriptor, &event) < 0) {
        throw std::system_error(errno, std::generic_category(), "epoll_ctl");
    }
}

}  // namespace

EpollEventLoop::EpollEventLoop() : descriptor_(::epoll_create1(EPOLL_CLOEXEC)) {
    if (descriptor_ < 0) {
        throw std::system_error(errno, std::generic_category(), "epoll_create1");
    }
}

EpollEventLoop::~EpollEventLoop() {
    close();
}

EpollEventLoop::EpollEventLoop(EpollEventLoop&& other) noexcept
    : descriptor_(std::exchange(other.descriptor_, -1)) {}

EpollEventLoop& EpollEventLoop::operator=(EpollEventLoop&& other) noexcept {
    if (this != &other) {
        close();
        descriptor_ = std::exchange(other.descriptor_, -1);
    }
    return *this;
}

void EpollEventLoop::add(int descriptor, EventInterest interest) {
    control_epoll(descriptor_, EPOLL_CTL_ADD, descriptor, interest);
}

void EpollEventLoop::modify(int descriptor, EventInterest interest) {
    control_epoll(descriptor_, EPOLL_CTL_MOD, descriptor, interest);
}

void EpollEventLoop::remove(int descriptor) {
    if (::epoll_ctl(descriptor_, EPOLL_CTL_DEL, descriptor, nullptr) < 0) {
        throw std::system_error(errno, std::generic_category(), "epoll_ctl(DEL)");
    }
}

std::vector<ReadyEvent> EpollEventLoop::wait(std::chrono::milliseconds timeout) {
    constexpr int max_events = 64;
    epoll_event events[max_events]{};

    if (timeout.count() < -1 ||
        timeout.count() > std::numeric_limits<int>::max()) {
        throw std::invalid_argument("epoll timeout is outside supported range");
    }

    const int ready_count = ::epoll_wait(
        descriptor_,
        events,
        max_events,
        static_cast<int>(timeout.count()));
    if (ready_count < 0) {
        if (errno == EINTR) {
            return {};
        }
        throw std::system_error(errno, std::generic_category(), "epoll_wait");
    }

    std::vector<ReadyEvent> ready_events;
    ready_events.reserve(static_cast<std::size_t>(ready_count));
    for (int index = 0; index < ready_count; ++index) {
        const auto flags = events[index].events;
        ready_events.push_back({
            events[index].data.fd,
            (flags & (EPOLLIN | EPOLLPRI)) != 0U,
            (flags & EPOLLOUT) != 0U,
            (flags & (EPOLLRDHUP | EPOLLHUP)) != 0U,
            (flags & EPOLLERR) != 0U,
        });
    }
    return ready_events;
}

void EpollEventLoop::close() noexcept {
    if (descriptor_ >= 0) {
        ::close(descriptor_);
        descriptor_ = -1;
    }
}

}  // namespace realm::network
