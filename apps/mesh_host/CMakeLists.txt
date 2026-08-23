add_executable(realm_mesh main.cpp)

target_link_libraries(realm_mesh PRIVATE
    realm_scheduler
    realm_scripting
    realm_service_host
)

target_compile_features(realm_mesh PRIVATE cxx_std_20)
set_target_properties(realm_mesh PROPERTIES
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(MSVC)
    target_compile_options(realm_mesh PRIVATE /W4 /permissive-)
else()
    target_compile_options(realm_mesh PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
    )
endif()
