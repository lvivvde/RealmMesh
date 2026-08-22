add_executable(realm_gateway
    main.cpp
)

target_link_libraries(realm_gateway PRIVATE
    realm_cluster
    realm_game_common
    realm_gateway_service
    realm_observability
    realm_scheduler
)

target_compile_definitions(realm_gateway PRIVATE
    REALMMESH_DEFAULT_GATEWAY_CONFIG="${PROJECT_SOURCE_DIR}/lua/config/services/gateway.lua"
)

target_compile_features(realm_gateway PRIVATE cxx_std_20)
set_target_properties(realm_gateway PROPERTIES
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(MSVC)
    target_compile_options(realm_gateway PRIVATE /W4 /permissive-)
else()
    target_compile_options(realm_gateway PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
endif()
