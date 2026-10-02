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
| 1401 | C2S | `ListCharacters` |
| 1402 | S2C | `CharacterList` |
| 1403 | C2S | `CreateCharacter` |
| 1404 | S2C | `CharacterCreated` |
| 1405 | C2S | `SelectCharacter` |
| 1406 | S2C | `CharacterSelected` |
| 1407 | C2S | `Train` |
| 1408 | S2C | `TrainResult` |
| 1409 | S2C | `RealmSessionDisplaced` |
| 1999 | S2C | `EdgeError` |

1101–1305 定义在 `edge/v1/edge.proto`，1401–1409 定义在 `realm/v1/realm.proto`；两者共用
`common/v1/Envelope`（协议版本不变）与 1999 `EdgeError`。

已退役的 ID（1001、1002、1101、1102、1103、1104、1201、1202）永久作废、不得复用：
`envelope.message_id` 落在这组编号上时，接收方按未知消息拒绝。

客户端在 Realm 入场受理（`EnterRealmAccepted`）后每 10 秒发送一次
`HeartbeatRequest`，服务端使用相同的 `request_id` 返回 `HeartbeatResponse`；
心跳在选角与游戏中两个阶段都受理、不改变阶段，连接断开时停止心跳。心跳用于刷新 Realm 的应用层连接活动时间，不能用 TCP
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
存储不可用，`1007` 账号不具备准入资格（封禁或白名单外；没有角色不再拒绝，选角在 Realm 里完成），`1008` 玩家数据暂
不可用（网关拉取重试耗尽，带 `retry_after_seconds`），`429` 限流（带 `retry_after_seconds`）。
`1005`/`1006`/`429` 在 Grant 仍有效时可重试同一凭据链；`1001` 必须重新走登录链；`1007`
是终态，客户端结束登录链、不重试；`1008` 时 Grant 已消费，客户端按 `retry_after_seconds`
（上限 5 秒）退避后从 Login Verifier 重新开始。

EnterRealm 票据只绑定账号与 Realm，不再携带角色。Realm 处理 `EnterRealm`（1304）时只
验票并单次兑换、不读玩家数据；票据无效或已兑换回 `3002` 并断开。兑换成功后 Realm
Session 进入选角阶段。

## Realm Session 业务

Realm Session 只有两个阶段，单向推进：**选角**（`ListCharacters` / `CreateCharacter` /
`SelectCharacter`）→ 选定角色后进入**游戏中**（`Train`）。请求按会话串行处理，回包回显
请求的 `request_id`；拒绝统一用 `EdgeError`（1999），除下表注明外会话保持可用。

| 消息 | 成功回包 | 规则 |
|---|---|---|
| `ListCharacters` | `CharacterList`：本账号在本 Realm 的角色与 `last_selected_character_id`（0 表示从未选过） | 仅选角阶段 |
| `CreateCharacter{name}` | `CharacterCreated`：新角色经验 0、等级 1 | 名字为 1–16 个 Unicode 码点、首尾无空白、无控制字符；名字在本 Realm 内唯一；每账号每 Realm 至多 3 个角色 |
| `SelectCharacter{character_id}` | `CharacterSelected`：角色当前状态与已确认的 `training_seq` | 角色须归属本账号与本 Realm；成功即记为上次选择并进入游戏中 |
| `Train{seq}` | `TrainResult{exp, level, seq, replayed}` | 仅游戏中；`seq == training_seq + 1` 执行一次训练并持久化；`seq == training_seq` 不重复执行，回当前状态且 `replayed = true`；其他序号拒绝 |

训练规则由 Realm 启动时从 `realm.training_rule_file` 加载的 Lua 模块给出（默认
`configs/services/realm/training.lua`：每次 +10 经验，等级 = `exp / 100 + 1`，10 级即经验
900 封顶），加载失败或以经验 0 试调时返回值形状不对即启动失败，不热更。

| 错误码 | 含义 |
|---|---|
| `2002` | 未兑换票据就发送业务消息 |
| `3003` | 阶段错位：选角阶段训练，或游戏中再列表/建角/选角 |
| `3004` | 角色名不合法 |
| `3005` | 角色名已被占用 |
| `3006` | 本账号角色数已达上限 |
| `3007` | 角色不存在，或不属于本账号或本 Realm |
| `3008` | 角色已满级 |
| `3009` | 训练序号非法（既不是 `training_seq + 1` 也不是 `training_seq`） |
| `3010` | 玩家数据源不可用，或训练规则执行出错；请求未生效，客户端可稍后重试同一请求 |
| `429` | 本会话排队请求过多或 Realm 数据访问满载，带 `retry_after_seconds`，请求未执行 |

同一账号在一个 Realm 内至多一个 Realm Session。新会话兑换成功后，Realm 向旧会话推送
`RealmSessionDisplaced`（1409，`request_id` 为 0）并关闭它；旧会话尚未回包的请求不再回包，
已提交的写入保持有效。新会话从选角阶段开始。

一次典型的 Realm Session 时序（心跳每 10 秒穿插其间，图中省略）：

```text
客户端                                   Realm
  │ EnterRealm(1304, 票据)              │
  │────────────────────────────────────▶│ 验票、单次兑换（不读玩家数据）
  │◀──────── EnterRealmAccepted(1305) ──│ 进入选角；同账号旧会话收 1409 后被关闭
  │ ListCharacters(1401)                │
  │────────────────────────────────────▶│ 有界工作者读角色
  │◀───────────── CharacterList(1402) ──│
  │ CreateCharacter(1403, name)         │ （可选，至多 3 个）
  │────────────────────────────────────▶│
  │◀────────── CharacterCreated(1404) ──│
  │ SelectCharacter(1405, id)           │
  │────────────────────────────────────▶│ 核对归属并写入上次所选
  │◀───────── CharacterSelected(1406) ──│ 进入游戏中，回 training_seq = n
  │ Train(1407, seq = n + 1)            │
  │────────────────────────────────────▶│ Lua 规则 + 条件写
  │◀──────────────── TrainResult(1408) ──│
```

客户端断线后重走登录链取得新票据，入场后重新选角；`CharacterSelected.training_seq`
告诉客户端下一次训练应使用的序号基准。

Login Verifier 的 HTTPS 错误体为 `{code, message, retry_after_seconds?}`：`1000` 请求非法、
`1001` 凭据无效、`1002` 封禁、`1003` 白名单外、`1004` 账号源暂不可用（`503`）、`1005`
验签工作者满额（`503`，带 `Retry-After` 头与 `retry_after_seconds`）。客户端对 `1005`
按 `retry_after_seconds`（上限 5 秒，不越过登录总窗口）退避后重验，其余验签失败终止回 idle。

## 初次降级规则

允许从 QUIC 转入 TLS/TCP 的结果只有：

- 客户端构建不支持 QUIC；
- 当前网络明确不可达 UDP/QUIC；
- QUIC 握手超时。

证书链/主机名、ALPN、业务鉴权和协议版本错误属于终止错误。当前规则只覆盖初次
建连；已建立 QUIC 连接断开后需重新走候选选择和鉴权。

## 演进规则

MongoDB 共享开发连接使用仓库外私有配置、TLS、账号认证及 SSH 隧道；这仅改变
Player Data Store 的部署连接，不改变上述客户端消息、HTTP 契约或凭据线格式。

- 已发布字段编号和消息 ID 不得复用。
- 删除字段使用 `reserved`。
- 兼容新增字段使用新编号，接收方接受未知字段。
- 破坏兼容性的变化进入新包版本并提升 Envelope 协议版本。
