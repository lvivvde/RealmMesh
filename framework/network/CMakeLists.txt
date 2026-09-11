add_library(realm_network
    src/core/byte_buffer.cpp
    src/codec/length_field_codec.cpp
    src/client/preferred_transport_connector.cpp
    src/reactor/event_loop_epoll.cpp
    src/reactor/event_loop_factory.cpp
    src/transport/transport_capabilities.cpp
    src/transport/transport_factory.cpp
    src/tcp/tcp_listener.cpp
    src/tcp/tcp_socket.cpp
    src/tls/tls_connection.cpp
    src/tls/tls_server_context.cpp
    src/tls/tls_tcp_transport.cpp
)

# The QUIC transport is built only where MsQuic was found (Linux, ADR-0002).
# Elsewhere the Transport Factory serves the TLS/TCP fallback only.
if(REALM_MESH_MSQUIC_FOUND)
    target_sources(realm_network PRIVATE src/quic/quic_transport.cpp)
    target_compile_definitions(realm_network PRIVATE REALM_NETWORK_HAS_QUIC)
    message(STATUS "realm_network: QUIC transport enabled (MsQuic found)")
else()
    message(STATUS "realm_network: QUIC transport disabled (no MsQuic, ADR-0002)")
endif()

# Platform Event Loop backend (ADR-0001): exactly one is compiled in. kqueue
# and iocp are recognized here but their backends are not implemented yet
# (ADR-0002), so the factory rejects them with a pointer back to the ADR.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(REALM_NETWORK_EVENT_LOOP_BACKEND epoll)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(REALM_NETWORK_EVENT_LOOP_BACKEND kqueue)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(REALM_NETWORK_EVENT_LOOP_BACKEND iocp)
else()
    message(FATAL_ERROR
        "realm_network has no Event Loop backend for ${CMAKE_SYSTEM_NAME} "
        "(see ADR-0001)")
endif()
string(TOUPPER "${REALM_NETWORK_EVENT_LOOP_BACKEND}" REALM_NETWORK_EVENT_LOOP_BACKEND_UPPER)
message(STATUS "realm_network Event Loop backend: ${REALM_NETWORK_EVENT_LOOP_BACKEND}")
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
