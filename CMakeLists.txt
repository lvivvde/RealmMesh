cmake_minimum_required(VERSION 3.20)

project(
    RealmMesh
    VERSION 0.1.0
    DESCRIPTION "A frame-driven C++ game server"
    LANGUAGES CXX
)

add_executable(realmmesh
    src/main.cpp
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
    add_test(
        NAME realmmesh_smoke
        COMMAND realmmesh --frames 2 --tick-rate 1000
    )
endif()
