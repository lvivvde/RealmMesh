# 排位凭据与网关准入凭据分离

---
status: accepted
---

ADR-0006 选择用带 `admitted` claim 的 Queue Number 同时承担排位与网关准入，但独立验证 Identity Token 和 Queue Number 允许合法票据被跨身份拼接，也无法表达 deployment 范围内的一次性准入。Queue Number 因此只保留可重复查询的排位与断线找回职责；Queue Scheduler 在固定放行窗口内另行签发绑定 `identity_jti`、来源 Queue Number 与 deployment 的短期 Admission Grant，Gateway 集群以 `identity_jti` 线性一致地单次消费。完整协议与验收条件由 Security Spec #79 定义。

## Considered Options

- **继续重签 Queue Number 并依赖 `admitted` 区分阶段**：被否。一个凭据类型承担可重复查询与单次准入，扩大 interface，也保留类型混用与拼接风险。
- **只把 Queue Number 绑定 `account_id`**：被否。排位应归属于一次登录尝试；账号重新登录产生的新 Identity Token 不得继承旧排位。
- **Admission Grant 携带完整 Queue Number 的哈希链**：被否。两者均由 Queue Scheduler 签名，`identity_jti`、来源号码、`aud` 与 `purpose` 已提供所需绑定，哈希链不增加信任。
- **客户端 proof-of-possession**：暂不采用。它能抵御完整 Bearer 凭据对失窃后的使用，但需要新的客户端密钥与设备身份体系。

## Consequences

- Queue Number 与 Admission Grant 使用互斥的 `aud`/`purpose`、独立 Codec、独立 Ed25519 签名密钥和 key ring；派生凭据均不得晚于 Identity Token 过期。
- 放行窗口从批次的 `released_at` 起算。Queue Scheduler 持久化窗口内的批次区间，而不保存每客户端放行状态；重复查询确定性地得到同一语义的 Admission Grant。
- Gateway Login Pipeline 在私有存储 seam 后以 `Reserved → Consumed` 两阶段协议隐藏集群消费；共享存储不可用时停止接纳新登录，但不终止已有 Edge Session。
- 已提交消费后发生不确定故障仍保持已消费。客户端必须重新登录排队，以 at-most-once 安全换取故障时较低可用性。
- 切换必须同时淘汰旧 Queue Number 准入路径；不保留长期双格式或回退开关。
