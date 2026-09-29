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
