# 代码归属：新功能放在哪里

本页回答"我要加 X，代码放哪、依赖谁"。分层与依赖方向的决策和理由见 [ADR-0014](../adr/0014-four-layer-dependency-direction.md)，由 `layering_guard_test` 守住。目录只为已实现的代码建立（[ADR-0003](../adr/0003-lean-tree-no-speculative-placeholders.md)）。下文提到的规划路径只是约定，代码真正落地时才创建。

## 四层

| 层 | 目录 | include 前缀 | 命名空间 | 可依赖 |
|---|---|---|---|---|
| 装配层 | `apps/mesh_host`、`tools/*` | `realmmesh/loadgen/…` 等 | 按工具 | 全部 |
| 服务层 | `game/<service>` | `realmmesh/game/<service>/` | `realm::game::<service>` | 共享领域层（除 `game/client`）、框架层 |
| 共享领域层 | `game/common`、`proto`、`game/client` | `realmmesh/game/common/`；`realmmesh/<域>/v1/*.pb.h` | `realm::game::common` | 框架层；`game/client` 另可依赖 `game/common`、`proto` |
| 框架层 | `framework/<模块>` | `realmmesh/<模块>/` | `realm::<模块>` | 只依赖框架层 |

服务之间互不依赖。两个服务都需要的东西，要么属于共享领域层，要么属于框架层。

## 判定顺序

依次回答下面的问题，第一个答"是"的就是归属：

1. **换一个游戏，这段代码还原样需要吗？** 需要则放 `framework/<模块>`，例如传输、长度帧、有界队列、etcd 注册、日志指标、Lua 运行时、宿主生命周期。只要用到本游戏的消息号、凭据格式或领域词，就不属于这里。
2. **它是线格式吗？** 是则放 `proto/realmmesh/<域>/v1/*.proto`，再在 [protocol.md](../protocol.md) 登记消息号与错误码。
3. **两个以上服务、或者服务与客户端，共用这段本游戏代码吗？** 共用则放 `game/common`，例如 Envelope 编解码、身份 Token、号牌、票据、玩家数据存储。
4. **它只属于一个服务吗？** 是则放 `game/<service>`。
5. **它是客户端侧的登录链或业务调用吗？** 是则放 `game/client`。
6. **它是进程入口或装配吗？** 是则放 `apps/mesh_host`。开发、压测、测量工具放 `tools/<工具>`，运维脚本放 `scripts/`。

## 我要加 X → 放在 Y

| 要加的东西 | 代码位置 | 同步改动 |
|---|---|---|
| 某服务的新业务消息 | `proto/realmmesh/<域>/v1/*.proto` 定义消息；`game/common/<域>_protocol.{hpp,cpp}` 编解码；`game/<service>` 处理 | `docs/protocol.md` 登记消息号与错误码；客户端调用加到 `game/client` |
| 服务的新配置项 | `game/<service>/include/…/<service>_config.hpp` 只声明值；`src/<service>_config_lua.cpp` 解析 | `configs/services/<service>.lua` 加默认值；解析测试 |
| 多个服务共用的配置节 | `game/common` 的 `*_config.hpp` 与 `*_config_lua.*`（参考 `player_data_config`） | `configs/common/*.lua` |
| Lua 业务规则 | 规则脚本放 `configs/services/<service>/*.lua`；C++ 调用侧放 `game/<service>`（参考 `training_rule`），头文件只前置声明 `LuaRuntime` | `tests/lua/<service>/` |
| 新服务（如规划中的 Scene、Chat） | `game/<service>/`，并按下方的新服务清单接入 | 同上，外加 README 实施状态 |
| 新的通用框架能力 | 已有模块能容纳就加进去，否则新建 `framework/<模块>/`，`CMakeLists.txt` 定义 `RealmMesh::<Module>` | `docs/architecture.md` 分层一节 |
| 玩家数据的新字段或集合 | `game/common` 的 `player_data_store` | `CONTEXT.md` 领域词；MongoDB 集成测试 |
| 客户端新调用 | `game/client`，编解码复用 `game/common` | 参考实现与 loadgen 适配 |
| 压测场景 | `tools/loadgen` | `tests/cpp/tools/loadgen/` |
| 测试 | `tests/cpp/<与源码同路径>/`，用 `realm_add_gtest` 注册 | 标签规则见 [tests/README](../../tests/README.md) |

## 新服务清单

`ServiceModule` 就位之前（ADR-0014 第 4 步），新服务不能接入宿主。就位之后按以下步骤接入：

- [ ] `game/<service>/` 下建 `include/realmmesh/game/<service>/`、`src/`、`CMakeLists.txt`，CMake 别名为 `RealmMesh::<Service>Service`，由根 `CMakeLists.txt` 引入。
- [ ] 实现该服务的 `ServiceModule`：自己解析配置节、持有自己的对象，只依赖 `game/common`、`proto` 与 `framework/*`。
- [ ] 在 `apps/mesh_host` 登记服务名 → 模块工厂，服务名就是 etcd 线名。已退役的线名（如 `login`）不得复用。
- [ ] 默认配置写进 `configs/services/<service>.lua`；若服务需要进入启动拓扑，同步 `configs/main.config` 的 `services` 表。
- [ ] 客户端可见的消息进 `proto/realmmesh/<service>/v1/` 与 `docs/protocol.md`。
- [ ] 测试放在 `tests/cpp/game/<service>/`。占端口或会拉起 `realm_mesh` 的测试加 `LABELS integration`。
- [ ] 同一个 PR 里更新 `docs/architecture.md`、`CONTEXT.md` 与 README 实施状态。
- [ ] `layering_guard_test` 通过，且没有往白名单里新增条目。

## 过渡期的已知倒挂

下列倒挂按 ADR-0014 的顺序逐个修复，守卫白名单与之一一对应，修完一条就删一条。迁移期间不要在这些依赖上继续叠加新代码：

| 倒挂 | 修复步骤 |
|---|---|
| `game/realm` → `game/gateway` | 2、3：拆 ingress 头文件，建 `framework/edge` |
| `framework/service_host` → `game/*` | 4、5：`ServiceModule` 与模块注册 |
| `framework/cluster` 的 `ServiceType` 枚举 | 4：改为字符串身份（守卫查不到，靠评审） |
