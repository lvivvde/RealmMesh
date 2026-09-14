# 客户端登录链路的线客户端共用、双段竞速与兑换口实现

---
status: accepted
---

`framework/client` 承载登录链路(七态状态机 + 分档轮询),但它不自己实现线协议:线客户端(HTTPS / 网关长度帧 / JSON 取字段)从 `tools/loadgen` 提升到 `framework/network/client/`,两份消费者共用同一实现。于是 loadgen 压测的线行为与客户端真实登录的线行为不可能分叉——压测测的就是客户端要跑的那条线。

两段建连(连网关、直连业务服)复用同一套竞速语义:0ms 起跑 QUIC + 350ms 起跑 TLS/TCP,各自独立的拨号器与 QUIC 负缓存。两条链路是两段独立竞速(`Segment::Gateway` / `Segment::Realm`),不是一次竞速复用两次。本仓没有 QUIC 客户端实现(ADR-0002:QUIC 仅 Linux 服务端),QUIC 候选即时判 `ConnectFailure::Unsupported`,该失败属于 `permits_transport_fallback`,竞速据此立即让 TLS/TCP 起跑。

Realm 段的 EnterRealm 兑换口是注入 seam `EnterRealmRedeemer`,生产实现 `WireEnterRealmRedeemer` 在已竞速建连的流上发 1304 `EnterRealm`、等 1305 `EnterRealmAccepted`,复用网关段同一套 `game/common` 信封编解码与 `network/client/EdgeClientConnection` 帧收发——Realm 段与网关段本就是同一条 edge 线(ALPN 也是同一个),不另起第二份线协议。竞速本身是真的:Realm 段照 1303 下发的端点竞速建连。

## Considered Options

- **客户端自带一份线实现**:被否。与 loadgen 两份实现会随时间分叉,压测结论不再能代表客户端行为;共用一份后,线协议变更同时影响两边。
- **把兑换协议按 1303 的形状猜出来**:被否。票据兑换是 #46 的契约(单次消费、Realm 侧状态),客户端猜测的协议会变成事实上的第二份规格。
- **等 #46 落地再做客户端**:被否。链路的七态、回退规则、轮询节奏、两段竞速都不依赖兑换内容;留一个显式失败的 seam 能现在交付,且 #46 落地时只换实现。

## Consequences

- 兑换口曾是为 #46 留的唯一一个洞,且如约只换了实现:`EnterRealmRedeemer::redeem` 拿到的是已竞速建好的字节流,协议落地时实现该接口即可,七态、回退规则与断言结构都没动。
- `realm_client` 只依赖 `RealmMesh::Network` 与 `RealmMesh::GameCommon`;线客户端在 network 层,不反向依赖 client。
- 凭据(身份 Token / 号牌 / EnterRealm 票据)纯内存持有,进程重启等于重新登录排队;落盘需求出现时是独立决策,不在本 ADR 范围内。
- 集成测试用真实 TLS 回环验证两段竞速与帧交互;Realm 段的服务端已是真实 realm 服务(`GatewayRuntime` + `ServiceFrame` + 共享票据键),不再是桩。
