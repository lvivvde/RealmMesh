cmake_minimum_required(VERSION 3.20)

project(
    RealmMesh
    VERSION 0.1.0
    DESCRIPTION "A frame-driven C++ game server"
    LANGUAGES C CXX
)

add_subdirectory(third_party/lua)
add_subdirectory(third_party/sol2)
add_subdirectory(third_party/kcp)
add_subdirectory(third_party/sodium)
add_subdirectory(third_party/json)
add_subdirectory(third_party/httplib)
add_subdirectory(framework/concurrency)
add_subdirectory(framework/scripting)
add_subdirectory(framework/scheduler)
add_subdirectory(framework/service)
add_subdirectory(framework/network)
add_subdirectory(framework/cluster)
add_subdirectory(game/common)
add_subdirectory(game/gateway)
add_subdirectory(apps/login)
add_subdirectory(apps/realm)
add_subdirectory(apps/gateway)

add_executable(realmmesh
    src/main.cpp
)

target_link_libraries(realmmesh PRIVATE
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
        GIT_REPOSITORY https://chromium.googlesource.com/external/github.com/google/googletest
        GIT_TAG v1.17.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(googletest)

    add_subdirectory(tests/cpp)
endif()
