add_library(realm_gateway_service
    src/account_fetch_port.cpp
    src/edge_attach_chain.cpp
    src/edge_fetch_scheduler.cpp
    src/edge_session_pipeline.cpp
    src/edge_session_table.cpp
    src/gateway_config_loader.cpp
    src/gateway_login_config.cpp
    src/gateway_login_pipeline.cpp
    src/gateway_primary_transport.cpp
    src/gateway_runtime.cpp
)

add_library(RealmMesh::GatewayService ALIAS realm_gateway_service)

target_include_directories(realm_gateway_service
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(realm_gateway_service PUBLIC
    realm_concurrency
    realm_cluster
    realm_game_common
    realm_network
    realm_observability
    realm_scripting
)

find_package(Threads REQUIRED)
target_link_libraries(realm_gateway_service PUBLIC Threads::Threads)

target_compile_features(realm_gateway_service PUBLIC cxx_std_20)
set_target_properties(realm_gateway_service PROPERTIES
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(realm_gateway_service PRIVATE /W4 /permissive-)
else()
    target_compile_options(realm_gateway_service PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
endif()
