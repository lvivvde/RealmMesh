add_library(realm_concurrency INTERFACE)

add_library(RealmMesh::Concurrency ALIAS realm_concurrency)

target_include_directories(realm_concurrency
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(realm_concurrency INTERFACE cxx_std_20)
