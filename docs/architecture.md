# RealmMesh 架构

本文档描述 RealmMesh 当前已经实现的结构，以及后续分布式服务规划。蓝色节点和
实线连接代表已有代码，灰色虚线节点和连接代表规划项。

## 服务拓扑

客户端依次连接登录服、角色服和网关服。前一个服务只向客户端签发短期票据并返回下一个服务的地址，不代理后续业务流量。

```mermaid
flowchart LR
    Client["游戏客户端"]

    subgraph Edge["当前接入链路"]
        Login["登录服<br/>realm_login :7000"]
        Realm["角色服<br/>realm_character :7100"]
        Gateway["网关服<br/>realm_gateway<br/>TCP :8000"]
        Auxiliary["辅助通道<br/>UDP :8001 / KCP :8002<br/>默认关闭"]
    end

    Client -->|"1. 账号认证"| Login
    Login -->|"LoginTicket + 角色服地址"| Client
    Client -->|"2. 选区与角色选择"| Realm
    Realm -->|"EnterGameTicket + 网关地址"| Client
    Client -->|"3. 建立正式会话"| Gateway
    Client -->|"鉴权后按需绑定（可选）"| Auxiliary
    Auxiliary --> Gateway

    subgraph Planned["规划中的分布式服务"]
        Coordinator["协调服 / 编排控制面"]
        Lobby["大厅服"]
        Scene["场景服"]
        Friend["朋友服"]
        Chat["聊天服"]
        Storage["存储服"]
    end

    subgraph Discovery["已实现：分布式服务发现"]
        Etcd["etcd v3<br/>Lease / KV / 前缀发现 / Watch"]
    end

    Gateway -.-> Lobby
    Gateway -.-> Scene
    Lobby -.-> Friend
    Lobby -.-> Chat
    Realm -.-> Storage
    Scene -.-> Storage

    Coordinator <-.-> Login
    Coordinator <-.-> Realm
    Coordinator <-.-> Gateway
    Coordinator <-.-> Lobby
    Coordinator <-.-> Scene
    Coordinator <-.-> Friend
    Coordinator <-.-> Chat
    Coordinator <-.-> Storage
    Coordinator -.-> Etcd

    Login <-->|"注册自身 / Watch 角色服"| Etcd
    Realm <-->|"注册自身 / Watch 网关服"| Etcd
    Gateway <-->|"注册自身 / Lease 续租"| Etcd

    classDef implemented fill:#e8f4ff,stroke:#1677ff,stroke-width:2px,color:#102a43;
    classDef planned fill:#fafafa,stroke:#8c8c8c,stroke-width:1.5px,stroke-dasharray:6 4,color:#595959;
    class Client,Login,Realm,Gateway,Auxiliary,Etcd implemented;
    class Coordinator,Lobby,Scene,Friend,Chat,Storage planned;
```

图中“已实现：分布式服务发现”区域及其三条实线就是当前已经落地的服务发现链路。
`EtcdServiceRegistry` 已实现带 Lease 的注册、自动续租、前缀发现和 Watch 缓存，
登录服、角色服与网关服会直接连接 etcd。灰色虚线的协调服仍属于规划中的编排
控制面，不作为基础服务发现的单点代理。

## 三阶段接入时序

```mermaid
sequenceDiagram
    autonumber
    actor Client as 游戏客户端
    participant Login as 登录服 :7000
    participant Realm as 角色服 :7100
    participant Gateway as 网关服 :8000

    Client->>Login: LoginRequest(account, credential)
    Login-->>Client: LoginSucceeded(LoginTicket, realm endpoint)
    Client->>Realm: RealmAuthenticate(LoginTicket)
    Realm-->>Client: CharacterList
    Client->>Realm: SelectCharacter(character_id)
    Realm-->>Client: EnterGameIssued(EnterGameTicket, gateway endpoint)
    Client->>Gateway: EnterGame(EnterGameTicket)
    Gateway-->>Client: EnterGameAccepted(account_id, character_id)
    Note over Client,Gateway: EnterGameTicket 在网关单次消费，防止重复进入
```

三个服务共享会话票据签名密钥，但客户端只能获得带用途和有效期的短期票据。正式公网部署还需要给登录服和角色服入口增加 TLS。

时序中的业务消息全部使用 Protocol Buffers，并由统一 `Envelope` 携带协议版本、
消息 ID 和请求 ID。TCP 的长度字段以及 UDP/KCP 的安全与可靠传输头位于
`Envelope` 外层，协议细节见 [protocol.md](protocol.md)。

## 网关线程与多协议模型

每个客户端对应一个逻辑 `ClientSession`。TCP 是主通道，UDP/KCP 只有在鉴权并绑定后才能加入同一会话；业务发送时可以逐包选择首选协议，通道不可用时按策略回退 TCP 或丢弃。

```mermaid
flowchart TB
    Client["客户端"]

    subgraph IO["I/O 线程：唯一的 socket 所有者"]
        TCP["TCP<br/>epoll + 长度字段分帧"]
        UDP["UDP<br/>单数据报单消息"]
        KCP["KCP<br/>可靠传输 + 加密会话"]
        Router["ClientSessionRouter<br/>通道绑定与协议回退"]
    end

    Inbound["有界入站队列<br/>GatewayEvent"]

    subgraph Frame["帧逻辑线程"]
        Drain["每帧批量消费"]
        Logic["鉴权 / 消息派发 / 游戏逻辑"]
        Scheduler["FrameScheduler"]
    end

    Outbound["有界出站队列<br/>发送 / 绑定 / 关闭命令"]

    Client <--> TCP
    Client <-.-> UDP
    Client <-.-> KCP
    TCP --> Router
    UDP --> Router
    KCP --> Router
    Router --> Inbound
    Inbound --> Drain
    Scheduler --> Drain
    Drain --> Logic
    Logic --> Outbound
    Outbound --> Router
```

队列满时不会无限增长：UDP 入站可以丢弃，TCP/KCP 入站过载会断开对应会话，TCP 单连接另有待发送字节高水位限制。

## C++ 与 Lua 分层

```mermaid
flowchart TB
    subgraph Apps["服务进程层"]
        LoginApp["realm_login"]
        RealmApp["realm_character"]
        GatewayApp["realm_gateway"]
    end

    subgraph Game["游戏服务层"]
        Common["game/common<br/>票据与边缘协议"]
        GatewayService["game/gateway<br/>会话、运行时与路由"]
    end

    subgraph Framework["C++ 框架层"]
        Network["network<br/>TCP / UDP / KCP"]
        Scheduler["scheduler<br/>帧驱动"]
        Concurrency["concurrency<br/>有界队列"]
        Service["service<br/>生命周期"]
        Cluster["cluster<br/>etcd 注册、续租与发现"]
        Scripting["scripting<br/>LuaRuntime"]
    end

    subgraph LuaLayer["轻量 Lua 层"]
        Config["lua/config<br/>启动配置"]
        Policy["lua/policy<br/>可热更规则（规划）"]
        Hotfix["lua/hotfix<br/>热修复脚本（规划）"]
    end

    subgraph ThirdParty["第三方依赖"]
        Lua["Lua 5.4"]
        Sol2["sol2 v3.3.1"]
        KcpLib["KCP"]
        Sodium["libsodium"]
        Etcd["etcd v3"]
    end

    Apps --> Game
    Game --> Framework
    Config --> Scripting
    Policy -.-> Scripting
    Hotfix -.-> Scripting
    Scripting --> Sol2 --> Lua
    Network --> KcpLib
    Network --> Sodium
    Cluster --> Etcd
    GatewayService --> Network
    GatewayService --> Scheduler
    GatewayService --> Concurrency

    classDef planned fill:#fafafa,stroke:#8c8c8c,stroke-dasharray:6 4,color:#595959;
    class Policy,Hotfix planned;
```

C++ 负责网络、并发、帧循环、服务治理和核心状态。Lua VM 由 `LuaRuntime` 持有并绑定到单个逻辑线程；业务脚本应在帧边界检查和切换，新版本加载失败时继续使用上一版本。
