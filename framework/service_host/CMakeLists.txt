add_library(realm_service_host
    src/frame_service_modules.cpp
    src/layered_config_loader.cpp
    src/mesh_host.cpp
    src/service_config_lua.cpp
    src/service_frame.cpp
    src/service_host.cpp
    src/service_module.cpp
    src/startup_topology.cpp
    src/startup_topology_lua.cpp
)

add_library(RealmMesh::ServiceHost ALIAS realm_service_host)

target_include_directories(realm_service_host
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(realm_service_host PUBLIC
    realm_cluster
    realm_edge
    realm_game_common
    realm_gateway_service
    realm_game_realm
    realm_observability
)
# Lua 合并脚本与宿主级/接入面节段解析只用在 layered_config_loader.cpp 与
# service_config_lua.cpp,Gateway/Realm 节段解析只用在
# frame_service_modules.cpp,拓扑装载只用在 startup_topology_lua.cpp;
# 公共头只带配置值与拓扑描述(#126、#127)。服务模块按名注册由进程入口
# 装配,本库不再链接 login_verify/queue(ADR-0014)。
target_link_libraries(realm_service_host PRIVATE realm_scripting)

target_compile_features(realm_service_host PUBLIC cxx_std_20)
set_target_properties(realm_service_host PROPERTIES CXX_EXTENSIONS OFF)

if(MSVC)
    target_compile_options(realm_service_host PRIVATE /W4 /permissive-)
else()
    target_compile_options(realm_service_host PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
endif()
