# 四层依赖方向：framework 只放与游戏无关的代码

---
status: accepted
---

到登录链完成时，仓库按目录分成了 `framework/`、`game/`、`apps/`、`tools/`，但依赖方向没有成文。结果出现了四处倒挂：

- `framework/service_host` 依赖全部四个游戏服务；
- `game/realm` 依赖 `game/gateway` 的连接运行时；
- `framework/client` 依赖 `game/common` 的协议与凭据格式；
- `framework/cluster` 的 `ServiceType` 枚举写死了四个游戏服务身份。

新功能该放哪里只能凭感觉判断，倒挂也会随新服务（Scene、Chat）继续扩散。因此决定固定四层与单向依赖，用 framework 的严格判据决定代码归属，并由单元测试守住依赖方向。

## 决策

依赖只能自上而下：

| 层 | 目录 | 可依赖 |
|---|---|---|
| 装配层 | `apps/`、`tools/` | 下面所有层；唯一知道全部服务的地方 |
| 服务层 | `game/<service>` | 共享领域层、框架层；**服务之间互不依赖** |
| 共享领域层 | `game/common`、`proto`、`game/client` | 框架层；`game/client` 另可依赖 `game/common` 与 `proto` |
| 框架层 | `framework/*` | 只依赖框架层 |

- **framework 严格判据**：换一个游戏，这段代码仍原样需要，才放进 `framework/`。只被本游戏协议、凭据、领域词汇使用的代码留在 `game/`。反方向不算倒挂：通用代码（如 `base64url`、`json`）暂住 `game/common` 是允许的，等 framework 真需要它时再下沉。
- **`game/client` 是客户端库，不是服务**：服务层不得依赖它，只有装配层与测试可以。它是协议的参考实现，也是 loadgen 与开发客户端的基座。引擎做的正式客户端不链接它，协议的权威来源仍是 `proto` 与 [protocol.md](../protocol.md)。
- **`proto` 生成头与 framework 模块共用 `realmmesh/` 前缀**：判定归属时，以 `.pb.h` 结尾的 include 一律归 `proto`。

## 四处倒挂的修法

迁移按下列顺序进行，每步一个 PR，每步修完就从守卫白名单删掉对应条目：

1. **`framework/client` 整体搬到 `game/client`**：命名空间改为 `realm::game::client`，测试目录一并迁移。公开头文件里本来就全是本游戏的登录链与 Realm 业务类型，没有可留在 framework 的通用部分，所以不拆。
2. **拆 `gateway_ingress.hpp`**：通用的来源地址归一化（直连、可信代理 CIDR、`X-Forwarded-For`/`Forwarded`）与解码 `EdgeAttach` 的凭据入口分开。连接运行时只依赖前者。
3. **新建 `framework/edge`**，承载与游戏无关的边缘连接运行时：
   - 迁入连接表、事件与命令的有界队列、QUIC 与 TLS/TCP 接入、pending→established 握手闸门、来源地址归一化，以及运行时参数；
   - 去掉 `Gateway` 前缀，例如 `GatewayRuntime`→`EdgeRuntime`；
   - 引入术语 **Edge Connection（边缘连接）**，对应 `EdgeConnectionId`。Edge Session 保持网关侧带登录阶段的原义，Realm Session 与 Edge Session 都绑定在某条 Edge Connection 上。术语随该 PR 写入 `CONTEXT.md`；
   - 凭据入口、`send_handoff` 等 Primary Transport 适配、登录管线、准入、账号拉取与消费存储留在 `game/gateway`；
   - Realm 改为依赖 `framework/edge`。
4. **`framework/service_host` 改为通用宿主加 `ServiceModule` 接口**，先迁 Queue 与 LoginVerify：
   - 宿主保留生命周期、日志与指标、etcd 注册与发布、分层配置机制和帧循环；
   - 每个 `game/<service>` 自带一个实现 `start / tick / ready / stop` 的模块，自己解析配置节、持有自己的对象；
   - `apps/mesh_host` 登记"服务名 → 模块工厂"；
   - 现名 `GatewayConfig` 里的宿主通用部分（监听、TLS、发现、日志）进入 framework；
   - `ServiceType` 改为字符串身份，etcd 线上本就按字符串编码，不影响既有数据解码。
5. **迁 Gateway 与 Realm 模块**：`ServiceFrame` 里的 EnterRealm 兑换、心跳等 Realm 逻辑进入 Realm 模块，`framework/service_host` 不再依赖任何 `game/*`。

`ServiceModule` 是新增虚接口，但它一开始就有四个真实实现，符合[接口边界约定](../research/interface-boundaries-proposal.md)里"已有真实多实现接口继续使用"的条件，不是凭空抽象。

## 守卫

`tests/cpp/layering/layering_guard_test.cpp` 是 `unit` 标签的 GTest，在进程内扫描源码树，核对两类依赖：

- `framework/`、`game/`、`apps/`、`tools/`、`proto/` 下源码的 `#include "realmmesh/…"`；
- 各模块 `CMakeLists.txt` 引用的本仓目标。

规则如下：

- 出现反向依赖，测试失败；
- 迁移期间守卫带一份已知倒挂白名单：某条修掉、不再出现时测试同样失败，提示删除该条，白名单只减不增。第 5 步删完最后三条后白名单清空，这套机制随之删除；
- `ServiceType` 枚举属于"framework 知道游戏服务身份"，不是 include 或链接依赖，守卫查不到。它由第 4 步消除，在此记录。

## Considered Options

- **宽松版 framework（可复用即可）**：被否。"可复用"没有边界，登录链、Realm 业务都能说成可复用。严格判据只问"换个游戏还要不要"，答案稳定。
- **`service_host` 保留，改由 `apps/` 用条件编译选服务**：被否。框架仍要知道每个服务的类型，只是把倒挂藏进宏里。
- **边缘运行时放进 `game/common`**：被否。它本身与游戏无关，按严格判据属于 framework。而且 common 会把 MsQuic 与网络依赖带给 client、login_verify、queue 等所有链接方。
- **维持 Realm 依赖 Gateway**：被否。Scene、Chat 也是直连业务服，到时都要依赖 gateway，服务互不依赖就无从谈起。
- **拆 `framework/client`，把重试分类、分档轮询留在 framework**：被否。这些部分没有第二个使用者，拆出来是凭空抽象，违反接口边界约定。
- **只写文档，不加守卫**：被否。四处倒挂都是在没有成文规则时自然长出来的。没有自动检查，新的倒挂只能靠评审发现。

## Consequences

- 新功能的归属按 [code-layout](../agents/code-layout.md) 查，不再凭感觉判断。
- 与 [ADR-0003](0003-lean-tree-no-speculative-placeholders.md) 的冲突：它把 `realm::cluster::ServiceType` 枚举定为服务身份的权威列表。第 4 步之后，权威列表改为 `apps/mesh_host` 的模块注册表。"只为已实现代码建立目录与身份"的原则不变。
- [ADR-0008](0008-client-login-chain-shared-wire-and-redeemer-seam.md) 中 `framework/client` 的位置已由第 1 步改为 `game/client`：目标改名 `realm_game_client`（别名 `RealmMesh::GameClient`），include 前缀改为 `realmmesh/game/client/`，命名空间改为 `realm::game::client`，守卫白名单删去 `framework/client` → `game/common`。线客户端留在 `framework/network/client`，客户端库只依赖 Network 与 GameCommon，这两条仍然成立。
- 第 2 步已把 `gateway_ingress.hpp` 拆成 `gateway_source_address.hpp`（来源地址归一化，只依赖 framework 与标准库）与 `gateway_credential_ingress.hpp`（凭据入口）。为使连接运行时头不经 `gateway_config.hpp` 传递拿到凭据入口，`GatewayRuntimeOptions` 同步移入 `gateway_runtime_options.hpp`，即第 3 步要迁入 `framework/edge` 的运行时参数。本步不删白名单条目：Realm 仍包含 `gateway_runtime.hpp`，`game/realm` → `game/gateway` 由第 3 步消除。
- 第 3 步已新建 `framework/edge`（目标 `realm_edge`，别名 `RealmMesh::Edge`，命名空间 `realm::edge`，include 前缀 `realmmesh/edge/`），迁入 `EdgeRuntime`、`EdgeConnectionTable`、`EdgeEvent`、`EdgeRuntimeOptions` 与 `EdgeSourceNormalizer`。`EdgeRuntime` 改为按传输配置、运行时参数与来源地址配置构造，不再认识 `GatewayConfig`。Primary Transport 适配改名 `EdgeRuntimePrimaryTransport`，与凭据入口、登录管线一起留在 `game/gateway`。Realm 改为链接 `realm_edge`，守卫白名单删去 `game/realm` → `game/gateway`。第 2 步留下的编译期断言 `gateway_runtime_headers_test.cpp`（运行时头拿不到凭据入口）随之删除：运行时已在 framework，凭据入口在 `game/gateway`，这条边界改由守卫保证。
- 第 4 步已把 `framework/service_host` 改为通用宿主：`LayeredConfigLoader::load` 只解析宿主级节段（`HostConfig`）与接入面节段（`edge::EdgeConfig`），返回持有合并根表的 `LayeredConfig`；`ServiceHost` 按服务名从入口传入的 `ServiceModuleRegistry` 取模块工厂，未注册的名字构造期抛错。`ServiceType` 改为字符串，`is_valid_service_type` 只校验 `[a-z0-9_]`，etcd 解码对非法类型按损坏记录丢弃。LoginVerify 与 Queue 的模块与配置装载入口迁入各自服务目录，`apps/mesh_host` 的 `realm_mesh_modules` 负责注册，`GatewayConfigLoader` 删除；守卫白名单删去 `framework/service_host` → `game/login_verify`、`game/queue`。Gateway 与 Realm 的模块（`frame_service_modules.hpp`）仍共用 `ServiceFrame`，暂留 `framework/service_host`，白名单另三条由第 5 步删除。
- 第 5 步已把 `ServiceFrame` 按服务拆开：Gateway 一半成为 `game/gateway` 的 `GatewayFrame`（解析 Realm 端点、驱动登录管线、上报额度），Realm 一半成为 `game/realm` 的 `RealmFrame`（EnterRealm 兑换、心跳、转交 `RealmSessions`、上报 `conn_free`）。两个服务各自的 `EdgeServiceModule` 与配置装载入口（`load_gateway_config`、`load_realm_config`）迁入本服务目录，`frame_service_modules.hpp` 删除，`framework/service_host` 只链接 `realm_cluster`、`realm_edge` 与 `realm_observability`。Realm 不再借 `GatewayConfig` 读帧参数：`max_events_per_frame` 与 `player_data` 并入 `RealmConfig`，用不到的 `downstream_*` 与 `tick_rate` 从 `configs/services/realm.lua` 删除，`realm` 的注册项也不再声明依赖 `gateway`（Realm 从不解析 Gateway 端点）。守卫白名单删去最后三条并整体移除，[architecture.md](../architecture.md) 同步更新。
- 新服务不能再往 `framework/service_host` 里接线。Scene、Chat 等新服务实现自己的 `ServiceModule`，在 `apps/mesh_host` 注册后接入宿主（第 4 步已提供）。
