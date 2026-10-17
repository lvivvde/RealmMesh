add_library(realm_edge
    src/edge_connection_table.cpp
    src/edge_runtime.cpp
    src/edge_source_address.cpp
)

add_library(RealmMesh::Edge ALIAS realm_edge)

target_include_directories(realm_edge
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(realm_edge PUBLIC
    realm_concurrency
    realm_network
    realm_observability
)

find_package(Threads REQUIRED)
target_link_libraries(realm_edge PUBLIC Threads::Threads)

target_compile_features(realm_edge PUBLIC cxx_std_20)
set_target_properties(realm_edge PROPERTIES
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(realm_edge PRIVATE /W4 /permissive-)
else()
    target_compile_options(realm_edge PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
endif()
