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

### Cluster

**Service Instance**:
一个服务进程在 etcd 里的一次注册:身份(取自 `realm::cluster::ServiceType` 枚举,线名 gateway/login/realm)、instance_id 与端点列表,随租约存活。
_Avoid_: node(node_id 指承载它的机器)、endpoint(端点只是实例的字段)

**Service Registry**:
服务实例的注册、续约、注销与按身份发现的统一接口;生产实现落在 etcd。
_Avoid_: DNS、目录服务

**Service Publisher**:
把本进程的 Service Instance 注册进 Registry 并周期续约、随进程生命周期注销的组件。
_Avoid_: 心跳(续约的载体是租约,不是独立的心跳通道)

**Service Resolver**:
订阅某一身份的实例变化并给出当前可用端点的组件;连接方用它取得 Login/Realm/Gateway 地址。
_Avoid_: load balancer(它只暴露端点,选择策略在连接层)

**Lease**:
借用 etcd v3 的租约语义:注册随 TTL 存活,停止续约即过期摘除,不引入额外的心跳通道。
_Avoid_: heartbeat、TTL(TTL 是租约的时长参数,不是租约本身)

**Watch**:
借用 etcd v3 的监视语义:实例的增删以事件流推送,Resolver 由此感知拓扑变化,而非轮询。
_Avoid_: polling、notification(这里专指服务实例事件,不是通用通知)

### Gateway

**Pending Connection**:
已完成 TLS 1.3 握手、但 EnterGame 票据尚未单次消费成功的连接;此时不存在 Client Session。
_Avoid_: pending session(未晋升,不是会话)、unauthenticated session

**Client Session**:
票据单次消费成功后由 Pending Connection 晋升而来的逻辑会话,以 gateway 内自增的 ClientSessionId 寻址(与传输层 SessionId 是两个 id 空间);恰有一个 Primary Transport,断开即终结,不透明迁移。
_Avoid_: player(玩家是业务概念)、user、connection

**Primary Transport**:
Client Session 当前唯一的传输通道;QUIC 与 TLS/TCP 是建连时的二选一候选,不是会话内的双通道。
_Avoid_: backup channel、secondary transport、failover

### Messaging

**Envelope**:
每条业务消息的传输无关封装(`common_v1::Envelope`),上限 64KiB。
_Avoid_: packet、frame(frame 指传输层的长度帧概念)

**Session Ticket**:
libsodium 签发的一次性准入凭据,用途分 Login 与 EnterGame(`TicketPurpose`);用途为 EnterGame 的票据在 Gateway 单次消费(重放防护),消费成功触发 Pending Connection 到 Client Session 的晋升——流程文档中的 `EnterGameTicket` 即指此用途的票据。
_Avoid_: token、credential、cookie
