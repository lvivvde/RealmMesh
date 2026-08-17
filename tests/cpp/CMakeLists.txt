add_executable(frame_scheduler_test
    framework/scheduler/frame_scheduler_test.cpp
)

target_link_libraries(frame_scheduler_test PRIVATE
    GTest::gtest_main
    realm_scheduler
)

target_compile_features(frame_scheduler_test PRIVATE cxx_std_20)

include(GoogleTest)
gtest_discover_tests(frame_scheduler_test)

add_executable(bounded_queue_test
    framework/concurrency/bounded_queue_test.cpp
)

target_link_libraries(bounded_queue_test PRIVATE
    GTest::gtest_main
    realm_concurrency
)

target_compile_features(bounded_queue_test PRIVATE cxx_std_20)

gtest_discover_tests(bounded_queue_test)

add_executable(lua_runtime_test
    framework/scripting/lua_runtime_test.cpp
)

target_link_libraries(lua_runtime_test PRIVATE
    GTest::gtest_main
    realm_scripting
)

target_compile_features(lua_runtime_test PRIVATE cxx_std_20)

gtest_discover_tests(lua_runtime_test)

add_executable(service_lifecycle_test
    framework/service/service_lifecycle_test.cpp
)

target_link_libraries(service_lifecycle_test PRIVATE
    GTest::gtest_main
    realm_service
)

target_compile_features(service_lifecycle_test PRIVATE cxx_std_20)

gtest_discover_tests(service_lifecycle_test)

add_executable(session_ticket_test
    game/common/session_ticket_test.cpp
)

target_link_libraries(session_ticket_test PRIVATE
    GTest::gtest_main
    realm_game_common
)

target_compile_features(session_ticket_test PRIVATE cxx_std_20)

gtest_discover_tests(session_ticket_test)

add_executable(three_stage_flow_test
    game/common/three_stage_flow_test.cpp
)

target_link_libraries(three_stage_flow_test PRIVATE
    GTest::gtest_main
    realm_game_common
    realm_network
)
target_compile_features(three_stage_flow_test PRIVATE cxx_std_20)
target_compile_definitions(three_stage_flow_test PRIVATE
    REALMMESH_LOGIN_EXECUTABLE="$<TARGET_FILE:realm_login>"
    REALMMESH_REALM_EXECUTABLE="$<TARGET_FILE:realm_character>"
    REALMMESH_GATEWAY_EXECUTABLE="$<TARGET_FILE:realm_gateway>"
)
add_dependencies(three_stage_flow_test
    realm_login
    realm_character
    realm_gateway
)

gtest_discover_tests(three_stage_flow_test
    PROPERTIES RUN_SERIAL TRUE TIMEOUT 15
)

add_executable(network_codec_test
    framework/network/codec/length_field_codec_test.cpp
)

target_link_libraries(network_codec_test PRIVATE
    GTest::gtest_main
    realm_network
)

target_compile_features(network_codec_test PRIVATE cxx_std_20)

gtest_discover_tests(network_codec_test)

add_executable(tcp_listener_test
    framework/network/tcp/tcp_listener_test.cpp
)

target_link_libraries(tcp_listener_test PRIVATE
    GTest::gtest_main
    realm_network
)

target_compile_features(tcp_listener_test PRIVATE cxx_std_20)

gtest_discover_tests(tcp_listener_test)

add_executable(transport_factory_test
    framework/network/transport/transport_factory_test.cpp
)

target_link_libraries(transport_factory_test PRIVATE
    GTest::gtest_main
    realm_network
)

target_compile_features(transport_factory_test PRIVATE cxx_std_20)

gtest_discover_tests(transport_factory_test)

add_executable(kcp_security_test
    framework/network/kcp/kcp_security_test.cpp
)

target_link_libraries(kcp_security_test PRIVATE
    GTest::gtest_main
    realm_network
)

target_compile_features(kcp_security_test PRIVATE cxx_std_20)

gtest_discover_tests(kcp_security_test)

add_executable(gateway_server_test
    game/gateway/gateway_server_test.cpp
)

target_link_libraries(gateway_server_test PRIVATE
    GTest::gtest_main
    realm_gateway_service
)

target_compile_features(gateway_server_test PRIVATE cxx_std_20)

target_compile_definitions(gateway_server_test PRIVATE
    REALMMESH_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

gtest_discover_tests(gateway_server_test)

add_executable(gateway_runtime_test
    game/gateway/gateway_runtime_test.cpp
)

target_link_libraries(gateway_runtime_test PRIVATE
    GTest::gtest_main
    realm_gateway_service
)

target_compile_features(gateway_runtime_test PRIVATE cxx_std_20)

gtest_discover_tests(gateway_runtime_test)

add_executable(client_session_router_test
    game/gateway/client_session_router_test.cpp
)

target_link_libraries(client_session_router_test PRIVATE
    GTest::gtest_main
    realm_gateway_service
)

target_compile_features(client_session_router_test PRIVATE cxx_std_20)

gtest_discover_tests(client_session_router_test)

add_executable(service_registry_test
    framework/cluster/etcd_service_registry_test.cpp
    framework/cluster/service_registry_test.cpp
    support/src/fake_service_registry.cpp
)

target_include_directories(service_registry_test PRIVATE
    ${PROJECT_SOURCE_DIR}/tests/cpp/support/include
)

target_link_libraries(service_registry_test PRIVATE
    GTest::gtest_main
    realm_cluster
    nlohmann_json::nlohmann_json
)

target_compile_features(service_registry_test PRIVATE cxx_std_20)

gtest_discover_tests(service_registry_test)

add_test(
    NAME realmmesh_smoke
    COMMAND realmmesh --frames 2 --tick-rate 1000
)
