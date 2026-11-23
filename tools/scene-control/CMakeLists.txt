add_executable(realm_mesh_scene_control main.cpp)
target_link_libraries(realm_mesh_scene_control PRIVATE
    RealmMesh::GameCommon RealmMesh::Network nlohmann_json::nlohmann_json)
target_compile_features(realm_mesh_scene_control PRIVATE cxx_std_20)
set_target_properties(realm_mesh_scene_control PROPERTIES CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(NOT MSVC)
    target_compile_options(realm_mesh_scene_control PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()
