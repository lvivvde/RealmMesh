# #119 真实 MongoDB 夹具测量资产

本轮原生初始化前后使用相同当前完整合集 668 项；R0 为同期重测的冻结 649 项。
它们用于用户确认的双重收束规则，不构成 P6 历史严格同合集验收。
完整结果见[专项报告](../../build-mongodb-fixture-119.md)。

## 文件与校验

- `summary.json`：两平台的逐次完整入口墙钟、配对、两道门槛、资源和环境摘要。
- `mac-evidence.tar.gz` / `lima-evidence.tar.gz`：逐次命令、原始完整日志、阶段 JSON、
  编译/链接观察事件、资源时间线、测试清单、环境、正式组与准备过渡组。Mac 包还
  含初始完整诊断、原型、无效样本、TDD 红绿证据、CI 日志与 M1–M4 报告；Lima 包
  含只读时钟预检及全程监测，`clock-interrupted/` 完整保留受合盖休眠污染的
  首轮全系列（含退出8的两项失败）；不是只保存失败的一个后侧。`linux-rerun.py`
  是整轮替代驱动，睡眠证据仅保存与异常对应的窗口。回退和删除恢复检查在各自 `*-check-results/`。
- `before.patch` / `after.patch`：从 base `ead8b7ce71d89e7aac05230a24a4517e1f3e9617`
  重建两侧测量源码；`shared-correctness.patch` 是两侧相同的新增守卫和回收修复。
- `r0-correctness.patch`：此前冻结的 R0 正确性补丁，SHA256
  `e85bd84a2c93341b6e71a84aba9cf15827a9968ba7c805d400c34fae165408ac`。
  不包含本轮优化、守卫或新接口。
- `*-source-manifest.json`：各冻结源码逐文件 SHA256；源码含全部当时登记文件，
  构建及工具目录不在清单内。正式汇总在测后对实际来源逐项复核。
- `paired-run.py` / `linux-formal.py` / `linux-rerun.py` / `collect-current.py`：实际采样和收集驱动；
  `tool/` 是固定的测量器和 launcher 源码。旧 R0 仍使用原 P6 launcher。
  脚本保留实际绝对目录；移到另一机器须显式替换 source/deps/R0/tool/output 路径。
- `audit-saved.py`：只读离线复核，不运行构建、不重新核验原始源码。校验已保存
  原始文件、完整测试结果、无跳过、热入口顶层 ALL 构建无编译/链接/归档、同当前合集、整段墙钟
  统计、资源和时钟摘要，并与采样时的 `*-samples.json` 比对。
- `measurement-protocol.json` 是正式开始前冻结的计划，`formal_started: false`
  表示冻结时尚未开始；`measurement-start.json` 是第一组开始后的实际观察记录，
  不把观察时间伪造成启动时间。协议最多允许噪声时增至五组，本次按预定三组完成。
- `manifest.json` 是本目录除自身外全部交付文件的字节数及 SHA256。归档内的
  `*-samples.json` 还含其原始测量文件逐项清单；准备、失败和正式数据均保留。

不交付已构建二进制、MongoDB 数据文件、依赖源码副本或重复完整仓库归档。
CI 日志对应报告中标明的精确代码提交；文档更新不冒充新的性能样本。

## 离线复核

```bash
asset_root=/absolute/path/to/RealmMesh/docs/research/assets/build-mongodb-fixture-119-2026-10-08
saved_root=/absolute/path/on/disk/119-saved
mkdir -p "$saved_root/mac" "$saved_root/lima"
python3 - "$asset_root" <<'PY'
import hashlib, json, pathlib, sys
root = pathlib.Path(sys.argv[1])
for name, record in json.loads((root / 'manifest.json').read_text()).items():
    path = root / name
    assert path.stat().st_size == record['bytes'], name
    assert hashlib.sha256(path.read_bytes()).hexdigest() == record['sha256'], name
print('asset checksums: PASS')
PY
tar -xzf "$asset_root/mac-evidence.tar.gz" -C "$saved_root/mac"
tar -xzf "$asset_root/lima-evidence.tar.gz" -C "$saved_root/lima"
python3 "$asset_root/audit-saved.py" "$saved_root/mac" mac
python3 "$asset_root/audit-saved.py" "$saved_root/lima" lima
```

离线检查通过表示证据完整且计算可复现；它不会把 `passed: false` 的性能结论改为
达标，也不替代原机器的时钟/资源有效性及源码测后核验。

## 完整重测

先按 tests/README.md 备齐真实 etcd、MongoDB、mongosh、CMake、Ninja、Make 与
平台依赖；选容量足够的独立磁盘目录。Linux 不继承容量不足的共享 `/tmp`，
不降低 MongoDB 磁盘要求。两平台所有重负载测量串行。

```bash
repo=/absolute/path/to/RealmMesh
bench_root=/absolute/path/on/disk/119-replay
base=ead8b7ce71d89e7aac05230a24a4517e1f3e9617
mkdir -p "$bench_root/before" "$bench_root/after" "$bench_root/r0"
git -C "$repo" archive "$base" | tar -xf - -C "$bench_root/before"
git -C "$repo" archive "$base" | tar -xf - -C "$bench_root/after"
git -C "$bench_root/before" apply "$asset_root/before.patch"
git -C "$bench_root/after" apply "$asset_root/after.patch"
git -C "$repo" archive 4ffb58df9b8d20beec59f4b056d3e9c90859401f | tar -xf - -C "$bench_root/r0"
git -C "$bench_root/r0" apply "$asset_root/r0-correctness.patch"
```

三份测量源码都没有 `.git`，保持测量时的生成版本行为。核验三个来源清单；将
`.tools` 链到已按项目脚本安装的固定真实工具，准备原固定 FetchContent 来源与
libsodium 下载包（URL/SHA 见 environment.json），禁止重测时临时升级依赖。
从资产复制 `tool/`、采样驱动及 Linux 时钟源码/驱动。原 P6 与本轮 launcher.cpp
同源（SHA256 `1374833a6c5e20d9d72f04055694ba4930c2066bc2147aa85116bf96d8f4034b`），
Mac 实际二进制 SHA 也相同。实际驱动对 R0 使用 `R0.parent/launcher`，当前使用
`ROOT/launcher`；上述重建布局中二者恰为同一路径，可以共用同源编译产物。若把
R0 放在别的父目录，必须另编译放置一份，或显式改驱动指向同一已验证版本。
每平台原 launcher 二进制 SHA 记在对应环境中，不能把 Mac Mach-O 拷到 Linux 当 ELF。

```bash
c++ -std=c++17 -O2 "$bench_root/tool/launcher.cpp" -o "$bench_root/launcher"
# 仅 Linux：linux-formal.py 直接执行此频率/tick只读观测器。
cc -O2 "$bench_root/clock-frequency.c" -o "$bench_root/clock-frequency"
```

按实际驱动替换固定目录后，依次运行 `paired-run.py setup before`、`setup after`、
`r0-stable`，以及三个 `refresh`，各三次稳定检查必须没有真实编译或链接。
R0 保持 Make/1 job，当前保持 dev Ninja（Mac 8 jobs、Lima 2 jobs）；缓存 OFF。
Linux 先执行 `linux-formal.py` 的只读时钟预检，再完整采样；Mac 直接执行
`paired-run.py formal`。每平台固定顺序：R0/前/后、后/前/R0、前/R0/后。
单次入口连续配置、ALL 构建与全部 CTest `-j 1`；失败、跳过、非热工作须停止并保存，
不能按快慢挑样本。CTest 内的真实构建契约不删减；根目录观察器计数为零
不代表整个 CTest 子进程树没有内部编译。最后运行 `collect-current.py ROOT OUTPUT mac|lima`，在核验实际
冻结源码后得到原始清单和两道门槛。正式采样结束再执行两个回退/恢复检查驱动，
它们的时间不进入性能统计。
