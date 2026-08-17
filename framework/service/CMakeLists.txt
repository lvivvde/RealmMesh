add_library(realm_service
    src/service_lifecycle.cpp
)

add_library(RealmMesh::Service ALIAS realm_service)

target_include_directories(realm_service
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(realm_service PUBLIC cxx_std_20)
set_target_properties(realm_service PROPERTIES
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(realm_service PRIVATE /W4 /permissive-)
else()
    target_compile_options(realm_service PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
endif()
