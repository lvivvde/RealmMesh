add_library(realm_gateway_service
    src/client_session_registry.cpp
    src/gateway_config_loader.cpp
    src/gateway_runtime.cpp
    src/gateway_server.cpp
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
