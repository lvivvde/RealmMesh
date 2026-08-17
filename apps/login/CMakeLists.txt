add_executable(realm_login main.cpp)

target_link_libraries(realm_login PRIVATE
    realm_game_common
    realm_gateway_service
    realm_scheduler
)

target_compile_definitions(realm_login PRIVATE
    REALMMESH_DEFAULT_LOGIN_CONFIG="${PROJECT_SOURCE_DIR}/lua/config/services/login.lua"
)
target_compile_features(realm_login PRIVATE cxx_std_20)
set_target_properties(realm_login PROPERTIES
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(MSVC)
    target_compile_options(realm_login PRIVATE /W4 /permissive-)
else()
    target_compile_options(realm_login PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
    )
endif()
