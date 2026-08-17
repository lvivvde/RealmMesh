# RealmMesh 协议

RealmMesh 的业务消息统一使用 Protocol Buffers。当前固定使用 Protobuf v35.0，
Linux x86-64 构建使用官方预编译 `protoc`，服务器只链接
`libprotobuf-lite`。生成的 C++ 文件位于 `build/dev/proto/generated/`，不进入源码仓库。

## 分层

```text
TCP: [4-byte big-endian length][Protobuf Envelope]
UDP: [security/session header][Protobuf Envelope]
KCP: [security/session header][KCP segments carrying Protobuf Envelope]
```

TCP 长度字段、UDP/KCP 会话与加密头属于传输层，不放进 Protobuf。所有传输协议
共享同一份业务 `Envelope`，因此服务器按包选择 TCP、UDP 或 KCP 时不需要转换业务
消息。

`realmmesh/common/v1/envelope.proto` 定义四个字段：

| 字段 | 用途 |
|---|---|
| `protocol_version` | 当前为 `1`；不支持的版本会被拒绝 |
| `message_id` | 标识 `payload` 的具体消息类型 |
| `request_id` | 客户端请求与服务器响应的关联 ID；通知可使用 `0` |
| `payload` | 具体业务消息序列化后的字节串 |

## 边缘消息

`realmmesh/edge/v1/edge.proto` 定义当前登录、角色选择和进服消息：

| 消息 ID | 方向 | 消息 |
|---:|---|---|
| 1001 | C2S | `LoginRequest` |
| 1002 | S2C | `LoginSucceeded` |
| 1101 | C2S | `RealmAuthenticate` |
| 1102 | S2C | `CharacterList` |
| 1103 | C2S | `SelectCharacter` |
| 1104 | S2C | `EnterGameIssued` |
| 1201 | C2S | `EnterGame` |
| 1202 | S2C | `EnterGameAccepted` |
| 1999 | S2C | `EdgeError` |

服务器响应会保留请求的 `request_id`。业务入口会同时校验 Envelope 版本、已知消息
ID 和具体 payload 类型；旧的单字节操作码格式不再兼容。

## 演进规则

- 已发布字段的编号和类型不得修改或复用。
- 删除字段时必须使用 `reserved` 保留原编号和名称。
- 消息 ID 永久唯一；删除消息后保留其编号，不分配给其他消息。
- 向后兼容的新增字段使用新编号，接收端必须接受未知字段。
- 破坏兼容性的改动放入新的包版本，例如 `edge.v2`，并提升 Envelope 协议版本。

这些规则遵循 Protobuf 的
[Proto3 更新消息类型约束](https://protobuf.dev/programming-guides/proto3/#updating)。

## 构建

```bash
./scripts/build.sh
```
