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

add_test(NAME MongoCtestFixturePrepare
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/scripts/mongodb_ctest_fixture.py"
        setup "${CMAKE_BINARY_DIR}/mongodb-ctest-fixtures" "${PROJECT_SOURCE_DIR}"
        "$<TARGET_FILE:realmmesh_mongodb_fixture>")
set_tests_properties(MongoCtestFixturePrepare PROPERTIES
    LABELS integration RUN_SERIAL TRUE TIMEOUT 35 FIXTURES_SETUP realmmesh_mongodb)
add_test(NAME MongoCtestFixtureCleanup
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/scripts/mongodb_ctest_fixture.py"
        cleanup "${CMAKE_BINARY_DIR}/mongodb-ctest-fixtures")
set_tests_properties(MongoCtestFixtureCleanup PROPERTIES
    LABELS integration RUN_SERIAL TRUE TIMEOUT 5 FIXTURES_CLEANUP realmmesh_mongodb)
add_test(NAME MongoCtestFixtureTest
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/scripts/mongodb_ctest_fixture_test.py"
        --source "${PROJECT_SOURCE_DIR}" --initializer "$<TARGET_FILE:realmmesh_mongodb_fixture>"
        --probe "$<TARGET_FILE:realmmesh_mongod_fixture_probe>")
set_tests_properties(MongoCtestFixtureTest PROPERTIES LABELS integration RUN_SERIAL TRUE TIMEOUT 120)
