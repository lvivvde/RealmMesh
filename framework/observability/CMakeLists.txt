add_library(realm_observability
    src/logger.cpp
)

add_library(RealmMesh::Observability ALIAS realm_observability)

target_include_directories(realm_observability
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

find_package(Threads REQUIRED)
target_link_libraries(realm_observability
    PUBLIC realm_concurrency
    PRIVATE
        Threads::Threads
        httplib::httplib
        nlohmann_json::nlohmann_json
        spdlog::spdlog
)

target_compile_features(realm_observability PUBLIC cxx_std_20)
set_target_properties(realm_observability PROPERTIES CXX_EXTENSIONS OFF)

if(MSVC)
    target_compile_options(realm_observability PRIVATE /W4 /permissive-)
else()
    target_compile_options(realm_observability PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
endif()
