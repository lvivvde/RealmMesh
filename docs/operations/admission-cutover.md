# Admission Grant 切换运维手册

- 日期：2026-09-30（依源码修订 `b394482` 起的准入迁移工作树核对；同日全量验收通过，见 #82 评论）
- 适用范围：把一个 deployment 的网关准入凭据从带 `admitted` 的 Queue Number 换成身份绑定的 **Admission Grant**
- 协议权威：[Security Spec #79](https://github.com/lvivvde/RealmMesh/issues/79)、[ADR-0009](../adr/0009-identity-bound-admission-grant.md)、[ADR-0006 修订](../adr/0006-stateless-queue-number.md)；接口与错误码见 [protocol.md](../protocol.md)
- 性质：**一次性、操作上原子**的协议替换。失败即关闭（fail-closed），不保留新旧选择器、特性开关、双格式或回退路径

## 1. 适用范围与前置条件

切换后的凭据链（每段一种凭据、一个签发方、一次消费）：

```text
身份 Token → 排队号牌 → Admission Grant → EnterRealm 票据
```

| 凭据 | 签发方 | 网关是否接受 | 消费次数 |
|---|---|---|---|
| 身份 Token | `login_verify` | 是（仅用于身份与 `identity_jti`） | 可重复校验 |
| 排队号牌（`aud=realmmesh-queue`、`purpose=queue-position`） | `queue` | **否** | 排位查询可重复 |
| Admission Grant（`aud=realmmesh-gateway`、`purpose=gateway-admission`） | `queue` | 是 | 同一 `identity_jti` 在一个 deployment 内至多一次 |
| EnterRealm 票据 | `gateway` | — | Realm 单次兑换 |

切换后旧 Queue Number 一律作废：不翻译、不重绑、不因“让发布继续”而重新接受。切换完成前必须同时满足：

1. 目标修订已按 `./scripts/build.sh` 重新配置、构建并通过全量 CTest；旧 `build/dev` 产物不构成证据。
2. Queue 与所有接受同一 deployment 的 Gateway 使用同一个 `deployment_id`。
3. 所有接受该 deployment 的 Gateway 共用**同一个线性一致消费存储**（etcd，前缀由 `admission.consumption_prefix` 指定，默认 `/realmmesh/admission/consumption`）。共享存储是准入权威，进程内状态不作为回退。
4. Queue Number 与 Admission Grant 使用**独立的 Ed25519 密钥与 kid**；私钥只存在于排队服进程的环境变量中，网关只装载公钥。
5. 网关 `admission.grant_window_seconds` 与排队服 `queue.admit_grace_seconds` **取值一致**，且不超过协议硬上限 600 s（`game/common/include/realmmesh/game/common/admission_grant.hpp`）。

## 2. 冻结与准备

1. **冻结修订与配置**：记录源码修订、`configs/services/queue.lua`、`configs/services/gateway.lua`（含 `admission.deployment_id`、`grant_keys` 的 kid 列表、`grant_window_seconds`、`consumption_prefix`）以及排队服的 `admit_grace_seconds`。切换窗口内不得再改这些值。
2. **生成并分发密钥**（每项一个来源，见 [README 运行段](../../README.md)）：
   - 排队服进程环境：`REALMMESH_QUEUE_NUMBER_KEY_SEED`、`REALMMESH_ADMISSION_GRANT_KEY_SEED`、`REALMMESH_IDENTITY_KEY_SEED`（`game/queue/src/queue_service.cpp`）。
   - 网关进程环境：`REALMMESH_ADMISSION_GRANT_PUBLIC_KEY`（与排队服 Grant 种子配对的公钥 hex，变量名由 `grant_keys[].public_key_environment` 指定）、`REALMMESH_ADMISSION_CONSUMPTION_DIGEST_KEY`、`REALMMESH_IDENTITY_KEY_SEED`、`REALMMESH_SESSION_TICKET_KEY`（`framework/service_host/src/service_host.cpp`）。
   - Realm 进程环境：`REALMMESH_SESSION_TICKET_KEY`（`framework/service_host/src/service_frame.cpp`）。
   - 密钥规则：每个签发方**恰好一把活动签名密钥**；验证方用静态加载的 kid 索引键环，未知 kid 直接拒绝、不试遍全环；退休验证键保留到该凭据最大寿命 + 时钟容差之后。
3. **备份将被作废的状态**（etcd，端点见 `queue.etcd_endpoint`；`etcdctl` 由 `./scripts/install-etcd.sh` 安装到 `.tools/etcd-v3.6.14/`）：
   ```bash
   ./.tools/etcd-v3.6.14/etcdctl --endpoints=http://127.0.0.1:2379 \
       get /realmmesh/queue/snapshot > queue-snapshot-$(date +%Y%m%d%H%M).backup.txt
   ./.tools/etcd-v3.6.14/etcdctl --endpoints=http://127.0.0.1:2379 \
       get --prefix --keys-only /realmmesh/admission/consumption
   ```
   - `/realmmesh/queue/snapshot`（键名以 `queue.snapshot_key` 为准，单键）：排队服权威状态，含 `released_number`、`next_number`、放行批次区间与速率窗口（`game/queue/src/queue_store.cpp`）。
   - `/realmmesh/admission/consumption` 前缀（以 `admission.consumption_prefix` 为准）：**只查看，不删除**；确认其中没有需要保留的切换前记录（处置规则见 §3 第 8 步）。
4. **确定排空策略**：选择“等待在途 attach 自然结束”（推荐，已在 Fetching/Handoff 的会话继续完成）或“入口立即拒绝新 attach”。二者都必须停止**新**登录，而不打断已受理的 Edge Session。

## 3. 切换序列

每步末尾的「停」是明确的决策点：不满足判据就停在此步并按 §5 处置，不要跳到下一步。

1. **暂停新放行与准入**：停止客户端新登录（或按部署手段从入口拒流）。确认排队服不再产生新批次、网关不再受理新 attach。
   → 判据：网关 `edge_sessions{stage="pending"}` 不再增长；排队服 `admit_batches_total` 停止增长。**停**。
2. **排空在途 attach**：按 §2 第 4 步的策略等待 `edge_sessions{stage="fetching"|"handed_off"}` 归零或达到允许的最大等待。
   → 判据：`fetching` 与 `handed_off` 会话已按策略结算。**停**。
3. **部署 Queue**（发布放行批次台账与凭据改动）：
   ```bash
   export REALMMESH_QUEUE_NUMBER_KEY_SEED="<hex seed>"
   export REALMMESH_ADMISSION_GRANT_KEY_SEED="<hex seed>"
   export REALMMESH_IDENTITY_KEY_SEED="<hex seed>"
   export REALMMESH_TLS_CERTIFICATE_FILE="<cert>"
   export REALMMESH_TLS_PRIVATE_KEY_FILE="<key>"
   ./build/dev/bin/realm_mesh --config configs --service queue
   ```
   生产必须保持 `snapshot_required = true`：etcd 不可达或快照损坏时启动失败，不得用 `false` 绕过。
   → 判据：进程存活且 `realmmesh_service_ready{service_name="queue"}` 为 1（§4）。**停**。
4. **部署 Gateway**（Admission Grant 校验 + 集群消费）：
   ```bash
   export REALMMESH_ADMISSION_CONSUMPTION_DIGEST_KEY="<hex>"
   export REALMMESH_ADMISSION_GRANT_PUBLIC_KEY="<与排队服种子配对的公钥 hex>"
   export REALMMESH_IDENTITY_KEY_SEED="<hex seed>"
   export REALMMESH_SESSION_TICKET_KEY="<hex>"
   ./build/dev/bin/realm_mesh --config configs --service gateway
   ```
   缺失或畸形的密钥材料必须在**监听前**抛错退出，不接受“先起来再补”。
   → 判据：`realmmesh_service_ready{service_name="gateway"}` 为 1，且 `edge_credential_result_total{result="store_unavailable"}` 不增长。**停**。
5. **部署客户端 wire**：客户端只提交 `identity_token` + `admission_grant`，不再携带准入用的 Queue Number。客户端与网关必须同批切换，中间不存在同时接受两种格式的版本。
   → 判据：切换后的客户端版本已全量下发。**停**。
6. **激活新的 key id**：确认排队服 `queue_number_kid` / `admission_grant_kid` 与网关 `grant_keys[].kid` 一致；轮换重叠期把退休 kid 一并列在 `grant_keys` 中，直到其凭据最大寿命 + 时钟容差过去再删除。
   → 判据：网关启动日志无键环解析错误；未知 kid 的凭据在验证阶段被拒。**停**。
7. **校验就绪**：按 §4 全表逐项确认。**任何一项不绿都不得进入下一步。**
8. **清除旧 Queue Number 与队列状态**：删除排队服快照键（备份见 §2 第 3 步），使旧号位与旧放行窗口整体失效：
   ```bash
   ./.tools/etcd-v3.6.14/etcdctl --endpoints=http://127.0.0.1:2379 \
       del /realmmesh/queue/snapshot
   ```
   键名以配置的 `queue.snapshot_key` 为准（单键）。**不要**为“解锁”删除 `/realmmesh/admission/consumption` 下的消费记录——已提交的消费必须保持已消费（ADR-0009）。若本 deployment 决定重置消费前缀，那是独立的、需事先批准的部署动作，不属于本手册的默认序列。
   → 判据：队列状态已清空且备份可读。**停**。
9. **重新开放取号与准入**：恢复入口流量，允许新登录。
   → 判据：§6 核验通过。

## 4. 就绪判据

| 检查 | 命令 / 信号 | 期望 |
|---|---|---|
| 进程就绪（四服务通用） | `curl -s http://127.0.0.1:<metrics_port>/metrics \| grep '^realmmesh_service_ready'`；端口：realm 9102、gateway 9103、login_verify 9104、queue 9105 | `realmmesh_service_ready{service_name="…",service_instance="…"} 1` |
| Gateway 消费存储可达 | 同上，看 gateway 的 `realmmesh_service_ready` | 为 1；存储探针失败时该值为 0，而进程存活不受影响 |
| 消费存储探测细节 | 网关 `GatewayAdmission` 以 1 s 间隔探针，etcd 实现向 `/v3/kv/range` 发起前缀 range 请求 | 探针为真才计入就绪；不得改用进程内存准入 |
| 键环装载 | 网关启动时逐把解析 `admission.grant_keys` 的 `public_key_environment` | 缺变量或非 hex 公钥 → 启动前失败 |
| Queue 权威状态 | 生产 `snapshot_required = true` 时启动读取 `queue.snapshot_key` | 快照缺失 = 确属空状态；etcd 不可达或快照损坏 = 启动失败 |
| 服务发现（如启用） | `configs/common/discovery.lua`：`enabled`、`required` | 启用且 `required=true` 时注册失败即退出；`required=false` 时仅告警且**不就绪**，续约成功后转就绪 |
| 窗口一致 | 网关 `admission.grant_window_seconds` 与排队服 `queue.admit_grace_seconds` | 相等且 ≤ 600 s |

注意：网关的就绪判据是「管线健康 + 本地额度可用（含消费存储探针）」，与进程存活是两个信号；不要用进程存活代替就绪。

## 5. 回滚与失败处置

| 现象 | 判据 | 处置 |
|---|---|---|
| 密钥材料缺失/畸形 | 进程在监听前抛错退出（如 `… is not set`、公钥解析失败） | 修正环境变量后重启；不得放宽校验或临时改小 `grant_window_seconds` 绕过 |
| 消费存储不可用 | 客户端收到 `1006`（可重试）；网关就绪为 0；`edge_credential_result_total{result="store_unavailable"}` 增长 | 恢复 etcd 后等待探针转真；**绝不**退回进程内消费，也**不**把存储故障当作凭据错误 |
| 排队服在发号中途重启 | 放行快照 + 批次台账的持久化边界 | 重启不得推断新的放行时间或续期未知窗口；无法恢复的号位按 §3 第 8 步整体作废，不做逐条修补 |
| 集群部分部署（部分 Gateway 已切换） | 未切换实例拒绝新凭据、已切换实例拒绝旧 Queue Number | 不保留双路径：要么在同一窗口内完成切换，要么整体回退到切换前修订并清空队列状态；不允许长期混跑 |
| 就绪始终不绿 | §4 任一项不满足 | 停止重开，按上表定位；旧凭据不作为“解锁”手段 |
| 已提交消费后故障 | 客户端未收到成功或 Handoff | 该 `identity_jti` 保持已消费，客户端必须重新走登录链（ADR-0009 的 at-most-once 取舍）；不按网关心跳释放已提交记录 |

铁律：切换后旧 Queue Number 永不重新接受；已提交的准入消费永不回滚释放；两者都不能为了“让发布继续”而开口子。

## 6. 切换后核验

1. **一次真实登录**（真实四服务 + etcd，走生产 wire）：
   ```bash
   ./build/dev/bin/realm_mesh_loadgen --phase full \
       --robots 1 --accounts 1 --credential <凭据> \
       --login-verify 127.0.0.1:<port> --queue 127.0.0.1:<port> --gateway 127.0.0.1:<port>
   ```
   `--phase full` 会走完 verify → tickets → 轮询兑换 → 网关 attach → Realm 入场（`tools/loadgen/main.cpp`）。注意开发配置里 `login_verify` 与 `queue` 的 `listen_port = 0`（内核分配），部署与核验时必须显式指定端口或从服务发现读取。
2. **负例抽查**：旧 `admitted` Queue Number 与跨身份拼接的 Grant 必须得到 `1001`；消费存储不可用时得到 `1006` 且网关就绪为 0。
3. **首批放行观察**：
   - 排队服：`tickets_issued_total`、`released_number`、`admit_rate`、`queue_length_est`、`admit_batches_total`（`game/queue/src/queue_service.cpp`）。
   - 网关：`edge_credential_result_total{result="…"}`、`edge_jti_replay_rejected_total`、`edge_credential_ingress_rejected_total{reason="…"}`、`edge_sessions{stage="…"}`、`edge_budget{kind="…"}`、`edge_handoff_granted`（`game/gateway/src/gateway_login_pipeline.cpp`）。
   - 预期：首个批次内 `released_number` 单调推进，无 `store_unavailable` 增长，无重复消费。
4. **记录**：修订、配置、`deployment_id`、kid 列表、命令、各阶段结果与失败注入，作为该 deployment 的切换证据。

## 7. 未决项

- **批准与维护窗口**：本手册不构成变更批准；具体窗口、影响面通告与回退决策人需由部署方指定，本仓库无法代为决定。
- **入口来源配置**：`configs/services/gateway.lua` 的 `ingress_source.mode`（`direct_peer` / `trusted_x_forwarded_for` / `trusted_forwarded`）与 `trusted_proxy_cidrs` 目前是开发值（`direct_peer` + 空列表），必须按实际边缘拓扑逐 deployment 决定；来源判定错误会让限流按错误身份计数。
- **脚本化演练缺口**：仓库目前没有一条脚本能同时启动四服务并注入本手册所需的全部密钥。`./scripts/dev-services.sh` 只管理 `realm` 与 `gateway`（`gateway → realm` 回收），`./scripts/dev-all-in-one.sh` 在单进程承载四服务；脚本驱动的多进程用例 `tests/scripts/dev_services_test.sh` 与 `tests/cpp/tools/loadgen/loadgen_integration_test.cpp` 仍注入退役的 `REALMMESH_QUEUE_KEY_SEED`，未使用四个新环境变量。切换演练需按 §3 手工注入，直到这些脚本对齐。
- **消费前缀的重置策略**：中止过的切换若要重放，消费前缀下可能残留记录。默认规则是不删除（§3 第 8 步）；跨中止重放的清理策略需要部署方显式批准并记录。
- **etcd 操作工具**：`etcdctl` 由 `./scripts/install-etcd.sh` 安装到 `.tools/etcd-v3.6.14/`，但仓库脚本与文档此前未使用它；本手册的 `etcdctl` 命令需在目标环境的 etcd 版本上自行确认。
- **真实数据路径**：网关当前仍使用固定延迟的成功拉取桩（`DelayedAccountFetchPort`，`framework/service_host/src/service_host.cpp`），因此本手册的核验只证明接入与准入契约，不代表真实账号/角色数据行为（见 #92）。
