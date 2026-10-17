add_library(realm_service_host
    src/layered_config_loader.cpp
    src/mesh_host.cpp
    src/service_config_lua.cpp
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

# 只依赖 framework/(ADR-0014):服务模块住在各自服务目录,由进程入口按名
# 注册。
target_link_libraries(realm_service_host PUBLIC
    realm_cluster
    realm_edge
    realm_observability
)
# Lua 合并脚本与宿主级/接入面节段解析只用在 layered_config_loader.cpp 与
# service_config_lua.cpp,拓扑装载只用在 startup_topology_lua.cpp;公共头
# 只带配置值与拓扑描述(#126、#127)。
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
