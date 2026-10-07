# P6 复测证据（2026-10-06–07，#129）

首批累计验收未通过，首个产品冻结后的 Linux 无操作/恢复检查因 VM 时间戳倒退失败，整组另存 `before-clock-fix/`，用户选择修正时钟并从空产物重跑全部 Linux 场景；修复前 Linux 检查的停止压力失败、未复现和中断整组均保留于 `before-stop-fix/`；修复及红绿证据见 `supervisor-fix/`，其后重新冻结完整取样。完整结果和有界后续问题见 [阶段报告](../../../build-optimization-results.md)。本目录的数据来自当前任务独立拥有的副本，不包含共享开发 MongoDB。

| 文件 | 用途 |
| --- | --- |
| `mac-samples.json` / `lima-samples.json` | 所有正式逐次命令、计时、实际源码/链接名单、归档与外部 sodium 工作量、原始统计和配对结果；环境、原始日志/事件/资源文件 SHA，测试身份、源码 SHA 与恢复检查 |
| `provenance-seed.json` / `revisions.json` / `tool-revision.txt` / `*-driver-provenance.json` | 冻结产品、工具、补丁与各平台实际驱动的来源 |
| `r0-correctness.patch` | R0 同步既有指标采样等待、worker 信号修复及本票重复等待修复；不降低覆盖/负载 |
| `run.py` / `checks.py` / `lima-checks.py` / `cold-complete.py` | 本次实际使用的独立目录驱动；Mac 检查另保存 Ninja 输入快照，平台驱动 SHA 分别登记，不是生产构建入口 |
| `all-linux-after-clock-fix.py` / `clock-monitor.py` / `clock-frequency.c` / `lima-clock-*-summary.json` | 用户授权的 VM 同步修正、完整重跑及只读时钟观测；旧组及校正来源在 `before-clock-fix/`，没有修补产物 mtime |
| `compose-report.py` / `report-notes.md` | 从两平台最终 JSON 生成 P6 正文；模板中的动态标记由生成器替换 |
| `collect.py` / `audit.py` | 归一化导出和证据完整性检查；不会将范围变化或性能回退改判为通过 |
| `*-hot-full-*.log` / `*-final-full.log` / `*-fallback-full.log` / `*-cold-complete-*.log` | 全部对应完整 CTest 原始输出；不归一化字节，便于校验 SHA |
| `*-login-chain*.md` / `*-login-chain*.log` | macOS 接入及 Linux QUIC/M1–M4 原始验收报告/日志 |
| `manifest.json` | 本目录交付文件的逐文件 SHA（清单自身除外） |

所有主结果使用同一固定 `measure.py`/launcher：从 `ee44ff80c78a0cc6e362e15f52c67e722ac6c849` 的 `tools/build-bench/` 获取；launcher 本机编译。旧 R0 工具、P5 CI 工具秒数没有与这些结果混合。CI 恢复/保存净等待引用既有 [P5 原始证据](../p5-ci-2026-10-06/ci-results.json)，本票没有重新运行 GitHub CI。

## 复现布局

在新拥有的根目录复制驱动、`revisions.json` 和 `tool-revision.txt`；分别将 `git archive` 的 R0 与最终产品解到 `r0/`、`final/`，在 R0 应用本目录补丁。`tool/` 放固定工具源码，根目录编译 `launcher`。预先准备固定 FetchContent 来源和 SHA 相符的 libsodium 原包，调整驱动的 `DEPS` 路径；两侧必须共用同一平台的固定来源。fixture 工具按仓库标准预先准备，链接 `.tools`；Linux 的 `tmp/` 必须在磁盘上。

源码从 Mac 再打包到 Linux 时设 `COPYFILE_DISABLE=1` 禁用 AppleDouble；同时核对所有冻结源码字节与不存在 `._*` 元数据文件。第一次未过滤的传输导致 R0 Unit 9/496 失败；1365 个本次副本元数据文件移除后 496/496 通过，完整正式组重新从头测量。原始污染组、失败和清理魔数/SHA 清单保留在 `before-stop-fix/lima-samples.json` 的独立字段及 raw_manifest，不计成功样本。工具目录层级自测失败也单列保留。Linux 长暂停后的 0/56、独立复查 0/4 和 0/0 过渡组同样保存在旧 Lima JSON 的 `stability_transition_run`；之后完整重取三轮稳定性，不隐藏初次失败或宣称已确定重新链接根因。

时钟修正前 Linux 原组稳定性首轮为 0/55，首个恢复后的无操作为 0/33，两次原检查驱动退出 1。用户选择修正时钟并重跑全部组，续跑也被精确中断；这些原始 rows、日志与门槛 false 在 `before-clock-fix/` 保存，未完成阶段没有补写成功。`audit.py` 的成功只表示证据完整性，实际门槛另有字段，不把扩展测试合集当成严格可比。Lima guestagent 每 10 秒校正约 428ms，125 次自然写入捕获 mtime 倒退约 325ms、单调时钟前进约 103ms。详细归档依赖和命令哈希保存在 `before-clock-fix/clock-findings.json`。时钟修正的细粒度频率与粗粒度 tick、残余步进及收敛后复测均保留。正式新组全程保存 wall/monotonic/raw 观测与 tick/频率读数。

Linux 将 `lima-checks.py` 复制为根目录的 `checks.py`；该版本省略 Mac 检查前后保存 Ninja 输入快照的旁路步骤，计时入口与门槛相同。

按顺序运行 `run.py cold short hot cache`、`checks.py stable recovery syntax full fallback`、标准接入验收和 `cold-complete.py`。两个平台不可重叠测量；`cold-complete` 的三组为独立连续准备/配置/ALL/完整 CTest 计时，不拼接已结束阶段。`finish-bench.py` 会从根目录 `product.bundle` 安装验收 Git 元数据；在原仓库用 `git bundle create /本次独立根目录/product.bundle HEAD` 准备（该 HEAD 必须包含冻结产品提交 `7858a925e10a85f6d190e34356cf8bfeb02ff12b`），验收前归档副本没有 `.git`，仅有工具链接 `.tools`。验收入口需在独立副本提供真实 Git HEAD 元数据；不得切换活动开发检出。

测试全集由 649 增到 667、Unit 由 496 增到 504，旧项无删除。数据保留严格可比性为 false；全部当前测试仍完整运行。CTest 单例舍入时间和只用于成本分解，不生成“扣除新增项后的主计时”。

Mac 原始目录 `/private/tmp/realmmesh-p6-fixed`，Lima `/home/edwin.guest/code/.bench/p6-clock-fixed-20261007`；完整编译事件和每秒资源时间线在这些本次拥有的目录保存，本目录提供全部校验清单和可移植逐次结果。资源口径包含性能与检查阶段，独立接入日志不冒充同一采样区间；瞬时/重挂子进程可能不进入树 RSS，平台整体内存压力另采。Mac OOM 指标未采集时保持 null。

## 最终导出校验

Mac 的 `finish-bench.py` 顺序完成检查、接入验收和独立冷入口；Linux 时钟异常组的 finisher 在稳定性门槛停下，旧续跑与中断单列；正式新组的 `all-linux-after-clock-fix.py` 运行完整原始阶段，并在观察器结束后审计环境时钟。收尾进程自身结束后，再运行 `collect.py ROOT PLATFORM-samples.json PLATFORM` 和 `audit.py PLATFORM-samples.json ROOT`。最终导出的 stdout 使用 `.stdout` 等不属于原始 run-log 集合的文件，避免把仍在写的收尾/导出日志 SHA 当成最终字节。交付的 `*-export-provenance.json` 与 `*-audit-final.txt` 记录这一校验；全部 raw_manifest 条目均须与原目录一致。然后 `package-evidence.py` 打包选定原始日志和 JSON，接收时按 shipment manifest 核验；完整 raw 文件仍保留于独立拥有的目录。

Linux 完整驱动运行前，将 `clock-frequency.c` 编译为根目录 `clock-frequency`，用 `clock-monitor.py environment-clock/preflight --duration 45 --frequency-reader ./clock-frequency --probe-mtime` 保存独立自然写入预检；要求时钟已修正且该摘要无倒退或超过 50ms 跳变。随后运行 `python3 all-linux-after-clock-fix.py > all-linux.stdout 2>&1`，由它管理全程只读观察、全部阶段和观察结束后的最终导出。复现不会自动关闭时间服务或执行频率重置。

`clock-frequency.c` 默认只读取 Linux 内核频率与 tick；本次环境修复显式使用 `--zero-frequency` 同时重置两项，命令及修改前后状态保存在旧组诊断。它是本票环境证据工具，不进入产品构建或服务路径。`clock-monitor.py` 在新组全程只读观测，最终源文件/二进制与驱动 SHA 分开登记。
