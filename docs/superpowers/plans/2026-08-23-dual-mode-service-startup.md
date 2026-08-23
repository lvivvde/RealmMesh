# 双模式服务启动实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 单一 `realm_mesh` 二进制以拓扑驱动方式支持 all-in-one(模式 1)与单服务分布式(模式 2)两种启动形态,DAG 波次启动 + 全员就绪门禁。

**Architecture:** 新库 `framework/service_host` 提供 `StartupTopology`(DAG 解析)、`LayeredConfigLoader`(公共层+服务层+CLI 覆盖)、`ServiceHost`(单服务装配)、`MeshHost`(波次编排)。新入口 `apps/mesh_host` 产出 `realm_mesh` 二进制。复用现有 `GatewayConfigLoader/GatewayRuntime/EtcdServiceRegistry/ServicePublisher/ServiceResolver`,不改各服务业务逻辑。

**Tech Stack:** C++20、sol2(经 `realm::scripting::LuaRuntime`)、spdlog、httplib、etcd v3、GoogleTest、CMake。

**Spec:** `docs/superpowers/specs/2026-08-23-dual-mode-service-startup-design.md`

## Global Constraints

- 命名:类型 CamelCase;函数/变量 snake_case;私有成员尾下划线;宏全大写(`.clang-format`/`.clang-tidy` 固化,提交前检查)。
- 构建:`build/dev`;测试:`cd build/dev && ctest`;本仓库 git 需 `--git-dir=.realmmesh-git --work-tree=.`。
- 第三方库仅走 CMake FetchContent;不支持 KCP。
- 注释 Doxygen 风格 `///` 与 `///<`,中文。
- CMake 二进制路径 `.tools/cmake/bin/cmake`(PATH 里没有 cmake)。
- 设计偏离说明:模式 2 的"放行门禁"在应用层实现(依赖未就绪时登录回维护错误码),不做传输层延迟监听;模式 1 靠构造顺序天然满足"gateway 端口最后开"。

---

### Task 1: StartupTopology——DAG 解析

**Files:**
- Create: `framework/service_host/include/realmmesh/service_host/startup_topology.hpp`
- Create: `framework/service_host/src/startup_topology.cpp`
- Create: `framework/service_host/CMakeLists.txt`
- Test: `tests/cpp/framework/service_host/startup_topology_test.cpp`

**Interfaces:**
- Produces(后续任务依赖的精确签名):

```cpp
namespace realm::service_host {

/// 单服务拓扑描述:name 必须非空;depends_on 引用 specs 内其他 name。
struct ServiceSpec {
    std::string name;
    std::vector<std::string> depends_on;
    bool entry{false};  ///< 入口服务(全部 ready 后才放行)
};

/// 拓扑解析:环、未知依赖、重名 → 抛 std::invalid_argument。
class StartupTopology final {
public:
    explicit StartupTopology(std::vector<ServiceSpec> specs);

    /// 启动波次:每波内服务可并行,波间必须等待前波 ready。
    [[nodiscard]] const std::vector<std::vector<std::string>>& waves() const noexcept;

    /// 关停顺序:启动完成序的严格反序(同波内保持声明序反转)。
    [[nodiscard]] std::vector<std::string> shutdown_order() const;

    /// 全部服务的 ready 才允许 entry 服务放行。
    [[nodiscard]] const std::vector<std::string>& all_names() const noexcept;
};
}  // namespace realm::service_host
```

- [ ] **Step 1: 写失败测试**(覆盖:无依赖并行同波、链式依赖分波、混合 DAG 好友服务并行、环抛异常、未知依赖抛异常、反序关停)

```cpp
#include "realmmesh/service_host/startup_topology.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

namespace realm::service_host {
namespace {

TEST(StartupTopologyTest, IndependentServicesShareFirstWave) {
    const StartupTopology topology(std::vector<ServiceSpec>{
        {"realm", {}, false},
        {"friends", {}, false},
        {"gateway", {"realm", "friends"}, true},
    });
    EXPECT_EQ(topology.waves().at(0),
              (std::vector<std::string>{"realm", "friends"}));
    EXPECT_EQ(topology.waves().at(1), std::vector<std::string>{"gateway"});
    EXPECT_EQ(topology.shutdown_order(),
              (std::vector<std::string>{"gateway", "friends", "realm"}));
}

TEST(StartupTopologyTest, ChainSplitsIntoWaves) {
    const StartupTopology topology(std::vector<ServiceSpec>{
        {"login", {"realm"}, false},
        {"realm", {}, false},
        {"gateway", {"login"}, true},
    });
    EXPECT_EQ(topology.waves().size(), std::size_t{3});
    EXPECT_EQ(topology.waves().at(2), std::vector<std::string>{"gateway"});
}

TEST(StartupTopologyTest, CycleThrows) {
    EXPECT_THROW(
        StartupTopology(std::vector<ServiceSpec>{
            {"a", {"b"}, false}, {"b", {"a"}, false}}),
        std::invalid_argument);
}

TEST(StartupTopologyTest, UnknownDependencyThrows) {
    EXPECT_THROW(
        StartupTopology(std::vector<ServiceSpec>{{"a", {"ghost"}, false}}),
        std::invalid_argument);
}

}  // namespace
}  // namespace realm::service_host
```

- [ ] **Step 2: 跑测试确认失败**

Run: `.tools/cmake/bin/cmake --build build/dev --target service_host_test -j && ./build/dev/tests/cpp/service_host_test`
Expected: 编译失败(头文件不存在)——先在测试 CMake 引用目标;或按本仓库测试注册模式把 `tests/cpp/CMakeLists.txt` 加目标后构建报错。

- [ ] **Step 3: 最小实现**

```cpp
// startup_topology.cpp
#include "realmmesh/service_host/startup_topology.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace realm::service_host {
namespace {

void validate(const std::vector<ServiceSpec>& specs) {
    std::unordered_set<std::string> names;
    for (const auto& spec : specs) {
        if (spec.name.empty() || !names.insert(spec.name).second) {
            throw std::invalid_argument("service names must be non-empty and unique");
        }
    }
    for (const auto& spec : specs) {
        for (const auto& dependency : spec.depends_on) {
            if (!names.contains(dependency)) {
                throw std::invalid_argument("unknown dependency: " + dependency);
            }
        }
    }
}

}  // namespace

StartupTopology::StartupTopology(std::vector<ServiceSpec> specs)
    : specs_(std::move(specs)) {
    validate(specs_);
    std::unordered_map<std::string, std::size_t> depth;
    /// Kahn 分层:节点深度 = 1 + 最深依赖;等价波次划分。
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto& spec : specs_) {
            std::size_t depth_value = 0;
            for (const auto& dependency : spec.depends_on) {
                depth_value = std::max(depth_value, depth[dependency] + 1);
            }
            const auto [it, inserted] = depth.emplace(spec.name, depth_value);
            if (inserted || it->second < depth_value) {
                it->second = depth_value;
                changed = true;
            }
        }
    }
    /// changed 稳定后仍有依赖深度在增长 → 环;Kahn 计数校验:
    std::size_t resolved = 0;
    (void)resolved;
    // 波次装配
    std::size_t max_depth = 0;
    for (const auto& [name, value] : depth) max_depth = std::max(max_depth, value);
    waves_.assign(max_depth + 1, {});
    for (const auto& spec : specs_) waves_[depth[spec.name]].push_back(spec.name);
    /// 完成序 = 波次序、波内声明序;shutdown 为其反序。
    for (const auto& wave : waves_) {
        for (const auto& name : wave) completion_order_.push_back(name);
    }
    std::reverse(completion_order_.begin(), completion_order_.end());
    /// 环检测:若存在环,环上节点永远算不出稳定深度——用 Kahn 出度校验。
    std::unordered_map<std::string, std::size_t> pending;
    for (const auto& spec : specs_) pending[spec.name] = spec.depends_on.size();
    // 逐轮消解依赖已满足的节点;一轮无进展即环。
    std::unordered_set<std::string> done;
    for (bool progressed = true; progressed;) {
        progressed = false;
        for (const auto& spec : specs_) {
            if (done.contains(spec.name)) continue;
            bool ready = true;
            for (const auto& dependency : spec.depends_on) {
                if (!done.contains(dependency)) { ready = false; break; }
            }
            if (ready) { done.insert(spec.name); progressed = true; }
        }
    }
    if (done.size() != specs_.size()) {
        throw std::invalid_argument("service dependency cycle detected");
    }
}
// waves()/shutdown_order()/all_names() 为对成员的简单访问器,见头文件声明。
}  // namespace realm::service_host
```

注意:实现时把环检测放到深度计算**之前**(先 Kahn 校验再算深度,代码更清晰;上面示意合并了两者,实现者应拆开)。

- [ ] **Step 4: 跑测试确认通过**

Run: 同 Step 2。Expected: 5 个测试全 PASS。

- [ ] **Step 5: CMake 接线并提交**

`framework/service_host/CMakeLists.txt` 参照 `framework/concurrency/CMakeLists.txt` 模式:`add_library(realm_service_host ...)`,`target_link_libraries` 暂无;在上级 CMake `add_subdirectory`;测试目标 `service_host_test` 参照 `tests/cpp/CMakeLists.txt` 现有 observability 测试模式。

```bash
git --git-dir=.realmmesh-git --work-tree=. add framework/service_host tests/cpp && git --git-dir=.realmmesh-git --work-tree=. commit -m "feat: add startup topology dag parsing"
```

---

### Task 2: GatewayConfigLoader::parse 公开化(行为不变重构)

**Files:**
- Modify: `game/gateway/include/realmmesh/game/gateway/gateway_config_loader.hpp`
- Modify: `game/gateway/src/gateway_config_loader.cpp:190-358`

**Interfaces:**
- Produces:

```cpp
class GatewayConfigLoader {
public:
    /// 从文件加载并解析(行为不变)。
    [[nodiscard]] static GatewayConfig load(const std::filesystem::path& path);
    /// 直接解析已合并的 Lua 根表(供分层加载器复用)。
    [[nodiscard]] static GatewayConfig parse(const sol::table& root);
};
```

- [ ] **Step 1: 确认现有测试基线绿**

Run: `.tools/cmake/bin/cmake --build build/dev -j && cd build/dev && ../.tools/cmake/bin/ctest -R gateway`
Expected: 现有 gateway 相关测试全 PASS(重构前基线)。

- [ ] **Step 2: 重构**

把 `load()` 中 `const sol::table root = ...;` 之后的全部解析体(move 到新静态方法 `parse(const sol::table& root)`),`load()` 变为:加载模块 → 取 root → `return parse(root);`。无任何解析逻辑改动。hpp 加声明并 include sol 前置(经 `realmmesh/scripting/lua_runtime.hpp` 已可见 sol)。

- [ ] **Step 3: 全量测试确认绿**

Run: `cd build/dev && ../.tools/cmake/bin/ctest`
Expected: 64 tests passed(数量以当前为准,全绿即可)。

- [ ] **Step 4: 提交**

```bash
git --git-dir=.realmmesh-git --work-tree=. add game/gateway && git --git-dir=.realmmesh-git --work-tree=. commit -m "refactor: expose gateway config table parser"
```

---

### Task 3: LayeredConfigLoader——公共层+服务层+CLI 覆盖

**Files:**
- Create: `framework/service_host/include/realmmesh/service_host/layered_config_loader.hpp`
- Create: `framework/service_host/src/layered_config_loader.cpp`
- Modify: `framework/service_host/CMakeLists.txt`(链接 `realm_gateway_service`、`realm_scripting`)
- Test: `tests/cpp/framework/service_host/layered_config_loader_test.cpp`

**Interfaces:**
- Consumes: `GatewayConfigLoader::parse(const sol::table&)`(Task 2)、`scripting::LuaRuntime::load_module/load_module_source/module`。
- Produces:

```cpp
namespace realm::service_host {

/// CLI 覆盖项:全部可选,覆盖合并结果。
struct CliOverrides {
    std::optional<std::string> instance_id;
    std::optional<std::string> node_id;
    std::optional<std::string> zone;
};

/// 分层配置加载:configs/<root> 下 common/*.lua 深合并 services/<name>.lua,
/// CLI 覆盖最后生效;日志 file_path 按实例身份生成。
class LayeredConfigLoader final {
public:
    /// config_root: 含 common/ 与 services/ 的目录。文件缺失/解析失败抛异常。
    [[nodiscard]] static game::gateway::GatewayConfig load(
        const std::filesystem::path& config_root,
        std::string_view service_name,
        const CliOverrides& overrides = {});
};
}  // namespace realm::service_host
```

- [ ] **Step 1: 写失败测试**(测试内用临时目录写 lua 文件)

```cpp
#include "realmmesh/service_host/layered_config_loader.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace realm::service_host {
namespace {

class LayeredConfigTest : public ::testing::Test {
protected:
    std::filesystem::path root_;

    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() /
                ("layered-cfg-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
        std::filesystem::create_directories(root_ / "common");
        std::filesystem::create_directories(root_ / "services");
        write(root_ / "common" / "logging.lua", "return { logging = { level = \"warn\", environment = \"test\", cluster = \"ci\", region = \"cn\", service_name = \"common\", console = false, metrics_port = 0 } }");
        write(root_ / "common" / "discovery.lua", "return { service_discovery = { enabled = false, endpoint = \"http://etcd:2379\", lease_ttl_seconds = 9 } }");
    }
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }
    static void write(const std::filesystem::path& path, std::string_view text) {
        std::ofstream output(path);
        output << text;
    }
};

TEST_F(LayeredConfigTest, ServiceLayerOverridesCommon) {
    write(root_ / "services" / "login.lua",
          "return { logging = { level = \"debug\" }, tick_rate = 30 }");
    const auto config = LayeredConfigLoader::load(root_, "login");
    EXPECT_EQ(config.logging.min_severity, observability::Severity::Debug);  // 服务层覆盖
    EXPECT_EQ(config.service_discovery.lease_ttl, std::chrono::seconds(9));  // 公共层保留
    EXPECT_EQ(config.tick_rate, std::uint32_t{30});
}

TEST_F(LayeredConfigTest, CliOverridesInstanceIdentity) {
    write(root_ / "services" / "login.lua",
          "return { service_discovery = { instance_id = \"file-id\", node_id = \"file-node\" } }");
    const auto config = LayeredConfigLoader::load(
        root_, "login", CliOverrides{.instance_id = "cli-id", .node_id = "cli-node"});
    EXPECT_EQ(config.service_discovery.instance_id, "cli-id");
    EXPECT_EQ(config.service_discovery.node_id, "cli-node");
    /// file_path 按实例生成:<root>/logs/login/login-cli-id.jsonl
    EXPECT_NE(config.logging.file_path.string().find("login-cli-id.jsonl"),
              std::string::npos);
}

TEST_F(LayeredConfigTest, MissingServiceFileThrows) {
    EXPECT_THROW(LayeredConfigLoader::load(root_, "ghost"), std::runtime_error);
}

}  // namespace
}  // realm::service_host
```

- [ ] **Step 2: 跑测试确认失败**(类型/文件不存在)

Run: `.tools/cmake/bin/cmake --build build/dev --target service_host_test -j && ./build/dev/tests/cpp/service_host_test`
Expected: 编译错误 layered_config_loader.hpp 不存在。

- [ ] **Step 3: 实现**

核心:纯 Lua 深合并脚本经 `load_module_source` 注入,common 各文件与服务文件经 `load_module` 加载,合并后应用 CLI 覆盖与 file_path 生成,最后 `GatewayConfigLoader::parse(root)`:

```cpp
GatewayConfig LayeredConfigLoader::load(
    const std::filesystem::path& config_root, std::string_view service_name,
    const CliOverrides& overrides) {
    scripting::LuaRuntime runtime;
    std::string error;
    constexpr std::string_view merge_script = R"lua(
return {
    merge = function(base, overlay)
        for key, value in pairs(overlay) do
            if type(base[key]) == "table" and type(value) == "table" then
                merge(base[key], value)  -- 注意:需自引用,见下
            else
                base[key] = value
            end
        end
        return base
    end,
}
)lua";
    // 注:merge 自引用用 local function merge(...) ... end; return {merge=merge} 形式书写。
    if (!runtime.load_module_source("config_merger", std::string(merge_script), &error)) {
        throw std::runtime_error("failed to load merge script: " + error);
    }
    auto load_file = [&](const std::string& module, const std::filesystem::path& path) {
        if (!runtime.load_module(module, path, &error)) {
            throw std::runtime_error("failed to load " + path.string() + ": " + error);
        }
        return runtime.module(module);
    };
    const sol::table root = load_file(
        "service_config", config_root / "services" / (std::string(service_name) + ".lua"));
    for (const auto& common : std::filesystem::directory_iterator(config_root / "common")) {
        if (common.path().extension() != ".lua") continue;
        const sol::table layer = load_file(
            "common_" + common.path().stem().string(), common.path());
        call_merge(runtime, root, layer);  // merge(layer→root):root=服务层在上
    }
    apply_overrides(root, service_name, overrides);  // 见下
    return game::gateway::GatewayConfigLoader::parse(root);
}
```

`apply_overrides`:
- `instance_id/node_id/zone` 非空 → 写入 `root["service_discovery"]` 对应键;
- 总是生成 `root["logging"]["file_path"] = "<config_root>/logs/<svc>/<svc>-<instance>.jsonl"`(instance 取覆盖值或服务表原 instance_id 或默认 `<svc>-01`),同时写 `root["logging"]["service_name"] = <svc>`。

注意 merge 方向:公共层合并进服务层(服务层值胜)。

- [ ] **Step 4: 跑测试确认通过**,`clang-format -i` 两份新文件后复跑。

- [ ] **Step 5: 提交**

```bash
git --git-dir=.realmmesh-git --work-tree=. add framework/service_host tests/cpp && git --git-dir=.realmmesh-git --work-tree=. commit -m "feat: add layered config loader with cli overrides"
```

---

### Task 4: ServiceHost——单服务装配

**Files:**
- Create: `framework/service_host/include/realmmesh/service_host/service_host.hpp`
- Create: `framework/service_host/src/service_host.cpp`
- Modify: `framework/service_host/CMakeLists.txt`(链接 realm_cluster、realm_observability、realm_gateway_service)
- Test: `tests/cpp/framework/service_host/service_host_test.cpp`

**Interfaces:**
- Consumes: `LayeredConfigLoader`(Task 3)、`GatewayRuntime`、`EtcdServiceRegistry`、`ServicePublisher`、`ServiceResolver`、`Logger`、`LoggerMetricsServer`、装配模式参照 [apps/login/main.cpp:58-130](file:///home/artis/Project/RealmMesh/apps/login/main.cpp)。
- Produces:

```cpp
namespace realm::service_host {

/// 单服务装配:封装原三个 main 的公共链。
/// 发现关闭时 ready() = runtime 监听成功;发现开启时还需注册 tick 成功
/// (required=true 注册失败 → start() 抛出)。
class ServiceHost final {
public:
    /// service_name/instance_id 驱动 LayeredConfigLoader;构造失败抛异常。
    ServiceHost(const std::filesystem::path& config_root,
                std::string_view service_name,
                const CliOverrides& overrides = {});
    ~ServiceHost();

    /// 启动 runtime 与发现注册;返回 ready 状态。
    [[nodiscard]] bool start();
    [[nodiscard]] bool ready() const noexcept;
    /// 优雅关停:停 runtime → 注销发现 → logger flush(2s)。
    void stop();

    [[nodiscard]] game::gateway::GatewayRuntime& runtime() noexcept;
    [[nodiscard]] observability::Logger& logger() noexcept;
    /// 依赖解析(发现开启时);未开启返回 nullptr。
    [[nodiscard]] cluster::ServiceResolver* resolver() noexcept;

    /// 轮询发现续约与配置重载(SIGHUP 语义),由编排器每帧调用。
    void tick();
    /// Prometheus 指标含 realmmesh_service_ready gauge。
    [[nodiscard]] std::string prometheus_metrics() const;
};
}  // namespace realm::service_host
```

- [ ] **Step 1: 写失败测试**(发现禁用路径,不起 etcd)

```cpp
TEST(ServiceHostTest, StartsRuntimeAndReadiesWithoutDiscovery) {
    // 复用 Task 3 的临时配置 fixture:common/discovery.lua enabled=false,
    // services/host_test.lua 带 1 个 tls_tcp transport(端口 0 随机,
    // 证书环境变量指向测试 fixture 的自签证书)。
    ServiceHost host(root_, "host_test");
    EXPECT_TRUE(host.start());
    EXPECT_TRUE(host.ready());
    EXPECT_NE(host.runtime().local_port(), std::uint16_t{0});
    EXPECT_NE(host.prometheus_metrics().find("realmmesh_service_ready 1"),
              std::string::npos);
    host.stop();
    EXPECT_FALSE(host.runtime().running());
}
```

证书 fixture:测试SetUp 用 openssl 命令生成临时自签证书并 `setenv("REALMMESH_TLS_CERTIFICATE_FILE",...)`(参照 `tests/cpp` 现有 TLS 测试的 fixture 复用方式,先 Glob 找现成 helper)。

- [ ] **Step 2: 确认失败** → **Step 3: 实现**

装配体照搬 login main:LayeredConfigLoader::load → Logger(文件路径已含实例)→ MetricsServer(port!=0 时)→ GatewayRuntime(std::move(config))→ 发现 enabled 时:Registry+Publisher(首 tick 注册;required 失败抛 runtime_error)+ Resolver(ServiceType 由 service_name 映射:gateway→解析 Login,realm→解析 Gateway,login→解析 Realm;协议 TlsTcp)。`ready_` 原子位在 start() 成功路径置位;prometheus 输出拼 `realmmesh_service_ready` gauge。stop():runtime.stop() → publisher 析构注销 → flush。tick():publisher->tick() + resolver->poll。

- [ ] **Step 4: 跑测试通过** → **Step 5: 提交** `feat: add service host assembly`

---

### Task 5: MeshHost——波次编排与就绪门禁

**Files:**
- Create: `framework/service_host/include/realmmesh/service_host/mesh_host.hpp`
- Create: `framework/service_host/src/mesh_host.cpp`
- Test: `tests/cpp/framework/service_host/mesh_host_test.cpp`

**Interfaces:**
- Consumes: `StartupTopology`(Task 1)、`ServiceHost`(Task 4)。
- Produces:

```cpp
namespace realm::service_host {

/// 服务拓扑条目(main.config 解析产物)。
struct MeshServiceSpec {
    std::string name;
    std::vector<std::string> depends_on;
    bool entry{false};
};

/// 编排器:波次构造 ServiceHost,entry 服务在全员 ready 后放行;
/// 关停按拓扑反序。
class MeshHost final {
public:
    MeshHost(std::filesystem::path config_root,
             std::vector<MeshServiceSpec> specs,
             CliOverrides overrides = {});
    /// 波次启动;任一失败 → 已启动者反序回收后返回 false。
    [[nodiscard]] bool start_all();
    /// entry 服务是否允许放行(全员 ready)。
    [[nodiscard]] bool entry_ready() const noexcept;
    /// 每 tick 轮询全部 host 的发现续约。
    void tick();
    /// 反序关停全部服务。
    void shutdown();
    /// 单服务访问(模式 2 单服务时也用它)。
    [[nodiscard]] ServiceHost& service(std::string_view name);
};
}  // namespace realm::service_host
```

- [ ] **Step 1: 写失败测试**

用 2 个无发现、随机端口的 host_test 服务(spec: a 无依赖、b 依赖 a、entry=a)验证:start_all 后 entry_ready()==true 且两个 runtime 都 running;shutdown 后全部 !running。再加一个"依赖服务启动失败(端口冲突配置)→ start_all 返回 false 且 a 已停止"的用例(失败注入:第二个服务 file_path 指向不可写目录使 Logger 构造抛异常)。

- [ ] **Step 2: 确认失败** → **Step 3: 实现**

```cpp
bool MeshHost::start_all() {
    for (const auto& wave : topology_.waves()) {
        for (const auto& name : wave) {
            try {
                auto host = std::make_unique<ServiceHost>(
                    config_root_, name, overrides_);
                if (!host->start()) throw std::runtime_error(name + " not ready");
                hosts_.emplace(name, std::move(host));
            } catch (const std::exception&) {
                shutdown();  // 反序回收已启动者
                return false;
            }
        }
    }
    return entry_ready();
}
```

波内"并行"说明:ServiceHost::start() 非阻塞(runtime 自持 IO 线程),同波顺序构造即可,语义等同并行——计划不引线程,文档注释写明。

- [ ] **Step 4: 测试通过** → **Step 5: 提交** `feat: add mesh host orchestration`

---

### Task 6: realm_mesh 入口 + 配置文件 + E2E

**Files:**
- Create: `apps/mesh_host/CMakeLists.txt`
- Create: `apps/mesh_host/main.cpp`
- Create: `configs/common/logging.lua`、`configs/common/discovery.lua`
- Create: `configs/services/gateway.lua`、`configs/services/realm.lua`、`configs/services/login.lua`(从 `lua/config/services/*.lua` 迁移,按分层拆出公共部分)
- Create: `configs/main.config`(Lua:`return { services = { {name="realm"}, {name="login", depends_on={"realm"}}, {name="gateway", depends_on={"login"}, entry=true} } }`)
- Test: `tests/cpp/framework/service_host/mesh_host_e2e_test.cpp`

**Interfaces:**
- Consumes: `MeshHost`、`LayeredConfigLoader`、`GatewayConfigLoader::parse`。
- Produces: 可执行目标 `realm_mesh`(CMake add_executable,安装到 build/dev/bin);CLI:`--config <dir>`(默认 `configs/`)、`--service <name>`(模式 2,限定单服务)、`--instance-id/--node-id/--zone`。

- [ ] **Step 1: 写失败 E2E 测试**

```cpp
TEST(MeshHostE2ETest, AllInOneStartsAndStopsCleanly) {
    /// 前置:fixture 生成自签证书 + 设置环境变量(复用 Task 4 fixture)。
    const std::filesystem::root = repo_root() / "configs";  // repo_root 经 REALMMESH_SOURCE_DIR 宏
    MeshServiceSpec realm{.name = "realm"};
    MeshServiceSpec login{.name = "login", .depends_on = {"realm"}};
    MeshServiceSpec gateway{.name = "gateway", .depends_on = {"login"}, .entry = true};
    MeshHost mesh(root, {realm, login, gateway});
    ASSERT_TRUE(mesh.start_all());
    EXPECT_TRUE(mesh.entry_ready());
    /// 依赖服务端口可连:gateway 的 realm downstream(127.0.0.1:7100)TCP 探活成功。
    mesh.shutdown();
    EXPECT_FALSE(mesh.service("realm").runtime().running());
}
```

- [ ] **Step 2: 确认失败** → **Step 3: 实现**

`main.cpp` 流程:解析 argv(`--config/--service/--instance-id/--node-id/--zone`)→ 读 `main.config` 拓扑(LuaRuntime 加载,`--service` 时收窄为单服务且 overrides 注入)→ MeshHost → start_all 失败 exit 1 → SIGINT/SIGTERM 置停止位 → SteadyFrameClock 帧循环调 `mesh.tick()`(帧体内做 reload/放行检查,复用 login main 的 SIGHUP 模式)→ 退出前 shutdown + flush。configs 文件迁移:login/realm/gateway 三份现有 lua 中 `logging` 公共字段(级别/队列/滚动/console 模板)进 common/logging.lua,`service_discovery` 公共字段进 common/discovery.lua,服务文件保留 transports/tick_rate/downstream/差异化字段;**端口不变**(7000/7100/8000/9101-9103)。

- [ ] **Step 4: E2E 通过 + 手动 smoke**

Run: `cd build/dev && ../.tools/cmake/bin/ctest -R mesh` 然后 `./bin/realm_mesh --config ../../configs`(Ctrl-C 干净退出,退出码 0)。

- [ ] **Step 5: 提交** `feat: add realm_mesh dual-mode host binary and configs`

---

### Task 7: 模式 2 验证 + 旧入口退役

**Files:**
- Modify: `scripts/dev-services.sh`(改拉起 `realm_mesh --service <name> --config configs`,保留 stop/status)
- Delete: `apps/gateway/main.cpp`、`apps/login/main.cpp`、`apps/realm/main.cpp` 及其 CMake 目标(E2E 通过、用户确认后)
- Test: `tests/cpp/framework/service_host/mode2_test.cpp`

- [ ] **Step 1: 写失败测试**:单服务 spec + `--service login` 语义(拓扑收窄为 1 服务、instance_id 覆盖生效)在 LayeredConfigLoader/MeshHost 层面断言;发现 enabled=false 时 gateway 单服务 ready 依赖 TCP 探活 downstream(127.0.0.1:7100)——探活失败则 entry_ready() 为 false。
- [ ] **Step 2-4: 实现 + 通过**:MeshHost 增加单服务收窄构造;dev-services.sh 改造。
- [ ] **Step 5: 全量回归**:`cd build/dev && ../.tools/cmake/bin/ctest` 全绿。
- [ ] **Step 6: 经用户确认后删除三个旧 main,提交** `chore: retire legacy service binaries`

## Self-Review

- Spec 覆盖:§2 架构(Task 4/5/6)、§3 配置(Task 2/3/6)、§4 DAG+门禁(Task 1/5/6,模式2门禁偏差已在 Global Constraints 声明)、§5 关停(Task 5)、§6 错误处理(Task 5 失败回收 + Task 3 校验异常)、§7 测试(各任务 TDD)、§8 迁移(Task 2/7)。✓
- 占位符:无 TBD;Task 4/5 测试代码给了核心断言与 fixture 指引。✓
- 类型一致性:`CliOverrides/ServiceSpec/MeshServiceSpec/StartupTopology/ServiceHost/MeshHost` 跨任务签名一致;`load(path)→load(root, name, overrides)` 无冲突。✓
