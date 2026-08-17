add_executable(realm_character main.cpp)

target_link_libraries(realm_character PRIVATE
    realm_cluster
    realm_game_common
    realm_gateway_service
    realm_scheduler
)

target_compile_definitions(realm_character PRIVATE
    REALMMESH_DEFAULT_REALM_CONFIG="${PROJECT_SOURCE_DIR}/lua/config/services/realm.lua"
)
target_compile_features(realm_character PRIVATE cxx_std_20)
set_target_properties(realm_character PROPERTIES
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(MSVC)
    target_compile_options(realm_character PRIVATE /W4 /permissive-)
else()
    target_compile_options(realm_character PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
    )
endif()
