# 四层依赖方向守卫(ADR-0014):运行期读取源码树，进程内完成，不链接任何本仓库目标。
realm_add_gtest(layering_guard_test
    SOURCES layering_guard_test.cpp
    DEFINITIONS
        REALMMESH_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
