# Linux 同为生产与开发基准，macOS 降为可选开发平台

---
status: accepted
---

ADR-0002 把 Linux 定为生产与行为基准、macOS 定为开发基准；ADR-0012 在此基础上让 macOS 装了
`libmsquic` 就本地编入 QUIC。实际开发已迁到 Linux：参考环境是 Ubuntu 24.04，本地以 WSL2
为参照（无 sudo 时依赖解到仓库内 `.tools/`，见 README「构建」）。继续把 macOS 当开发基准，
意味着日常验证跑在非生产平台上、QUIC 只能靠可选的 Homebrew 包，而生产平台的行为要推到 CI
才知道。因此改为：**Linux（Ubuntu 24.04）同时是生产与开发基准**——编译、起拓扑、全量
`ctest` 与 QUIC 回归都以 Linux 结果为准，缺失 MsQuic 仍令配置失败；**macOS 降为可选开发平台**，
保留编译与运行能力，但其结果不作为阶段完成或合并的依据，macOS 特有问题不阻塞 Linux 主路径。

平台差异继续用条件分支表达（`#if defined(__linux__)` / `__APPLE__`、CMake 的 `CMAKE_SYSTEM_NAME`、
脚本里的 `uname -s`），新增 Linux 专属行为时保留 macOS 分支的原有写法，不物理删除 macOS
分支、kqueue 后端、Homebrew 发现逻辑或 `__APPLE__` 下的负载缩放。

## Considered Options

- **维持 macOS 开发基准**：被否。开发机已是 Linux，继续以 macOS 为基准只会让文档与实际脱节。
- **彻底移除 macOS 支持**：被否。删除分支是不可逆的成本，保留它们几乎不增加维护量，
  macOS 开发者仍可按 ADR-0012 本地启用 QUIC。
- **双基准（两平台都必须全绿）**：被否。macOS 上 MsQuic 非一等公民、CI 慢数倍，
  双门槛只增加阻塞而不提升对生产平台的置信度。

## Consequences

- 取代 ADR-0002 中「macOS 为开发基准」的定位；ADR-0002 的 Windows 只保留开关、
  Linux 为 QUIC 回归权威这两点继续有效。
- ADR-0012 的 macOS 本地 QUIC 自动检测、版本对齐、SecTrust 与 `GTEST_SKIP` 约定继续有效，
  作为可选平台的使用说明；其中「Linux 仍是生产与行为基准」由本 ADR 扩展为生产与开发基准。
- CI 分工不变：`linux` job 是全量与 QUIC 的门槛，`macos` job 继续只走 TLS/TCP，
  作为兼容性信号保留；是否把它改为非阻塞另行决定。
- 本地完整验证（`./scripts/build.sh` 全绿）以 Linux 结果为准；macOS 上失败而 Linux 通过的问题
  按兼容缺陷登记，不阻塞阶段完成。
- 历史研究记录与已完成里程碑（如「macOS 本机四服务登录闭环」）按原样保留，不回改。
