# RealmMesh

RealmMesh 是一个 C++20 帧驱动游戏服务器。当前公网接入统一使用标准加密传输：

- Gateway：QUIC + TLS 1.3 主通道，同端口 TLS 1.3/TCP 兜底。
- Login、Realm：TLS 1.3/TCP。
- 业务消息：4 字节大端长度 + Protocol Buffers `Envelope`。
- 不支持裸 TCP、裸 UDP、KCP、自研传输加密或逐消息协议回退。

详细结构见 [架构文档](docs/architecture.md)，线上消息约定见
[协议文档](docs/protocol.md)。已实施的集中日志基线和生产演进边界见
[分布式日志架构](docs/logging-architecture.md)。
本地独立日志服务的启动说明见
[observability development stack](deploy/observability/README.md)。

## 安全传输约束

QUIC 与 TLS/TCP 使用相同证书、SNI 和 ALPN `realmmesh-edge/1`。两条路径都只接受
TLS 1.3；首版关闭 0-RTT、会话恢复、QUIC Datagram 与 keepalive。QUIC 每条连接只允许
一个客户端发起的长期双向流，允许连接地址迁移。

客户端建连策略由 `PreferredTransportConnector` 表达：QUIC 在 0ms 开始，TLS/TCP 在
350ms 后参与竞速，单次握手上限 3 秒、整轮上限 5 秒，第一个安全握手成功者胜出。
只有 `Unsupported`、`NetworkUnreachable`、`HandshakeTimeout` 可以触发降级；证书、
ALPN、鉴权或协议错误不会。网络不可达结果按当前网络缓存 5 分钟，网络切换时清除。
业务层只能在胜出的连接上发送一次性 `EnterGameTicket`。

当前仓库包含可移植的竞速策略与错误分类参考实现，首个客户端集成目标为 Windows；
生产客户端 SDK 不在本阶段范围内。

## 构建

服务器支持 Linux，要求 CMake 3.20+、C++20 编译器、OpenSSL 3 开发包和 MsQuic 2.x。
Ubuntu 24.04 开发环境可执行：

```bash
sudo apt install libssl-dev libxdp1 libnl-3-200 libnl-route-3-200 libnuma1
./scripts/install-msquic-dev.sh
./scripts/build.sh
```

MsQuic 开发安装脚本固定使用 Microsoft 官方 `libmsquic 2.5.10` 包和对应头文件，
下载内容均校验 SHA-256。也可自行安装 MsQuic，并通过 `MSQUIC_ROOT` 指向其前缀。

## 开发证书

私钥不得提交仓库。下面示例生成仅用于本机的短期证书；生产环境应由受信 CA 签发，
客户端必须进行 DNS 名称和证书链校验。

```bash
mkdir -p .local/tls
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout .local/tls/private-key.pem \
  -out .local/tls/certificate.pem \
  -days 7 -subj /CN=localhost \
  -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1'

export REALMMESH_TLS_CERTIFICATE_FILE="$PWD/.local/tls/certificate.pem"
export REALMMESH_TLS_PRIVATE_KEY_FILE="$PWD/.local/tls/private-key.pem"
export REALMMESH_SESSION_TICKET_KEY="$(openssl rand -hex 32)"
```

服务收到 `SIGHUP` 后会重新读取证书和私钥；新身份只用于新连接，已有连接继续使用原
TLS 上下文。加载失败时保留上一份可用身份。

## 运行三段服务

开发环境可使用管理脚本一键启动或重启全部服务：

```bash
./scripts/dev-services.sh          # 默认 restart
./scripts/dev-services.sh status
./scripts/dev-services.sh stop
```

脚本默认使用 `.runtime/tls/` 中的开发证书，将 PID 写入 `.runtime/pids/`，并把控制台
输出追加到 `.runtime/logs/<service>/console.log`。首次运行前需要完成构建和开发证书生成。

也可以手动启动：

```bash
./build/dev/bin/realm_mesh --config configs
```

默认入口：

| 服务 | 端口 | 传输 |
|---|---:|---|
| Login | 7000 | TLS/TCP |
| Realm | 7100 | TLS/TCP |
| Gateway | 8000 | QUIC 优先，TLS/TCP 兜底 |

Gateway 会把两个候选端点一并下发，候选项包含 `protocol/address/port/priority`。QUIC
与 TLS/TCP 使用相同主机名和数字端口（分别占用 UDP 与 TCP 端口空间）。

## 配置

启动配置位于 `configs/`。安全传输示例：

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

服务通过 etcd v3 Lease 发布端点并 Watch 下游服务。`required = false` 时 etcd 不可用
会使用 Lua 固定下游地址并持续重试；生产环境可设为 `true`。

## 会话与线程模型

安全握手完成后连接先进入 pending 状态。只有首条业务鉴权成功，运行时才以一个原子
命令发送响应并把它晋升为 `ClientSession`。每个逻辑会话只有一个 primary transport，
不存在辅助通道或逐包回退。已建立 QUIC 连接中断后需要重新建连和鉴权，本阶段不提供
透明 QUIC→TCP 热切换。

MsQuic 回调、TLS/epoll 事件都被适配为统一 `GatewayEvent`，再进入有界入站队列；
帧线程只消费事件并提交有界出站命令。过载时可靠连接会被关闭，不允许无界增长。

## 测试

```bash
ctest --preset dev
```

测试覆盖真实 TLS 1.3/ALPN 往返、真实 MsQuic 往返、无 ALPN 不创建业务连接、QUIC
竞速与安全降级分类、IPv6 双栈、端点序列化、pending→session 原子晋升，以及完整的
Login→Realm→Gateway TLS 链路。测试证书和私钥只生成在 `build/` 中。

## License

许可证尚未确定；正式添加许可证前默认保留所有权利。
