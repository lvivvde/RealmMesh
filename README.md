# RealmMesh

RealmMesh 是一个计划采用帧驱动模型构建的 C++ 游戏服务器。

服务器以固定或可配置的帧率推进游戏世界。在每一帧中，系统依次处理网络消息、游戏逻辑、定时任务与状态同步，使逻辑执行顺序清晰且可预测。

整体服务拓扑、客户端接入时序、网关线程模型和 C++/Lua 分层见 [项目架构文档](docs/architecture.md)。

## 核心思路

```text
接收客户端消息
       ↓
更新当前帧的游戏状态
       ↓
处理定时任务与游戏逻辑
       ↓
向客户端同步结果
       ↓
等待下一帧
```

## 计划功能

- 可配置的服务器帧率
- 基于事件的网络消息处理
- 房间与玩家生命周期管理
- 定时任务和延迟事件
- 多线程任务调度
- 日志、性能统计与优雅停服

## 构建

环境要求：

- Linux
- 支持 C++20 的编译器（GCC 10+ 或 Clang 12+）
- CMake 3.20+

一键配置、编译并测试：

```bash
./scripts/build.sh
```

也可以分别执行：

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

首次配置会自动获取固定版本的第三方依赖。

运行最小服务器示例：

```bash
./build/dev/realmmesh
```

默认以每秒 20 帧运行 5 帧后退出。持续运行或修改帧率：

```bash
./build/dev/realmmesh --frames 0 --tick-rate 30
```

当前优先支持 Linux。

## C++ / Lua 脚本

项目已接入 [sol2](https://github.com/ThePhD/sol2)，版本固定为 `v3.3.1`，底层使用仓库构建的 Lua 5.4。C++ 仍然负责网络、帧循环、并发、服务治理和核心状态；Lua 用于配置以及需要热更的轻量业务规则。

`realm::scripting::LuaRuntime` 提供持久 Lua VM、C++ 函数绑定、受保护调用和按文件时间戳热更。Lua 模块必须返回一张导出表：

```lua
return {
    damage = function(base)
        return cpp_double(base) + 1
    end,
}
```

```cpp
realm::scripting::LuaRuntime lua;
lua.set_function("cpp_double", [](int value) { return value * 2; });

std::string error;
if (!lua.load_module("combat", "lua/scripts/combat.lua", &error)) {
    throw std::runtime_error(error);
}

const int damage = lua.call<int>("combat", "damage", 20);
```

建议每个逻辑分片或逻辑线程独占一个 `LuaRuntime`，不要跨线程访问同一个 Lua VM。`reload_changed()` 应在帧边界调用：新脚本会先在隔离环境中完成编译和执行，成功后才替换旧模块；失败时保留上一版本并返回错误报告。

默认脚本环境不开放 `package`、`os`、`io`、`debug`、`dofile` 和 `loadfile`。这能限制脚本自行访问文件和系统接口，但不等同于可运行不受信任代码的完整安全沙箱；如果未来允许用户提交脚本，还需要指令数、执行时间和内存配额。

## etcd 服务发现

登录服、角色服和网关服已经接入 etcd v3。服务启动后会在 `/realmmesh/services/<类型>/<实例 ID>` 下发布带 Lease 的多协议端点，后台自动续租，正常退出时主动撤销；登录服通过 Watch 缓存角色服地址，角色服通过 Watch 缓存网关地址。

安装并启动固定版本的本地 etcd：

```bash
./scripts/install-etcd.sh
./scripts/run-etcd-dev.sh
```

服务发现参数位于各服务的 Lua 配置中：

```lua
service_discovery = {
    enabled = true,
    required = false,
    endpoint = "http://127.0.0.1:2379",
    key_prefix = "/realmmesh/services",
    instance_id = "gateway-dev-01",
    node_id = "development-node",
    zone = "development",
    advertise_address = "127.0.0.1",
    lease_ttl_seconds = 15,
}
```

`required = false` 时，etcd 暂时不可用不会阻止服务启动，登录服和角色服会降级使用 Lua 中的固定下游地址，并持续重试注册与发现。生产环境可以设为 `true` 以便启动失败时立即退出。

当前客户端使用 etcd 官方 JSON gRPC gateway，Watch 通过可恢复的前缀快照增量对比实现。开发配置仅使用本机 HTTP；公网或跨主机部署前还需增加 TLS、etcd 身份认证以及多 endpoint 故障切换。自定义协调服仍属于后续控制面，不再承担基础服务发现的单点代理职责。

## 可配置网关协议

网关网络层使用统一的 `IMessageTransport` 接口。目前已经实现：

- TCP：非阻塞 socket + epoll，使用 4 字节大端长度字段分帧
- UDP：按远端地址维护逻辑会话，单个数据报对应一条消息
- KCP：基于 UDP 的可靠消息传输，带短期票据握手和加密会话

协议开关、监听地址和端口位于
[`lua/config/services/gateway.lua`](lua/config/services/gateway.lua)。一个服务可以同时启用多个协议，也可以只启用其中一个：

```lua
return {
    transports = {
        {
            name = "client_tcp",
            protocol = "tcp",
            enabled = true,
            listen_address = "0.0.0.0",
            listen_port = 8000,
        },
        {
            name = "client_udp",
            protocol = "udp",
            enabled = false,
            listen_address = "0.0.0.0",
            listen_port = 8001,
        },
    },
}
```

启动开发阶段的 Echo 网关：

```bash
./build/dev/bin/realm_gateway
```

也可以选择另一份 Lua 配置，或临时覆盖其中启用的 TCP 监听地址：

```bash
./build/dev/bin/realm_gateway --config lua/config/services/gateway.lua
./build/dev/bin/realm_gateway --listen 127.0.0.1 --port 9000
```

网关配置已通过 sol2 加载。配置属于启动期数据，因此读取完成后即可释放对应 Lua VM；需要热更的业务脚本使用上面的持久 `LuaRuntime`。当前 Echo 行为只用于验证协议模块，后续会替换为鉴权、消息派发和服务路由。

KCP 默认安全关闭。启用前先生成 32 字节票据主密钥，并通过环境变量注入；密钥不会写入 Lua 或仓库：

```bash
export REALMMESH_KCP_TICKET_KEY="$(openssl rand -hex 32)"
```

然后在 `gateway.lua` 中将 `client_kcp.enabled` 改为 `true`。登录服使用相同主密钥调用 `KcpTicketCodec::issue()` 签发短期票据，客户端只获得单次会话票据和会话密钥，不会获得主密钥。详细协议和边界见 [KCP 安全协议](docs/kcp-security.md)。

当前实现包含 XChaCha20-Poly1305 加密认证、滑动窗口抗重放、空闲会话回收，以及认证通过后的客户端地址迁移。它尚未经过独立安全审计，正式公网发布前仍应安排协议审计、密钥轮换和压力测试。

服务发现中的实例现在可以发布多个带协议类型的端点，因此协调服能够同时登记同一场景服的 TCP、UDP 或 KCP 地址。

## 多协议客户端会话

网关把一个客户端建模为一个逻辑 `ClientSession`：TCP 是主通道，UDP 和 KCP 是经过鉴权后按需绑定的辅助通道。业务层发送消息时可以逐包选择首选协议；如果客户端尚未建立该通道，或该通道发送失败，默认自动改用 TCP：

```cpp
const auto result = gateway.send(
    client_session_id,
    payload,
    {
        .preferred = realm::network::TransportProtocol::Udp,
        .fallback = realm::game::gateway::FallbackPolicy::UseTcp,
    });
```

对于只适合指定协议、不能降级的消息，可以使用 `DropIfUnavailable`。TCP 断开时整个逻辑会话会被移除；UDP 或 KCP 断开只解绑对应辅助通道。

TCP 建连后网关会自动创建逻辑会话。登录鉴权流程确认辅助通道属于同一客户端后，调用 `GatewayServer::bind_channel()` 完成绑定。绑定令牌的线上消息格式将随登录协议一起定义，不能仅凭来源地址自动绑定。

## 网络线程与帧线程

网关可执行程序现在使用生产者—消费者模型，网络 socket 和协议会话只由 I/O 线程操作：

```text
I/O 线程：epoll + TCP/UDP/KCP
       │ 生产 GatewayEvent
       ▼
有界入站队列
       │ 每帧批量消费
       ▼
帧逻辑线程
       │ 生产发送/绑定/关闭命令
       ▼
有界出站队列
       │ 批量消费
       ▼
I/O 线程
```

队列满时不会无限占用内存。UDP 入站消息允许丢弃；TCP/KCP 入站过载会主动断开对应会话；业务层提交出站命令时会立即得到 `Queued`、`Full` 或 `Stopped`。TCP 每个连接还使用 `max_pending_output_bytes` 限制尚未写入内核的发送数据。

运行参数在 `gateway.lua` 中配置：

```lua
tick_rate = 20,
max_events_per_frame = 4096,
runtime = {
    inbound_capacity = 65536,
    outbound_capacity = 65536,
    max_commands_per_cycle = 4096,
    io_poll_interval_ms = 2,
},
```

`GatewayRuntime::drain_events()` 由帧线程调用；`try_send()`、`try_send_channel()`、`try_bind_channel()` 和 `try_close_channel()` 只向出站队列提交命令。运行时退出时会输出丢包、过载断连、出站拒绝、TCP 回退和发送失败统计。

## 三阶段客户端接入

当前提供三个可执行程序和三个 TCP 公网入口：

| 程序 | 默认端口 | 用途 |
|---|---:|---|
| `realm_login` | 7000 | 账号认证并签发 `LoginTicket` |
| `realm_character` | 7100 | 校验登录票据、返回角色列表并签发 `EnterGameTicket` |
| `realm_gateway` | 8000 | 单次消费进服票据并建立正式游戏会话 |

三个进程必须共享同一个 32 字节票据签名密钥：

```bash
export REALMMESH_SESSION_TICKET_KEY="$(openssl rand -hex 32)"

./build/dev/bin/realm_gateway &
./build/dev/bin/realm_character &
./build/dev/bin/realm_login
```

客户端消息继续使用 TCP 的 4 字节大端长度字段分帧，帧内统一使用
Protocol Buffers `Envelope`：协议版本、消息 ID、请求 ID 和具体消息载荷彼此分离。
UDP/KCP 与 TCP 复用同一业务消息格式，只有外层传输封装不同。完整流程为：

```text
LoginRequest(account, credential)
  -> LoginSucceeded(LoginTicket, realm endpoint)
RealmAuthenticate(LoginTicket)
  -> CharacterList
SelectCharacter(character_id)
  -> EnterGameIssued(一次性 EnterGameTicket, gateway endpoint)
EnterGame(EnterGameTicket)
  -> EnterGameAccepted
```

协议源文件位于 `proto/realmmesh/`，构建时由固定版本的 `protoc` 生成 C++
代码，生成物只保存在 `build/`，不提交到仓库。字段编号与消息 ID 一经发布不得
复用；详细分层、编号和演进规则见 [docs/protocol.md](docs/protocol.md)。

监听地址、端口、下游地址和队列容量分别位于 `login.lua`、`realm.lua` 和 `gateway.lua`。当前账号认证器仅用于纵向链路开发，只接受凭据 `dev`；角色数据也是内存生成数据，不能用于生产环境。正式接入前需要替换为平台认证和存储服，并为登录与角色 TCP 链路增加 TLS。票据已经包含用途隔离、有效期、HMAC-SHA256 防篡改；`EnterGameTicket` 还会在网关单次消费以防重放。

## 参与贡献

欢迎通过 Issue 提交建议或问题，也欢迎提交 Pull Request。较大的功能改动建议先创建 Issue 讨论设计方案。

## License

许可证尚未确定。在正式添加开源许可证前，本项目默认保留所有权利。
