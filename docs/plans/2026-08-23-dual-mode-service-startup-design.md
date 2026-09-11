# 双模式服务启动设计（拓扑驱动单一宿主）

- 日期:2026-08-23
- 状态:已与用户对齐设计,待实现
- 范围:apps/* 启动装配、配置体系、启动/关停编排

## 1. 背景与目标

当前三个服务二进制(realm_gateway / realm_character / realm_login)由
`scripts/dev-services.sh` 无序并发拉起,依赖靠 etcd 发现 + Lua 静态地址
兜底,存在启动窗口期客户端拿到"半成品"服务的问题。三个 main.cpp 的装配
逻辑(配置→Logger→Metrics→发现→Runtime→主循环→关停)高度重复。

目标:

1. **模式 1(测试/非分布式)**:`realm_mesh --config main.config` 单命令
   启动全部服务,所有服务就绪后才对客户端放行。
2. **模式 2(分布式)**:同一 `realm_mesh` 二进制,`--service <name>
   --instance-id <id> --node-id <id>` 等参数在每台机器上以不同身份启动
   单个服务;所有机器分发同一份二进制与同一套配置文件,差异全在启动参数。

非目标:

- 不改各服务 Runtime 的业务逻辑与传输实现。
- 不引入进程内内存传输(互连先走真实回环 socket)。
- 不做跨机关停编排(交给部署层,二进制只保证自身优雅退出)。
- 不支持 KCP。

## 2. 架构

### 2.1 ServiceHost 库(新 framework/service_host)

封装现三个 main 中重复的装配链:

```
配置加载 → Logger → MetricsServer → 发现注册/解析 → Runtime → 帧循环 → 优雅关停
```

- 每个服务实例一个 `ServiceHost`,持有该服务全部组件。
- 对外:`construct(config) → start() → ready() → run(frame) → stop()`,
  `ready()` = runtime 监听成功 + 服务发现注册成功(或发现未启用时跳过)。
- 就绪状态导出 Prometheus gauge `realmmesh_service_ready`。

### 2.2 realm_mesh 二进制(新 apps/mesh_host)

唯一分发产物。三个旧 main 改为调用 ServiceHost 的薄壳,迁移期保留,
E2E 通过后删除。

### 2.3 进程内互连

模式 1 下服务间互连走真实回环 socket(127.0.0.1:7000/7100,TLS 用现有
自签证书体系),复用现有传输栈,测试真实链路。将来集成测试若嫌 TLS
握手慢,再评估 in-memory transport(本设计不含)。

## 3. 配置体系

```
configs/
  common/logging.lua      ← 公共层:级别/队列/滚动策略(无实例字段)
  common/discovery.lua    ← 公共层:etcd endpoint/key_prefix/超时
  services/gateway.lua    ← 服务层:transports/tick_rate/容量/downstream/depends_on
  services/realm.lua
  services/login.lua
  main.config             ← 模式 1 拓扑:services = {"realm","login","gateway"}
```

- **加载与合并**:公共层深合并服务层,CLI 参数最后覆盖。
- **CLI 覆盖项**:`--service <name>`(限定只跑该服务→模式 2)、
  `--instance-id`、`--node-id`、`--zone`、`--config <path/dir>`。
- **实例字段推导**:file_path 内实例名、metrics 端口等由 host 按身份
  自动生成,配置文件不含机器差异。
- **模式 1 推导**:实例身份 `<service>-allinone-01`,downstream 地址
  自动取回环 + 依赖服务监听端口,main.config 不写地址。
- 格式沿用现有 Lua 加载器。

## 4. 启动顺序与就绪门禁(DAG)

- 服务在服务层配置声明 `depends_on`(如 login.lua:
  `depends_on = {"realm"}`);host 拓扑排序,检测到环则启动失败退出。
- **波次启动**:每轮启动所有"依赖已 ready"的服务;无依赖的服务第一波
  并行启动,彼此互不等待。无关联服务(如未来的好友服务)不阻塞也不被
  阻塞,总耗时 ≈ 最长依赖链。
- **放行门禁**:`entry = true` 的服务(现即 gateway)在图中**全部节点**
  ready 后才开启客户端监听。这就是"等全部服务启动后再放人"。
- **模式 2 就绪门禁**:gateway 实例等待 etcd 中出现 login/realm 注册
  (复用 ServiceResolver 轮询,超时可配)才开客户端口;发现未启用时退化
  为对 downstream 的 TCP 探活。

## 5. 关停

- **模式 1**:依赖反序(gateway → login → realm,无依赖者并行),每个
  服务:停帧循环 → 停收新连接 → 排空 → `flush(2s)`。单进程顺序析构。
- **模式 2**:SIGTERM 语义与现状一致(drain + flush + exit);跨机顺序
  交给部署层。

## 6. 错误处理

- 任一服务构造/启动失败:模式 1 整进程 fail-fast,已启动服务按关停序
  回收,错误经 logger 落盘(构造 logger 之前失败则 stderr);模式 2
  同现状 exit 1。
- 配置校验失败(缺依赖服务、depends_on 环、未知服务名)启动即报错,
  不进入部分启动状态。

## 7. 测试策略(TDD)

- **单元**:配置深合并优先级(公共<服务<CLI)、CLI 覆盖、拓扑解析
  (并行波次、环检测)、实例字段推导。
- **集成**:all-in-one 启动后 gateway 客户端口才可连;依赖未就绪时
  连接被拒/注册未完成;`realmmesh_service_ready` 指标随就绪翻转。
- **E2E smoke**:`realm_mesh --config main.config` 起全链路 → 健康检查
  → 干净关停(退出码 0,无残留进程)。

## 8. 迁移步骤(实现计划输入)

1. 抽 ServiceHost 库,三个旧 main 改薄壳(行为不变,现有测试全绿)。
2. 配置分层加载器 + CLI 覆盖(单元测试)。
3. DAG 波次启动 + 就绪门禁(集成测试)。
4. realm_mesh 入口 + main.config(模式 1 打通,E2E)。
5. 模式 2 参数化启动验证(etcd 双机模拟)。
6. 删除三个旧 main 与 dev-services.sh 的编排逻辑(保留 stop/status
   辅助功能按需)。
