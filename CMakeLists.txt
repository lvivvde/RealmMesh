cmake_minimum_required(VERSION 3.20)

project(
    RealmMesh
    VERSION 0.1.0
    DESCRIPTION "A frame-driven C++ game server"
    LANGUAGES C CXX
)

add_subdirectory(third_party/lua)
add_subdirectory(third_party/sol2)
add_subdirectory(third_party/sodium)
add_subdirectory(third_party/json)
add_subdirectory(third_party/httplib)
add_subdirectory(third_party/msquic)
add_subdirectory(third_party/protobuf)
add_subdirectory(third_party/spdlog)
add_subdirectory(proto)
add_subdirectory(framework/concurrency)
add_subdirectory(framework/observability)
add_subdirectory(framework/scripting)
add_subdirectory(framework/scheduler)
add_subdirectory(framework/service)
add_subdirectory(framework/network)
add_subdirectory(framework/cluster)
add_subdirectory(framework/service_host)
add_subdirectory(game/common)
add_subdirectory(game/gateway)
add_subdirectory(apps/login)
add_subdirectory(apps/realm)
add_subdirectory(apps/gateway)
add_subdirectory(apps/mesh_host)

add_executable(realmmesh
    src/main.cpp
)

target_link_libraries(realmmesh PRIVATE
    realm_observability
    realm_scheduler
)

target_compile_features(realmmesh PRIVATE cxx_std_20)
set_target_properties(realmmesh PROPERTIES
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(realmmesh PRIVATE /W4 /permissive-)
else()
    target_compile_options(realmmesh PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
endif()

include(CTest)
if(BUILD_TESTING)
    include(FetchContent)

    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
        URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(googletest)

    add_subdirectory(tests/cpp)
endif()
