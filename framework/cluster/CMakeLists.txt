add_library(realm_cluster
    src/etcd_service_registry.cpp
    src/service_bootstrap.cpp
    src/service_publisher.cpp
    src/service_resolver.cpp
)
add_library(RealmMesh::Cluster ALIAS realm_cluster)

target_include_directories(realm_cluster
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(realm_cluster PUBLIC cxx_std_20)
set_target_properties(realm_cluster PROPERTIES CXX_EXTENSIONS OFF)

target_link_libraries(realm_cluster
    PUBLIC realm_network
    PRIVATE httplib::httplib nlohmann_json::nlohmann_json
)

if(MSVC)
    target_compile_options(realm_cluster PRIVATE /W4 /permissive-)
else()
    target_compile_options(realm_cluster PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
    )
endif()
