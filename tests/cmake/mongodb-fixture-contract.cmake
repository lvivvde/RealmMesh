find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(
    NAME MongoFixtureTest
    COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/scripts/mongodb_fixture_test.py"
        --source "${PROJECT_SOURCE_DIR}"
        --initializer "$<TARGET_FILE:realmmesh_mongodb_fixture>"
        --probe "$<TARGET_FILE:realmmesh_mongod_fixture_probe>"
)
set_tests_properties(MongoFixtureTest PROPERTIES
    LABELS integration
    RUN_SERIAL TRUE
    TIMEOUT 120
)
