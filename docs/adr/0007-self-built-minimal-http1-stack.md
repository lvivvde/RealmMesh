# HTTPS 服务栈自研最小 HTTP/1.1,不引入第三方服务端库

---
status: accepted
---

登录健全服与排队调度服需要监听 HTTPS,而 framework/network 只有 QUIC 与 TLS/TCP(长度帧)两种传输。选择在 `framework/network/http/` 自研有界 HTTP/1.1 解析与最小服务端,复用现有 reactor(编译期 epoll/kqueue)与 TlsServerContext(TLS 1.3 + ALPN),零新依赖。洪峰负载形态(keep-alive 轮询长连、潜在数十万 QPS)排除线程池阻塞模型;现有 `TlsConnection` 直接接受外部 `SSL_CTX*`,而候选库都自建 SSL_CTX 且无法注入(cpp-httplib 经核实连 ContextSetupCallback 都不能接管),固定 endpoint 集的 HTTP/1.1 子集只是几百行 codec 工作,与 `LengthFieldCodec` 同构。

## Considered Options

- **Boost.Beast**:被否。引入 Asio 的第二套事件循环模型,与现有 reactor 双后端策略(ADR-0001)冲突。
- **h2o**:被否。自带事件循环的服务器优先设计,拖入 h2/h3。
- **libevent/evhttp**:被否。自管事件循环与现有 reactor 重叠,HTTP 层陈旧,净收益为负。
- **cpp-httplib**:部分保留。阻塞线程池模型不适合洪峰监听,但继续承担 etcd 客户端与 /metrics 等内部低 QPS 职责。

## Consequences

- 边缘 TLS 终结后回源明文/内网协议作为契约选项记录(边缘已验签 + 内网信任),不替代原生 HTTPS 监听能力。
- service_host 装配为加法式扩展(可选 HTTP listener,配置门控),不改 TransportFactory/IMessageTransport/QUIC 路径。
