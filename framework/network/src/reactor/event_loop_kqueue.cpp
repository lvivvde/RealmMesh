#include "realmmesh/network/reactor/kqueue_event_loop.hpp"

#include <cerrno>
#include <limits>
#include <stdexcept>
#include <sys/event.h>
#include <sys/time.h>
#include <system_error>
#include <unistd.h>
#include <unordered_map>
#include <utility>

namespace realm::network {
namespace {

std::int16_t to_kqueue_filter(EventInterest interest) noexcept {
    return interest == EventInterest::Write ? EVFILT_WRITE : EVFILT_READ;
}

bool wants(EventInterest interest, EventInterest direction) noexcept {
    return (static_cast<std::uint8_t>(interest) &
            static_cast<std::uint8_t>(direction)) != 0U;
}

// Adds or deletes a single filter. Deleting a filter that was never registered
// is not an error: a handle registered read-only has no write filter to remove.
void apply_filter(
    int kqueue_descriptor,
    EventLoopHandle handle,
    EventInterest direction,
    bool enabled) {
    struct kevent change {};
    EV_SET(
        &change,
        static_cast<std::uintptr_t>(handle),
        to_kqueue_filter(direction),
        enabled ? (EV_ADD | EV_ENABLE) : EV_DELETE,
        0,
        0,
        nullptr);

    if (::kevent(kqueue_descriptor, &change, 1, nullptr, 0, nullptr) < 0) {
        if (!enabled && errno == ENOENT) {
            return;
        }
        throw std::system_error(errno, std::generic_category(), "kevent");
    }
}

void apply_interest(
    int kqueue_descriptor, EventLoopHandle handle, EventInterest interest) {
    apply_filter(
        kqueue_descriptor,
        handle,
        EventInterest::Read,
        wants(interest, EventInterest::Read));
    apply_filter(
        kqueue_descriptor,
        handle,
        EventInterest::Write,
        wants(interest, EventInterest::Write));
}

}  // namespace

KqueueEventLoop::KqueueEventLoop() : kqueue_descriptor_(::kqueue()) {
    if (kqueue_descriptor_ < 0) {
        throw std::system_error(errno, std::generic_category(), "kqueue");
    }
}

KqueueEventLoop::~KqueueEventLoop() {
    close();
}

KqueueEventLoop::KqueueEventLoop(KqueueEventLoop&& other) noexcept
    : kqueue_descriptor_(std::exchange(other.kqueue_descriptor_, -1)),
      interests_(std::move(other.interests_)) {}

KqueueEventLoop& KqueueEventLoop::operator=(KqueueEventLoop&& other) noexcept {
    if (this != &other) {
        close();
        kqueue_descriptor_ = std::exchange(other.kqueue_descriptor_, -1);
        interests_ = std::move(other.interests_);
    }
    return *this;
}

void KqueueEventLoop::add(EventLoopHandle handle, EventInterest interest) {
    if (!interests_.emplace(handle, interest).second) {
        throw std::invalid_argument("handle is already registered");
    }
    apply_interest(kqueue_descriptor_, handle, interest);
}

void KqueueEventLoop::modify(EventLoopHandle handle, EventInterest interest) {
    const auto entry = interests_.find(handle);
    if (entry == interests_.end()) {
        throw std::invalid_argument("handle is not registered");
    }
    entry->second = interest;
    apply_interest(kqueue_descriptor_, handle, interest);
}

void KqueueEventLoop::remove(EventLoopHandle handle) {
    if (interests_.erase(handle) == 0U) {
        throw std::invalid_argument("handle is not registered");
    }
    apply_filter(kqueue_descriptor_, handle, EventInterest::Read, false);
    apply_filter(kqueue_descriptor_, handle, EventInterest::Write, false);
}

std::vector<ReadyEvent> KqueueEventLoop::wait(std::chrono::milliseconds timeout) {
    if (timeout.count() < -1 ||
        timeout.count() > std::numeric_limits<int>::max()) {
        throw std::invalid_argument("kqueue timeout is outside supported range");
    }

    constexpr int max_events = 64;
    struct kevent events[max_events]{};

    struct timespec duration {};
    duration.tv_sec = static_cast<time_t>(timeout.count() / 1000);
    duration.tv_nsec =
        static_cast<long>((timeout.count() % 1000) * 1'000'000L);
    const bool block_forever = timeout.count() < 0;

    const int ready_count = ::kevent(
        kqueue_descriptor_,
        nullptr,
        0,
        events,
        max_events,
        block_forever ? nullptr : &duration);
    if (ready_count < 0) {
        if (errno == EINTR) {
            return {};
        }
        throw std::system_error(errno, std::generic_category(), "kevent");
    }

    // One handle can report both filters, while ReadyEvent carries both
    // directions in a single record -- the same shape epoll produces.
    std::vector<ReadyEvent> ready_events;
    ready_events.reserve(static_cast<std::size_t>(ready_count));
    std::unordered_map<EventLoopHandle, std::size_t> index_by_handle;

    for (int index = 0; index < ready_count; ++index) {
        const auto& event = events[index];
        const auto handle = static_cast<EventLoopHandle>(event.ident);

        auto position = index_by_handle.find(handle);
        if (position == index_by_handle.end()) {
            position = index_by_handle
                           .emplace(handle, ready_events.size())
                           .first;
            ready_events.push_back({handle, false, false, false, false});
        }
        auto& ready = ready_events[position->second];

        if (event.filter == EVFILT_READ) {
            ready.readable = true;
        } else if (event.filter == EVFILT_WRITE) {
            ready.writable = true;
        }
        if ((event.flags & EV_EOF) != 0) {
            ready.peer_closed = true;
        }
        // EV_ERROR with data == 0 only reports a successful change request.
        if ((event.flags & EV_ERROR) != 0 && event.data != 0) {
            ready.error = true;
        }
    }
    return ready_events;
}

void KqueueEventLoop::close() noexcept {
    if (kqueue_descriptor_ >= 0) {
        ::close(kqueue_descriptor_);
        kqueue_descriptor_ = -1;
    }
    interests_.clear();
}

}  // namespace realm::network
