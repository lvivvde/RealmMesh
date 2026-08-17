add_library(realm_game_common
    src/edge_protocol.cpp
    src/session_ticket.cpp
)

add_library(RealmMesh::GameCommon ALIAS realm_game_common)

target_include_directories(realm_game_common
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(realm_game_common PUBLIC
    RealmMesh::Protocol
    Sodium::Sodium
)
target_compile_features(realm_game_common PUBLIC cxx_std_20)
set_target_properties(realm_game_common PROPERTIES CXX_EXTENSIONS OFF)

if(MSVC)
    target_compile_options(realm_game_common PRIVATE /W4 /permissive-)
else()
    target_compile_options(realm_game_common PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
    )
endif()
