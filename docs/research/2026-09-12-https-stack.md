# 调研：HTTPS 服务栈进 framework/network 的路线

- 日期：2026-09-12
- 票据：[lvivvde/RealmMesh#24](https://github.com/lvivvde/RealmMesh/issues/24)（地图 [#23](https://github.com/lvivvde/RealmMesh/issues/23)）
- 状态：已定稿（推荐结论见 [TL;DR](#tldr)）
- 方法：一手来源核查——本仓库源码/CMake/ADR 逐文件核实；vendored cpp-httplib v0.51.0 源码（`build/dev/_deps/cpp_httplib_source-src/httplib.h`，与 `third_party/httplib/CMakeLists.txt` 固定的 URL_HASH 同一份）；候选库官方 README/文档。所有引用见文末。

## TL;DR

**推荐自研路线：在 `framework/network/http/` 新增有界 HTTP/1.1 请求解析与最小 HTTP 服务边，复用现有 reactor（`IEventLoop`，编译期 epoll/kqueue，ADR-0001）与 `TlsServerContext`（TLS 1.3-only + ALPN）证书上下文；不引入任何新第三方库。** 已在树内的 cpp-httplib（MIT）保留现有职责（etcd 客户端、`/metrics` 端点），但不承担洪峰监听。对照项「边缘 TLS 终结 → 回源明文 HTTP/内网协议」**成立为契约选项**（私有网段 + 边缘验签前提），按 ADR-0003 只记契约、暂不实现。

关键理由：

1. **负载形状排除线程每连接模型，事件驱动是硬前提。** 排队调度服的轮询（间隔 ≥2s 起步自适应）配合 keep-alive，常驻连接数与在队人数同阶（全层可达 ~1M，单实例对标网关 5~10 万），峰值轮询 ~500k QPS。cpp-httplib 源码核实：每个已接受 socket 占住线程池一个 worker 直至连接关闭，池默认 `max(8, cores-1)`、上限 4x——万级以上常驻连接即不可行；改关 keep-alive 则每轮询一次握手一次，~500k TLS 握手/s CPU 不可承受。事件驱动 reactor（网关现役同一套）是唯一装得下该形状的模型，而自研解析天然长在它上面。
2. **TLS 1.3 证书上下文字面复用。** `TlsConnection` 构造直接接受 `SSL_CTX*`（`TlsServerContext::native_handle()`），`TlsServerContext` 已固化 min=max=TLS1_3_VERSION、禁重协商、零 early-data、严格 ALPN 选择；HTTP 监听复用 = 同一份 `TlsServerIdentity` 配置、ALPN 换 `http/1.1`。候选库全部自建 SSL_CTX（cpp-httplib 的 `tls::ContextSetupCallback` 只能配置它自己 `create_server_context()` 出的 ctx，且默认 min TLS 1.2）——复用只剩「读同样的证书文件」，TLS 策略出现第二份真相。
3. **有界解析成本可控，其余路线全被否。** 服务面固定（两服务的少量端点，JSON over POST），HTTP/1.1 子集（请求行 + 头 + Content-Length，拒收 chunked，严格尺寸限额）是与 `LengthFieldCodec` 同型的几百行 codec 工作，GTest 可全覆盖。引入 Beast/h2o/libevent/nghttp2 任一，要么带第二套事件循环模型、要么解错层、要么净收益为负（见问题 2），均与 ADR-0003 的精简树取向冲突。

---

## 背景与仓库事实（已核实）

- **传输层现状**：`IMessageTransport` 是轮询模型（`poll_once`/`send`/`close`），`TransportProtocol` 仅 `Quic|TlsTcp`。`TlsTcpTransport` = `TcpListener` + `IEventLoop`（编译期 epoll/kqueue，ADR-0001）+ `TlsConnection` 表。**长度帧解码在 `TlsConnection` 内部**（成员 `LengthFieldCodec codec_`，`receive_frames()` 输出解帧批次），`TransportEvent::MessageReceived` 的 payload 是已解帧负载——HTTP 字节流无法直接骑在现有帧通道上。
- **TLS 1.3 证书上下文**：`TlsServerContext`（配置体 `TransportConfig::TlsServerIdentity`：证书链 + 私钥 + ALPN）在 `SSL_CTX` 上固化 TLS 1.3-only、`SSL_OP_NO_RENEGOTIATION`、`max_early_data=0`、严格 ALPN（客户端不提供配置的 alpn 即 fatal alert）。ALPN 是配置字符串，HTTP 监听传 `http/1.1` 即可；注意严格选择意味着只提供 `h2` 的客户端会被拒（v1 契约可接受，后续按需放宽为列表）。
- **已有 HTTP 事实（重要先例）**：`third_party/httplib/CMakeLists.txt` 已 FetchContent 固定 cpp-httplib v0.51.0（MIT，`URL_HASH` 锁定；INTERFACE target `realm_httplib`，**未定义** `CPPHTTPLIB_OPENSSL_SUPPORT`，当前纯 HTTP）。两处在用：`framework/cluster/src/etcd_service_registry.cpp`（`httplib::Client` 访问 etcd）、`framework/observability/src/logger.cpp`（`httplib::Server` 挂 `/metrics`，独立 `jthread`）。即「内部/低 QPS HTTP 用 cpp-httplib」已是既成惯例；缺口只在洪峰监听。
- **装配层现状**：`ServiceHost` = 配置 → Logger（+ 可选 `LoggerMetricsServer`，port≠0 才建）→ `ServiceFrame` + `GatewayRuntime` → 可选 etcd 注册；`parse_service_identity` 只认 gateway/login/realm。ADR-0003 已确立「新服务实现时才登记 ServiceType」（2026-09-11 清理先例）；地图前提：新服务沿用 `realm_mesh --service` 单入口。
- **平台/依赖**：CI 仅 macOS + Linux（ADR-0002，无 Windows）；OpenSSL 3.0 是 `realm_network` 现有 PUBLIC 依赖——TLS 能力不新增依赖面。

## 问题 1：负载形状决定模型——线程池 vs 事件驱动

- **Login Verifier**：1M/10min ≈ 1.7k rps 峰值，请求短、连接短——任何模型都扛得住，不是区分项。
- **Queue Scheduler**：轮询 ≥2s 起步自适应 → ~500k QPS 潜力；洪峰期间在队客户端长期挂连接轮询，keep-alive 下常驻连接 ≈ 在队人数。
- **cpp-httplib（源码核实）**：`process_and_close_socket` 在 worker 线程上跑完整个 keep-alive 连接生命周期；池上限 `CPPHTTPLIB_THREAD_POOL_MAX_COUNT = 4x`。10 万常驻连接 → 10 万 worker，不可行。
- **结论**：洪峰监听必须事件驱动、与 reactor 同模型。这把可选范围收窄到「自研」（libevent 算半个，见下）。

## 问题 2：候选路线对比

| 路线 | 许可 | 事件模型 | TLS 1.3 上下文复用 | 结论 |
|---|---|---|---|---|
| **自研 HTTP/1.1 子集（framework/network/http/）** | 无新依赖 | 现有 `IEventLoop` reactor（epoll/kqueue 编译期，ADR-0001），零平台分支 | **字面复用**（`TlsConnection` 直收 `SSL_CTX*`；同一 identity 配置） | **推荐** |
| Boost.Beast | BSL-1.0 | 绑死 Boost.Asio 异步模型：`io_context` 是第二套事件循环子系统，与 `IEventLoop` 并存 = 双 reactor，或重写传输层上 asio | 自建（不接外部 ctx） | 否——第二套事件循环 + Boost 体量，违反 ADR-0003 |
| h2o | MIT | server-first（README："can also be used as a library"），自带 evloop 变体（`libh2o.pc.in` / `libh2o-evloop.pc.in` 两套链接面），嵌入 = 第二个循环 | 自建 | 否——循环模型不合，拖 HTTP/2/3 全套 v1 用不上 |
| libevent + evhttp | BSD | 自带事件循环，与 `IEventLoop` 职责重叠（手驱 `event_base` 或并存皆别扭）；evhttp 层老旧（官方 manual 自述 work-in-progress） | `bufferevent_openssl` 另行粘合，不接 ctx | 否——相对几百行自研解析净收益为负 |
| nghttp2 | MIT | 只做 h2 framing（README："The framing layer of HTTP/2 is implemented as a reusable C library"），无 HTTP/1.1、无 TLS、无 accept 循环 | 不涉及 | 否——解错层：用了它仍要写自研路线的全部代码 |
| cpp-httplib（已 vendored） | MIT | 阻塞线程池、worker 占满连接生命周期 | 自建 SSL_CTX（min TLS 1.2；`ContextSetupCallback` 无法注入外部 ctx） | 否（洪峰监听）——保留现有内部/低 QPS 职责 |

**对照项（边缘 TLS 终结 → 回源明文）**：成立为契约选项，不是 HTTPS 监听的替代品——见问题 4。

## 问题 3：自研的最小集成形状（推荐细则）

- **位置**：`framework/network/http/`（解析器 + 最小服务边），与 `codec/` 同型。**不动** `transport_factory` 与 `IMessageTransport`——HTTP 请求/响应不是会话-消息传输，硬塞进 `IMessageTransport` 属 ADR-0003 禁止的投机抽象。
- **复用 TLS 状态机**：`TlsConnection` 是唯一含 SSL accept/read/write 状态分类（`WantRead`/`WantWrite`/`Closed`/`Failed`）的类。推荐给它加一个解码缝（注入 receive 解码器或并列 stream 接收路径，`LengthFieldCodec` 行为为默认），HTTP 边组合 `TcpListener` + `IEventLoop` + `TlsConnection`（stream 模式）+ `TlsServerContext`；避免第二份 SSL 状态机。
- **解析范围（v1）**：请求行 + 头部 + Content-Length body；`Transfer-Encoding: chunked` 拒收（4xx；客户端与边缘行为由契约固定，将来需要时按需补）；严格尺寸限额（复用 `max_payload_size`）；keep-alive 按 RFC 9112 默认；400/413/431 错误响应。不做：h2（需要时边缘翻译 h2→h1.1，或届时再加 nghttp2）、压缩、websocket、multipart。
- **测试**：`tests/cpp/framework/network/http/` GTest，与 `length_field_codec_test` 同型，标签 `unit`。

## 问题 4：对照项——边缘 TLS 终结 → 回源明文，是否成立为契约选项

**成立，作为契约选项而非替代品：**

1. **前提自洽**：地图已定「边缘只定契约、不在本仓库实现」+ Identity Token（JWT/EdDSA）边缘可验签——回源明文的信任锚是「边缘已验签并转发声明」+ 私有网段隔离，不引入共享状态。
2. **边界**：它只覆盖有边缘的生产拓扑；dev/本地/无边缘部署仍需原生 HTTPS 监听（票据前提即「两服务都要监听 HTTPS」）。所以 **HTTPS 监听是本体，明文回源是拓扑契约选项**。
3. **ADR-0003**：现在只把选项写进契约文本（监听私网接口、要求边缘转发可验签声明）；`TransportConfig::validate()` 现强制 TlsTcp 必带 TLS identity，明文监听是一条新配置路径，等有真实拓扑要部署时再建。

## 对 service_host 装配层的影响面

- **加法、配置门控**：照 `LoggerMetricsServer` 先例（port≠0 才建），`ServiceHost` 增加可选 HTTP 监听组件与处理器注册；现有 gateway/login/realm 帧路径零改动。
- **服务身份**：实现时在 `parse_service_identity`/`ServiceType` 新增两服务身份（ADR-0003 先例），`realm_mesh --service` 单入口不变。
- **配置**：TLS identity 复用 `TlsServerIdentity`（证书链 + 私钥，ALPN=`http/1.1`）；`reload_credentials()` 语义随 `TlsServerContext` 现有机制走。
- **不触碰**：`TransportFactory`、`IMessageTransport`、QUIC 路径、ADR-0001 后端选择机制——HTTP 边经由 `IEventLoop` 消费平台差异，自身零平台分支，epoll/kqueue 双后端策略不受影响。

## 参考链接

- 仓库事实（逐文件核实）：`framework/network/include/realmmesh/network/transport/message_transport.hpp`、`tls/tls_connection.hpp`、`tls/tls_tcp_transport.hpp`、`tls/tls_server_context.hpp`（+ `src/tls/tls_server_context.cpp`）、`transport/transport_config.hpp`、`src/transport/transport_factory.cpp`、`src/reactor/event_loop_factory.cpp`、`framework/observability/src/logger.cpp`（`LoggerMetricsServer::Impl`）、`framework/cluster/src/etcd_service_registry.cpp`、`framework/service_host/src/service_host.cpp`、`third_party/httplib/CMakeLists.txt`、根 `CMakeLists.txt`、`docs/adr/0001`、`docs/adr/0003`、`CONTEXT.md`「Access」
- cpp-httplib v0.51.0 源码（vendored，与 `third_party/httplib/CMakeLists.txt` 的 URL_HASH 同份）：线程池默认与上限（`CPPHTTPLIB_THREAD_POOL_COUNT` / `_MAX_COUNT`）、`class SSLServer`、`SSLServer(const tls::ContextSetupCallback&)`（只配置自建 ctx）、`create_server_context()`（`TLS_server_method` + min TLS 1.2）、`LICENSE`（MIT）：https://github.com/yhirose/cpp-httplib/tree/v0.51.0
- Boost.Beast 官方 README（"HTTP and WebSocket built on Boost.Asio"、header-only、"Boost Software License, Version 1.0"）：https://github.com/boostorg/beast
- h2o 官方 README（"Written in C and licensed under the MIT License, it can also be used as a library"；HTTP/1.x/2/3；`libh2o.pc.in`/`libh2o-evloop.pc.in` 链接变体）：https://github.com/h2o/h2o
- libevent 官方仓库（event notification library；`epoll.c`/`kqueue.c`/`event_iocp.c` 后端文件、`bufferevent_openssl.c`；manual "work-in-progress"）：https://github.com/libevent/libevent
- nghttp2 官方 README（"The framing layer of HTTP/2 is implemented as a reusable C library"；HTTP/1.1 仅在 nghttpx/h2load 工具层；MIT）：https://github.com/nghttp2/nghttp2
