# 保留现有平台基线并支持无 sudo 的 Linux 开发环境

---
status: rejected
---

#187 提议将开发基线从 macOS 迁到 Ubuntu。整合 #189 时保留 ADR-0002 和 ADR-0012 的平台分工。Linux 是生产基线，CI 验证 QUIC。macOS 是开发基线，本机可使用 Homebrew 的 libmsquic 验证 QUIC，macOS CI 验证 TLS/TCP。

Ubuntu 和 WSL2 上的开发者可以在仓库的 `.tools/` 中准备依赖。构建和动态库查找步骤见 [README 的构建说明](../../README.md#构建)。
