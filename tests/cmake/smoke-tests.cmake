# 帧调度器 demo(根 CMakeLists 中 src/main.cpp 的 realmmesh 目标)的冒烟测试。
add_test(
    NAME realmmesh_smoke
    COMMAND realmmesh --frames 2 --tick-rate 1000
)
