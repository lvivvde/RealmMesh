#include "realmmesh/network/reactor/event_loop.hpp"

#if defined(REALM_NETWORK_EVENT_LOOP_BACKEND_EPOLL)
#include "realmmesh/network/reactor/epoll_event_loop.hpp"
#elif defined(REALM_NETWORK_EVENT_LOOP_BACKEND_KQUEUE)
#error "kqueue event loop backend is not implemented yet (RealmMesh issue #6)"
#elif defined(REALM_NETWORK_EVENT_LOOP_BACKEND_IOCP)
#error "iocp event loop backend is not implemented (see ADR-0002)"
#else
#error "no event loop backend selected for this platform (see ADR-0001)"
#endif

#include <memory>
#include <utility>

namespace realm::network {

std::unique_ptr<IEventLoop> make_default_event_loop() {
#if defined(REALM_NETWORK_EVENT_LOOP_BACKEND_EPOLL)
    return std::make_unique<EpollEventLoop>();
#endif
}

}  // namespace realm::network
