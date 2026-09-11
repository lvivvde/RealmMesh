# 三平台支持等级:Linux 生产基准、macOS 开发基准、Windows 仅保留开关

---
status: accepted
---

三端互通的目标深度是"开发+测试互通":Linux 为生产与行为基准(QUIC 专属逻辑只在此测试),macOS 为开发基准(编译、跑拓扑、ctest 全绿,QUIC 经 `TransportFactory` 按平台能力禁用、只走 TLS/TCP 回退路径),Windows 当前**只交付切换机制**——CMake 后端开关接受 `iocp` 为合法值但显式报"未实现",不编写 IOCP 实现,不做构建验证,不进 CI。理由:MsQuic 的依赖接线为 Linux 专属且其 macOS 支持非一等公民,TLS 回退路径足以支撑 macOS 开发;盲写 IOCP(完成端口回调模型,与事件模型差异大)几乎必然存在缺陷,且无法被任何本机构建验证,将来启用时代价更高。

## Consequences

- macOS 测试永不执行 QUIC 专属逻辑;QUIC 行为回归完全依赖 Linux CI。
- Windows 真正启用是一个独立决策,届时需解决:MsQuic/Windows 发现、libsodium 构建路径、`LUA_USE_LINUX`、protoc 预编译包、bash 脚本与 fork/exec 测试的替代——应另立 ADR。
- 观测栈(docker compose)不在 macOS 开发范围。
