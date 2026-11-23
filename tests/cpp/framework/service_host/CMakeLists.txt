realm_add_gtest(service_host_test
    SOURCES
        layered_config_loader_test.cpp
        mesh_host_e2e_test.cpp
        mesh_host_test.cpp
        mode2_test.cpp
        observability_artifacts_test.cpp
        service_host_test.cpp
        startup_topology_test.cpp
    INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/tests/cpp/support/include
    LIBS realmmesh_test_mongodb
        httplib::httplib
        nlohmann_json::nlohmann_json
        realm_mesh_modules
        realm_game_realm
        # layered_config_loader_test 直接驱动 player_data 的 Lua 解析入口(#126)。
        realm_scripting
        realm_service_host
    DEFINITIONS
        REALMMESH_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        REALMMESH_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        REALMMESH_TEST_TLS_CERTIFICATE="${REALMMESH_TEST_TLS_CERTIFICATE}"
        REALMMESH_TEST_TLS_PRIVATE_KEY="${REALMMESH_TEST_TLS_PRIVATE_KEY}"
    DEPENDS realmmesh_test_tls_identity
    # E2E/Mode2 用例占用真实固定端口且依赖 setenv,须与其他测试串行。
    # 整目标属集成测试:不进快速子集,见 test_helpers.cmake。
    LABELS integration
    DISCOVER_PROPERTIES RUN_SERIAL TRUE
)

# 普通配置头不经传递 include 引入 sol2(#126):编译即断言。
realm_add_gtest(config_headers_test
    SOURCES config_headers_test.cpp
    LIBS realm_service_host
        realm_gateway_service
        realm_game_realm
        realm_login_verify_service
        realm_queue_service
)

# 真实 configs/ 树的生产加载冒烟:不占端口、不起进程,留在快速子集。
# 用例直接驱动 LuaRuntime:显式链接 realm_scripting(#126);
# 各服务节段经各自服务模块的装载入口读取(ADR-0014)。
realm_add_gtest(configs_load_smoke_test
    SOURCES configs_load_smoke_test.cpp
    INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/tests/cpp/support/include
    LIBS realm_service_host
        realm_game_chat
        realm_gateway_service
        realm_game_realm
        realm_login_verify_service
        realm_queue_service
        realm_scripting
    DEFINITIONS
        REALMMESH_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        REALMMESH_TEST_TLS_CERTIFICATE="${REALMMESH_TEST_TLS_CERTIFICATE}"
        REALMMESH_TEST_TLS_PRIVATE_KEY="${REALMMESH_TEST_TLS_PRIVATE_KEY}"
    DEPENDS realmmesh_test_tls_identity
)

# 服务模块注册表(ADR-0014):按名查找与非法/重复名拒绝，不起进程。
realm_add_gtest(service_module_registry_test
    SOURCES service_module_registry_test.cpp
    LIBS realm_service_host
)

# 拓扑装载入口(#127):调用方只拿 ServiceSpec 列表与错误，不碰 Lua。
realm_add_gtest(load_topology_test
    SOURCES load_topology_test.cpp
    LIBS realm_service_host
)
