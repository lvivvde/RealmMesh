add_library(realm_queue_service
    src/queue_config.cpp
    src/queue_core.cpp
    src/queue_handler.cpp
    src/queue_service.cpp
    src/queue_store.cpp
    src/queue_number.cpp
)

add_library(RealmMesh::QueueService ALIAS realm_queue_service)

target_include_directories(realm_queue_service
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(realm_queue_service PUBLIC
    realm_cluster
    realm_game_common
    realm_network
    realm_observability
    realm_scripting
)
# etcd v3 应答解析只在 queue_store 实现(.cpp):私有依赖。
target_link_libraries(realm_queue_service PRIVATE nlohmann_json::nlohmann_json)

target_compile_features(realm_queue_service PUBLIC cxx_std_20)
set_target_properties(realm_queue_service PROPERTIES
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(realm_queue_service PRIVATE /W4 /permissive-)
else()
    target_compile_options(realm_queue_service PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
endif()
