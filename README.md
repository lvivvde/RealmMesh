# RealmMesh

RealmMesh 是一个 C++20 分布式游戏服务端框架。接入侧的目标场景是**瞬时登录洪峰**：
百万级客户端在分钟级窗口内同时发起登录，系统按准入额度有序放行，不雪崩、不丢位次。
业务侧将以一条轻量 MMO 参考链路验证服务拆分、跨进程路由、持久化与故障隔离；
下文的“必做路线图”是尚待交付的项目承诺，不代表当前实现。

接入链路按「健全 → 排队 → 网关管线 → 直连业务服」分层：

```mermaid
flowchart LR
    C[客户端] -->|HTTPS + 凭据| Edge["边缘 CDN/WAF<br/>验签放行 · DDoS"]
    Edge -->|HTTPS 回源| LV[登录健全服<br/>login_verify]
    LV -->|身份 Token| C
    C -->|HTTPS + 身份 Token| QS[排队调度服<br/>queue]
    QS -->|号牌 · 放行时 Admission Grant| C
    C -->|QUIC 主 / TLS·TCP 降级 竞速| GW[网关<br/>gateway · 登录管线]
    GW -->|EnterRealm 票据 + 业务端点| C
    C -->|竞速 直连| RL[业务服<br/>realm]
    LV & QS & GW & RL <-->|Lease / Watch| Etcd[(etcd v3)]
```

- **登录健全服 `login_verify`**：无状态、可水平扩展，只回答账号是否有效（有效/封禁/
  白名单），签发身份 Token（JWT/EdDSA）并暴露 JWKS。
- **排队调度服 `queue`**：发号、查号，按准入额度分批放行；已确认发号映射、下一号码、
  已放行水位与放行批次由 etcd 持久化，响应丢失或进程重启后可按同一身份尝试找回原号。
- **网关 `gateway`**：只承载登录管线。Edge Session 三阶段
  `pending → fetching → handed-off`；拉取账号数据完成后下发 EnterRealm 票据与业务服
  端点，不中转业务流量。
- **业务服 `realm`**：EnterRealm 票据兑换点与客户端长连接落点；Realm Session 内完成
  角色列表、创建、选择与训练（Lua 规则，序号幂等），同账号新会话顶掉旧会话。场景指令
  路由和聊天入口列在后续必做路线图中。
- **边缘 CDN/WAF**：只定契约（验签放行、DDoS 防护），不在本仓库实现；边缘验签是
  能力分级优化，健全服/排队服/网关的本地验签不可省略。
- 业务消息为 4 字节大端长度 + Protobuf `Envelope`（上限 64KiB）；不支持裸 TCP/UDP、
  KCP、自研传输加密或逐消息协议回退。

设计与术语：[架构规格](docs/specs/2026-09-12-login-chain-surge.md)、
[ADR-0004](docs/adr/0004-identity-token-jwt-eddsa.md)、
[ADR-0005](docs/adr/0005-gateway-login-pipeline-direct-connect.md)、
[ADR-0006](docs/adr/0006-stateless-queue-number.md)、
[ADR-0007](docs/adr/0007-self-built-minimal-http1-stack.md)、
[CONTEXT.md](CONTEXT.md)、[架构文档](docs/architecture.md)、
[协议文档](docs/protocol.md)、[分布式日志架构](docs/logging-architecture.md)、
[observability development stack](deploy/observability/README.md)。

## 实施状态

规格本身仍是评审稿；下表"已实现"指代码、配置与测试均已落地。
网关准入凭据是绑定身份的 **Admission Grant**；排队号牌只用于排位与找回，网关不接受
号牌（[ADR-0009](docs/adr/0009-identity-bound-admission-grant.md)、
[协议文档](docs/protocol.md#凭据链与网关-attach)）。阶段 0 的收束范围与完成标准见下方
必做路线图。

| 范围 | 状态 |
|---|---|
| 传输基座：QUIC + TLS 1.3、客户端竞速与降级分类 | 已实现 |
| 凭据编解码：极小 JSON（#37）、EdDSA JWT 与 JWKS（#38） | 已实现 |
| HTTPS 服务边：有界 HTTP/1.1 解析与 TLS 传输（#39） | 已实现 |
| 集群：etcd v3 注册/发现、实例额度上报（`conn_free`/`fetch_free`） | 已实现 |
| AccountStore 抽象与配置账号表（#40） | 已实现 |
| 登录健全服 `login_verify`（#41） | 已实现 |
| 排队调度服 `queue`（#42） | 已实现 |
| 网关三阶段管线与实例额度聚合（#43） | 已实现 |
| 限流拉取管线（#44） | 已实现 |
| handoff 与 EnterRealm 签发（#45） | 已实现 |
| 业务服 EnterRealm 兑换与直连入场（#46） | 已实现 |
| 指标/告警/仪表盘（#47）、L1 定向压测与 `realm_mesh_loadgen`（#48） | 已实现 |
| 客户端状态机与自适应轮询（#49） | 已实现 |
| 退役旧 Login 链路（#50） | 已实现 |
| 身份 Token 回放守卫收敛（#63）、客户端真实兑换口（#64） | 已实现 |
| 权威玩家数据源：MongoDB 副本集取代 SQLite（#99） | 已实现 |
| 登录链玩家数据加固：验签与 Realm 角色复核移出事件线程、Realm 必须配置数据源、`1007`/`1008` 分流、网关拉取超时与并发（#98；Realm 入场角色复核已由 #93 移除） | 已实现 |
| Realm 最小业务闭环：票据不再带角色、Realm Session 选角/游戏中阶段、角色列表/创建/选择、Lua 训练规则与序号幂等、同账号新会话顶替旧会话、客户端 Realm API（#93） | 已实现 |
| 共享临时开发库：TLS/认证、SSH 隧道与仓库外私有配置 | 已实现，见[连接说明](docs/operations/shared-mongodb.md) |
| Linux ARM64 开发环境：依赖安装、QUIC 与原生构建 | 已验证构建及四进程登录链；首次集成测试 142/143 通过，fd 断言有一次偶发失败，见[验证记录](docs/operations/linux-arm64-development.md) |
| 构建优化 P5：原生 ccache AUTO/ON/OFF 与 CI 可信缓存（#124） | 已接入；配对结果、完整验证与待验收项见[阶段报告](docs/research/build-optimization-results.md) |
| 构建优化 P6：双平台最终复测与有界后续（#129） | 已复测；首批未验收通过，热完整入口超过回退上限，Linux VM 时钟异常已定位并按完整组重跑；测试合集变化与后续范围见[阶段报告](docs/research/build-optimization-results.md#p6双平台最终复测与有界后续决策129) |
| MongoDB 测试夹具初始化（#119） | 独立原生辅助进程初始化真实副本集并等待 Primary；保留真实回读、隔离和回收，性能收束待双平台配对复测 |
| 开发 Supervisor 停止信号与进程回收（#143/#129） | 已修复解析竞态及 wait 再次中断后的提前清理；逆序停止与真实进程回收证据见[诊断记录](docs/research/dev-services-signal-stop.md) |

旧 `Login → Realm 选角 → Gateway 入场` 链路已整体退役：`login` 服务身份、7000 端口与
旧入场消息编号都已删除，线名 `login` 永不复用（见[架构文档](docs/architecture.md)）。
启用新链路不需要任何开关，仓库里就只有这一条链路。

## 必做路线图

按单人、逐步可验收的节奏推进。每阶段完成前保留“规划中”状态；实现状态以代码、
测试和可重复运行的验收记录为准。详细步骤见[接入与参考业务路线图](docs/plans/2026-09-27-login-chain-roadmap.md)，
当前与目标拓扑见[架构文档](docs/architecture.md)。

| 顺序 | 必做交付 | 完成标准 |
|---|---|---|
| 0 | 收束 Admission Grant 迁移 | 当前源码重新构建、测试；凭据不可互换、重复消费和存储故障均有证据；文档与现行协议一致。 |
| 1 | macOS 本机四服务登录闭环 | 可控数据桩下完成取号、放行、Gateway Handoff、Realm 入场、心跳和断开；Queue 重启保号，故障与恢复可重复验收。 |
| 2 | Linux 登录链接入可靠性与 M1–M4 | 验证 QUIC/TLS、跨进程和跨机器的接入链、故障恢复及分级负载；各级结果有环境、配置和指标报告。 |
| 3 | 真实账号与角色数据 | 账号与角色有真实持久来源；完成角色列表、创建、选择和归属校验，重启后可恢复；数据故障有明确反馈。 |
| 4 | 多实例 Scene 参考业务 | 客户端保持 Realm 长连接，Realm 将业务指令路由到独立 Scene 实例；两个玩家移动可互相看到，跨实例转场；实例动态加入、排空，故障隔离后可重新入场并恢复已确认的角色落点。 |
| 5 | 独立 Chat 服务 | 场景内和跨场景频道可在线收发；同频道消息顺序与失败反馈有验收；Chat 故障不阻断场景业务。 |
| 6 | 云端百万登录 M5 | 单独测量百万级瞬时登录的放行、位次、故障恢复和最终 Realm 连接容量；不以此宣称百万玩家移动或聊天容量。 |

**服务与部署边界：** 已接线的 `login_verify`、`queue`、`gateway`、`realm`，以及计划新增的
`scene`、`chat`，都应能以独立服务身份、独立进程部署和扩容；开发时允许同机合并运行。
`gateway` 只承载登录管线，业务消息由客户端直连 `realm` 后转交 Scene 或 Chat。
同一种 `scene` 服务可以有多个实例，各自承载场景分片；地图或房间不各占一个服务身份。
账号、角色与已确认场景落点由相应业务边界持久化，并复用存储接口，不预设通用
`storage` 服务或具体数据库产品。新增 Scene/Chat 业务链的必做验收范围是**本机多进程**；
Linux 跨机器 M1–M4 针对登录链接入，二者的验收结论分别记录。

**候选方向，尚非必做：** 好友、邮件、FPS 大厅/匹配/房间，以及聊天历史和离线补投。
新增独立服务身份应有独立扩缩容、状态归属或故障隔离的真实需求；不为候选功能预建目录或
注册服务身份。

## 传输与安全

QUIC 与 TLS/TCP 使用相同证书、SNI 和 ALPN `realmmesh-edge/1`。两条路径都只接受
TLS 1.3；首版关闭 0-RTT、会话恢复、QUIC Datagram 与 keepalive。QUIC 每条连接只允许
一个客户端发起的长期双向流，允许连接地址迁移。

HTTP 服务边（健全服、排队服）复用同一证书与 reactor，ALPN 为 `http/1.1`，走自研的
有界 HTTP/1.1 编解码与最小服务端（[ADR-0007](docs/adr/0007-self-built-minimal-http1-stack.md)），
不引入第三方服务端库。

客户端建连策略由 `PreferredTransportConnector` 表达：QUIC 在 0ms 开始，TLS/TCP 在
350ms 后参与竞速，单次握手上限 3 秒、整轮上限 5 秒，第一个安全握手成功者胜出。
只有 `Unsupported`、`NetworkUnreachable`、`HandshakeTimeout` 可以触发降级；证书、
ALPN、鉴权或协议错误不会。网络不可达结果按当前网络缓存 5 分钟，网络切换时清除。
竞速在连网关与直连业务服两条连接上彼此独立适用。

当前仓库包含可移植的竞速策略与错误分类参考实现，首个客户端集成目标为 Windows；
生产客户端 SDK 不在本阶段范围内。

## 凭据体系

| 凭据 | 格式 | 签发方 | 消费方 | 有效期 | 单次消费 |
|---|---|---|---|---|---|
| 身份 Token | JWT/EdDSA（`iss=realmmesh/login-verify`） | 健全服 | 边缘验签、排队服、网关 | 30 min | 是（经 Admission Grant 按 `jti` 至多一次） |
| 排队号牌 | JWT/EdDSA（`iss=realmmesh/queue`，`aud=realmmesh-queue`、`purpose=queue-position`，含号值与 `identity_jti`） | 排队服 | 排队服查号、客户端轮询与找回 | ≤ 身份 Token 到期（`queued_number_ttl` 默认 1 h） | 否（位次查询复用） |
| Admission Grant | JWT/EdDSA（`iss=realmmesh/queue`，`aud=realmmesh-gateway`、`purpose=gateway-admission`，绑定 `identity_jti`、来源号值与 `deployment_id`） | 排队服 | 网关校验与集群消费 | 放行批次起算（`admit_grace` 默认 5 min，协议上限 10 min），且 ≤ 身份 Token 到期 | 是（同一 `identity_jti` 在一个 deployment 内至多一次） |
| EnterRealm 票据 | SessionTicket（对称键，用途字节 `EnterRealm=3`；旧用途字节作废不复用） | 网关 | 业务服 Realm 兑换 | 60 s | 是（重放防护） |

Ed25519 私钥只在健全服（`REALMMESH_IDENTITY_KEY_SEED`，hex 注入），公钥经
`GET /.well-known/jwks.json` 发布，内部消费方配置静态载入；轮换走 `kid` 单调递增 +
24h 重叠窗双键并存。封禁/白名单状态不入 Token（签发时检查 + 网关拉取时二次校验兜底）。

## HTTPS API

JSON over HTTP/1.1，keep-alive 必开：

| 端点 | 服务 | 请求 | 成功响应 |
|---|---|---|---|
| `POST /v1/login/verify` | 健全服 | `{account, credential}` | `200 {identity_token, account_id, expires_in}` |
| `POST /v1/queue/tickets` | 排队服 | Bearer 身份 Token | `202 {queue_number_token, number, estimated_wait_seconds}`；同一未过期 `identity_jti` 幂等；权威存储不可用时 `503` + `Retry-After`，不返回未提交号码 |
| `GET /v1/queue/progress` | 排队服 | — | `200 {released_number, admit_rate, server_time}`；可 CDN 缓存，位次/ETA 客户端本地算 |
| `GET /v1/queue/tickets/me` | 排队服 | Bearer 号牌 | `200 {status, position, estimated_wait_seconds}`；放行时 `status="admitted"` 并附 `admit_grant {admission_grant, number, expires_in}` |
| `GET /.well-known/jwks.json` | 健全服 | — | JWKS |
| `/healthz` | 两个服务 | — | 健康检查 |

错误模型为 `{code, message, retry_after_seconds?}`（HTTP 状态与 `code` 并存），段号约定
沿用 `edge.proto`：`1xxx` 凭据段（`1001` 凭据无效、`1002` 封禁、`1003` 白名单外、`1004` 玩家数据源暂不可用，HTTP `503`）、
`2xxx` 排队段（`2001` 号牌无效/过期、`2002` 发号存储暂不可用）。网关边另有一套 `EdgeError.code`：`1001` 凭据无效、
`1004` 满额拒绝 attach、`1005` 准入处理中（可重试）、`1006` 准入存储不可用（可重试）、
`2002` 未认证、`3002` 入场票据无效，以及限流时的 `429`（带 `retry_after_seconds`）。
这两套 code 与 `Envelope.message_id` 是三个独立编号空间，数值相同不代表同一含义。各服务
在独立 metrics 端口暴露 Prometheus 指标（见“运行”）。

## 网关登录管线

`GatewayLoginPipeline` 是 Edge Session 登录阶段、准入额度、账号拉取结算、Handoff、
会话收尾与对应指标的唯一权威。`ServiceFrame` 每帧只解析当前 Realm 端点并调用一次
`advance()`；传输事件与命令分别经 `GatewayRuntimePrimaryTransport` 适配，账号拉取经
非阻塞 `DelayedAccountFetchPort` 适配。旧的分步编排与公开 helper 已删除，不存在双路径。

```mermaid
stateDiagram-v2
    [*] --> Pending: 握手成功
    Pending --> Fetching: 身份 Token + Admission Grant 验讫（按 identity_jti 集群单次消费）
    Pending --> Closed: 凭据无效 / 额度外拒绝
    Fetching --> HandedOff: 拉取完成，签发 EnterRealm + 端点下发
    Fetching --> Closed: 重试耗尽（归还连接与拉取额度）
    HandedOff --> Closed: 短宽限收尾（客户端已直连业务服）
    Closed --> [*]
```

`pending → fetching` 的三重闸是身份 Token → Admission Grant → 集群消费提交，全部
通过才迁移并占用拉取额度；额度探针先于消费提交，满额拒绝不烧凭据，准入存储不可用
只返回可重试结果。拉取按全局预算池限流，指数退避 ≤3 次 × 2s，失败断开并归还连接额度
与拉取预算。协议上网关以 `EdgeAttach`（1301/1302）接收凭据、以 `EnterRealmGranted`
（1303）下发直连凭证与业务服端点。

MsQuic 回调、TLS/epoll 事件被适配为统一 `GatewayEvent`，再进入有界入站队列；帧线程
只消费事件并提交有界出站命令。过载时可靠连接会被关闭，不允许无界增长。

## 服务发现与额度

服务发现默认关闭，此时使用 Lua 中的固定下游地址。开启后，服务在运行时监听器就绪后
通过 etcd v3 Lease 发布端点并 Watch 下游；首次注册成功才进入 ready。`required = true`
会将注册失败显式报错，`false` 会记录告警并保持 not-ready，统一启动器不放行未就绪的
服务。

网关与业务服同时按阈值向 etcd 上报实例额度：key 为
`/realmmesh/budgets/service/<gateway|realm>/<instance_id>/budget`，值
`{conn_free, fetch_free?, updated_at}`（`fetch_free` 只在有拉取管线的 `gateway` 上出现，
`realm` 的 `has_fetch = false`），与实例注册挂同一租约（实例死则额度消失）。排队调度服
Watch 该前缀，按 `min(Σ网关 fetch_free/conn_free, Σ业务服 conn_free, 配置步长)` 定时放批
（≥2s 一批）。

## 构建

三平台支持等级见 [ADR-0002](docs/adr/0002-cross-platform-support-policy.md) 与
[ADR-0012](docs/adr/0012-macos-local-quic.md)：Linux 是生产与行为基准（缺失 MsQuic
即配置失败，CI 的 QUIC 回归在此），macOS 是开发基准（编译、起拓扑、`ctest` 全绿；
装了 Homebrew `libmsquic` 即在本地编入并测试 QUIC，未装则 `TransportFactory` 按平台
能力禁用 QUIC，只走 TLS/TCP），Windows 当前只交付后端开关（`iocp` 可配置但显式未实现，不进 CI）。同一构建只编入一个平台后端，
后端在配置期选定（[ADR-0001](docs/adr/0001-compile-time-platform-backends.md)）。

共同要求：CMake 3.20+、Ninja 1.11+（默认预设 `dev` 的生成器；没有 Ninja 时配置会停下并提示，
可显式改用 Make 回退 `--preset dev-make`）、C++20 编译器与 OpenSSL 3 开发包。第三方依赖（Lua、sol2、
libsodium、nlohmann/json、cpp-httplib、protobuf、spdlog）随源码树一并构建，无需另行
安装。

Ubuntu 24.04（生产基准，含 QUIC）：

```bash
sudo apt install libssl-dev libnuma1 ninja-build
./scripts/install-msquic-dev.sh
./scripts/install-etcd.sh
./scripts/install-mongodb.sh  # 自动化集成测试需要；手动联调使用共享库
./scripts/build.sh
```

macOS（开发基准；装 `libmsquic` 即本地启用 QUIC，不装则只走 TLS/TCP）：

```bash
brew install openssl@3 ninja libmsquic
export PATH="$(brew --prefix openssl@3)/bin:$PATH"   # 测试证书生成要用 openssl(1)
./scripts/build.sh
```

### 第三方来源与配置状态

配置开头由 `cmake/RealmMeshOpenSSL.cmake` 统一选定并校验 OpenSSL，Mongo 驱动与网络层都复用这一份结果；
配置输出的 `RealmMesh: OpenSSL` 行给出版本、安装前缀与 root 来源。

- **OpenSSL**：显式 `OPENSSL_ROOT_DIR`（`-D` 或环境变量）优先，无效即失败、不回退；未指定时
  macOS 默认 Homebrew `openssl@3` 专用前缀。不能用共享前缀 `/opt/homebrew/include`：其中还有
  Homebrew 的 Abseil（`mongodb-community` 会顺带装上），会盖过工程固定版本。配置期会校验：头文件来自专用
  include 目录，头与库属于同一安装，探针程序能链接（架构一致），头与运行时主次版本一致。
- **protoc**：默认下载并校验官方 protoc 35.0（无官方包的宿主从源码构建）。要用本机已有的 protoc，唯一入口是
  `-DREALMMESH_PROTOC_EXECUTABLE=<绝对路径>`，版本必须是 35.0，无效即失败、不改走下载。
  `WITH_PROTOC` 只是内部映射，不接受外部设置。
- **Mongo 驱动版本**：C 驱动 2.5.5 与 C++ 驱动 4.6.0 在 `third_party/mongo` 各自设定，通用键
  `BUILD_VERSION` 不进缓存，避免重新配置后改写版本头、触发大批重编。

Mongo、protobuf 等 FetchContent 依赖按固定 URL + SHA256 获取；`FETCHCONTENT_SOURCE_DIR_<NAME>` 只作显式的
开发/实验覆盖（如复用已下载并核对过的源码），不属于验收构建的默认来源。

旧构建目录里残留的冲突缓存会让配置明确失败，错误信息列出缓存项与迁移命令，按提示定向移除即可，无需删构建目录：

| 残留 | 迁移 |
| --- | --- |
| 旧版配置留下的 `BUILD_VERSION`，或手动 `-DBUILD_VERSION` | `cmake --preset dev -UBUILD_VERSION` |
| `OPENSSL_INCLUDE_DIR` 等发现结果不在选定 root 内（如 `/opt/homebrew/include`） | `cmake --preset dev -UOPENSSL_INCLUDE_DIR`（报错列出的其他键同理） |
| `-DWITH_PROTOC=...` | `cmake --preset dev -UWITH_PROTOC -DREALMMESH_PROTOC_EXECUTABLE=<路径>` |

本机路径（OpenSSL、protoc、MsQuic 前缀等）不写进仓库预设：复制
[`CMakeUserPresets.example.json`](CMakeUserPresets.example.json) 为 `CMakeUserPresets.json`（已被
`.gitignore` 忽略），改成本机路径并删去不需要的键；它以 `dev-local` 继承 `dev`，构建目录为 `build/dev-local`，
用 `cmake --preset dev-local`、`cmake --build --preset dev-local`、`ctest --preset dev-local` 配置、构建、测试。
`dev` 改用 Ninja 后，继承它的 `dev-local` 也随之是 Ninja：`build/dev-local` 里若还留着 Make 生成的缓存，
CMake 会拒绝换生成器，删掉该目录后重新配置即可；想在本机预设上继续用 Make，就让它改为继承 `dev-make`。

仓库预设：`dev`（Ninja，`build/dev-ninja`，日常默认）与 `dev-make`（Unix Makefiles，`build/dev-make`，
Make 回退）。两者都继承隐藏的 `realmmesh-base`，后者把缓存变量 `REALMMESH_PRESET` 设为当前预设名；派生的
用户预设继承 `dev` 时同样得到自己的名字。`dev` 要求 Ninja 1.11+（`ninja -t missingdeps` 的最低版本）：
缺失或过旧时配置在 `project()` 之前停下，提示安装或显式改用 `--preset dev-make`，从不悄悄换生成器。
Ninja 下库与生产程序的链接进深度 1 的原生池，编译仍用满 `--jobs`；GTest 可执行文件不进池（链接边里带着用例
发现，见[实施记录](docs/research/build-tool-cache-decisions.md#实施记录123)）；libsodium 照旧是单独的 `make -j1`。旧的 `build/dev`
（`dev` 曾用 Make 时的目录）不会被删除、搬移或换生成器，确认不再需要后自行删除即可。

原生 C/C++ 编译缓存（#124）：`REALMMESH_CCACHE` 缺省 `AUTO`，PATH 有 ccache 4.x（≥4.8）即启用，缺失时打印关闭；`ON` 缺工具或显式工具路径无效会配置失败。macOS 可 `brew install ccache`，Ubuntu 可 `sudo apt install ccache`，入口不自动安装。`cmake --preset dev -DREALMMESH_CCACHE=OFF` 关闭并移除项目管理的 launcher，之后 `./scripts/build.sh` 保持该缓存选择；`AUTO` 可重新开启。已有自定义 compiler launcher 与开启缓存冲突时明确报错，OFF 保留用户 launcher。

专用缓存缺省 `${sourceDir}/.cache/ccache`（在 build 外，git 忽略），上限 5GiB；构建目录清理不会删除它。用户预设可覆盖 `REALMMESH_CCACHE_DIR`（绝对路径）、`REALMMESH_CCACHE_MAX_SIZE` 和 `REALMMESH_CCACHE_EXECUTABLE`。配置隔离个人/系统 ccache 设置并在每次调用固定 content 编译器校验、空 sloppiness、调试目录哈希、系统头检查、压缩及禁止硬链接；不强设路径映射，也不缓存 libsodium 的外部 Make 编译。`CCACHE_DIR=<专用目录> ccache --show-stats --verbose` 查看命中/未命中/大小/淘汰，未命中正常源码编译，编译错误不重试。

CI 两平台显式 ON，专用目录上限 2GiB，仅恢复 ccache 对象。兼容键包括 OS/架构、编译器内容/版本/目标、SDK/sysroot/标准库宏、工具版本、Debug 配置、固定依赖元数据及补丁；每桶每周最多发布一个不可变快照。PR 只恢复，成功完成全部验证的 main push 或 main 手动维护运行才发布；恢复/保存失败允许继续，统计和传输秒数留在日志。实际收益与尚未覆盖的 CI 运行见[阶段报告](docs/research/build-optimization-results.md)。

配置时 `cmake/RealmMeshBuildDirInfo.cmake` 记下「预设 → 真实构建目录」：`build/.build-dirs/<预设>.txt`
是登记，`<构建目录>/realmmesh-build-dir.txt` 是该目录自带的身份。脚本入口都接受 `--preset NAME`
（默认 `dev`），按这份记录找构建目录，不自行解析预设 JSON；预设未配置、登记与目录身份不符或目录来自另一
棵源码树时直接失败并提示配置命令。`./scripts/build-dir.sh --preset NAME` 输出该预设已配置的构建目录。

`scripts/build.sh [--preset NAME] [--jobs N]` 用同一个预设依次执行 `cmake --preset`、`cmake --build --preset` 和
`ctest --preset -j 1`（配置 + 构建 ALL + 串行全量测试），并把仓库根的 `compile_commands.json` 符号链接指向所选
预设的编译数据库；切换预设时改指向，仓库根若是普通文件则保留并告警。只有 `build.sh` 与 `test-fast.sh` 会刷新这个链接：
单独执行 `cmake --preset dev` 不改它，切到 Ninja 后它仍指向旧目录（如 `build/dev`），跑一次上述脚本即可改指向。

Linux 的 MsQuic 开发安装脚本固定使用 Microsoft 官方 `libmsquic 2.6.1` 包和对应头文件，
下载内容均校验 SHA-256；macOS 用 Homebrew 当前的 `libmsquic` 2.6.x。CMake 输出会打印
找到的 MsQuic 版本，次版本偏离 2.6 时告警。也可自行安装 MsQuic，并通过 `MSQUIC_ROOT`
指向其前缀。
Linux 安装脚本支持 x86_64 与 ARM64（aarch64），按宿主架构选择 MsQuic、etcd、
MongoDB 与 mongosh 的固定版本及校验和；MsQuic 和 MongoDB 使用 Ubuntu 24.04
上游构建。CMake 在 Linux ARM64 上也下载并校验官方 protoc 35.0，避免进入
Protobuf 编译器源码的 bootstrap 构建路径。ARM64 开发环境也必须编入 QUIC，配置输出应包含
`realm_network: QUIC transport enabled`。Linux CI 仍以 Ubuntu 24.04 x86_64 为门槛；
其他发行版版本与 ARM64 的兼容性须由本机实际构建和测试确认。
Ubuntu 26.04 ARM64 的本机步骤与测试边界见[Linux ARM64 开发环境](docs/operations/linux-arm64-development.md)。
服务发现需要本地 etcd：`./scripts/install-etcd.sh` 安装固定版本 3.6.14，
`./scripts/run-etcd-dev.sh` 以前台单节点启动。

玩家数据存放在 MongoDB 副本集（[ADR-0011](docs/adr/0011-mongodb-authoritative-player-data.md)）。
手动开发联调（macOS 与 Linux 都一样）直连远程共享开发库，开发机不需要安装或运行
MongoDB 服务：`./scripts/configure-shared-mongodb.sh vps` 取一次私有连接材料，之后用
`./scripts/with-shared-mongodb.sh <命令>` 启动服务，详见[共享库说明](docs/operations/shared-mongodb.md)。

自动化集成测试不连共享库，而是由夹具在本机拉起临时 `mongod`，所以跑全量测试的机器
需要 MongoDB 二进制：macOS 用 `brew tap mongodb/brew && brew trust mongodb/brew && brew install mongodb-community mongosh`；
Linux（CI 自动执行）用 `./scripts/install-mongodb.sh` 下载固定版本到 `.tools/`。
离线时可用 `./scripts/run-mongodb-dev.sh` 在 `127.0.0.1:27017` 前台启动本机单节点 `rs0`
（数据在 `.runtime/mongodb`，不碰 Homebrew 服务的配置与数据目录；两者都占 27017，
同时只能运行一个），此时不经包装命令，配置回落到默认本机 URI。

## 开发证书与密钥

私钥不得提交仓库。下面示例生成仅用于本机的短期证书；生产环境应由受信 CA 签发，
客户端必须进行 DNS 名称和证书链校验。健全服与排队服还需各自的 Ed25519 签名种子
（hex），网关另需 Admission Grant 验签公钥与消费记录摘要键：

```bash
mkdir -p .local/tls
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout .local/tls/private-key.pem \
  -out .local/tls/certificate.pem \
  -days 7 -subj /CN=localhost \
  -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1'

export REALMMESH_TLS_CERTIFICATE_FILE="$PWD/.local/tls/certificate.pem"
export REALMMESH_TLS_PRIVATE_KEY_FILE="$PWD/.local/tls/private-key.pem"
export REALMMESH_SESSION_TICKET_KEY="$(openssl rand -hex 32)"   # 网关签发 / Realm 兑换
export REALMMESH_IDENTITY_KEY_SEED="$(openssl rand -hex 32)"   # 健全服签发身份 Token
export REALMMESH_QUEUE_NUMBER_KEY_SEED="$(openssl rand -hex 32)"      # 排队服签发排队号牌
export REALMMESH_ADMISSION_GRANT_KEY_SEED="$(openssl rand -hex 32)"   # 排队服签发 Admission Grant
export REALMMESH_ADMISSION_GRANT_PUBLIC_KEY="<与上一行种子配对的 Ed25519 公钥 hex>"  # 网关验签
export REALMMESH_ADMISSION_CONSUMPTION_DIGEST_KEY="$(openssl rand -hex 32)"  # 网关消费记录摘要键
```

网关只持 Admission Grant 公钥，私钥仅存在于排队服；仓库目前没有从种子导出公钥的脚本，
公钥由部署方的密钥流程提供。集群消费记录用摘要键派生稳定的 `grant` 摘要，不保存原始凭据。

统一入口当前会忽略 `SIGHUP`，证书与私钥只在进程启动时加载。轮换开发或生产凭据后，
需要重启对应进程。

## 运行

开发环境可使用管理脚本一键启动或重启全部服务：

```bash
# all-in-one：单进程承载完整拓扑（四个服务）
./scripts/dev-all-in-one.sh          # 默认 restart
./scripts/dev-all-in-one.sh status
./scripts/dev-all-in-one.sh stop

# 同机多进程开发模式：realm 与 gateway 各占一个进程
./scripts/dev-services.sh          # 默认 restart
./scripts/dev-services.sh status
./scripts/dev-services.sh stop

# 用另一个预设的构建产物启动（默认 dev）
./scripts/dev-services.sh --preset dev-make restart
```

脚本默认使用 `.runtime/tls/` 中的开发证书，将 PID 写入 `.runtime/pids/`。all-in-one
控制台输出追加到 `.runtime/logs/all-in-one/console.log`；多进程模式的 supervisor
日志写入 `.runtime/logs/supervisor/console.log`，各服务输出追加到
`.runtime/logs/<service>/console.log`。首次运行前需要完成构建和开发证书生成。
启动时按 `--preset`（默认 `dev`）取已配置构建目录中的 `realm_mesh`（macOS 上连同
`realm_detach`）；`stop` / `status` 只看 PID 文件，不需要构建目录。

多进程脚本由后台 supervisor 按 `Realm → Gateway` 启动，每个服务的
`realmmesh_service_ready` 指标变为 `1` 后才启动下一个服务。默认每项最多等待
10 秒，可用 `REALMMESH_STARTUP_TIMEOUT_SECONDS` 调整。任一服务启动失败或运行中
退出时，supervisor 会按 `Gateway → Realm` 回收整组进程。
Supervisor 管理进程在 `wait` 边界接收停止信号，工作进程通过本地停止文件在安全点收尾；
管理进程回收工作进程后清理该文件，避免 Bash 5.2 的信号 trap 重入命令替换解析器。

这里的“多进程”只表示同一开发机上的独立服务进程，仍使用 Lua 中的环回地址，
不代表已经支持跨机器生产部署、服务多副本或高可用。

也可以手动启动。`realm_mesh` 是唯一入口，默认按 `configs/main.config` 的拓扑
all-in-one 启动全部服务（`dev` 预设的构建目录即 `build/dev-ninja`，其他预设用
`./scripts/build-dir.sh --preset NAME` 查）：

```bash
./build/dev-ninja/bin/realm_mesh --config configs
./build/dev-ninja/bin/realm_mesh --config configs --service login_verify   # 单服务模式
```

用法：`realm_mesh [--config <dir>] [--service <name>] [--instance-id <id>]
[--node-id <id>] [--zone <zone>]`。`--config` 默认当前目录下的 `configs/`；
`--service` 取 `login_verify` / `queue` / `realm` / `gateway`，收窄为单服务
进程；`--instance-id` / `--node-id` / `--zone` 覆盖服务发现身份。`SIGINT` / `SIGTERM`
触发优雅停机。

默认监听：

| 服务 | 端口 | 传输 | metrics |
|---|---|---|---|
| `login_verify` | 0（内核分配） | HTTPS/JSON | 9104 |
| `queue` | 0（内核分配） | HTTPS/JSON | 9105 |
| `gateway` | 8000 | QUIC 优先，TLS/TCP 兜底 | 9103 |
| `realm` | 7100 | TLS/TCP | 9102 |

健全服与排队服在开发配置里以 `listen_port = 0` 由内核分配端口（避免端口冲突），
部署时显式指定。Gateway 会把两个候选端点一并下发，候选项包含
`protocol/address/port/priority`；QUIC 与 TLS/TCP 使用相同主机名和数字端口（分别占用
UDP 与 TCP 端口空间）。

## 配置

启动配置位于 `configs/`，按 `common/` + `services/<name>.lua` 分层装载，拓扑由
`main.config` 描述。安全传输示例：

```lua
{
    name = "client_quic",
    protocol = "quic", -- 或 "tls_tcp"
    enabled = true,
    listen_address = "0.0.0.0",
    listen_port = 8000,
    max_sessions = 10000,
    max_payload_size = 65536,
    max_pending_output_bytes = 4194304,
    handshake_timeout_ms = 3000,
    idle_timeout_ms = 30000,
    alpn = "realmmesh-edge/1",
    certificate_chain_file_environment = "REALMMESH_TLS_CERTIFICATE_FILE",
    private_key_file_environment = "REALMMESH_TLS_PRIVATE_KEY_FILE",
}
```

健全服的账号有效性数据源是 `AccountStore`；生产配置由共享 MongoDB 玩家数据源实现（`configs/common/player_data.lua`），
`configs/common/accounts.lua` 只在空库首次启动时导入。Gateway 通过有界异步读取复核
账号与所选角色，Realm 入场时再次核对角色归属；已提交数据在进程重启后保留。
排队服的放行步长（`release_step`）与批次间隔（`release_interval_seconds`）、网关的
拉取并发预算（`pipeline_fetch_capacity`）、拉取工作线程（`fetch_workers`）、单次拉取超时
（`fetch_timeout_ms`）与重试参数、健全服的验签工作者（`verify_workers`）与在途上限
（`verify_capacity`）都在各自服务配置中。

## 测试

归置、注册与标签约定见 [tests/README.md](tests/README.md)。分类标签只有 `unit` 与
`integration` 两类：进程内单测与 Lua 模块测试归 `unit`（快速子集），占用固定端口
或拉起 `realm_mesh` 的用例必须显式标 `integration`；Lua 套件另带的 `lua` 标签只作
筛选（`ctest -L '^lua$'`）。

```bash
./scripts/test-fast.sh        # 快速子集：配置 + 只构建 Unit 聚合目标 + 4 路跑 unit 用例
./scripts/test-fast.sh --target lua_runtime_test --test-regex Reload   # 聚焦一个 Unit 目标与部分用例
./scripts/test-watch.sh       # 保存即重跑快速入口（单跑一轮加 --once）
./scripts/build.sh            # 配置 + 构建 ALL + 全量测试（串行）
./scripts/build.sh --preset dev-local --jobs 4   # 换预设、指定编译并行
ctest --preset dev -j 1       # 只跑全量测试
ctest --preset dev -L '^lua$' # 只筛 Lua 用例（标签筛选是正则，要加锚点）
```

快速入口不是完整验证；退出码、编译并行预算（缺省开发 Mac 8 路、Lima 2 路）与筛选规则见
[tests/README.md](tests/README.md#运行方式)。

单元测试基座是 Google Test 1.17 与 LuaUnit v3.5（均经 FetchContent 固定版本），
分别由 `realm_add_gtest` / `realm_add_lua_test` 注册；`configs/*.lua` 的真实加载路径
在 C++ 侧由 `configs_load_smoke_test` 覆盖，不在 Lua 里测。

集成测试覆盖真实 TLS 1.3/ALPN 往返、真实 MsQuic 往返（Linux，以及装了 `libmsquic` 的 macOS）、无 ALPN 不创建
业务连接、QUIC 竞速与安全降级分类、IPv6 双栈、端点序列化、Edge Session 三阶段管线
与凭据校验链（身份 Token 校验、Admission Grant 身份绑定与集群单次消费、EnterRealm
票据单次兑换），
以及完整的 Gateway 准入→直连票据→Realm 入场链路；同时覆盖 all-in-one 和同机多进程
启动、就绪门禁、失败整组回收与反序停机，以及已退役消息编号在 Realm 与 Gateway 两侧
被拒。prometheus 规则与 Grafana 仪表盘的指标引用由 `observability_artifacts_test`
守住，改动指标名而不更新规则会失败。

压测工具 `realm_mesh_loadgen`（#48）以真实服务回环驱动机器人，按阶段
（`--phase verify|tickets|poll|gateway|gateway_soak|full`，旧 `all` 是
`gateway_soak` 的 CLI 别名）与内置档（`--profile soak|m2`）跑定向压测。
所有目标与应用客户端共用 `framework/client` 的 Login Chain；工具层只负责配置转换、
既有五相位指标和报告汇总，不保留独立登录状态机，也不进入服务拓扑。测试证书和私钥
只生成在 `build/` 中。push / PR 时 GitHub
Actions 在 macOS 与 Linux 双平台跑全量 `ctest --preset dev`
（`.github/workflows/ci.yml`；预设名集中在工作流的 `REALMMESH_CI_PRESET`，验收报告按
`scripts/build-dir.sh` 解析出的构建目录上传），CI 的 QUIC 路径仅在 Linux 覆盖。

## License

许可证尚未确定；正式添加许可证前默认保留所有权利。
