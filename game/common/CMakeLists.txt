add_library(realm_game_common
    src/account_store.cpp
    src/admission_grant.cpp
    src/admission_security_config.cpp
    src/base64url.cpp
    src/compact_jws.cpp
    src/edge_protocol.cpp
    src/identity_token.cpp
    src/json.cpp
    src/player_data_config.cpp
    src/player_data_store.cpp
    src/queue_number_v2.cpp
    src/session_ticket.cpp
)

find_package(SQLite3 REQUIRED)
# CMake 4.3 起 FindSQLite3 才提供 SQLite3::SQLite3,更早版本(含 Ubuntu 24.04
# CI 的 3.31)只有 SQLite::SQLite3;补一个全局别名,让测试目录也能用同一名字。
if(NOT TARGET SQLite3::SQLite3)
    add_library(SQLite3::SQLite3 INTERFACE IMPORTED GLOBAL)
    target_link_libraries(SQLite3::SQLite3 INTERFACE SQLite::SQLite3)
endif()

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
# Lua 装载只存在于 ConfigAccountStore 实现(.cpp),不进公共头:私有依赖。
target_link_libraries(realm_game_common PRIVATE
    RealmMesh::Scripting
    SQLite3::SQLite3
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
