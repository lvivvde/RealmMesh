# RealmMesh

分布式的游戏服务端框架:Realm / Login / Gateway 服务经 etcd 服务发现组成拓扑,玩家客户端经 Gateway 入场。

## Language

### Network

**Event Loop**:
等待并分发 socket 就绪事件的 reactor;平台差异(epoll、kqueue)封装在后端实现中,业务代码不感知后端。
_Avoid_: epoll(仅指 Linux 后端实现时才用)、reactor、selector

**Transport**:
Session 之间收发消息的统一抽象;具体形态是 QUIC 或 TLS/TCP,对消费者不可见。
_Avoid_: connection、socket、channel

**Session**:
一条已建立的端到端连接,以不透明的 SessionId 寻址;上层永远接触不到底层 fd。
_Avoid_: connection、fd、handle

**Backend**:
Event Loop 在单一平台上的实现(Linux 为 epoll,macOS 为 kqueue),编译期选定,同一时刻源码树中只激活一个。
_Avoid_: driver、provider
