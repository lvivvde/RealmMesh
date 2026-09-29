# RealmMesh 边缘协议

## 传输帧

QUIC 与 TLS/TCP 共享同一业务帧格式：

```text
[4-byte big-endian payload length][Protobuf Envelope]
```

QUIC 在一个客户端发起的长期可靠双向流上承载该字节流；不使用 QUIC Datagram 或
一消息一流。载荷上限为 64KiB。传输握手必须协商 TLS 1.3 和 ALPN
`realmmesh-edge/1`，0-RTT 数据一律不接受。

`Envelope` 字段：

| 字段 | 用途 |
|---|---|
| `protocol_version` | 当前为 `1` |
| `message_id` | payload 的消息类型 |
| `request_id` | 请求/响应关联 ID，通知可为 `0` |
| `payload` | 具体 Protobuf 消息 |

## 端点候选

`ServiceEndpoint` 包含：

| 字段 | 说明 |
|---|---|
| `protocol` | `QUIC` 或 `TLS_TCP` |
| `address` | 必须参与证书 DNS/SNI 校验的主机名 |
| `port` | 端口；Gateway 两种协议使用同一数字 |
| `priority` | 数字越小优先级越高 |

`EnterRealmGranted.realm_endpoints` 是候选列表。
客户端不得把证书或 ALPN 错误解释为“网络不支持 QUIC”。

## 消息 ID

| ID | 方向 | 消息 |
|---:|---|---|
| 1105 | C2S | `HeartbeatRequest` |
| 1106 | S2C | `HeartbeatResponse` |
| 1301 | C2S | `EdgeAttach` |
| 1302 | S2C | `EdgeAttachAccepted` |
| 1303 | S2C | `EnterRealmGranted` |
| 1304 | C2S | `EnterRealm` |
| 1305 | S2C | `EnterRealmAccepted` |
| 1999 | S2C | `EdgeError` |

已退役的 ID（1001、1002、1101、1102、1103、1104、1201、1202）永久作废、不得复用：
`envelope.message_id` 落在这组编号上时，接收方按未知消息拒绝。

客户端在 Realm 入场受理（`EnterRealmAccepted`）后每 10 秒发送一次
`HeartbeatRequest`，服务端使用相同的 `request_id` 返回 `HeartbeatResponse`；
连接断开时停止心跳。心跳用于刷新 Realm 的应用层连接活动时间，不能用 TCP
KeepAlive 代替。Gateway 会话不承载心跳：attach 受理后网关只负责下发直连凭证。

Gateway 的两个传输可以并行进行安全握手，但只有竞速胜出的连接可以发送
`EdgeAttach`。Admission Grant 由网关按 `identity_jti` 在一个 deployment 内单次消费，
后到连接不得重发；`EnterRealm` 直连凭证在 Realm 单次兑换，重复提交按重放拒绝。

## 凭据链与网关 attach

接入链每个阶段使用一种凭据，各自独立签发与验签（[ADR-0009](adr/0009-identity-bound-admission-grant.md)；
完整协议与验收条件见 [Security Spec #79](https://github.com/lvivvde/RealmMesh/issues/79)）：

```text
身份 Token → 排队号牌 → Admission Grant → EnterRealm 票据
```

| 凭据 | 签发方 | 职责 | 消费方与次数 |
|---|---|---|---|
| 身份 Token | `login_verify` | 一次登录尝试的身份（`account_id` 与 `jti`） | 排队服查号、网关校验；不单独构成准入 |
| 排队号牌 | `queue` | 位次查询与断线找回（`aud=realmmesh-queue`、`purpose=queue-position`） | 排队服查号，可重复使用；网关不接受 |
| Admission Grant | `queue` | 放行后的准入（`aud=realmmesh-gateway`、`purpose=gateway-admission`，绑定 `identity_jti`、来源号值与 `deployment_id`） | 网关按 `identity_jti` 在 deployment 内单次消费 |
| EnterRealm 票据 | `gateway` | 业务服直连兑换 | Realm 单次兑换 |

`EdgeAttach`（1301）提交 `identity_token` 与 `admission_grant`。校验通过后网关返回
`EdgeAttachAccepted`（1302），随后以 `EnterRealmGranted`（1303）下发 EnterRealm 票据与
`ServiceEndpoint` 候选列表。拒绝时返回 `EdgeError`：`1001` 凭据无效（含 Grant 的签名、
绑定、deployment、时效与已消费），`1004` 网关满额拒绝 attach，`1005` 准入处理中，`1006` 准入
存储不可用，`429` 限流（带 `retry_after_seconds`）。`1005`/`1006`/`429` 在 Grant 仍
有效时可重试同一凭据链，`1001` 必须重新走登录链。

## 初次降级规则

允许从 QUIC 转入 TLS/TCP 的结果只有：

- 客户端构建不支持 QUIC；
- 当前网络明确不可达 UDP/QUIC；
- QUIC 握手超时。

证书链/主机名、ALPN、业务鉴权和协议版本错误属于终止错误。当前规则只覆盖初次
建连；已建立 QUIC 连接断开后需重新走候选选择和鉴权。

## 演进规则

- 已发布字段编号和消息 ID 不得复用。
- 删除字段使用 `reserved`。
- 兼容新增字段使用新编号，接收方接受未知字段。
- 破坏兼容性的变化进入新包版本并提升 Envelope 协议版本。
