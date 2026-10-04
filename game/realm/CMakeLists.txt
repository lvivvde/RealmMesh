add_library(realm_game_realm
    src/character_name.cpp
    src/realm_config_lua.cpp
    src/realm_sessions.cpp
    src/training_rule.cpp
)

add_library(RealmMesh::GameRealm ALIAS realm_game_realm)

target_include_directories(realm_game_realm
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Realm 跑在 GatewayRuntime 上(会话编号与出站命令),角色数据与线协议在
# game/common。
target_link_libraries(realm_game_realm PUBLIC
    realm_concurrency
    realm_game_common
    realm_gateway_service
    realm_observability
)
# Lua 只用在训练规则与配置解析实现:training_rule.hpp 前置声明 LuaRuntime
# (#127),配置头不带 Lua(#126)。包含 realm_config_lua.hpp 的目标自行链接
# realm_scripting。
target_link_libraries(realm_game_realm PRIVATE realm_scripting)

target_compile_features(realm_game_realm PUBLIC cxx_std_20)
set_target_properties(realm_game_realm PROPERTIES
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(realm_game_realm PRIVATE /W4 /permissive-)
else()
    target_compile_options(realm_game_realm PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
endif()
