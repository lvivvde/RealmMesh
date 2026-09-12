# 测试目标按源码树 1:1 拆分,每个目录的 CMakeLists 只注册自己的目标。
# 公共注册逻辑见 ../cmake/test_helpers.cmake。
#
# REALMMESH_TEST_WORKING_DIRECTORY 必须在这里(而不是各子目录)取值:它是拆分前
# 隐式的 tests/cpp 构建目录,所有 gtest 用例的相对路径语义都依赖它保持不变。
set(REALMMESH_TEST_WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/test_helpers.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/test-tls-identity.cmake)

add_subdirectory(framework/scheduler)
add_subdirectory(framework/concurrency)
add_subdirectory(framework/scripting)
add_subdirectory(framework/service)
add_subdirectory(framework/observability)
add_subdirectory(framework/cluster)
add_subdirectory(framework/service_host)
add_subdirectory(framework/network/codec)
add_subdirectory(framework/network/client)
add_subdirectory(framework/network/tcp)
add_subdirectory(framework/network/reactor)
add_subdirectory(framework/network/transport)
add_subdirectory(framework/network/tls)
add_subdirectory(framework/network/http)
add_subdirectory(framework/network/quic)
add_subdirectory(game/common)
add_subdirectory(game/gateway)
add_subdirectory(game/login_verify)

# 跨切面:不属于任何单个源码目录的测试入口。
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/net-backend-contract.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/dev-services-tests.cmake)
