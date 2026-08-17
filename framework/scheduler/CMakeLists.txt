add_library(realm_scheduler
    src/frame_scheduler.cpp
)

add_library(RealmMesh::Scheduler ALIAS realm_scheduler)

target_include_directories(realm_scheduler
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(realm_scheduler PUBLIC cxx_std_20)
set_target_properties(realm_scheduler PROPERTIES
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(realm_scheduler PRIVATE /W4 /permissive-)
else()
    target_compile_options(realm_scheduler PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
endif()
