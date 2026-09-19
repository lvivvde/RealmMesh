# 负载发生器(spec #48):核心库 + 同源 CLI。核心库可被 integration
# 测试链接(机器人经真实服务 HTTPS 回环驱动);不入 ServiceType。
add_library(realm_loadgen_core
    src/stats.cpp
    src/metrics_scrape.cpp
    src/login_chain_adapter.cpp
    src/login_chain_metrics.cpp
    src/robot.cpp
    src/loadgen.cpp
)
add_library(RealmMesh::LoadgenCore ALIAS realm_loadgen_core)

target_include_directories(realm_loadgen_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(realm_loadgen_core PUBLIC
    RealmMesh::Client
    RealmMesh::Network
    RealmMesh::GameCommon
)
target_compile_features(realm_loadgen_core PUBLIC cxx_std_20)
set_target_properties(realm_loadgen_core PROPERTIES CXX_EXTENSIONS OFF)

add_executable(realm_mesh_loadgen main.cpp)
target_link_libraries(realm_mesh_loadgen PRIVATE realm_loadgen_core)
target_compile_features(realm_mesh_loadgen PRIVATE cxx_std_20)
set_target_properties(realm_mesh_loadgen PROPERTIES
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(MSVC)
    target_compile_options(realm_loadgen_core PRIVATE /W4 /permissive-)
    target_compile_options(realm_mesh_loadgen PRIVATE /W4 /permissive-)
else()
    target_compile_options(realm_loadgen_core PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
    target_compile_options(realm_mesh_loadgen PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
endif()
