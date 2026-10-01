# macOS 本地启用 QUIC

---
status: accepted
---

ADR-0002 以"MsQuic 的依赖接线为 Linux 专属且其 macOS 支持非一等公民"为由,让 macOS 只走 TLS/TCP。这一前提已部分失效:Homebrew 收录 `libmsquic`(2.6.x,依赖 `openssl@3`);`quic_transport.cpp` 只依赖 `msquic.h` 与 POSIX 头,没有 Linux 专属 API;传输选择完全由 `platform_transport_capabilities()` 驱动。真正的 Linux 绑定只在 MsQuic 的 CMake 发现逻辑与安装脚本。因此 macOS 改为**自动检测**:装了 Homebrew `libmsquic`(或设置 `MSQUIC_ROOT`)就编入 QUIC 并注册 QUIC 测试,找不到则照常只走 TLS/TCP 并在 CMake 输出里提示 `brew install libmsquic`。Linux 仍是生产与行为基准,缺失 MsQuic 仍令配置失败;CI 分工不变,`linux` job 是 QUIC 回归的权威,`macos` job 不装 `libmsquic`、继续只走 TLS/TCP。详见 #97。

## Considered Options

- **维持 ADR-0002,QUIC 只在 Linux 编译与测试**:被否。改 QUIC 代码只能推到 CI 才知道能否编译、能否跑通,往返代价高。
- **本地 Linux 虚拟机或容器**:被否。开发机需要额外的运行时与镜像维护,且不能在 IDE 的同一构建里直接调试。
- **macOS CI 也装 `libmsquic`**:暂不做。两条 job 都跑 QUIC 只加 CI 时长,不增加对生产平台的置信度。

## Consequences

- 版本按次版本对齐:Linux 安装脚本固定 Microsoft 官方 2.6.1 包,Homebrew 只提供当前 2.6.x 且无法钉补丁版本。CMake 从库文件真实路径读出版本,次版本偏离 2.6 时给 WARNING,不让配置失败。
- macOS 的 MsQuic 发现分支没有 CI 覆盖(macOS job 不装 `libmsquic`),由 macOS 开发者在本机发现问题。
- MsQuic 在 macOS 上默认用系统 SecTrust 校验证书、忽略 `CaCertificateFile`;Linux 默认走 OpenSSL 内置校验。用自签 CA 的 QUIC 客户端须显式设置 `QUIC_CREDENTIAL_FLAG_USE_TLS_BUILTIN_CERTIFICATE_VALIDATION`,两平台行为才一致。
- QUIC 用例只在 macOS 失败时先按我方缺陷修;确认是 MsQuic 的 macOS 平台限制后,只对该用例 `GTEST_SKIP` 并写明原因与出处链接,不整体关闭 macOS QUIC。
- 本仓客户端没有 QUIC 拨号器,登录链在两平台上都经 TLS/TCP 承载;编入 QUIC 的 Mac 上 Gateway 只是多一个 QUIC 监听。`run-macos-login-acceptance.sh` 报告的传输因此恒为 TLS/TCP,并记录 QUIC 监听是否编入。
- macOS 仍不是 QUIC 的生产或部署平台;XDP 等 Linux 性能路径与 Windows 不在本决定范围内。
