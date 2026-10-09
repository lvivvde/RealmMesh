add_executable(realmmesh_mongodb_fixture mongodb_fixture.cpp)
target_compile_features(realmmesh_mongodb_fixture PRIVATE cxx_std_20)
target_link_libraries(realmmesh_mongodb_fixture PRIVATE mongoc_static)
target_include_directories(realmmesh_mongodb_fixture PRIVATE include)

# Only real MongoDB fixture consumers need this helper and its executable path.
# The driver remains in the separate process, outside the tested executable.
add_library(realmmesh_test_mongodb INTERFACE)
add_dependencies(realmmesh_test_mongodb realmmesh_mongodb_fixture)
target_compile_definitions(realmmesh_test_mongodb INTERFACE
    REALMMESH_TEST_MONGODB_INITIALIZER_PATH="$<TARGET_FILE:realmmesh_mongodb_fixture>")

add_executable(realmmesh_mongod_fixture_probe mongod_fixture_probe.cpp)
target_compile_features(realmmesh_mongod_fixture_probe PRIVATE cxx_std_20)
target_include_directories(realmmesh_mongod_fixture_probe PRIVATE include)
target_compile_definitions(realmmesh_mongod_fixture_probe PRIVATE
    REALMMESH_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

include(${PROJECT_SOURCE_DIR}/tests/cmake/mongodb-fixture-contract.cmake)
