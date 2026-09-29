# 排队采用无状态号牌与全局进度端点

---
status: accepted
---

> **Amended by ADR-0009:** Queue Number 继续承担无状态排位与断线找回，
> 但不再通过 `admitted` 重签兼任网关准入凭据；放行阶段改用独立的
> Admission Grant。下文相关描述仅保留为历史背景。

排队调度服把每客户端状态压到零:号牌是签名 JWT(号值+`admitted`),服务端唯一权威状态是"已放行号"与实测放行速率两个原子量;位次与预计等待由客户端凭全局 progress 端点(单调量、CDN 可缓存 1~2s)本地计算。百万客户端轮询因此被 CDN 卸载,源站只承担取号与放行切换;断线重连凭号牌找回位次,过期(放行+5min 宽限)即重排。

## Considered Options

- **每客户端会话状态表(内存/Redis)**:被否。状态面随队列人数线性增长,洪峰下百万级条目的存储、过期与恢复都是负担。
- **位次由服务端逐请求计算(仅 /tickets/me)**:被否。50 万 QPS 级轮询全部打源站,而位次只是两个单调量之差。
- **断线不保位**:被否。洪峰下重排体验差且放大取号流量。

## Consequences

- 排队服 v1 单实例 + etcd 快照冷备即可承载(取号 ≈1700/s、查询被卸载);号段分片留 TODO。
- progress 端点的响应带签名与 Cache-Control,是 CDN 卸载成立的前提;客户端时钟偏差只影响 ETA 平滑,不影响位次(位次只依赖 released_number)。
- 放行凭证＝号牌重签(`admitted` claim),不引入第三种凭据。

## 2026-09-28 准入凭据分离（修订）

[ADR-0009](0009-identity-bound-admission-grant.md) 落地后，上文"放行凭证＝号牌重签
(`admitted` claim)"的结论不再成立：

- 排队号牌只承担无状态排位与断线找回：schema 为号值、`identity_jti` 与时间 claims，
  `aud=realmmesh-queue`、`purpose=queue-position`，`exp` 取身份 Token 到期与签发时 TTL
  的较小值。位次计算与 CDN 卸载的结论不变。
- 放行阶段由 Queue Scheduler 另行签发 **Admission Grant**：`aud=realmmesh-gateway`、
  `purpose=gateway-admission`，绑定同一 `identity_jti`、来源号值与 `deployment_id`，
  `exp = min(身份 Token 到期, released_at + 放行窗口)`。窗口锚定持久化的放行批次时间，
  轮询或重启都不会延长它。
- Gateway 只接受 Admission Grant，并按 `identity_jti` 在一个 deployment 内至多消费一次；
  共享消费存储不可用时停止接纳新登录，不回退到进程内状态。Queue Number 即使仍有效也
  无法在 Gateway 换取准入。
- 网关与排队服的执行路径不再签发、接受或解析带 `admitted` 的号牌，不保留双格式与回退
  开关；v1 编解码模块在切换后已无生产调用方，其移除属于 #82 的收尾范围。

完整协议与验收条件见 Security Spec #79；术语以 [CONTEXT.md](../../CONTEXT.md) 为准。

## 2026-09-30 崩溃一致发号（修订）

“服务端零逐客户端状态”只适用于高频位次轮询，不适用于首次发号的提交边界。Queue
现在把未过期 `identity_jti` 的恢复映射与递增后的 `next_number`/放行快照放进同一笔
etcd 事务，并且只在事务成功后返回 `202`。事务应答不确定时先回读映射与快照：读到即
恢复原号码和原始签发时间；若回读也失败，当前进程永久停止发号并返回可重试的
`503/2002`，同时拒绝放行快照写入，直到重启后重新装载权威状态；它既不拿旧内存
水位继续分配，也不允许放行帧用旧水位覆盖可能已经提交的事务。
映射以身份 Token 到期时间为生命周期，按 60 秒到期桶共享 etcd Lease；轮询与
Admission Grant 查询仍由号牌和全局水位计算，不建立百万客户端会话表。

### Considered Options

- **继续只在放行帧保存快照**：被否。两次放行之间的成功响应会在重启后消失，并可能
  复用号码。
- **每次重写一个完整幂等映射**：被否。写放大随排队人数线性增长，恢复和过期清理也
  需要扫描大对象。
- **把号码分配移到 Login Verifier 或扩充 Identity Token 协议**：被否。它把排队顺序
  的权威移出 Queue Scheduler，并扩大已经发布的凭据边界。
- **每次发号原子写一条有租约的映射和快照**：采用。它把响应与可恢复事实绑定在同一
  提交点，且只保留尚未过期登录尝试的恢复状态。

### Consequences

- 新发号一笔事务写两个 key；同一尝试重放走事务失败分支回读两个 key。通常相同身份
  到期分钟内的请求共用一条 Lease，因此 Lease grant 约为每个到期桶一次，而非每号一次。
- 按 1700 次新发号/秒、身份 Token 最长 30 分钟估算：etcd 承担 1700 笔事务/秒、
  **3400 次 key mutation/秒**（覆盖一次 snapshot + 新增一条映射）；稳态约保留
  **306 万条映射**与约 30 个活跃到期桶 Lease。映射 key + JSON value 的裸载荷按约
  135 B/条计算约 **394 MiB**，尚未计入 etcd MVCC、索引、Raft/WAL、副本与碎片开销，
  容量规划必须按实测放大。冷备启动会扫描并校验这些未过期映射，恢复时延也随之线性增长。
- `QueueCore` 不再维护第二套内存发号映射；它只承载由权威快照恢复的水位、放行批次与
  速率。当前部署仍是单活动 Queue + 冷备，不支持两个活动实例并发发号。
- 2026-09-30 在 macOS 本机、Debug 构建、单节点 etcd 上连续提交 1700 个号码，实测约
  **200 次/秒**（`QueueStoreEtcdIntegrationTest.MeasuresIssuanceWriteLoadAgainstTarget`）。
  这否定了本 ADR 原先“单实例即可承载约 1700/s”的未经验证假设；该数值不是跨机器
  性能门槛。#91 必须在目标 Linux 环境比较批提交/权威存储替代方案并达标后，才能宣称
  1700/s 容量。当前选择优先把崩溃一致性做正确，不把容量假设冒充验收结果。
