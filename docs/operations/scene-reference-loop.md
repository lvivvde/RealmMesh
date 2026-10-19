# Scene S2 参考闭环

S2 提供独立 Scene、分片 1、至多 16 个角色的进入和移动互见。客户端仍只连 Realm；
Realm 按 ADR-0015 条件写入 MongoDB 后才确认和广播。单步曼哈顿距离至多 100，坐标为
0..10000 的整数，首次出生点 (0,0)。转场、动态分片和完整故障矩阵分别属于 S3–S5。

## 自动验收

```bash
./scripts/run-scene-acceptance.sh --preset dev --jobs 6
```

入口先配置并构建本步目标，再运行状态机、mTLS、工作线程边界和真实多进程旅程。
隔离夹具启动 etcd 与 MongoDB 副本集；证书只在构建目录生成，服务 CA 独立于客户端
接入证书。验收不会连接共享开发库，也不读取其 URI 环境变量。

报告位于 `<所选构建目录>/acceptance/scene-reference-loop/<运行编号>/`，包含版本、工作树
差异摘要、配置 SHA256、平台、逐例结果、原始 CTest/JUnit 和三组服务日志。双人用例
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
错误 CA、角色身份或 ALPN 均无法执行内部操作。首次认证后用 SNAPSHOT 探测 incarnation，
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
该链路的可见成员。S2 一个 Scene 实例使用一个专用线程；多实例隔离扩展在后续阶段实现。

## 观测

Realm 9102 暴露 `scene_commands_total{operation,outcome}`、`scene_command_duration_seconds`、
`scene_active_bindings`、`scene_queue_rejections_total`、`scene_stale_rejections_total`、
`scene_write_uncertain_total`、`scene_pending_readbacks`、`scene_snapshot_resync_total`。
Scene 9106 暴露 `scene_shard_members`、`scene_prepared_members`、`scene_draining`。
标签只有有限操作/结果集；账号、角色和 request_id 不作指标标签。
Grafana 的 `scene.json` 和两条 Scene 告警与实际指标名一起验证；排空编排仍待 S4。
