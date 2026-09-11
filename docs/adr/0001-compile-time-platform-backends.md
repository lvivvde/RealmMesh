# 编译期选择平台后端,业务代码零平台分支

---
status: accepted
---

为达成 Windows/macOS/Linux 三端互通,Event Loop 与 TCP 套接字层按平台各提供一组实现文件(`event_loop_epoll.cpp` / `event_loop_kqueue.cpp` / `event_loop_iocp.cpp` 等),由 CMake 在配置期选定其中一个编入,统一通过 `IEventLoop` 纯虚接口和工厂暴露;`TcpListener`/`TcpSocket` 保持具体类、按平台替换实现文件。预处理器 `#if` 只允许出现在后端实现文件内部,业务代码与传输层禁止平台分支。拒绝在单一文件内用 if/else 或文件级 `#if` 混排三平台:`sys/epoll.h` 与 `winsock2.h` 无法共存于一个编译单元,且运行时分支毫无意义——同一份二进制本就不能跨平台运行。

## Considered Options

- **单文件内运行时 if/else**:用户最初提案。不可行(头文件互斥)、不可测(每平台仍只走一支)、噪音大。
- **引入 asio/libuv 承载事件循环**:现仓库为手写 reactor 且业务已稳定在其抽象上,引入等于重写传输层,收益不抵成本。
- **每个类都抽虚接口**:Event Loop 需要接口(可测试、多后端);`TcpListener`/`TcpSocket` 是 RAII 值类型,虚开销与测试替身无意义,按平台换实现文件即可。

## Consequences

- 新增平台支持 = 新增一组后端实现文件 + 工厂处一行注册,业务层零改动。
- 同一构建中只有一个后端存在;不存在"运行时切换后端"的能力。
