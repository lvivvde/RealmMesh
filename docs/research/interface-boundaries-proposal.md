# 公共接口头与内部实现：首批拆分决策

关联[确定公共接口头、内部实现与模块依赖的拆分原则](https://github.com/lvivvde/RealmMesh/issues/113)。用户已通过 Q1–Q4 确认通用能力、调用迁移、先隔离间接传播及完整首批范围。本文件记录决策；配置类型一项已由 [#126](https://github.com/lvivvde/RealmMesh/issues/126)（P3a）落地，TrainingRule 与拓扑装载已由 [#127](https://github.com/lvivvde/RealmMesh/issues/127)（P3b）落地，GatewayRuntime 与 GatewayPrimaryTransport 两项已由 [#128](https://github.com/lvivvde/RealmMesh/issues/128)（P3c）落地，见文末[实施记录（#126）](#实施记录126)、[实施记录（#127）](#实施记录127)与[实施记录（#128）](#实施记录128)。

已确认：保留通用 C++ ↔ Lua 绑定与任意函数调用能力，使用时显式选择绑定头；同步迁移仓库内 include 和调用位置，不保留旧入口兼容层，保持运行行为、Lua 扩展能力和线上协议。

## 原则与文件位置

公共接口是消费者必须知道的类型与契约，不等于为每个类创建虚基类。只有不同职责、不同消费者或可以切断实际依赖传播时，才新增头或隐藏实现。保留现有 include/realmmesh/...、src/ 与 .hpp 约定；私有帮助类留在 cpp 或附近的私有头。不按扩展名集中搬目录，不逐结构体拆文件，不建立空模块目录。

优先普通声明、类外实现与前置声明；按值成员、模板实例化、optional 等需要完整定义时保留必要 include。已有真实多实现接口继续使用；具体 RAII 类和单一实现模块不额外抽虚接口。

## 首批候选与保留项

| 对象 | 真实消费者/问题 | 建议 |
| --- | --- | --- |
| 配置类型 | 配置值消费者不需要 Lua；公开 parse(sol::table) 传播 sol2 | 配置声明保留在各模块，Lua 解析移到明确的解析头/实现入口 |
| TrainingRule | 公共 train/level 已是业务接口；unique_ptr<LuaRuntime> 的析构已在 cpp | 头前置声明 LuaRuntime，完整 Lua include 留在 cpp |
| 拓扑装载 | main.cpp 把 Lua table 转成 ServiceSpec 列表 | 下沉为装载入口，应用入口只拿列表与错误 |
| Lua 通用入口 | 原始 table、泛型绑定和调用是真实已测试能力 | 明确选择才引入，首批保留底层，复测后再决定深入封装 |
| GatewayRuntime | 方法已在 cpp；私有队列/线程/锁布局仍在公开头 | 先拆轻量事件、启动配置与消费者包含；完整 PIMPL 需另测传播 |
| GatewayPrimaryTransport | 已有生产/内存实现；只需事件与 runtime 指针 | 事件单独轻量头，runtime 前置声明，完整头留在实现 |
| MongoPlayerDataStore | 已 PIMPL，驱动实现为 PRIVATE；视图是真实契约 | 保留已有实现隔离；不凭 Lua 基线推断它需进一步拆头 |

配置 DTO 与同契约的值类型可继续同头。Lua parser 若跨目标复用，就提供明确的 Lua 解析入口和编译使用需求；不能移动到私有目录后让别的 target 偷用，也不制造新递归配置值树。

## 两种 Lua 深度

方案 A（已选择为首批）：保留现有 LuaRuntime 通用能力，仅真实解析 cpp、规则 cpp、首次导入实现和绑定测试显式引入它。配置 DTO、TrainingRule、业务及应用头不再间接带 sol2。按需绑定头可以沿用现有重头作为入口，不新增纯转发头来凑分层。

方案 B：另建不含 sol2 的 LuaRuntime 生命周期核心（PIMPL）与可选 LuaBindings。LuaBindings 借用 runtime，提供 module、set_function、call；状态访问/查表桥接的非模板操作放 cpp，泛型模板留绑定头。不拥有第二个 VM，不给同一类靠额外 include 动态补成员，不手写 sol 内部别名的前置声明。该方案增加接口和迁移范围，本轮不选为首批；后续测量证据充分时再讨论。

接口草图表达消费者角色，名称为实施草图，未添加新代码：

```cpp
// 普通配置头：保持现有 GatewayConfig / QueueConfig / RealmConfig 等领域类型。
// Lua 解析头：只有解析实现、合并装载器与解析测试明确选择。
QueueConfig parse_queue_config(const sol::table& table);
// TrainingRule：既有业务调用保持，Lua 实现不进入公共头。
std::optional<std::uint64_t> train(std::uint64_t exp);
std::uint32_t level(std::uint64_t exp);
// 拓扑装载实现负责 Lua；应用入口只消费既有 ServiceSpec。
std::vector<ServiceSpec> load_topology(const std::filesystem::path& path);
```

## 必须保持的契约

- LuaRuntime 创建线程归属、跨线程 logic_error、load 的 bool/error、缺模块/函数或执行错误的 runtime_error；成功才替换模块，热更失败保留旧模块与失败时刻策略。现有沙箱禁止 os/io/package、dofile/loadfile。
- Lua 表/函数句柄不能超过其状态生命周期。LayeredConfigLoader 的 MergedLayers 必须先创建 runtime、后创建 root，按相反顺序销毁；合并与解析在此范围内完成，再返回普通配置值。
- TrainingRule 保持严格整数/nil 与取值范围验证，启动 train(0)/level(0) 检查；切换帧线程使用已捕获源码重建，不重新读取文件、不引入热更或并发共享。
- Gateway/Realm 继续复用 GatewayRuntime。stop/request_stop/join 后才能释放 IO 访问对象；ServiceHost 继续先回收 Realm 数据工作线程，再释放被其引用的 outbox、规则和存储，后释放 runtime/logger。
- AccountFetchPort 对读取视图的所有权、有界并发和错误行为不变。Mongo 已有 majority/事务/单一权威来源语义、已有窄视图和测试替身保持，不新建存储服务或改线上协议。

## CMake 与验收

头中真实 Lua 使用需求与 CMake 使用需求同步调整：普通配置/业务目标不再 PUBLIC 传播 sol2；显式 Lua 解析与绑定消费者明确获得所需 include/模板需求。必要时用可选 INTERFACE target 表达使用需求，不另构建 VM 或复制实现。PRIVATE 不等于静态依赖不用最终链接，不能因此承诺消除 53 次链接。

基线 Lua 公共头改动是双平台各 33 次实际编译、53 次链接；实现改动为 1 次编译、53 次链接。Gateway/Mongo 头暂无对应计时，禁止推广 Lua 数字或承诺提速百分比。

实施后复测并用真实 launcher 事件和源码名单验收：

1. 普通配置/业务头独立编译，不靠传递 include 获得 sol2、Mongo 驱动或平台后端声明。
2. Lua 实现/绑定变化不再重编译仅消费配置结果的业务、应用与间接消费者；方案 A 的真正 Lua 直接消费者仍可能重编译，方案 B 进一步隔离私有布局。
3. 稳定接口变化仍会重编译其真实消费者；重编译范围不能只看 compile_commands。
4. 保存既有绑定、配置装载、热重载、线程、TrainingRule 测试；保留全套 CTest、双平台与 QUIC 约束。历史基线 Linux 两项失败须单独报告；若已修复，记录修复后的基线，不将旧非全绿样本当作验收通过。
5. 53 次链接另由构建目标/验证入口决策处理；本票不假定分头即可改善这项等待。量化时间标准由后续验收票确定。

## 源码证据与限制

当前调查 HEAD 为 3c80dfb70011f9b98be0b947d39604d60dbb16cb。图为 Verify 层级、generation 2026-10-02T09:31:00Z；相关查询分页已完成、相关路径覆盖已核验并直接读完记录的缺口。关键生命周期以源码验证，未把启发式调用边当作完整证明。

- LuaRuntime：framework/scripting/include/realmmesh/scripting/lua_runtime.hpp:3、:28；TrainingRule：game/realm/include/realmmesh/game/realm/training_rule.hpp:21。
- 配置传播：game/realm/include/realmmesh/game/realm/realm_config.hpp:3、game/queue/include/realmmesh/game/queue/queue_config.hpp:4、game/login_verify/include/realmmesh/game/login_verify/login_verify_config.hpp:5、game/common/include/realmmesh/game/common/player_data_config.hpp:4。
- 生命周期与拓扑：framework/service_host/src/layered_config_loader.cpp:88、framework/service_host/include/realmmesh/service_host/service_host.hpp:105、apps/mesh_host/main.cpp:58。
- Gateway：game/gateway/include/realmmesh/game/gateway/gateway_runtime.hpp:51、:77、:169；gateway_primary_transport.hpp:3；game/gateway/src/gateway_runtime.cpp:60。
- 存储：game/common/include/realmmesh/game/common/player_data_store.hpp:153、:205；game/gateway/include/realmmesh/game/gateway/account_fetch_port.hpp:95。
- 构建：framework/scripting/CMakeLists.txt:13，game/common/CMakeLists.txt:29，game/gateway/CMakeLists.txt:22，game/realm/CMakeLists.txt:18。GameCommon 的“Lua 不进公共头”注释与 player_data_config.hpp 现实不一致，已在 #126 中随代码一并纠正。

[原始双平台基线](build-baseline-2026-10-02.md)使用较早冻结快照；本次未跑新的耗时实验、未实施优化。本票确定目的与首批范围，全面 Gateway PIMPL、存储视图另拆头和其他模块只有在新证据支持时继续判断。

## 实施记录（#126）

P3a 按方案 A 落地配置类型一项；上文约定未改动的部分不再重复。耗时与编译名单见[构建优化结果的 P3a 节](build-optimization-results.md#p3a分离配置-dto-与-lua-解析入口126)。

- **两类头。** 普通配置头（`player_data_config.hpp`、`gateway_config_loader.hpp`、`login_verify_config.hpp`、`queue_config.hpp`、`realm_config.hpp`）只声明配置值与不碰 Lua 的辅助函数。各模块新增 Lua 解析入口 `*_config_lua.hpp`，声明 `parse_<模块>_config(const sol::table&)` 自由函数，只包含 `<sol/forward.hpp>`；`*_config_lua.cpp` 实现解析并包含完整 sol2。原 `GatewayConfigLoader::parse`、`QueueConfigLoader` 等静态类入口删除，不留兼容层。
- **选择解析入口的只有三处**：合并装载器 `layered_config_loader.cpp`、`GatewayConfigLoader::load`（单文件装载，运行时与根表都在函数内创建和销毁，只返回配置值），以及直接驱动解析的测试。`player_data_config.cpp` 拆为不碰 Lua 的路径解析部分与 `player_data_config_lua.cpp`。
- **CMake 使用需求。** gateway、login_verify、queue 与 service_host 对 `realm_scripting` 改为 PRIVATE；直接驱动 Lua 的测试显式链接它。realm 仍 PUBLIC，因为公共头 `training_rule.hpp` 还包含 `lua_runtime.hpp`，由 #127 收口。解析入口的“包含方须自行链接 RealmMesh::Scripting”目前写在头注释与 CMake 注释里，没有另建 INTERFACE target：两个跨目标使用方都已 PRIVATE 链接，按上文“必要时”不新增目标。
- **守卫。** `config_headers_test` 包含配置值消费者会用到的普通头（含 `layered_config_loader.hpp`、`mesh_host.hpp`、各服务头），任一经传递 include 引入 sol2（`SOL_HPP` / `SOL_FORWARD_HPP`）即编译失败。
- **生命周期。** LayeredConfigLoader 的 MergedLayers 顺序不变：先建 runtime、后建 root，解析在这一范围内完成，只把配置值返回给调用方。

## 实施记录（#127）

P3b 按方案 A 落地 TrainingRule 与拓扑装载两项，上文约定与 #126 记录不再重复。耗时与编译名单见[构建优化结果的 P3b 节](build-optimization-results.md#p3btrainingrule-前置声明与拓扑装载下沉127)。

- **TrainingRule。** `training_rule.hpp` 只前置声明 `realm::scripting::LuaRuntime`；`unique_ptr<LuaRuntime>` 的析构本来就在 cpp，私有 `runtime()` 返回引用也只需声明。`lua_runtime.hpp` 改由 `training_rule.cpp` 包含。严格整数/nil 与取值范围验证、启动 `train(0)`/`level(0)` 检查、换线程用已捕获源码重建都没有改动，原有 TrainingRule 测试原样通过。
- **拓扑装载。** `main.cpp` 中把 `main.config` 的 `services` 表转成 `ServiceSpec` 列表的代码原样移到 service_host，入口为 `startup_topology.hpp` 的自由函数 `load_topology(config_root)`，实现在 `startup_topology_lua.cpp`。声明与 `ServiceSpec` 同头，不另建头：它只用普通类型，消费者就是 `ServiceSpec` 的消费者。错误契约不变：加载或执行失败抛 `runtime_error`，表缺失、为空或条目格式错抛 `invalid_argument`，依赖关系仍由 `StartupTopology` 校验。`realm_mesh` 只拿列表与异常，不再包含 Lua 头，也不再链接 `realm_scripting`。新增 `load_topology_test`（unit）以临时 `main.config` 固定错误契约与缺省字段；随包 `configs/main.config` 的装载结果按测试归置约定放进 `configs_load_smoke_test`。
- **CMake 使用需求。** `realm_game_realm` 对 `realm_scripting` 改为 PRIVATE；至此 gateway、login_verify、queue、realm、service_host 与 `realm_mesh` 都不再 PUBLIC 传播 sol2，只有直接驱动 Lua 的测试显式链接它。`realm_config_lua.hpp` 的包含方仍须自行链接 `RealmMesh::Scripting`，现有唯一跨目标使用方 service_host 已 PRIVATE 链接。
- **守卫。** `config_headers_test` 增加 `training_rule.hpp`；`startup_topology.hpp` 经 `mesh_host.hpp` 已在守卫范围内。

## 实施记录（#128）

P3c 按上表 GatewayRuntime 与 GatewayPrimaryTransport 两行落地，只拆轻量头与收窄消费者包含，不做 runtime PIMPL。耗时与编译名单见[构建优化结果的 P3c 节](build-optimization-results.md#p3cgateway-轻量事件与启动配置头128)。

- **事件头。** `GatewayEventKind` 与 `GatewayEvent` 移到 `gateway_event.hpp`，只依赖 `edge_session_table.hpp` 与 `message_transport.hpp`。二者是 runtime 与主传输边界共用的同一契约，同头；字段、缺省值与语义不变。
- **启动配置头。** `GatewayRuntimeOptions` 与 `GatewayConfig` 移到 `gateway_config.hpp`，属于普通配置头：不碰 Lua，也不带 runtime 的私有队列、线程与锁布局。`gateway_config_loader.hpp`、`gateway_config_lua.hpp` 改含它，切断 `layered_config_loader.hpp` → `gateway_config_loader.hpp` → `gateway_runtime.hpp` 这条把 runtime 布局带进 `service_host.hpp`、`mesh_host.hpp` 及其消费者的传播链。`mesh_host.cpp` 只用 `GatewayConfig`，改含配置头；`apps/mesh_host/main.cpp` 原有的 runtime 包含未被使用，删除。
- **主传输边界。** `gateway_primary_transport.hpp` 只包含事件头并前置声明 `GatewayRuntime`：生产适配器只存 `GatewayRuntime*`，内联构造只取地址。`gateway_runtime.hpp` 由 `gateway_primary_transport.cpp` 包含；构造真实 runtime 的 `gateway_primary_transport_test` 自行包含。登录管线与准入经它同样不带 runtime。
- **runtime 头。** `gateway_runtime.hpp` 只留 `QueueResult`、`GatewayRuntimeStats` 与 `GatewayRuntime`，网络层只含 `message_transport.hpp`，去掉不再使用的 `player_data_store.hpp`、`service_discovery_config.hpp`、`gateway_login_config.hpp` 与 `<filesystem>`。剩余包含方都真实构造或驱动 runtime（`try_send`、`drain_events`、`running()`、`local_port()` 或持有 `optional<GatewayRuntime>`），属于“稳定接口变化仍重编真实消费者”。
- **契约。** 未新增虚接口；`GatewayRuntime::stop()` 仍在 IO 线程 request_stop 后 join，ServiceHost 的回收与销毁顺序未改动。
- **守卫。** `config_headers_test` 断言普通配置头（含 `gateway_config.hpp`、`gateway_config_loader.hpp`、`mesh_host.hpp`）拿不到完整的 `GatewayRuntime`；新增 `gateway_headers_test`（unit）对事件、启动配置、主传输、登录管线与准入头做同样断言，并经内存适配器走一遍事件。
