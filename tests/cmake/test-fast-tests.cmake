# 快速入口与编译资源预算(#125):bash 在临时小工程里驱动真实 test_helpers.cmake、
# CMakePresets.json 与 scripts/ 入口,拉起子 cmake 配置、构建、测试与 watch 循环,
# 所以归 integration。
add_test(
    NAME TestFastScriptTest.FastEntryWatchAndJobsBudget
    COMMAND bash
        "${PROJECT_SOURCE_DIR}/tests/scripts/test_fast_test.sh"
        "${PROJECT_SOURCE_DIR}"
        "${CMAKE_COMMAND}"
)
set_tests_properties(
    TestFastScriptTest.FastEntryWatchAndJobsBudget
    PROPERTIES LABELS integration TIMEOUT 600
)
