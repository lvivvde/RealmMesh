# 单主机阶段以 SQLite 作为权威玩家数据源

---
status: superseded by ADR-0011
---

当前 M1–M4 拓扑在一台主机上运行 Login Verifier、Gateway 与 Realm，但三者分别依赖 Lua 样例账号、固定延迟拉取桩和票据内未经复核的角色编号，无法证明同一账号事实贯穿登录链，也无法在进程重启后恢复角色数据。选择一个由三者通过窄接口共享的 SQLite 文件作为本阶段的权威玩家数据源；`configs/common/accounts.lua` 只在空库首次启动时原子导入，导入完成后不再参与认证。

## Considered Options

- **继续使用 Lua 账号表并另存角色文件**：被否。会形成两个事实来源，Gateway 无法可靠复核 Login Verifier 看到的账号状态。
- **立即引入独立 PostgreSQL/MySQL 服务**：暂不采用。当前验收是本机多进程与 Linux 单主机链路，额外数据库进程会显著扩大部署面；后续多主机/多写需求出现时可在现有接口后替换。
- **把玩家数据放入 etcd**：被否。etcd 承担服务发现、准入额度和小型协调状态，不适合作为账号口令与角色业务数据存储。
- **SQLite + WAL**：采用。它提供事务、崩溃恢复和跨进程已提交读，符合当前单主机边界且无需新增服务身份。

## Consequences

- `SqlitePlayerDataStore` 同时实现 `AccountStore` 与只读 `PlayerDataReader`；账号口令以 libsodium Argon2 哈希保存，数据库启用 WAL、外键、`synchronous=FULL` 与版本化 schema migration。
- Login Verifier 直接认证数据库；Gateway 通过有界异步 `SqliteAccountFetchPort` 读取最新账号准入状态及所选角色；Realm 在消费 EnterRealm 票据后重新确认角色仍属于该账号与 Realm。
- Gateway 查询不占用 I/O 帧循环，队列满时施加背压；SQLite busy timeout、管线既有重试上限与低基数结果指标提供超时、重试和观测边界。Login Verifier 数据错误返回明确的 503。
- 数据库路径、busy timeout、导入源与口令哈希成本由公共 `player_data` 配置提供。开发配置使用 `configs/data/player-data.sqlite`，该运行时目录不入库。
- 任一服务（Login Verifier、Gateway 或 Realm）打开空库时都在同一 `BEGIN IMMEDIATE` 事务内完成导入，先到者导入、后到者看到非空库即跳过；因此只启动 Gateway/Realm 的开发组合同样拿到账号。
- `credential_hash_cost` 默认 `interactive`（libsodium 交互档 Argon2id）。`minimum` 只给大批量机器人夹具使用，压测报告须注明。校验参数取自哈希串本身，改档只影响之后写入的口令。
- 这是单主机阶段的选择，不承诺多主机共享文件或多写扩展。未来替换为网络数据库时保留 `AccountStore` / `PlayerDataReader` 边界，并另作迁移 ADR。
