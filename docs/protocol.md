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
| 1501 | C2S | `EnterScene`（已接线） |
| 1502 | S2C | `SceneCommandResult` |
| 1503 | C2S | `MoveInScene`（已接线） |
| 1504 | C2S | `GetSceneState`（已接线） |
| 1505 | S2C | `SceneState` |
| 1506 | S2C | `SceneEvent` |
| 1507 | C2S | `TransferScene`（已接线） |
| 1508 | S2C | `SceneUnavailable` |
| 1701 | C2S | `GetChatState`（C1编解码，C3接线待实现） |
| 1702 | S2C | `ChatState` |
| 1703 | C2S | `SendChat`（C1编解码，C3接线待实现） |
| 1704 | S2C | `ChatSendResult` |
| 1705 | S2C | `ChatMessage`（通知） |
| 1706 | S2C | `ChatUnavailable`（通知） |
| 1801 | Realm→Chat | `ChatInternalRequest`（仅内部线，C2独立监听） |
| 1802 | Chat→Realm | `ChatInternalReply`（仅内部线，C2独立监听） |
| 1803 | Chat→Realm | `ChatInternalPush`（仅内部线，C2独立监听） |
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

## Scene 线契约（S1–S4）

1501–1508 已定义于 `scene/v1/scene.proto`，共用 Envelope v1，编解码在
`game/common/scene_protocol`；S2 已在 Realm 接线进入、移动与查询，选角前回 3003。
S3 已接线 1507 转场，目标必须在服务端允许列表（当前允许分片 1/2，动态模式下承载者可变），不能转向当前分片。请求/响应的 `request_id` 必须非零，1506/1508 推送必须为 0。

`EnterScene{scene_seq}` 不传账号或目标；`MoveInScene{scene_seq,x,y}` 为绝对整数坐标，
范围 0..10000；`TransferScene{scene_seq,target_shard_id}` 只能指定非零目标分片。
场景序号取 1..INT64_MAX，独立于训练序号；`request_id` 不作幂等键。
`SceneCommandResult` 返回操作类型、序号、落点、绑定版本与回放标志。
`SceneState` 返回阶段、最后已提交序号/结果、确认落点、当前版本，以及可用时的快照；
首次进入前序号与版本为 0 且无落点。撤销后保留最后结果但版本可比该结果更新。

快照包含稳定 shard_id、分片 incarnation、事件水位和至多 16 个互不重复的角色成员；每个
成员只有角色 id、绑定版本与位置，位置必须属于该分片。事件带递增序号、加入/移动/离场
类型及成员，`SceneUnavailable` 带分片、绑定版本及 1..5 秒重试建议。游戏中 active 状态
必须含快照；接收者仍须核对水位、绑定版本和本地当前路由，结构解码不代替状态机校验。
所有 Scene Envelope（含头）上限 64KiB；坏帧、版本不符、错误消息编号、缺必需值、未知
枚举、越界坐标、错分片或重复成员均拒绝。

场景业务使用 1999 `EdgeError` 的以下错误：

| 错误码 | 含义 |
|---|---|
| 3101 | 尚未进入或正在切换 |
| 3102 | 位置或目标参数不合法 |
| 3103 | 序号非法或同序号不同参数 |
| 3104 | 确定未提交且 Scene 暂不可用 |
| 3105 | 结果不确定，必须查询或以同一业务指令重试 |
| 3106 | 绑定失效，需重新进入 |

### Realm 与 Scene 内部契约

`scene_internal/v1/scene_internal.proto` 独立于客户端线；S2 独立监听使用 TLS/TCP、双向
服务身份校验及 ALPN `realmmesh-scene/1`，不接收客户端票据。双方只信任配置的服务 CA，
按精确 DNS SAN 校验实例角色身份，不接受 CN 回退或通配符。Scene 的客户端允许列表与
Realm 的服务端身份均由服务配置指定；框架不认识游戏服务角色。
内部帧同样使用长度帧与 Envelope v1：1601 `SceneRequest`、1602 `SceneReply`、
1603 `ScenePush`，由 `scene_internal_protocol` 编解码。两组消息不会互相认领。
所有内部消息带 `realm_incarnation`、`scene_instance_id`、`scene_incarnation`（各为
1..128 个非空白可打印 ASCII 字节），用于认证后再核对进程代次；字段不能替代 TLS 身份。
Route 含非零 realm_id、character_id、binding_epoch、session_binding 与 shard_id；动态模式额外要求非零 shard_version，0 只兼容静态模式。

| operation | 请求必需内容 | STATUS_OK 回复必需内容 |
|---|---|---|
| 1 PREPARE | Route、scene_seq、32 字节 command_digest、location | Route、scene_seq、location（暂存，不可见） |
| 2 PROPOSE_MOVE | Route、scene_seq、command_digest、location | Route、scene_seq、合法候选 location |
| 3 CONFIRM | Route、scene_seq、command_digest、已持久 location | Route、scene_seq、location（已应用） |
| 4 ACTIVATE | Route、scene_seq、command_digest，不带 location | Route、scene_seq、原子快照 |
| 5 SNAPSHOT | shard_id，动态模式另带 shard_version；无 Route/序号/摘要 | 快照，无 Route/序号 |
| 6 REMOVE | Route，无序号/摘要/location | Route，无序号 |
| 7 SET_DRAINING | 动态模式 draining=true、drain_id、drain_timeout_ms，无角色字段 | draining、active/prepared_members、drain_phase、drain_id、shards |
| 8 STATUS | 无角色、分片、序号、排空参数 | dynamic、business_ready、draining、drain_phase、drain_id、shards 与成员总数 |

成功值为 1 OK；拒绝值为 2 INVALID、3 STALE_BINDING、4 BUSY、5 DRAINING、
6 NOT_FOUND、7 UNAVAILABLE。拒绝回复只回身份、操作和关联 ID，不夹带半份候选或快照。
请求/回复关联 ID 非零，1603 推送 ID 为 0、携带完整 `SceneEvent`。角色操作的 location、
快照 shard_id 必须与 Route 一致。字节摘要是规范业务参数 SHA-256；数据库使用等价的
64 字符 hex 表示，摘要不含 request_id、序号或传输身份，重放还要核对序号与原结果。

动态 `STATUS{scene_incarnation="discover"}` 只作认证后的业务就绪/进程探测，不授予分片。
`ShardStatus` 含 realm_id、shard_id、承载 version、active_members 与 prepared_members，
至多两个不同分片，单片各至多 16 人；回复聚合成员数必须与分片之和相同。`DrainPhase` 为
IDLE、RUNNING、COMPLETE 或 FAILED；COMPLETE 必须零成员、零准备、零承载且业务停用。
`drain_id` 为 1..128 非空白可打印 ASCII 字节，单次截止为 100..60000 毫秒；相同标识重复调用
不延长截止、不中断存量移动，失败后以新标识继续尝试。管理通道沿用 Realm 服务身份的 mTLS
允许列表，公网票据或其他服务证书不能调用；状态字段不是角色授权凭据。

### 已实现的持久边界

`RealmSceneStore` 由既有 `MongoPlayerDataStore` 实现：`initialize_scene` 惰性建立空状态，
`scene_state` 回读，`commit_scene` 原子推进指令结果，`revoke_scene` 条件撤销绑定。
`character_scenes` 使用复合 `_id{realm_id,character_id}` 保证唯一；读写均先核对不可转移的
角色归属。与训练字段分离，不修改 AccountStore 或登录读路径。
进入/重绑和转场递增 binding_epoch；移动保留版本；撤销只递增版本、清除 owner，保留
确认落点及最后序号。绑定与场景序号不允许 INT64_MAX 回绕；域 id 仍保留完整 uint64。

提交的过滤条件包括当前版本、完整旧 owner 与前一场景序号；owner 包含 Realm incarnation、
会话绑定、Scene 实例与 incarnation，动态模式还包含 shard_version。进入不能改变既有落点，移动不能改承载者或分片，
转场不能换 Realm 会话；合法移动判定由 SceneShard 负责，单步曼哈顿距离上限 100。
相同序号只有业务参数、结果位置、结果版本与结果 owner 全部相同才回放，不重复写。
已发出的写若驱动或结果解析失败，抛 `SceneWriteUncertain`；调用方不得宣称未生效。
`resolve_scene_write` 只在回读到精确结果时确认已提交，缺记录、旧值或被覆盖历史都仍是
Unknown；禁止把一次旧值回读当作允许重新编号的依据。

## 演进规则

开发服务组的停止协调使用本地 `.runtime/supervisor.shutdown` 文件；它属于
[开发 Supervisor 的进程控制](architecture.md#开发服务组的监督与停止143)，
不作为客户端消息、HTTP 接口或服务发现字段发布。

MongoDB 共享开发连接使用仓库外私有配置、TLS、账号认证及 SSH 隧道；这仅改变
Player Data Store 的部署连接，不改变上述客户端消息、HTTP 契约或凭据线格式。

自动化测试的真实副本集由本地辅助进程初始化并确认可写 Primary；一次 CTest 运行
共享后端时，每例仍用独立数据库，生命周期绑定该 CTest 所有者。其命令行、就绪与退出
状态属于[测试编排接口](../tests/README.md#mongodb-夹具初始化119)，不发布为客户端
消息、HTTP 接口或服务发现字段。

- 已发布字段编号和消息 ID 不得复用。
- 删除字段使用 `reserved`。
- 兼容新增字段使用新编号，接收方接受未知字段。
- 破坏兼容性的变化进入新包版本并提升 Envelope 协议版本。

内部通道先以配置的 shard_id 做 `SNAPSHOT{scene_incarnation="discover"}` 探测：仅在 mTLS
身份验证成功后返回该 Scene 的新运行 incarnation；所有角色操作必须携带精确 incarnation。
准备成员与移动候选各保留三秒，同一角色只留一个；同一已提交指令的重复激活/确认不增加事件。
已提交但候选过期时，Realm 可用权威落点重新 PREPARE/ACTIVATE 同一绑定；不会重新写入或
重复发布已生效的事件。旧版本 REMOVE 不删除新版本成员。

EnterScene 完成表示 Realm 已取得激活快照；参考客户端随后以 GetSceneState 获取快照，
以其水位衔接 SceneEvent。Realm 对重复事件去重，发现缺口主动重取快照，并发出
SceneUnavailable，暂停该观察者事件直到 GetSceneState 交付快照；不把缺口后事件直接透传。
Scene 链路失败同样发出暂不可用通知，权威已确认落点保持。参考客户端的 poll_scene 与
场景请求复用同一 Realm 帧流，心跳和训练不会吞掉推送；有界缓存满时终结该客户端会话。


S3 的 TransferScene 与进入/移动共用同一序号。服务端冻结该角色变更，结算已派发移动；
B PREPARE 不可见成员后，Realm 以旧版本、完整 owner 与前一序号 CAS 原子切到 B，随后
B ACTIVATE 并交付快照，A REMOVE 独立执行。提交前失败保留 A，提交后激活失败为 3105，
只能从已提交的 B 落点恢复；移除失败不能回滚或阻塞 B。条件移除不会删除后续新版本。

GetSceneState 不推进序号，返回最后持久结果；重登或同名 Scene 重启后可能为 UNAVAILABLE。
同序号重试只回放已存结果，不复活旧 incarnation/旧会话的成员；需要重新进入时以最后序号
加一 EnterScene 取得新 binding_epoch，落点保持。恢复前后的旧条件写必须失败。
跨分片切换后丢弃源实例事件；旧 LEAVE 只可移除观察者快照中同版本的旧成员，不能移除新成员。


S4 动态承载使用 `SceneShardStore`：`scene_shards` 按 `(realm_id,shard_id)` 唯一，保存承载者
instance_id/incarnation、version 与 draining；释放保留记录并增加版本，不删除或复用编号。
`scene_instances` 的接纳状态与分配、排空互相栅栏。新承载/释放增加版本，draining 标记不
改变版本；新进入/转场提交要求目标未排空，存量移动仍可提交。角色条件写与分片承载栅栏在
同一 majority/journal 事务内完成，旧承载或旧 incarnation 不能推进角色落点。

动态快照与事件的 `shard_incarnation` 为进程 incarnation 和承载版本的组合，外层
`scene_incarnation` 仍是进程身份。故障重建保持 shard_id 和数据库落点，客户端显式以
下一 EnterScene 取得新角色版本。Scene 同时承载两片时，Realm 额外按 shard_id 过滤推送。

排空产生的服务端 Transfer 与客户端动作共享 scene_seq：先结算已到达指令，再以最后序号
加一提交转场，结果同样可查询和回放。成功后 1508 通知客户端读取 SceneState，取得新序号
及目标快照继续；它不表示已确认落点丢失，查询可能已是 ACTIVE。若在序号竞争中收到 3103，
先查询当前结果，不能自行跳号。未确定的持久写、目标激活和源移除没有结算前，不报告排空完成。


S5 验收不增加消息号或改变 wire 字段。参考客户端最多缓存 256 条未消费的 Scene 通知，
溢出关闭该 Realm Session，避免无界内存；其他会话与分片继续服务。客户端重新登录、选角后
查询权威状态，再以最后序号加一 EnterScene，取得新绑定与完整快照；旧通知和绑定不复用。
MongoDB 在 initialize_scene 读取/初始化阶段不可用时返回 3104；发出场景提交之后结果不确定
才返回 3105，不能仅凭“数据库命令失败”把两个阶段混为一谈。两条边界及恢复后序号/落点
均由阶段故障矩阵核对，报告缺少证据时不宣称验收通过。服务端 TLS 出站满额也会关闭
该会话；写入遇到 WANT_READ/WANT_WRITE 时保持原字节长度重试，后续排队字节单独发送，
避免背压期间改变重试参数导致 bad write retry。


## Chat线契约（C1编解码、C2内部服务已实现）

`chat/v1/chat.proto` 定义 1701–1706 客户端消息与 1801–1803 内部消息；两组的编号白名单
独立，均沿用 Envelope v1。`game/common/chat_protocol` 提供严格双向编解码，非法对象编码
抛 `std::invalid_argument`，非法输入解码返回空值。所有 Envelope **含头**至多 65536 字节；
请求/响应 request_id 非零，通知为零。未知字段按 protobuf 演进规则保留，未知枚举拒绝。
C2已有独立chat身份、内部TLS监听和频道状态机；Realm/客户端业务调用仍待C3，完整故障矩阵待C4/C5。

### 值与客户端消息

- `Channel{realm_id,kind,shard_id}`：realm_id非零；kind为1 SCENE或2 REALM。
  SCENE只允许shard_id=1/2，REALM必须为0；客户端只用kind选择当前服务端派生频道。
- `Epoch{channel,chat_incarnation}`：轮次包含完整频道；incarnation为1..128个非空白可打印
  ASCII字节。不同轮次/频道序号不比较，空频道仍不能复用同轮次的序号。
- `Membership{channel,character_id,realm_incarnation,session_binding,generation,scene_binding_epoch}`：
  角色和会话非零，Realm incarnation同上。Scene资格必须有场景绑定版本，Realm资格必须为0。
  generation在每条活动Realm–Chat链路上跨角色/频道严格递增，不能按资格各自重用。
- generation、link_generation、scene_binding_epoch及消息/发送计数取1..INT64_MAX，水位允许0，
  耗尽拒绝且不回绕；角色/会话ID仍支持完整非零uint64。
- `Receipt{epoch,channel_seq,client_send_seq}`：只表示内存在线Accepted，不表示持久、送达或已读。

| 消息 | 严格形状 |
|---|---|
| GetChatState | 仅kind；不接受可信发送者身份 |
| ChatState | kind、phase、资格代次、Epoch、join_watermark W、last_client_send_seq及可选last_receipt |
| SendChat | kind、资格代次、expected_epoch、client_send_seq和原始text；Epoch频道种类须与kind一致 |
| ChatSendResult | kind、资格代次、Epoch、client_send_seq、outcome、reason、可选receipt和retry_after_ms |
| ChatMessage | 接收者资格代次、Epoch、channel_seq、sender_character_id与text；发送者也收自己的推送 |
| ChatUnavailable | kind、非零有限reason及rejoin_required；不表示场景或训练丢失 |

phase为1 UNAVAILABLE或2 ACTIVE。UNAVAILABLE只带kind/reason，不夹带旧资格、轮次、水位或
回执；ACTIVE必须带有效资格/Epoch，reason=NONE。last_client_send_seq=0时无回执，否则必须
匹配回执的发送序号/Epoch，且回执channel_seq减原加入水位W不得小于last_client_send_seq。回执channel_seq也不得小于
client_send_seq；校验用减法防溢出，初次JOIN可以W=0。
重复活动JOIN必须回原Epoch/W和原发送回执；C2的ChatChannels保存这些状态，结构解码仍不代替状态机。

text是1..512个UTF-8字节，拒绝过长/截断/孤立续字节/过长编码/代理码点/超出U+10FFFF、
NUL和ASCII控制字符（含CR/LF/DEL）；保留原始正文，不归一化、不去首尾空格。proto用bytes，
避免protobuf在非法UTF-8错误日志中打印正文。应用日志和指标也不得记录原始正文。

outcome为1 ACCEPTED、2 REJECTED或3 UNKNOWN：

- ACCEPTED必须有同Epoch、同client_send_seq的回执，reason=NONE，retry_after_ms=0。
- REJECTED无回执且reason非零，不能用OUTCOME_LOST/GAP当作确定未受理。
- UNKNOWN无回执，reason仅DEADLINE/OUTCOME_LOST/UNAVAILABLE，重试建议为0；不能自动换号重发。

同资格每次新SEND必须last+1，至多一个在途；只保留最后受理的原始参数与回执。同号同参数
返回原回执且不重发，同号冲突、过旧或跳号拒绝。STATE无记录不证明迟到请求不会受理；资格
结束或重启后不承诺恢复旧回执。C2已实现以上状态迁移、受理前资源预留及有界分发，C1编解码只校验形状。

| reason编号 | 含义 |
|---|---|
| 0 | NONE |
| 1–5 | NOT_CONFIGURED、UNAVAILABLE、NOT_MEMBER、STALE_MEMBERSHIP、WRONG_EPOCH |
| 6–10 | INVALID_TEXT、RATE_LIMITED、BUSY、DEADLINE、OUTCOME_LOST |
| 11–15 | INVALID_SEQUENCE、SEQUENCE_CONFLICT、GAP、RESOURCE_LIMIT、COUNTER_EXHAUSTED |

REJECTED的RATE_LIMITED/BUSY/RESOURCE_LIMIT必须带1..1000ms建议，其他原因不带重试建议。
DEADLINE可以表示派发前确定拒绝或派发后UNKNOWN，接收者必须按outcome判断，不能单看reason。
EdgeError新增3201（Chat输入形状非法）与3202（尚未选角）；C3接线前不会发出这些业务错误。

### Realm–Chat内部线

C2内部线使用独立TLS/TCP、`realmmesh-chat/1`、服务CA和精确DNS SAN允许列表；编解码不提供认证。
`Link{realm_id,realm_instance_id,realm_incarnation,chat_instance_id,chat_incarnation,link_generation}`
的身份字符串均为1..128个非空白可打印ASCII字节。HELLO请求只留chat_incarnation为空、
link_generation=0，成功回复取得精确轮次与新链路代次；拒绝HELLO回显请求形状。其余消息必须
携带完整活动链路。C2核对证书/允许实例/Realm并替换旧链路，不允许etcd或客户端票据授权。

| operation | 请求必需内容 | STATUS_OK回复内容 |
|---|---|---|
| 1 HELLO | Link探测形状，无资格/SEND | 完整新Link，无资格/状态/发送结果 |
| 2 JOIN | Link、完整Membership | 同Membership、ACTIVE ChatState（原Epoch/W/最后回执） |
| 3 LEAVE | Link、完整旧Membership | 同旧Membership，无状态/结果 |
| 4 SEND | Link、Membership、SendChat | 同Membership、ChatSendResult；OK不等于Accepted |
| 5 STATE | Link、Membership | 同Membership、ACTIVE ChatState；无资格用失败状态 |

status取1 OK、2 INVALID、3 STALE_LINK、4 STALE_MEMBERSHIP、5 FORBIDDEN、6 BUSY、
7 UNAVAILABLE。非OK只回Link/operation/status，不夹带半份资格、状态或回执。内部通知1803带
Link、接收者Membership与ChatMessage，ID=0。编解码要求资格Realm/Realm incarnation匹配Link、
嵌套资格代次匹配、频道匹配且Epoch Chat incarnation匹配Link；它无法确认一个非零角色ID是否
属于已认证会话，也不能确认配置承载者。C2核对活动连接和配置Realm；C3仍须核对当前会话、场景绑定和角色权限。
旧链路完成、旧LEAVE和旧JOIN不能影响新资格；LEAVE先到也要推进链路highest_seen_generation，
活动资格的重复JOIN回原水位；具体有界状态机见[规格](specs/2026-10-23-chat-reference-loop.md)。

### 资源边界

轻量`chat_limits.hpp`固化规格中的三个频道、至多两个实例、64角色、每角色2资格、Scene16人/
Realm64人、每资格一个在途、5条/秒/突发10条、内部64请求、每链路8排队、1秒总截止、
1024待分发事件/4MiB、每客户端256通知/256KiB、16条传输连接、每轮64请求/64推送/每资格一个新
SEND、每链路1MiB出站及最多64接收者。`ChatBudget`的纯预留检查同时校验计数和字节，以减法
避免size_t溢出；C2实现服务端原子占用/释放、公平调度、计时器及TLS出站拒绝；Realm的全角色跨频道限流与
未收结果预算仍待C3。
运行、排队、待撤销和未收结果的占用都必须计入。C1单测不证明真实网络。C2的生产进程测试证明内部mTLS/排序/回执与单帧TLS输出预算拒绝，
不替代C4/C5的双实例隔离或真实慢消费者积压饱和验收。
