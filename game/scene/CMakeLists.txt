add_library(realm_game_scene src/scene_shard.cpp src/scene_module.cpp)
add_library(RealmMesh::SceneService ALIAS realm_game_scene)
target_include_directories(realm_game_scene PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(realm_game_scene PUBLIC realm_game_common PRIVATE realm_edge realm_service_host)
target_compile_features(realm_game_scene PUBLIC cxx_std_20)
set_target_properties(realm_game_scene PROPERTIES CXX_EXTENSIONS OFF)
if(NOT MSVC)
    target_compile_options(realm_game_scene PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()
