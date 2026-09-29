# RealmMesh 接入链与参考业务路线图

- 日期：2026-09-27
- 状态：规划确认；阶段 0 的 Admission Grant 迁移（#82）已实施并在 2026-09-30 通过全量验收（518 条用例全绿，另有 6 条 dev-services 用例在可用 `ps` 的环境下 6/6），验收证据见 #82 评论
- 实施节奏：单人逐步交付；每一步产出可重复运行的验收证据
- 第一验收环境：macOS 本机四服务多进程、真实 TLS/TCP 与 etcd，账号拉取使用可控数据桩

## 已有基础与边界

现有代码覆盖 Login Verifier、Queue Scheduler、Gateway Login Pipeline、Realm 的 EnterRealm 兑换，以及客户端 Login Chain。Gateway 的生产接线使用 etcd 共享的 Admission Grant 消费存储；存储不可用时拒绝新的准入，遵循 [ADR-0009](../adr/0009-identity-bound-admission-grant.md)。Admission Grant 迁移已在 2026-09-30 重新构建并跑通全量测试（真实 etcd、真实 TLS/TCP 的多进程服务组），此前的旧构建产物不再作为验收依据。

仍需面对的实际边界：

- Gateway 生产装配仍使用固定延迟并成功的 `DelayedAccountFetchPort`，尚未拉取真实账号数据。见 `framework/service_host/src/service_host.cpp` 与 `game/gateway/src/account_fetch_port.cpp`。
- Queue 已在 #89 中把每次登录尝试的发号映射与递增后的快照合入同一笔 etcd 事务，响应前提交；响应丢失先回读，进程重启可恢复原号。当前本机 Debug + 单节点 etcd 同步路径实测约 200 次/s，只证明恢复语义，不证明约 1700 次/s 容量目标；批提交或权威存储重选留待 #91 的目标 Linux 负载验收。
- Realm 当前完成票据验讫、内存会话与心跳；没有选角或游戏消息处理。见 `framework/service_host/src/service_frame.cpp`。
- `loadgen` 有 `full` 登录目标，但报告没有单列 Realm 入场结果。见 `tools/loadgen/src/loadgen.cpp`。
- [README](../../README.md) 与[旧登录链规格](../specs/2026-09-12-login-chain-surge.md)的部分段落仍描述 `admitted` Queue Number；现行决策是独立 Admission Grant。旧规格中的容量推演属于假设，不能当作实测结论。

## 实施顺序

### 0. 收束当前 Admission Grant 迁移（[#82](https://github.com/lvivvde/RealmMesh/issues/82)）

1. 在当前工作树重新配置、构建并运行相关单测、集成测试和全量 `ctest`，记录结果；旧 `build/dev` 的测试列表不能代表当前源码。
2. 验证身份 Token、Queue Number、Admission Grant 不可互换；跨身份拼接、重复准入、共享存储不可用都被拒绝；正常情况下 Gateway 到 Realm 的路径可完成。
3. 更新 README、协议/架构说明及旧规格的修订注记，使术语与 [CONTEXT.md](../../CONTEXT.md)、ADR-0009 一致，保留历史决策的可追溯性。

**完成条件：** 当前源码的构建和测试结果可复现，文档不再把 Queue Number 写作网关准入凭据。此阶段完成前不开始依赖新凭据语义的端到端验收。

### 1. macOS 本机登录闭环（[#89 取号恢复](https://github.com/lvivvde/RealmMesh/issues/89) → [#90 全链验收](https://github.com/lvivvde/RealmMesh/issues/90)）

按以下顺序交付小增量：

1. **取号恢复语义。** 明确序号、`identity_jti` 幂等映射与放行水位各自的权威存储和提交顺序；比较持久化方案及约 1700 次/秒取号目标的写入成本，再选择实现。发号响应前必须达到已定义的持久化边界。验收包括 Queue 在两次放行之间重启后号码不重复、同一登录尝试重复取号仍得到原号码、既有号牌不会丢位次。
2. **确定性全链路用例。** 在本机启动真实 Login Verifier、Queue、Gateway、Realm 和 etcd，以可控账号及拉取桩驱动 `verify → tickets → progress/me → Admission Grant → Gateway Handoff → EnterRealmAccepted`，随后验证 Realm 会话心跳与正常断开。用固定测试数据和隔离端口让用例可反复运行；跨进程或占端口测试按 `tests/README.md` 标为 `integration`。
3. **故障与恢复矩阵。** 注入 Queue 重启、Gateway/Realm 退出、etcd 暂时不可用、超时及凭据重放；逐项核对客户端终态、重新取号/重新登录规则、准入失败关闭、额度回收与服务恢复后的再次成功。对已成功消费但响应不确定的情况，遵循 ADR-0009 的至多一次语义，不许用回退路径重复放行。
4. **验收报告。** 为完整登录结果记录 Realm 入场成功/失败与延迟，连同现有 verify、tickets、poll、attach、handoff 指标给出一份可重复运行的本机报告和运行命令。报告明确 macOS 只验证 TLS/TCP，账号拉取仍是可控桩。

**完成条件：** 新构建上主路径和故障矩阵稳定重复通过；Queue 重启不重号、不丢同一登录尝试的幂等结果；Realm 入场、心跳和断开有自动化证据。此结果只证明本机登录契约，不宣称真实存储或百万容量。

### 2. Linux 接入可靠性与 M1–M4 负载（[#91](https://github.com/lvivvde/RealmMesh/issues/91)）

1. 在 Linux 验证 QUIC 与 TLS/TCP 两条路径、竞速降级规则、真实证书校验和多进程部署；随后扩到跨机器的服务发现、etcd TLS/认证、故障切换和停机恢复。
2. 以[登录链规格](../specs/2026-09-12-login-chain-surge.md)中的 M1 soak、M2 放行、M3 轮询、M4 故障注入为验收框架。给每级补齐可执行命令、输入规模、延迟/错误/资源门槛、采集方式和重复次数；门槛以实际环境和测量结果校准。
3. 在负载下验证 Queue 发号持久化成本、Grant 集群单次消费、Gateway/Realm 额度收敛、进程故障后的恢复，并据实测调整批次、轮询和告警参数。

**完成条件：** Linux 的 QUIC/TLS 行为和 M1–M4 各有可复现报告；报告列出硬件、拓扑、版本、配置、失败注入和结果。云端百万 M5 不计入本阶段完成条件。

### 3. 真实账号与角色数据、选角（[#92 数据](https://github.com/lvivvde/RealmMesh/issues/92)、[#93 角色业务起点](https://github.com/lvivvde/RealmMesh/issues/93)）

1. 先确定账号、角色各自的事实来源、身份键、并发更新与封禁状态传播语义，再按读写模式、恢复目标和压测要求选择数据库/缓存。若取舍难以逆转且有真实备选方案，再记录 ADR；不预先指定产品。
2. 用持久实现替换 `ConfigAccountStore` 的示例账号来源与 Gateway 的 `DelayedAccountFetchPort`，保持现有接口边界。实现有界并发、超时、重试、回压和故障注入；确保数据错误不会被当作拉取成功。
3. 建立角色数据模型与 Realm 入场后的加载路径，完成角色列表、创建、选择，验证身份与角色归属、重启恢复、迁移及数据源不可用时的行为。用相同 `full` 路径重跑 Linux 登录链验收，区分模拟拉取与真实数据结果。

**完成条件：** 账号和角色不依赖进程内样例数据；Gateway 实际读取并校验，同一账号能在 Realm 列出、创建、选择自己的角色；重启与数据故障均有测试及指标。

### 4. 多实例 Scene 参考业务（待拆 Spec）

Realm 保持客户端长连接和角色入口，将场景指令路由到独立的 `scene` 服务身份。一个
`scene` 身份可启动多个实例，各自承载场景分片；地图本身不成为服务身份。先明确角色、
会话、场景分片、已确认落点和唯一场景归属的权威边界，再完成以下可重复用例：

1. 两名玩家进入同一场景，移动后互相看到状态；至少两个 Scene 实例存在，并完成跨实例转场。
2. 运行中加入 Scene 实例可承载新场景；停用实例时排空并转移其场景，不要求全站重启。
3. 一个 Scene 实例退出时其他场景继续运行；受影响玩家重新入场后恢复已确认的角色落点，不能同时归属两个场景。

**完成条件：** 本机多进程自动化验收覆盖移动可见、转场、动态加入、排空、故障隔离与重新入场恢复。此结果不宣称已验证 Scene 业务的跨机器部署或玩法负载容量。

### 5. 独立 Chat 服务（待拆 Spec）

新增独立 `chat` 服务身份，由 Realm 路由客户端聊天请求。提供场景内频道和跨场景频道；
同一频道的在线消息顺序和失败反馈应有明确契约。聊天记录、离线补投、好友与邮件不进入本阶段。

**完成条件：** 本机多进程下，两个 Scene 实例中的玩家可按频道收发；跨实例转场后仍可收发；Chat 实例故障时场景移动与转场不被阻断，聊天失败可见，恢复后可再次发送。

### 6. 云端百万 M5 单独验收（[#94](https://github.com/lvivvde/RealmMesh/issues/94)）

在真实数据、Scene 与 Chat 参考路径稳定后，单列云端百万瞬时登录实验。先核算环境预算、压测流量生成能力、CDN/边缘条件和可观测成本；在批准的环境中按[规格目标](../specs/2026-09-12-login-chain-surge.md)测放行时长、位次保持、故障恢复及最终 Realm 连接容量。M1–M4 或本机结果不能替代 M5；M5 不代表百万玩家移动或聊天容量。

## 决策点与维护规则

- **取号权威存储：** 阶段 1 开始时比较可用方案，给出崩溃一致性与吞吐证据，再定实现。不能仅凭目前的放行快照宣布取号持久化。
- **真实数据源：** 阶段 3 开始时决定账号和角色存储技术、迁移与缓存策略；第一阶段的数据桩只用于接入契约验收。
- **ADR 与术语：** 现有 Queue Number、Admission Grant、Edge Session、Login Chain 等术语沿用 `CONTEXT.md`。`scene`、`chat` 目前是规划中的服务身份，进入 Spec 时再明确角色、场景分片、频道等领域术语；只有遇到难逆转、非显然且有实际备选方案的取舍，才补 ADR。
- **目标范围：** [README 必做路线图](../../README.md#必做路线图)是承诺入口；[架构文档](../architecture.md#目标业务拓扑规划中)说明目标服务边界。好友、邮件、FPS 大厅/匹配/房间是非必做候选，不预建服务目录或身份。
- **Issue 跟踪：** 阶段 0 的实施票是 #82；阶段 1–2 对应 #89–#91，阶段 3 从 #92、#93 开始，#94 对应 M5。Scene 与 Chat 的完整工作项还需单独拆分；#88 的 L1 soak 重试缺陷是 #91 的阻塞项。Issue 的实时状态仍以 GitHub 为准。
