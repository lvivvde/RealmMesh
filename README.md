# RealmMesh

RealmMesh 是一个计划采用帧驱动模型构建的 C++ 游戏服务器。

服务器以固定或可配置的帧率推进游戏世界。在每一帧中，系统依次处理网络消息、游戏逻辑、定时任务与状态同步，使逻辑执行顺序清晰且可预测。

## 核心思路

```text
接收客户端消息
       ↓
更新当前帧的游戏状态
       ↓
处理定时任务与游戏逻辑
       ↓
向客户端同步结果
       ↓
等待下一帧
```

## 计划功能

- 可配置的服务器帧率
- 基于事件的网络消息处理
- 房间与玩家生命周期管理
- 定时任务和延迟事件
- 多线程任务调度
- 日志、性能统计与优雅停服

## 构建

环境要求：

- 支持 C++20 的编译器（GCC 10+、Clang 12+ 或 MSVC 2022）
- CMake 3.20+

一键配置、编译并测试：

```bash
./scripts/build.sh
```

也可以分别执行：

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

运行最小服务器示例：

```bash
./build/dev/realmmesh
```

默认以每秒 20 帧运行 5 帧后退出。持续运行或修改帧率：

```bash
./build/dev/realmmesh --frames 0 --tick-rate 30
```

当前优先支持 Linux。

## 参与贡献

欢迎通过 Issue 提交建议或问题，也欢迎提交 Pull Request。较大的功能改动建议先创建 Issue 讨论设计方案。

## License

许可证尚未确定。在正式添加开源许可证前，本项目默认保留所有权利。
