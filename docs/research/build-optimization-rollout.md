# 构建优化：实施顺序与三类耗时验收标准

关联[锁定优化实施顺序与三类耗时验收标准](https://github.com/lvivvde/RealmMesh/issues/115)。用户已通过两轮 Q1–Q12 全部采纳推荐答案，并最终确认完整方案。本文是后续实施的顺序、测量协议、门槛与退出约定，未实施优化代码，也不承诺任何未测提速。

前置决策（实施时一并读取）：

- [双平台构建与验证基线](https://github.com/lvivvde/RealmMesh/blob/ca6512e53030dee176a0228c38accbfc459ee5fc/docs/research/build-baseline-2026-10-02.md)
- [构建工具与缓存研究笔记](https://github.com/lvivvde/RealmMesh/blob/1660cee1b89d380a28749adb6439ca9ac62263e0/docs/research/build-acceleration-options.md)
- [构建工具、并行缓存与依赖复用策略](https://github.com/lvivvde/RealmMesh/blob/a6999c41ce2c5c6cb20f65490bfeda642251346a/docs/research/build-tool-cache-decisions.md)
- [第三方头文件与配置状态隔离](https://github.com/lvivvde/RealmMesh/blob/d6d7df19da7e2a2edb8dcddc662f6c88ef80a3fc/docs/research/dependency-isolation-decisions.md)
- [日常反馈与完整验证入口](https://github.com/lvivvde/RealmMesh/blob/bfee94185cc842b3d1bee95c8133bedec7a571eb/docs/research/build-test-entry-decisions.md)
- [公共接口头与内部实现首批拆分](https://github.com/lvivvde/RealmMesh/blob/223add13bb49fb50a6fb517c058c055208851481/docs/research/interface-boundaries-proposal.md)

## 新增事实

最近三次 main 绿色 CI（运行 37011005130、36979742757、36892395721）：`cmake --build --preset dev` 未传并行参数，即串行构建。Linux job Build 648–816 秒、Test 273–301 秒、M1–M4 接入验收约 265 秒，总 21–24 分钟；macOS job Build 684–797 秒、Test 313–342 秒，总 18–21 分钟。CI 构建步骤是三类耗时中绝对值最大的单项。

基线 Linux 两项失败中，HTTP half-close 已有修复提交 14c19d0；RealmJourney 仅能推测与 TLS 修复 433d6ba 相关。两项均未在当前 HEAD 用 Linux 完整测试复验。

## 阶段顺序

| 阶段 | 内容 | 类别 | PR |
| --- | --- | --- | --- |
| P0 | 测量工具入仓；当前 HEAD 双平台重测基线并达到全绿；补代表探针 | 测量 | 1 |
| P1 | 第三方头来源与配置状态隔离 | 低风险构建配置 | 1 |
| P2a | 预设、构建目录信息、统一 `--preset` 与目录消费者迁移 | 低风险构建配置 | 1 |
| P2b | Ninja 默认、保留 dev-make 回退、`--jobs` 资源预算、CI 显式 2 路 | 低风险构建配置 | 1 |
| P3 | ccache：本机 AUTO/ON/OFF，CI 显式启用与缓存 | 低风险构建配置 | 1 |
| P4 | `test-fast.sh`、Unit 聚合目标与身份标签、watch 改造 | 低风险入口 | 1 |
| P5 | 接口首批拆分：配置 Lua 解析分离；TrainingRule 前置声明与拓扑装载下沉；Gateway 轻量事件与启动配置 | 运行时影响 | 3 |
| P6 | 全量复测与公共头扰动扫描，按毕业规则决定新票 | 测量与决策 | 报告 |

依赖：P0 → P1 → P2a → P2b → P3；P4 只依赖 P2a，可与 P2b/P3 并行；P5 三个 PR 均须在 P3 与 P4 合入后开始，使复测只有接口一个变量，各 PR 合入后分别复测；P6 在 P5 全部合入后进行。

理由：隔离是缓存与生成器比较的前提，不能用缓存掩盖配置污染；预设与目录信息是 Ninja、入口与 watch 共同底座；生成器切换与目录迁移分开，Ninja 不达标时只回退切换。P5 按模块拆，单独复测、单独回退。全部阶段经 PR 与 Linux CI，squash 合并，一个阶段 PR 的回退是一个 revert。

## 测量协议

- **工具**：基线 `launcher.cpp` 与 `measure.py` 在 P0 入仓，置于 `tools/build-bench/` 并附使用说明；此后每阶段用同一工具。它是测量工具，不属于日常入口“不新增 Python”约束的范围。
- **探针**：沿用 `lua_runtime.cpp` / `lua_runtime.hpp`，P0 补 `gateway_runtime.hpp`、`player_data_store.hpp` 与一个 `.proto` 修改。
- **样本**：无操作、单 .cpp、公共头、`.proto`、Unit 与冷构建各 3 次，报告中位数与最小–最大值；完整 CTest 本地 1 次，并以同提交 CI 运行作第二样本。
- **平台**：本机 Mac、Lima aarch64、CI Linux x86_64 与 CI macOS 各自只和自己的 P0 比较，不跨平台比较。Lima 与 Mac 共享宿主，顺序执行，不重叠测量。
- **记录**：每个阶段记录提交、工具版本、jobs、缓存模式与状态、QUIC 能力、各阶段墙钟、实际编译/链接名单与次数、内存采样、失败与缓存统计，追加到 `docs/research/build-optimization-results.md`。

## 判定形式

- **工作量为硬门槛**：编译/链接次数、第三方额外编译为 0 等不达标即不合并。
- **噪声带**：每个指标取 max(极差 ÷ 中位数, 5%)。基线三次样本极差约为中位数的 5–8%。
- 阶段针对的指标，中位数改善须超出噪声带才算有效，否则触发该阶段退出条件。
- 非针对指标，中位数恶化超出噪声带即为回归，须解释或修复后才能合并。
- 下列整体目标值未达到不阻止合并，但须在结果报告中说明原因。阶段比较均相对 **P0 当前 HEAD 重测值**，不用旧冻结基线。

## 三类耗时目标

### 全量冷构建

新目录、依赖源码已获取、缓存 OFF 或空。

| 平台与条件 | 默认配置 | 目标 |
| --- | --- | --- |
| Mac 本机 | 8 路 | ≤ P0 的 40% |
| Lima | 2 路 | ≤ P0 的 65% |
| CI Build 步骤，缓存未命中 | 2 路 | ≤ P0 中位数的 60% |
| CI Build 步骤，ccache 热命中 | 2 路 | ≤ P0 中位数的 35% |
| 本机 ccache 热命中 | 同 jobs | ≤ 同 jobs 缓存 OFF 的 40% |

量级依据：旧基线 Mac 冷构建 336 秒中编译合计 237 秒，Linux 328 秒中 280 秒；链接、libsodium 外部串行构建与测试发现不受编译缓存影响。

### 日常增量

`test-fast.sh`，总墙钟时间包含每次配置。

| 场景 | 目标 |
| --- | --- |
| 无改动 | 双平台 ≤ 5 秒 |
| 单 .cpp 改动，全部 Unit | ≤ P0 等价流程（ALL 构建 + 串行 Unit）的 50% |
| 单 .cpp 改动，`--target` 聚焦 | 双平台 ≤ 10 秒 |
| `lua_runtime.hpp` 改动（P5 后） | 编译次数 ≤ P5 列出的真实 Lua 直接使用者数量（硬门槛）；时间 ≤ P0 的 50% |

### 配置到完整测试结束

- 本地 `build.sh`：以串行完整 CTest 为主，只要求不回归超出噪声带，构建部分缩短单独报告。
- CI job 总时长：缓存未命中 ≤ P0 的 80%，热缓存 ≤ 70%。
- 完整 CTest 并行、M1–M4 接入验收时长不在本轮压缩范围内。

## 内存与可复现性

**内存**：构建期间每秒采样整机可用内存、换页/swap 与 OOM 事件。

- Lima（约 7.7 GiB、无 swap）：可用内存始终 ≥ 1 GiB，且无 OOM kill。
- Mac：memory_pressure 不进入 critical，swap 增长 ≤ 1 GiB。
- CI：无 OOM，记录 runner 峰值。

任一项触发，该平台默认 jobs 降一档后复测并记录。同时报告单次编译最大 RSS 与整体峰值，不用单次最大值乘 jobs 推断。

**可复现性**：每个构建配置阶段都须满足：

- 新目录连续配置并构建 3 次，后两次第三方与项目编译均为 0。
- 生成版本头的 hash 与 mtime 不变。
- 稳定无操作不调用编译器。
- 同一提交 Make 回退与 Ninja、ccache ON 与 OFF 的完整 CTest 结果一致。

不要求 Debug 二进制逐字节一致。

## 行为验证

- **前提**：P0 必须在当前 HEAD 取得双平台全绿。RealmJourney 若仍失败，单独开 bug issue 修复，阻塞 P0 完成；旧的非全绿样本不作任何阶段的比较基准。
- **P1–P4**：本地一个平台 `build.sh`，加 PR 双平台 CI（含 Linux QUIC 与 M1–M4）。P1、P2 涉及双平台工具链，另在 Lima 跑一次完整 CTest。
- **P5 每个 PR**：
  - Mac 与 Lima 本地都跑完整 `build.sh`，加双平台 CI、Linux QUIC 与 M1–M4。
  - 点名复跑以下测试各 3 次：LuaRuntime 绑定、热重载与线程归属，TrainingRule，LayeredConfigLoader 生命周期，Gateway stop/join，ServiceHost 销毁顺序。
  - 保持接口拆分决策列出的全部契约。

## 退出与回退

| 阶段 | 退出条件 |
| --- | --- |
| P1 | 仅硬门槛：Abseil 实际命中固定来源；连续 3 次配置后第三方额外编译为 0。不达标不合并 |
| P2a | 正确性：全部目录消费者迁移完毕，启动与验收不误用旧 `build/dev` |
| P2b | 同 jobs 下 Ninja 冷构建或增量在任一平台劣于 Make 且超出噪声带，该平台不切换默认，保留 Make 并开票调查；内存判据触发即降一档 jobs 复测 |
| P3 | 本机未命中开销（空缓存对 OFF）> 10% 或热命中改善未超出噪声带，本机默认由 AUTO 改为 OFF；CI 计入恢复/保存时间后净收益不为正则关闭 CI 缓存 |
| P4 | Unit 4 路连续 10 轮出现任何不稳定，默认退回 1 路，并为该用例开 issue |
| P5 各 PR | 代表头编译次数未下降，或行为验证未全绿，撤回该 PR，不保留无收益复杂度 |
| 合入后 | main CI 变红或不稳定且归因到某阶段 PR，先 revert，修复后重新提交，不在 main 原地修 |

## 文档随阶段更新

| 阶段 | 文档 |
| --- | --- |
| P0 | 测量工具使用说明随 `tools/build-bench/` 提交（符合 ADR-0003）；新建 `docs/research/build-optimization-results.md`，各阶段追加条件、样本、中位数/极差、工作量与目标差距 |
| P1 | 构建说明补 OpenSSL/protoc 覆盖、`CMakeUserPresets.json` 示例与 `.gitignore`、旧缓存迁移说明 |
| P2 | AGENTS.md、tests/README、README 与 CI 中的 `build/dev` 路径与预设名；`--preset`、`--jobs` 与 Make 回退 |
| P3 | ccache AUTO/ON/OFF、容量与 CI 缓存键 |
| P4 | AGENTS.md 快速循环由 `ctest --preset dev -L unit` 改为 `./scripts/test-fast.sh`；tests/README 补 Unit 聚合、身份标签与 watch |
| P5 | `docs/architecture.md` 模块头文件结构；纠正 GameCommon “Lua 不进公共头”注释。不改线上协议与领域术语，CONTEXT.md、protocol.md 与 README 实施状态表无需改动 |

Ninja 与 ccache 均可经回退开关撤回，不满足 ADR 的难以逆转条件，不新建 ADR。本方案不引入新领域术语。

## P6 毕业规则

P6 用测量工具对每个 `include/realmmesh/**/*.hpp` 扰动一次，记录真实编译次数，并统计过去 90 天 git 修改次数。

- 编译次数 ≥ 20 且 90 天内修改 ≥ 3 次的头，按模块各开一张决策票。Gateway 完整 PIMPL、Mongo 视图另拆头与 Lua 方案 B 只经此规则进入。
- PCH/Unity：仅当 P3 后 CI 未命中冷构建仍高于上述目标，且最慢编译单元集中于重复解析同一重头时，才开评估票。
- 都不满足的，在结果报告中写明“无证据，不开票”。

这条规则承接决策地图“尚未明确”中的首批复测后续问题。

## 实施跟踪

本票关闭后，另建“构建优化实施”追踪 issue，以 GitHub 原生 sub-issues 列出 P0、P1、P2a、P2b、P3、P4、P5×3、P6，并按上文接依赖。决策地图随之到达终点并关闭。
