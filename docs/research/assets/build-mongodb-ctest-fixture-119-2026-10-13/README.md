# #119 CTest 共享 MongoDB 的证据与复现

本轮当前两侧均为 671 项、相同标签；同期原冻结 R0 为 649 项。
使用用户确认的双重规则：本轮完整入口净收益，以及两平台当前全部测试中位耗时
不超过同期 R0 的 1.05 倍。不追认 P6 的历史严格同合集验收。
结果见[专项报告](../../build-mongodb-ctest-fixture-119.md)。

## 文件与限制

- `mac-evidence.tar.gz` / `lima-evidence.tar.gz`：完整命令/日志、阶段 JSON、
  资源、环境、测试清单、来源清单与补丁、冻结工具、准备及回退/恢复验证。
  Linux 包含只读时钟、失败用例真实重放及整套替代驱动；Mac 包含候选 CI 日志与 M1–M4 报告。
- `*-formal-summary.json` / `*-independent-audit.json`：精简逐次记录与独立复算。
- `diagnostic-evidence.tar.gz`：原型筛查与红绿反馈，不作为正式中位数。
  `failed-formal-quic-collision.tar.gz` 和 `aborted-formal-repeat-fix.tar.gz`：
  端口冲突失败与重复准备修复前主动中止的完整记录。
- `lima-clock-invalid-evidence.tar.gz`：受宿主休眠污染的全部九次记录，整套排除，
  保留最后失败入口及连续时钟，不将八次通过记录补成有效系列。
- `*.archive.json`：归档字节数、SHA256 及逐文件核验数。正式包与 Linux 失效包
  内部清单为 `files.json`，两个早期失败包为 `sha256.json`（不计清单自身）；
  初始诊断包的清单另存 `diagnostic-evidence.files.json.gz`，由已有不可变归档独立
  枚举生成，不冒充原采样时的新记录。所有历史归档字节和原 SHA 保持不变。
- `measurement-protocol.json`：正式前冻结，`formal_started: false` 表示冻结时点；
  实际开始另记 `measurement-start.json`。最少三组，任一侧前三组的
  `(max-min)/median > 5%` 才按预定顺序追加完整两组，所有有效样本保留。
  environment.json 的 `samples: 5` 是测量器上限，实际组数看协议和逐次结果。
- `revisions.json` / `reconstruction*.json`：最终冻结与独立重建校验；当前逻辑源码
  各 852 项、R0 429 项一致。对照清单额外含一个已披露的生成 Python 缓存。
  `preliminary-*.json` 是重复准备修复前历史版本，不与最终正式数据混用。
- `audit-saved.py`：从原始文件只读复算；`manifest.json` 校验本目录除其自身外的全部交付文件。

不交付已构建二进制、MongoDB 数据、完整依赖或重复仓库副本。RSS 是每秒采样的
原进程树和活动 detached 后端树的去重并集，可能漏短进程，不称绝对峰值。
采样后已核验源码、资源和时钟；离线检查不能重新证明原机器物理环境。

## 离线复核

```bash
asset_root=/absolute/path/to/RealmMesh/docs/research/assets/build-mongodb-ctest-fixture-119-2026-10-13
saved_root=/absolute/path/on/disk/119-saved
mkdir -p "$saved_root/mac" "$saved_root/lima"
python3 - "$asset_root" <<'PY'
import hashlib, json, pathlib, sys
root = pathlib.Path(sys.argv[1])
for name, v in json.loads((root / 'manifest.json').read_text()).items():
    p = root / name
    assert p.stat().st_size == v['bytes'], name
    assert hashlib.sha256(p.read_bytes()).hexdigest() == v['sha256'], name
print('asset checksums: PASS')
PY
tar -xzf "$asset_root/mac-evidence.tar.gz" -C "$saved_root/mac"
tar -xzf "$asset_root/lima-evidence.tar.gz" -C "$saved_root/lima"
python3 "$asset_root/audit-saved.py" "$saved_root/mac/mac-evidence" mac
python3 "$asset_root/audit-saved.py" "$saved_root/lima/lima-evidence" lima
```

脚本核验原始文件、逐项 Passed、零跳过/未运行、同合集、热 ALL 零工作、
三或五组规则、缓存/资源/时钟和双重门槛，不启动服务或读取原绝对源码目录。

## 完整重测

按 tests/README.md 备齐真实 etcd、mongod、mongosh 和平台构建工具。独立磁盘
目录容量足够；Linux 使用磁盘 TMPDIR。宿主开盖接电，Mac 与 Lima 重负载串行，
不调整 VM 资源、时钟或测试负载。

从平台归档复制 `tool/`、采样驱动、协议、来源清单和补丁到新根目录，勿复制旧
`*-results` 作为新样本。驱动保留当时绝对依赖路径，迁移须明确更新、登记新 SHA，
形成新系列。以下补丁均来自原始包：

```bash
repo=/absolute/path/to/RealmMesh
bench_root=/absolute/path/on/disk/119-replay
base=a420ee80bbdb38b73573da2bd2ed20a53808ec76
mkdir -p "$bench_root/after" "$bench_root/before" "$bench_root/r0"
git -C "$repo" archive "$base" | tar -xf - -C "$bench_root/after"
git -C "$repo" archive "$base" | tar -xf - -C "$bench_root/before"
git -C "$bench_root/after" apply "$bench_root/measured-source.patch"
git -C "$bench_root/before" apply "$bench_root/measured-source.patch"
git -C "$bench_root/before" apply "$bench_root/before-detach.patch"
git -C "$repo" archive 4ffb58df9b8d20beec59f4b056d3e9c90859401f | tar -xf - -C "$bench_root/r0"
git -C "$bench_root/r0" apply "$bench_root/r0-correctness.patch"
c++ -std=c++20 -O2 "$bench_root/tool/launcher.cpp" -o "$bench_root/launcher"
# Linux 的只读时钟频率观测器：
cc -O2 "$bench_root/clock-frequency.c" -o "$bench_root/clock-frequency"
```

来源没有 `.git`，保持实测版本生成行为。核对全部逻辑源码；清单中的生成 Python
缓存按 reconstruction.json 披露处理。`.tools` 链到已安装的固定真实工具；依赖
URL/SHA 见 environment.json 和 dependency-restoration.json，sol2/httplib 逐文件
一致性见 dependency-cross-platform-verified.json。Mac 各侧统一 OpenSSL 3.6.5，
与此前系列不同，不复用旧秒数。launcher 各平台本地编译，R0.parent 与 ROOT 在
上述布局相同；另放 R0 须明确驱动指向同源平台 launcher。

依次执行 `paired-run.py setup r0`、`setup before`、`setup after` 及各侧 `refresh`；
连续三次稳定构建必须零编译/链接。当前 dev Ninja（Mac 8/Lima 2 jobs），
R0 原 Make/1 job，各侧 OFF、CTest 串行 1。Mac 用 `paired-run.py formal`，随后
`collect-current.py ROOT OUTPUT mac`；Linux 用 `linux-formal.py` 自动预检、采样、收集。

前三组 R0/前/后、后/前/R0、前/R0/后；需要扩展时第四组 R0/后/前、第五组前/后/R0。
每次连续配置、ALL 构建和完整 CTest，不相加阶段中位数。任何失败/跳过/非热工作
都停止并保留，环境污染需整套替代。顶层观察器零工作不表示 CTest 内部构建契约
没有编译，其原断言与工作量保持。

正式后串行运行 `additional-validation.py recovery` 和 `make`。删除本轮辅助程序，
验证 Unit 不依赖它、ALL 单次链接恢复、三轮无操作、真实契约与 504 项 Unit；
Make/OFF 新目录完整构建并验证全部 671 项。补充验证不进入性能中位数。
独立构造仍自启后端；Make/OFF 保留构建回退。撤回共享优化须撤回消费者 fixture
绑定，不能静默跳过契约。

冻结环境收集器仅扫描名含 `configure` 的日志，新组合阶段可能留下 QUIC:null。
必须核对真实配置行，使用归档中的 `repair-environment-capability.py` 对实际结果目录
导出能力字段；保留 unresolved 原 JSON、每份配置日志 SHA 与 provenance，再运行
收集/复核。正式计时和八个测量工具不改，不能手写能力或用旧准备字段替代本轮证据。
本轮补充验证也使用这条独立来源校验路径。

补充驱动中 `QueueStoreMongoIntegrationTest.*` 的可选表达式没有匹配实际测试名；
原恢复阶段实际执行的是两项夹具契约（内部也运行真实 C++ Probe），不能声称
覆盖了这个不存在的类。额外串行运行 `supplement-consumer.py`，明确选择现有
`PlayerDataStoreTest.PersistsAccountAndSelectedCharacterAcrossReconnect`，连同自动
准备/清理共三项，用恢复后的辅助程序验证实际 C++ 消费者。保留原驱动和选择记录，
不改测试源码或任何正式样本。
