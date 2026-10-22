# Scene S1–S4 参考闭环

S2 提供独立 Scene、分片 1、至多 16 个角色的进入和移动互见。客户端仍只连 Realm；
Realm 按 ADR-0015 条件写入 MongoDB 后才确认和广播。单步曼哈顿距离至多 100，坐标为
0..10000 的整数，首次出生点 (0,0)。静态双实例转场与归属恢复已在 S3 接线；S4 已增加动态分配、同分片重建与排空；完整故障矩阵属于 S5。

## 自动验收

```bash
./scripts/run-scene-acceptance.sh --preset dev --jobs 6
```

入口先配置并构建本步目标，再运行状态机、mTLS、工作线程边界和真实多进程旅程。
隔离夹具启动 etcd 与 MongoDB 副本集；证书只在构建目录生成，服务 CA 独立于客户端
接入证书。验收不会连接共享开发库，也不读取其 URI 环境变量。

报告位于 `<所选构建目录>/acceptance/scene-reference-loop/<运行编号>/`，包含版本、工作树
差异摘要、配置 SHA256、平台、逐例结果、原始 CTest/JUnit 和各实例服务日志。双人用例
核对双方各十次移动后的快照、连续事件和数据库最终 seq=11、落点 (100,0)/(0,100)。
重复指令不增加事件/写入，非法移动不消费序号，离场可见；Scene 缺失时心跳和训练仍成功。
该入口仅验证本机多进程 TLS/TCP，不代表 Scene QUIC、跨机或玩法容量。

## 显式启用开发服务

默认 `configs/main.config` 继续声明四个登录服务，`realm.scene.enabled=false`。启用 Scene 时：

1. 在自己的配置树 `main.config` 的 services 中加入 `{ name = "scene" }`，不把它加入
   Realm 的 `depends_on`；分进程启动 `realm_mesh --config <配置树> --service scene`。
2. 使用 `services/scene.lua` 的独立 TLS/TCP 监听（默认 127.0.0.1:7200）。配置服务 CA、
   Scene 证书/私钥和 Realm DNS SAN 允许列表；初版一个 Scene 固定承载 `(realm_id=1,shard_id=1)`。
3. 将 Realm 配置的 `realm.scene.enabled` 设为 true，配置 Scene 静态后备端点、预期实例 id、
   服务端 DNS SAN、服务 CA 以及 Realm 客户端证书/私钥。开启发现时由 `scene` 发现端点覆盖
   静态端点；发现记录不授予身份或角色归属，Realm 不等待 Scene 启动才就绪。
4. 手动启动 Realm/登录服务前遵循 [共享 MongoDB 说明](shared-mongodb.md)，经
   `scripts/with-shared-mongodb.sh` 启动，凭据仍留在仓库外。

默认环境变量映射：

| 用途 | 环境变量 |
|---|---|
| 独立服务 CA | `REALMMESH_SERVICE_CA_FILE` |
| Scene 证书/私钥 | `REALMMESH_SCENE_CERTIFICATE_FILE` / `REALMMESH_SCENE_PRIVATE_KEY_FILE` |
| Realm 客户端证书/私钥 | `REALMMESH_REALM_CERTIFICATE_FILE` / `REALMMESH_REALM_PRIVATE_KEY_FILE` |

默认精确 DNS SAN 为 `scene-dev-01.scene.service` 与 `realm-dev-01.realm.service`；
换实例需同时修改证书与允许列表。通道仅 TLS 1.3、ALPN `realmmesh-scene/1`，缺证书、
错误 CA、角色身份或 ALPN 均无法执行内部操作。静态模式首次认证后用 SNAPSHOT 探测 incarnation，
之后每条消息绑定该次进程运行，不能以同名重启复用旧身份。

## 参考客户端与恢复

`RealmSession::scene()` 返回独立 `SceneSession` 接口，复用 Login Chain 已建立的连接。
选角后先 `get_scene_state()` 读取最后序号，以最后序号 + 1 `enter_scene()`；成功后再取完整
快照，按其 incarnation/水位消费 `poll_scene()` 的加入、移动和离场事件。
训练和心跳等待期间的通知保留在有界缓存中，缓存满时结束该客户端会话。

`move_in_scene()` 使用绝对坐标和下一场景序号；重复同序号同参数回放最后结果。
3105 表示结果不确定，不得改参数或跳号；先查询/重试原指令。提交后激活/确认失败仍保留
已提交事实，恢复时从数据库落点准备/激活。会话关闭/顶替立即停止派发和回包，异步撤销只
匹配旧版本及完整 owner，不能撤销新会话的归属。SceneUnavailable 后重新查询快照；Realm
发现事件缺口会主动重同步并暂停观察者的后续事件，直到 GetSceneState 交付新快照。

单实例内部请求上限 64（含运行、排队及未收结果），同会话最多一个执行加八个排队。
清理与探活使用相同的三秒链路预算，正常延迟不能因更短的探测截止误断共享链路。
三秒指令总截止包含排队；帧线程对尚未发写的任务取消并回 3104，已发写只能回 3105，后台继续
对账，不能把超时当作未提交。数据访问和内部网络各自受已有连接超时约束；阻塞调用只在
专用场景线程，训练工作池、心跳帧处理独立。Scene 暂存成员/候选三秒回收，链路关闭移除
该链路的可见成员。S3 使用一个编排线程、每实例一个惰性 IO 线程，以及独立于训练的惰性四线程场景数据池。
协程只在编排线程恢复，线程之间只传递有界任务/future；同一角色的旧、新会话串行结算。
每实例最多 64 个指令、两个实例最多 128 条编排记录，回包/内部推送各 1024，数据池至多 256。

## 观测

Realm 9102 暴露 `scene_commands_total{operation,outcome}`、`scene_command_duration_seconds`、
`scene_active_bindings`、`scene_queue_rejections_total`、`scene_stale_rejections_total`、
`scene_write_uncertain_total`、`scene_pending_readbacks`、`scene_snapshot_resync_total`。
Scene 9106 暴露 `scene_shard_members`、`scene_prepared_members`、`scene_draining`。
标签只有有限操作/结果集；账号、角色和 request_id 不作指标标签。
Grafana 的 `scene.json` 和两条 Scene 告警与实际指标名一起验证；排空编排仍待 S4。


## 双实例转场与重启恢复（S3）

第二个 Scene 配置使用相同服务名 scene、独立 instance_id（示例 scene-dev-02）、监听端口
7201 与服务证书 DNS SAN scene-dev-02.scene.service，将 scene.shard_id 设为 2。两个进程各
使用自己的配置树；默认四服务拓扑仍可独立运行。Realm 的 realm.scene.targets 显式增加：

```lua
targets = {{ shard_id = 2, host = "127.0.0.1", port = 7201,
    instance_id = "scene-dev-02", server_identity = "scene-dev-02.scene.service",
    entry_x = 200, entry_y = 300 }},
```

主分片 1 沿用原配置与入口 (0,0)。S3 只接受一个额外目标分片 2；同分片转场和未知目标回
3102。发现按配置 instance_id 查找；双向 TLS 仍必须验证精确服务身份。

参考客户端 transfer_scene(next_seq, target_shard_id, deadline) 等待目标持久结果；成功后
get_scene_state() 获取目标快照。提交前失败可以继续在源分片移动；已提交结果不确定先查询、
原序号重试，不能跳号或回滚源分片。A 的条件移除失败不会阻塞 B 的已确认移动。

Realm 或 Scene 崩溃后，先等旧 etcd 注册租约失效，再启动同名实例，保留原服务身份/分片配置。
重登并选角后查询最后序号与落点；需要重新激活时用最后序号 + 1 enter_scene()，位置从数据库
恢复，不重置序号。同序号回放本身不会复活旧进程的成员。S3 不把原分片重新分配给其他实例。

正常停机等待已发出的持久写结算，再对每个会话尝试一次清理；数据库持续失败或 Scene
持续返回移除繁忙时，不无限续试。实例链路关闭后 Scene 回收可见成员，持久清理失败的
旧归属仍按上述新 incarnation 流程恢复，已确认指令与落点不会为退出而删除。

统一验收包含两个真实 Scene 进程间的 A→B→A、相同序号回放、旧版本 CAS 拒绝、Scene SIGKILL
后另一分片继续移动、以及 Realm 在目标已持久提交但尚未激活时 SIGKILL 后恢复。最后一项用
生产 SceneShard 与真实 mTLS 暂停 B 的激活边界；Realm 与 MongoDB 保持真实进程和后端。
常规与 Scene SIGKILL 用例使用入口组、Realm、Scene A/B 四个独立 realm_mesh 进程。


## 动态分片承载与排空（S4）

有界参考配置为一个逻辑 Realm、分片 1/2、两个预授权 Scene 实例；每片至多 16 人，每实例
可承载两片。Realm 启动时配置两个实例的身份允许列表与逻辑分片入口，第二实例可运行中加入；
已有角色不会因为新实例加入而自动重平衡。`scene.shard_id` 只决定静态模式的分片，动态模式
按共享承载记录工作。超出配置允许列表的实例不参与分配。

在 Realm 与两个 Scene 的独立配置树中显式设置：

```lua
-- common/discovery.lua：动态模式要求真实注册，初次失败不发布业务就绪。
service_discovery = { enabled = true, required = true }
-- services/realm.lua：保留上方 targets 分片2允许列表和独立mTLS身份。
realm = { scene = { enabled = true, dynamic = true } }
-- 两个 services/scene.lua：instance_id、端口、证书各自独立。
scene = { dynamic = true }
```

以上为要合入现有配置表的字段，不替代训练、TLS、日志或数据源配置。Realm/Scene 指向相同
Player Data 数据库；手动开发仍须按共享库说明经 `with-shared-mongodb.sh` 启动，URI、CA 与
凭据留在仓库外。动态模式不使用静态端点后备；发现完整快照、精确服务身份和业务就绪全部
成立后才分配。Scene 缺失不阻塞 Realm 登录后的心跳、训练；etcd 必需注册失败会让服务启动
失败，不能把发现故障当成所有实例已退出。

MongoDB 的 `scene_instances`、`scene_shards` 是实例接纳/分片承载事实，不手工删除记录或
重置版本。实例排空事务同时禁止新分配并标记其全部分片；新承载版本与角色写在事务内互相
栅栏。旧进程/旧版本不能继续提交位置。故障分片在另一就绪实例上重建时保留 shard_id 和
已确认坐标；客户端按最后序号 + 1 EnterScene 重新绑定，不是转往另一个逻辑分片。

### 有身份校验的控制入口

先构建 `realm_mesh_scene_control`。它使用 Realm 服务身份的 mTLS 客户端证书，默认读
`REALMMESH_SERVICE_CA_FILE`、`REALMMESH_REALM_CERTIFICATE_FILE`、
`REALMMESH_REALM_PRIVATE_KEY_FILE`，也可显式指定 `--ca`、`--certificate`、`--key` 文件路径。
不接受公网客户端票据；Scene 证书或错误 CA/服务端身份无法调用。

```bash
# dev 预设示例；其他预设使用其实际构建目录。
./build/dev-ninja/bin/realm_mesh_scene_control --status \
  --port 7200 --instance scene-dev-01 --server-identity scene-dev-01.scene.service
./build/dev-ninja/bin/realm_mesh_scene_control --drain \
  --port 7200 --instance scene-dev-01 --server-identity scene-dev-01.scene.service \
  --operation-id deploy-1 --timeout-ms 20000
```

输出逐行 JSON，含 phase、draining、operation_id、active_members、prepared_members 与
owned_shards。`--drain` 退出 0 只代表 COMPLETE；退出 2 是失败或客户端等待结束但未完成，
此时查看 JSON phase，实例仍保持 draining。`--status` 退出 0 代表查询成功，仍需看 phase；
认证、配置或传输错误退出 1。

尝试标识默认使用实例 id。同一标识重复调用不会延长服务端截止或重复迁出；RUNNING 期间
不同标识也合并到原尝试。FAILED 后修复目标容量/连接，再以新的 `--operation-id` 重试；
这不会恢复接纳源角色。单次服务端截止为 100..60000 毫秒。完成后的重复调用仍返回 COMPLETE。

Realm 先结算该角色已经派发的场景指令，再以下一 scene_seq 做 S3 转场。成功后 1508 通知
客户端查询 SceneState，取得新序号和目标快照继续；与排空竞争的请求若获 3103，同样先查询，
不猜测序号。持久结果不确定、目标激活/源移除未完时不报告完成。目标满员或不可用时源角色
仍可移动，实例保持排空并在截止后明确失败。只有成员/准备清空、在途写及未完迁出均结算后
才释放分片；业务停用后保留管理端点与发现注册，确认 COMPLETE 后再停止该进程。

### 观测与验收

Scene 新增 `scene_owned_shards`、`scene_drain_remaining_members`、`scene_drain_failed`、
`scene_drain_attempts_total`、`scene_drain_failures_total{reason="timeout"}`。
Realm 新增 `scene_ready_instances`、`scene_drain_pending_transfers`、`scene_drain_transfers_total`、
`scene_drain_retries_total{reason="capacity"|"unavailable"}`、
`scene_shard_assignments_total{operation="assign"|"recover"|"release"}` 与
`scene_shard_reconcile_errors_total`。剩余成员为零时仍要核对未完转场及承载分片；不能单凭一个
指标宣称完成。Grafana Scene 面板和排空/承载协调告警引用同一组指标。

统一脚本强制包含 SceneShardStoreTest、SceneDynamicJourneyTest 和 SceneDynamicJoinJourneyTest。
真实 MongoDB 验证并发分配及排空/分配事务竞态；多进程验证运行中加入、成功排空与重复请求、
16 个真实角色占满目标时保留源服务并明确失败、同分片故障重建和旧版本拒绝。角色入场仍走
生产 LoginChain、选角、进入和转场；四路独立 transport 仅并发准备登录，容量与全部断言不缩减。
