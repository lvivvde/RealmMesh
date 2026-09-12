add_library(realm_login_verify_service
    src/login_verify_config.cpp
    src/login_verify_handler.cpp
    src/login_verify_service.cpp
)

add_library(RealmMesh::LoginVerifyService ALIAS realm_login_verify_service)

target_include_directories(realm_login_verify_service
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(realm_login_verify_service PUBLIC
    realm_cluster
    realm_game_common
    realm_network
    realm_observability
    realm_scripting
)

target_compile_features(realm_login_verify_service PUBLIC cxx_std_20)
set_target_properties(realm_login_verify_service PROPERTIES
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(realm_login_verify_service PRIVATE /W4 /permissive-)
else()
    target_compile_options(realm_login_verify_service PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
endif()
