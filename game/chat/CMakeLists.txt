add_library(realm_game_chat src/chat_config.cpp src/chat_config_lua.cpp src/chat_channels.cpp src/chat_module.cpp)
add_library(RealmMesh::ChatService ALIAS realm_game_chat)
target_include_directories(realm_game_chat PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(realm_game_chat PUBLIC realm_game_common PRIVATE realm_network realm_service_host realm_scripting)
target_compile_features(realm_game_chat PUBLIC cxx_std_20)
set_target_properties(realm_game_chat PROPERTIES CXX_EXTENSIONS OFF)
if(NOT MSVC)
    target_compile_options(realm_game_chat PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()
