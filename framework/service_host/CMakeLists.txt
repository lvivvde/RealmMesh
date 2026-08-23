add_library(realm_service_host
    src/startup_topology.cpp
)

add_library(RealmMesh::ServiceHost ALIAS realm_service_host)

target_include_directories(realm_service_host
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(realm_service_host PUBLIC cxx_std_20)
set_target_properties(realm_service_host PROPERTIES CXX_EXTENSIONS OFF)

if(MSVC)
    target_compile_options(realm_service_host PRIVATE /W4 /permissive-)
else()
    target_compile_options(realm_service_host PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
endif()
