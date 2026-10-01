# 以 MongoDB 作为唯一权威玩家数据源

---
status: accepted
---

取代 ADR-0010。玩家数据源换成 MongoDB 副本集（开发与 CI 为单节点 `rs0`），由 Login Verifier、Gateway 与 Realm 经既有 `AccountStore` / `PlayerDataReader` 窄接口共享；SQLite 实现整体删除，不提供数据迁移工具。这比路线图把网络数据库放在多主机阶段之后更早，是有意偏离：项目要先以最终产品形态的数据库承载登录链，而不是在单主机原型上积累迁移成本。

## Considered Options

- **保留 SQLite，等多主机阶段再换**：被否。SQLite 文件无法跨主机共享，迟早要换；越晚换，依赖单文件语义的代码与数据越多。
- **MongoDB 与 Redis 同时接入，准入事实先写 Redis 再异步落 MongoDB**：被否。账号、凭据、封禁/白名单、角色归属与所选角色属于准入事实，写入频率低但不可丢失；write-behind 在 Redis 故障时会丢失已确认的封禁或角色归属，且让登录链读到两个可能不一致的事实来源。
- **登录链三处读取（`authenticate`、`login_facts`、`character`）前置 Redis 读缓存**：被否。它们正是需要看到最新封禁与归属的复核点，缓存会重新引入 ADR-0010 想消除的陈旧准入判断；关闭排队登录专项设计中"Redis 账号读缓存"这一开放问题。
- **MongoDB 单一权威源，准入事实同步写入**：采用。

## Consequences

- 准入事实以 `writeConcern: majority, j: true` 同步写入，读取使用 `readConcern: majority`；需要跨文档一致的写（角色开通、选择角色、Lua 首次导入）在事务内完成，因此部署必须是副本集，单机 `mongod` 也要以 `--replSet` 启动并完成初始化。
- 账号与角色编号在库中以 int64 存放，与领域内 uint64 按位一一对应（`std::bit_cast`），不收窄取值范围，Lua 派生出的大编号照常可用。
- `configs/common/accounts.lua` 仍只在库内尚未导入时导入一次：三个服务竞争同一个元数据文档，先插入者在事务内导入，后到者看到标记即跳过。
- 驱动为 mongo-c-driver 与 mongo-cxx-driver，以固定版本源码包 + SHA256 静态构建。服务器用本机原生二进制，不使用 Docker：macOS 开发机与 CI 用 Homebrew 的 `mongodb-community` 与 `mongosh`；Linux 用 `scripts/install-mongodb.sh` 安装固定版本 + 校验和到 `.tools/`。脚本与测试夹具先查 PATH，再退回 `.tools/`，所以两个平台的服务器小版本可以不同；代码只依赖副本集、事务与 majority 读写这些稳定语义。集成测试每个用例自启临时 `mongod`，数据目录在临时目录，不动 Homebrew 服务自己的数据目录。
- 手动开发联调连远程共享开发库：单节点 `rs0`，只监听服务器回环地址，强制 TLS 与 SCRAM 认证，开发机经 SSH 隧道接入；连接串经 `player_data.uri_environment` 指定的环境变量注入，凭据与 CA 只存在仓库外的私有配置中。开发机因此不必安装或运行 MongoDB 服务；只有跑自动化集成测试的机器需要 `mongod` 二进制，测试永远不连共享库。本机默认 URI（无认证、明文、只监听本机）保留给离线开发。
- Redis 推迟到阶段 4，作为玩法热状态（高频、可容忍 write-behind 窗口）的写入面，再异步落 MongoDB；它不承载准入事实，也不进入登录链读路径。排行榜、注册与角色列表/创建/选择不在本决定范围内。
