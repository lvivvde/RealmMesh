# 玩家数据事务重试验证（#186）

日期：2026-11-15。修复基于 PR #192 的 `08887f2`，只补充事务重试与故障验收，不改写其前置平台迁移。

## 修复范围

五处跨文档写统一通过私有事务执行器：角色创建、Scene 实例归属、分片归属、带分片栅栏的场景写、Lua 首次导入。
事务体和提交共享 `transaction_timeout_ms`（默认 5000）操作间预算，使用单调时钟；事务体的瞬态错误可以重跑，
未知提交结果仅重试同一事务的提交。所有重试均检查预算，并保留驱动一次提交网络重试及 majority/journal 语义。

预算到期不证明回滚。Realm 返回既有 `3010` 后，客户端先查询已提交角色；训练保留原序号，Scene 保留原命令与版本。
原测试对共享 MongoDB 全局日志级别的修改已删除，故障测试全部使用独占副本集。

## 证据与复现

修复前已运行 `ctest --preset dev -R '^PlayerDataTransactionTest.BoundsPersistentCommitFailure$' --output-on-failure`：
持续注入提交确认失败，3 秒内 future 未就绪；解除故障后旧实现返回成功，两个断言失败。
同一测试在修复后通过。回归还覆盖提交断网、事务体持续冲突、一次冲突后成功，以及真实 Realm RPC 的 `3010` 与回读恢复。

提交前断网可能留下尚未结束的服务端事务；网络恢复测试仅在独占副本集把 `transactionLifetimeLimitSeconds` 设为 1，
验证过期清理后的重试，不要求客户端超时即释放服务端事务锁，也不修改生产配置。

定向入口：

```sh
ctest --preset dev -R 'PlayerDataTransactionTest|PlayerDataRetryFailureTest|RealmTransactionFailureTest' --output-on-failure
./scripts/test-fast.sh
./scripts/build.sh
```

## 验收边界

本变更消除驱动固定 120 秒的事务重试循环，**不提供整个玩家请求的 5000 毫秒硬截止**。
预算从事务执行开始，不涵盖连接池获取、会话准备、schema 初始化和账号哈希；在途同步调用与 abort 清理仍需等待底层超时。
因此 #186 关于整个请求完整耗时上界的验收仍保留，不以本变更直接关闭 issue。

## 2026-11-15 验证结果

| 入口 | 平台 | 结果 | CTest 用时 |
| --- | --- | --- | --- |
| `./scripts/test-fast.sh --jobs 6` | macOS ARM64 | 567/567 通过 | 3.97 秒 |
| `./scripts/build.sh --jobs 6` | macOS ARM64 | 833/833 通过，退出码 0 | 763.11 秒 |
| `./scripts/build.sh --jobs 3` | Linux ARM64 / Lima ubuntu | 834/834 通过，退出码 0 | 668.67 秒 |

两份完整 configure 输出均包含 `realm_network: QUIC transport enabled (MsQuic found)`。
Linux 与 macOS 的受测补丁（`git diff --binary`）SHA-256 一致：
`43a15f27502e9a6666f7cd1ce658fae55a19eb056cd352612407c6db2a0a8057`。
新建的本验证记录不在上述受测补丁摘要中。编辑循环的定向故障测试已通过；中断运行不计入完整通过结果。

完整日志本地保存在 `build/validation-186/`，摘要如下：


- `macos-full-final.log`：`f9cc77ecac951cc6b0f9d3e9ebd62ea30ab34b393b8b597da9fa4fdc70ecefee`。

- `linux-full-final.log`：`bdc113e5243c0e5b730fe3d8fdcc77d1869703e8208fdae475c32bee367883f4`。

## 2026-11-16：连接池等待上限

继续收窄 #186 的资源等待边界：新增 `pool_wait_timeout_ms`（默认 2000），映射到驱动原生
`waitQueueTimeoutMS`。池耗尽时不再无限等待；超时抛出既有 `PlayerDataError`，不留下后台任务。
同名 URI 参数（包括大小写变体与 0）被拒绝，配置值必须为正的 32 位毫秒数。

真实副本集回归把池设为一个客户端，用有限的 `failCommand` 阻塞读取并经 `waitForFailPoint`
确认占用，再发出第二个读取。修复前在 1 秒内未就绪，连接释放后第二个读取继续成功，两个断言失败；
修复后第二个读取按 150 毫秒池预算报错，持有者归还连接后下一次读取成功。
定向配置、拒绝覆盖与耗尽恢复测试 5/5 通过（含 CTest 副本集准备/回收）。

此上限与事务预算独立，仍不构成在途同步操作的硬取消；完整 RPC 截止验收继续保留。

| 入口 | 平台 | 结果 | CTest 用时 |
| --- | --- | --- | --- |
| `./scripts/test-fast.sh --jobs 6` | macOS ARM64 | 567/567 通过 | 3.38 秒 |
| `./scripts/build.sh --jobs 6` | macOS ARM64 | 835/835 通过，退出码 0 | 768.26 秒 |
| `./scripts/build.sh --jobs 3` | Linux ARM64 / Lima ubuntu | 836/836 通过，退出码 0 | 668.31 秒 |

两平台 configure 均确认启用 QUIC。逐文件核对相对 `08887f2` 的 12 个改动文件，
受测内容一致（本节最终验证结果在测试后补入）。两项只读复审未发现新增问题。
日志保存在本地 `build/validation-186/`，SHA-256 如下：


- `pool-red.log`：`41328d4f0c6a845bed931f3db8f29c3313ec45012d19445ca32378de7c123b82`。

- `pool-green.log`：`6785282c503129ad66f57badfe1a11afc79e335847d0e1ba992e8e4ba2b72098`。

- `pool-macos-full.log`：`f1f72a25a6f109e8d9d7cb3248686db78258e2013c72c84561186ae5f5cafa17`。

- `pool-linux-full.log`：`997fc5b5f14d5a25250fc53b746963e62fe30a5e6e855e95b5726c25fc556d1d`。
