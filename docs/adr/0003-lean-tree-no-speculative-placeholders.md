# 目录树只反映已实现代码,不为规划中的服务留空目录

---
status: accepted
---

仓库曾长期保留 23 个只含 `.gitkeep` 的空目录:6 个服务身份在 `apps/` 与 `game/` 下各占一份、5 个预留 framework 模块、`lua/`、`tests/integration`、`tests/lua`,以及已被 `configs/` 取代的 `config/`。这些目录无一被任何 `CMakeLists.txt` 引用,唯一作用是提示"将来要建什么"。问题在于读者会把目录的存在误读为能力的存在:23 个空目录占了可见目录的四成,却没有任何代码支撑,反而掩盖了已实现的真实边界。因此决定全部删除,把规划意图移入 `docs/architecture.md` 的"未实现的服务与模块"一节;新增服务或模块时按需建目录,不再预先占位。

## Considered Options

- **保留空目录作为骨架**:被否。空目录只能表达"曾经想过",无法表达优先级、依赖或是否仍然要建;它把"目录存在"与"能力存在"混为一谈,是本次"散乱"观感的主要来源。
- **空目录内改放一句话 README**:被否。仍然是 23 个只承载"将来"的目录,只是把 `.gitkeep` 换成了另一种占位符,噪音量与误解风险都不变。
- **删除 + 文档**:采用。服务身份的权威列表本就在代码中(`realm::cluster::ServiceType` 枚举与 etcd 线名映射),文档只需指向它,不重复维护第二份列表。

## Consequences

- 空目录不再"提醒"下一步要建什么;规划的入口收敛到 `docs/architecture.md` 一处,改规划必须改文档。
- 新增服务 = 新建目录 + `CMakeLists.txt` + 根 `CMakeLists.txt` 一行 `add_subdirectory`,没有现成骨架可填。
- 5 个 framework 预留模块(`base`/`memory`/`rpc`/`serialization`/`storage`)在代码与文档中都没有任何职责定义,因此按"有代码才建"处理,不为它们补写推测性说明。
- 被删目录在 git 历史中仍可恢复,但恢复与否都不影响构建——没有任何构建脚本引用这些路径。
- 同一原则延伸到代码:2026-09-11 从 `ServiceType` 枚举与 etcd 线名映射中移除了 6 个未实现的
  身份(`Coordinator`/`Lobby`/`Scene`/`Friend`/`Chat`/`Storage`),新服务在实现时才登记身份;
  实例类型在线上是字符串编码,删除不影响既有 etcd 数据的解码。
