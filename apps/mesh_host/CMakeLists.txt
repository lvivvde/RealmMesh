# 服务清单单列为库:realm_mesh 与集成测试共用同一份服务名 → 模块注册表。
add_library(realm_mesh_modules service_modules.cpp)

target_include_directories(realm_mesh_modules
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(realm_mesh_modules
    PUBLIC
        realm_service_host
    PRIVATE
        realm_game_realm
        realm_gateway_service
        realm_login_verify_service
        realm_queue_service
)

add_executable(realm_mesh main.cpp)

target_link_libraries(realm_mesh PRIVATE
    realm_mesh_modules
    realm_scheduler
    realm_service_host
)

foreach(target IN ITEMS realm_mesh_modules realm_mesh)
    target_compile_features(${target} PRIVATE cxx_std_20)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
endforeach()

set_target_properties(realm_mesh PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
