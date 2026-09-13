add_library(realm_network
    src/core/byte_buffer.cpp
    src/codec/length_field_codec.cpp
    src/client/preferred_transport_connector.cpp
    src/reactor/event_loop_factory.cpp
    src/transport/transport_capabilities.cpp
    src/transport/transport_factory.cpp
    src/tcp/tcp_listener.cpp
    src/tcp/tcp_socket.cpp
    src/http/http1_parser.cpp
    src/http/http1_response.cpp
    src/http/http1_response_parser.cpp
    src/http/http_server.cpp
    src/tls/tls_connection.cpp
    src/tls/tls_server_context.cpp
    src/tls/tls_tcp_transport.cpp
)

# The QUIC transport is built only where MsQuic was found (Linux, ADR-0002).
# Elsewhere the Transport Factory serves the TLS/TCP fallback only. CMake
# supplies both the capability value and the QUIC construction seam, so the
# transport layer itself carries no preprocessor branch (ADR-0001).
if(REALM_MESH_MSQUIC_FOUND)
    target_sources(realm_network PRIVATE
        src/quic/quic_transport.cpp
        src/quic/quic_transport_factory.cpp
    )
    target_compile_definitions(realm_network PRIVATE
        REALM_NETWORK_QUIC_AVAILABLE=1
    )
    message(STATUS "realm_network: QUIC transport enabled (MsQuic found)")
else()
    target_sources(realm_network PRIVATE
        src/quic/quic_transport_factory_unavailable.cpp
    )
    target_compile_definitions(realm_network PRIVATE
        REALM_NETWORK_QUIC_AVAILABLE=0
    )
    message(STATUS "realm_network: QUIC transport disabled (no MsQuic, ADR-0002)")
endif()

# Platform Event Loop backend (ADR-0001): exactly one backend implementation is
# compiled in, and the TCP layer's POSIX differences live in a per-platform TU.
# REALMMESH_NET_BACKEND selects it; `auto` follows the platform, an explicit
# value must match it, and iocp is recognised but unimplemented (ADR-0002).
set(REALMMESH_NET_BACKEND "auto" CACHE STRING
    "Event Loop backend: auto | epoll | kqueue | iocp (see ADR-0002)")
set_property(CACHE REALMMESH_NET_BACKEND PROPERTY STRINGS auto epoll kqueue iocp)

include("${PROJECT_SOURCE_DIR}/cmake/RealmMeshNetBackend.cmake")
realm_mesh_select_net_backend(
    "${CMAKE_SYSTEM_NAME}"
    "${REALMMESH_NET_BACKEND}"
    REALM_NETWORK_EVENT_LOOP_BACKEND
)

set(REALM_NETWORK_EVENT_LOOP_SOURCES)
set(REALM_NETWORK_TCP_PLATFORM_SOURCES)
if(REALM_NETWORK_EVENT_LOOP_BACKEND STREQUAL "epoll")
    set(REALM_NETWORK_EVENT_LOOP_SOURCES src/reactor/event_loop_epoll.cpp)
    set(REALM_NETWORK_TCP_PLATFORM_SOURCES src/tcp/tcp_platform_linux.cpp)
elseif(REALM_NETWORK_EVENT_LOOP_BACKEND STREQUAL "kqueue")
    set(REALM_NETWORK_EVENT_LOOP_SOURCES src/reactor/event_loop_kqueue.cpp)
    set(REALM_NETWORK_TCP_PLATFORM_SOURCES src/tcp/tcp_platform_darwin.cpp)
endif()
string(TOUPPER "${REALM_NETWORK_EVENT_LOOP_BACKEND}" REALM_NETWORK_EVENT_LOOP_BACKEND_UPPER)
message(STATUS
    "realm_network Event Loop backend: ${REALM_NETWORK_EVENT_LOOP_BACKEND} "
    "(REALMMESH_NET_BACKEND=${REALMMESH_NET_BACKEND})")
target_sources(realm_network PRIVATE
    ${REALM_NETWORK_EVENT_LOOP_SOURCES}
    ${REALM_NETWORK_TCP_PLATFORM_SOURCES}
)
target_compile_definitions(realm_network PRIVATE
    REALM_NETWORK_EVENT_LOOP_BACKEND_${REALM_NETWORK_EVENT_LOOP_BACKEND_UPPER}
)

find_package(OpenSSL 3.0 REQUIRED COMPONENTS SSL Crypto)

add_library(RealmMesh::Network ALIAS realm_network)

target_include_directories(realm_network
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Debian-style OpenSSL development sysroots keep generated configuration
# headers in an architecture-specific include directory. FindOpenSSL exposes
# only the common include root, so propagate the companion directory when it
# exists as well.
if(CMAKE_LIBRARY_ARCHITECTURE AND
   EXISTS "${OPENSSL_INCLUDE_DIR}/${CMAKE_LIBRARY_ARCHITECTURE}/openssl/opensslconf.h")
    target_include_directories(realm_network SYSTEM PUBLIC
        "${OPENSSL_INCLUDE_DIR}/${CMAKE_LIBRARY_ARCHITECTURE}"
    )
endif()

target_compile_features(realm_network PUBLIC cxx_std_20)
target_link_libraries(realm_network PUBLIC
    Sodium::Sodium
    OpenSSL::SSL
    OpenSSL::Crypto
)
if(REALM_MESH_MSQUIC_FOUND)
    target_link_libraries(realm_network PUBLIC MsQuic::MsQuic)
endif()
target_link_libraries(realm_network PRIVATE
    RealmMesh::Observability
)
set_target_properties(realm_network PROPERTIES
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(realm_network PRIVATE /W4 /permissive-)
else()
    target_compile_options(realm_network PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
endif()
