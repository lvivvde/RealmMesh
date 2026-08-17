add_library(realm_cluster INTERFACE)
add_library(RealmMesh::Cluster ALIAS realm_cluster)

target_include_directories(realm_cluster
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(realm_cluster INTERFACE cxx_std_20)

target_link_libraries(realm_cluster INTERFACE realm_network)
