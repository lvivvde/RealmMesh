add_library(realm_network
    src/core/byte_buffer.cpp
    src/codec/length_field_codec.cpp
    src/reactor/epoll_event_loop.cpp
    src/transport/transport_factory.cpp
    src/tcp/tcp_connection.cpp
    src/tcp/tcp_listener.cpp
    src/tcp/tcp_transport.cpp
    src/udp/udp_transport.cpp
    src/kcp/kcp_security.cpp
    src/kcp/kcp_transport.cpp
)

add_library(RealmMesh::Network ALIAS realm_network)

target_include_directories(realm_network
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(realm_network PUBLIC cxx_std_20)
target_link_libraries(realm_network PUBLIC
    Kcp::Kcp
    Sodium::Sodium
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
