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
借用 etcd v3 的监视语义:某一身份的实例增删以事件形式到达 Resolver,消费方不感知获取机制——轮询差分、流式订阅都是后端实现细节。
_Avoid_: polling(轮询可以是底层实现,但不是这个概念的名字)、notification(这里专指服务实例事件,不是通用通知)

### Gateway

**Edge Session**:
一条边缘客户端连接在边缘服务内的统一寻址:以不透明的 EdgeSessionId 从连接打开标识到关闭;票据消费成功即由 pending 阶段迁入 established 阶段。
_Avoid_: Client Session(旧名)、Pending Connection(旧名,pending 是阶段,不是另一种实体)、player(玩家是业务概念)、connection、handle

**Primary Transport**:
Edge Session 当前唯一的传输通道;QUIC 与 TLS/TCP 是建连时的二选一候选,不是会话内的双通道。
_Avoid_: backup channel、secondary transport、failover

### Messaging

**Envelope**:
每条业务消息的传输无关封装(`common_v1::Envelope`),上限 64KiB。
_Avoid_: packet、frame(frame 指传输层的长度帧概念)

**Session Ticket**:
libsodium 签发的一次性准入凭据,用途分 Login 与 EnterGame(`TicketPurpose`);用途为 EnterGame 的票据在 Gateway 单次消费(重放防护),消费成功触发 Edge Session 由 pending 阶段迁入 established 阶段——流程文档中的 `EnterGameTicket` 即指此用途的票据。
_Avoid_: token、credential、cookie

### Testing

**Unit Test**:
进程内验证单个组件行为的 GTest 用例:不拉子进程、不占固定端口,CTest 标签 `unit`,构成快速子集(`ctest -L unit`)。
_Avoid_: 快速子集(那是运行时选择的名字,不是测试类别)

**Integration Test**:
驱动真实二进制、真实端口或跨进程协作的测试(e2e、bash 脚本驱动、RUN_SERIAL 性质);CTest 标签 `integration`,不进快速子集,全量与 CI 覆盖。
_Avoid_: e2e(e2e 只是其中驱动完整二进制拓扑的形态,不是整类的别名)
