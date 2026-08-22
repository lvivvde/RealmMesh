add_library(realm_network
    src/core/byte_buffer.cpp
    src/codec/length_field_codec.cpp
    src/client/preferred_transport_connector.cpp
    src/reactor/epoll_event_loop.cpp
    src/quic/quic_transport.cpp
    src/transport/transport_factory.cpp
    src/tcp/tcp_listener.cpp
    src/tls/tls_connection.cpp
    src/tls/tls_server_context.cpp
    src/tls/tls_tcp_transport.cpp
)

find_package(OpenSSL 3.0 REQUIRED COMPONENTS SSL Crypto)

add_library(RealmMesh::Network ALIAS realm_network)

target_include_directories(realm_network
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(realm_network PUBLIC cxx_std_20)
target_link_libraries(realm_network PUBLIC
    Sodium::Sodium
    OpenSSL::SSL
    OpenSSL::Crypto
    MsQuic::MsQuic
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
