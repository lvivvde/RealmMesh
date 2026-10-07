# #119 重复配置与发现诊断资产

源码冻结：`216979de85719b445a1d92c0052de503012d87fc`。这些是单次诊断和
正确性检查，**不是优化前后性能配对**，也不是 5% 达标证据。

## 文件

- `diagnostic-events.tar.gz`：Mac/Lima 六个未修改契约的 baseline/profile 日志和
  JSONL、Native subprocess/sleep 诊断、Linux 缺 sodium 文件与旧构建图失败、
  被磁盘重跑组替代的早期 Linux 诊断、实际 R0 API 编译失败、现有 MongoDB
  用例的三次 mongosh 诊断。归档内含平台部署的包装器。
- `summary.json`：从有效组的原始 JSONL 复算。只有显式包装的工具调用，不代表
  整个进程树。再次请求相同 binaryDir 不代表该配置可以删去。
- `mac-tools.json` / `linux-tools.json`：本机工具版本。诊断不是严格负载控制实验，
  不推断最低工具版本、其他架构或 CI runner 的性能。
- `product-source-manifest.json`：416 项产品、配置、构建与测试文件的 SHA256；
  `*-full-source-match.json`：全量检查所用源码与冻结 main 逐项相同。
- `mac-full.log.gz` / `linux-disk-full.log.gz` 与相应 JSON：完整正确性运行。
  `linux-full-invalid-tmpfs.log.gz` / JSON：磁盘余量不足的全量中断，退出 130；
  `disk-environment.txt` 保留 tmpfs 与有效磁盘目录的容量证据。
- Python 驱动与包装器为研究资产，未接入测试注册；`manifest.json` 是本目录
  除自身之外的逐项 SHA256。

## 复现

先按 tests/README.md 备齐真实 CMake、Ninja、ccache、etcd 和 MongoDB；检出
冻结源码，并另保留包含本报告资产的检出；冻结源码提交尚不包含本轮的新资产。
全量图检查需要同源码已经配置的 Ninja build，不能使用旧主检出的图。
源码、工具和输入必须保持不变；Mac/Lima 不同时运行测量。

以下路径变量均需按本机设置；`task_out` 用独立磁盘目录，特别不能继承空间不足的
Linux `/tmp`。包装器只是观测额外运行，普通 baseline 用同一个真实 CMake/CTest。

```bash
source_root=/absolute/path/to/frozen/RealmMesh
asset_root=/absolute/path/to/report/RealmMesh/docs/research/assets/build-fixture-config-cost-119-2026-10-08
task_out=/absolute/path/on/disk/119-config-diagnostic
mkdir -p "$task_out/bin" "$task_out/tmp"
export TMPDIR="$task_out/tmp"
# 包装器第一行改为本机 python3 的绝对路径；原逻辑不变。
python3 - "$asset_root/tool_probe.py" "$task_out/bin" <<'PY'
import pathlib, shutil, sys
source = pathlib.Path(sys.argv[1]).read_text().splitlines()
source[0] = '#!' + shutil.which('python3')
for name in ('cmake', 'ctest'):
    path = pathlib.Path(sys.argv[2]) / name
    path.write_text('\n'.join(source) + '\n')
    path.chmod(0o700)
PY
python3 "$asset_root/diagnose.py" --source "$source_root" \
  --out "$task_out/main" --probe-bin "$task_out/bin"
python3 "$asset_root/native_profile.py" --source "$source_root" --out "$task_out/main"
python3 "$asset_root/other_contracts.py" --source "$source_root" \
  --out "$task_out/other" --probe-bin "$task_out/bin" \
  --binary-dir "$source_root/build/dev-ninja"
```

`linux_disk.py` 是本轮磁盘重跑的实际驱动：工作根包含上述四个驱动、`source`
源码副本和 `bin` 包装器，`--full-source` 指向完整同源码/已有 Ninja 图的检出。
它在所有诊断和全量运行中统一设置自己拥有的磁盘 TMPDIR，失败即停止。

对保存的原始资产独立复算：

```bash
mkdir -p "$task_out/saved"
tar -xzf "$asset_root/diagnostic-events.tar.gz" -C "$task_out/saved"
python3 "$asset_root/summarize.py" "$task_out/saved" "$task_out/recomputed.json"
cmp "$asset_root/summary.json" "$task_out/recomputed.json"
```

R0 API 证据在归档 `r0-api-probe/`：原始 `startup_topology.hpp` 来自
`git show 4ffb58d:framework/service_host/include/realmmesh/service_host/startup_topology.hpp`，
`clang++ -std=c++20 -fsyntax-only load_topology.cpp` 在该目录失败；不是产品新失败。

额外 mongosh 诊断只对现有
`ctest --preset dev -j 1 -R '^PlayerDataStoreTest.AccountWithoutCharactersIsEligible$'`
包装真实 shell。设置 `REALMMESH_MONGOSH_BINARY` 为本机解释器适配后的
`mongosh_probe.py`、`REALMMESH_PROBE_REAL_MONGOSH` 为真实 shell、
`REALMMESH_PROBE_MONGOSH_LOG` 为独立事件文件。包装器不保存连接串或 JS 内容；
三次观察不等于完整合集调用次数或可实现的净收益。
