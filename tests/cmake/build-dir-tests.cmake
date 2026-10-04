# 统一预设与构建目录信息(#122):bash 驱动真实 CMakePresets.json、
# cmake/RealmMeshBuildDirInfo.cmake 与 scripts/ 入口,在临时小工程里拉起子
# cmake 配置、构建与测试,所以归 integration。
add_test(
    NAME BuildDirScriptTest.PresetEntriesResolveRecordedBuildDirs
    COMMAND bash
        "${PROJECT_SOURCE_DIR}/tests/scripts/build_dir_info_test.sh"
        "${PROJECT_SOURCE_DIR}"
        "${CMAKE_COMMAND}"
)
set_tests_properties(
    BuildDirScriptTest.PresetEntriesResolveRecordedBuildDirs
    PROPERTIES LABELS integration TIMEOUT 180
)
