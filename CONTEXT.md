# RealmMesh

分布式的游戏服务端框架:Realm / Gateway 服务经 etcd 服务发现组成拓扑,登录验证与排队服务经 HTTPS 边服务接入,玩家客户端经 Gateway 入场。

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
一个服务进程在 etcd 里的一次注册:身份(取自 `realm::cluster::ServiceType` 枚举,线名 gateway/realm/login_verify/queue)、instance_id 与端点列表,随租约存活。
_Avoid_: node(node_id 指承载它的机器)、endpoint(端点只是实例的字段)

**Service Registry**:
服务实例的注册、续约、注销与按身份发现的统一接口;生产实现落在 etcd。
_Avoid_: DNS、目录服务

**Service Publisher**:
把本进程的 Service Instance 注册进 Registry 并周期续约、随进程生命周期注销的组件。
_Avoid_: 心跳(续约的载体是租约,不是独立的心跳通道)

**Service Resolver**:
订阅某一身份的实例变化并给出当前可用端点的组件;连接方用它取得 Realm/Gateway 地址。
_Avoid_: load balancer(它只暴露端点,选择策略在连接层)

**Lease**:
借用 etcd v3 的租约语义:注册随 TTL 存活,停止续约即过期摘除,不引入额外的心跳通道。
_Avoid_: heartbeat、TTL(TTL 是租约的时长参数,不是租约本身)

**Watch**:
借用 etcd v3 的监视语义:某一身份的实例增删以事件形式到达 Resolver,消费方不感知获取机制——轮询差分、流式订阅都是后端实现细节。
_Avoid_: polling(轮询可以是底层实现,但不是这个概念的名字)、notification(这里专指服务实例事件,不是通用通知)

### Gateway

**Gateway Login Pipeline**:
网关接收 Edge Session 后,从凭据验讫、Fetching 到 Handoff 的唯一权威管线;统一拥有阶段迁移、准入额度、拉取结算与会话收尾的顺序,不得并行保留第二套状态或额度编排。它是网关内部的登录职责,不合并 Login Verifier、Queue Scheduler、网关与 Realm 的独立部署。
_Avoid_: Login Chain(客户端概念)、Gateway(部署身份不等于管线)、登录服务(这里不是独立部署单元)

**Edge Session**:
一条边缘客户端连接在边缘服务内的统一寻址:以不透明的 EdgeSessionId 从连接打开标识到关闭;凭据验讫由 pending 阶段迁入 fetching 阶段,账号数据拉取完成、直连凭证下发后进入终态 handed-off 阶段。网关只承载登录管线,不承载业务长连接。
_Avoid_: Client Session(旧名)、Pending Connection(旧名,pending 是阶段,不是另一种实体)、player(玩家是业务概念)、connection、handle、established(旧终态名:网关没有业务长连,终态是 handed-off)

**Fetching**:
Edge Session 的中间阶段:准入暂扣——凭据已验讫,正在限额拉取玩家账号数据,或拉取已完成但 Handoff 尚未被 Primary Transport 接纳;fetch 额度在拉取完成时即可归还,阶段只在 Handoff 被接纳后迁出。每次进出该阶段都是一次原子状态迁移。
_Avoid_: loading、provisioning、暂扣(是阶段名,不是动作)

**Handoff**:
网关把直连凭证与业务服端点下发给客户端、Edge Session 进入终态的动作;之后客户端直连业务服,网关不中转业务流量。
_Avoid_: bridge、proxy(网关不中转)、handover

**Primary Transport**:
Edge Session 当前唯一的传输通道;QUIC 与 TLS/TCP 是建连时的二选一候选,不是会话内的双通道。
_Avoid_: backup channel、secondary transport、failover

### Access

**Login Verifier**:
登录链路的第一站:无状态、可水平扩展,只回答账号是否有效(有效、封禁、白名单),通过后签发身份 Token。
_Avoid_: 登录服(登录由验票与排队两段承担)、鉴权服(鉴权在本仓库专指票据兑换)、auth server

**Identity Token**:
登录健全服签发的可验签凭据(JWT/EdDSA);边缘、排队调度服、网关各自本地验签,不引入共享状态。
_Avoid_: 裸用 token(须指明是身份 Token 还是排队号牌)、login ticket(旧机制名)、credential(那是登录请求里的口令字段)

**AccountStore**:
登录健全服背后的账号有效性数据源抽象:只回答「口令是否命中」,命中返回账号事实(id、封禁、白名单);v1 用 Lua 配置装载内存表,DB/Redis 真源以新实现替换,调用方零改动。
_Avoid_: 用户表(账号集不含注册/计费语义)、account service(它是数据源,不是服务)

**Queue Scheduler**:
发号、查号并按准入额度分批放行玩家进网关集群的服务;唯一权威状态是已放行号。
_Avoid_: 排队服(可作口头简称,文档用全称)、matchmaking(没有匹配语义)

**Queue Number**:
排队调度服签发的排位凭据;归属于一次身份 Token(`identity_jti`)所代表的登录尝试且不晚于该身份过期,客户端凭它轮询位次、断线找回,但不能凭它进入网关。
_Avoid_: 号(裸号值不防伪)、放行号牌、ticket(与 Session Ticket 冲突;线路由
`/v1/queue/tickets*` 沿用主 spec 已发布契约,不属命名)

**Admission Grant**:
排队调度服在 Queue Number 获准后的固定放行窗口内签发的短期准入凭据;绑定同一次登录尝试、来源 Queue Number 与 deployment,且不晚于身份过期,该登录尝试在同一 deployment 的网关集群中只能成功消费一次。它与可重复查询的 Queue Number 是不同阶段的凭据。
_Avoid_: admitted Queue Number、放行号牌、Gateway Ticket、Session Ticket

**Admission Controller**:
排队调度服内的速率阀门:按可用准入额度定时放一批号进网关集群。
_Avoid_: rate limiter(限流专指网关本地拉取的执行概念)、gate、valve

**Admission Budget**:
网关实例广播的剩余接纳能力(连接余量与拉取并发余量);准入控制器放批的依据。
_Avoid_: capacity(容量是规格层面的总量概念)、load(负载是原始观测,额度是上报值)

### Messaging

**Envelope**:
每条业务消息的传输无关封装(`common_v1::Envelope`),上限 64KiB。
_Avoid_: packet、frame(frame 指传输层的长度帧概念)

**Session Ticket**:
libsodium 签发的一次性准入凭据(`TicketPurpose`),在兑换点单次消费(重放防护)。唯一的活用途是 **EnterRealm**:网关在拉取完成后签发,客户端携带,Realm 兑换后直连入场(即直连凭证)。入场前置凭据为身份 Token + Admission Grant;网关入口的绑定凭据由集群单次消费。用途数值 1、2 已随旧链退役,永不复用。
_Avoid_: token、credential、cookie

### Client

**Login Chain**:
客户端从凭据到入场的七态驱动器:`verifying → queued → admitted → gateway_connecting → handoff_received → realm_connecting → in_game`;失败按规则回退(verify 失败回 idle、号牌过期自动重取、网关失败在放行宽限内重入、Realm 直连失败在 EnterRealm 票据窗口内重试),不自作主张重排队。
_Avoid_: 登录流程(流程指纸上步骤,链路是能跑出状态的实体)、session(那是服务端概念)

**Login Stage**:
登录链路的当前状态,取值为上述七态之一;`idle` 是链路的初始态与所有回退的落点。
_Avoid_: step(步骤是动作,阶段是状态)、phase(phasing 是服务端的拉取分期概念)

**Adaptive Polling**(分档轮询):
排队进度轮询的节奏策略:按位次分档(初始 2s、位次 >1000 用 5s、≤10 用 1s)、±20% 抖动、连续 3 次失败起指数退避至 30s 封顶,且不短于 progress 的缓存窗口。
_Avoid_: 定时轮询(丢掉了「随位次与失败自适应」的要义)、退避(退避只是失败分支,不是整套策略)

**Staged Race**(竞速建连):
同一段建连的两个传输候选错时起跑:QUIC 立即、TLS/TCP 延后 350ms,先成者胜出并取消另一路;连网关与直连业务服各自独立跑一次。
_Avoid_: 双通道(同一时刻只有一条通道存活,没有并行双路)、failover(不是故障切换,是建连竞赛)

**EnterRealm Redeemer**(兑换口):
Realm 段在已竞速建连的流上兑换 EnterRealm 票据的注入点;生产实现 `WireEnterRealmRedeemer` 发 1304 `EnterRealm`、等 1305 `EnterRealmAccepted`,被拒、坏帧与超时统一归 `EnterRealmRejected`(不新增分型)。
_Avoid_: realm login(兑换是凭据消费,不是再登录一次)、handoff(handoff 指网关下发凭据与端点)

### Testing

**Unit Test**:
进程内验证单个组件行为的 GTest 用例:不拉子进程、不占固定端口,CTest 标签 `unit`,构成快速子集(`ctest -L unit`)。
_Avoid_: 快速子集(那是运行时选择的名字,不是测试类别)

**Integration Test**:
驱动真实二进制、真实端口或跨进程协作的测试(e2e、bash 脚本驱动、RUN_SERIAL 性质);CTest 标签 `integration`,不进快速子集,全量与 CI 覆盖。
_Avoid_: e2e(e2e 只是其中驱动完整二进制拓扑的形态,不是整类的别名)

**Lua 业务模块测试**:
`realm_add_lua_test` 注册的 Lua 套件,类别同 Unit Test;在 `unit` 之外另带 CTest 标签 `lua`,只用于 `ctest -L lua` 单筛。
_Avoid_: 第三类测试(lua 是筛选用标签,不与 unit / integration 并列)
