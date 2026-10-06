# 开发 Supervisor 停止信号解析竞态（#143）

## 原始失败与复现条件

[Issue #143](https://github.com/lvivvde/RealmMesh/issues/143) 保存了
[Linux CI attempt 1](https://github.com/lvivvde/RealmMesh/actions/runs/37337682825/attempts/1)
在 `DevServicesScriptTest.StopIsReverseOrdered` 的失败：

```text
manager running (pid 20472)
gateway running (pid 20588)
realm   running (pid 20512)
scripts/dev-services.sh: trap: line 2: unexpected EOF while looking for matching `)'
20588  1  Ssl  .../realm_mesh --service gateway --config .../configs
20512  1  Ssl  .../realm_mesh --service realm --config .../configs
```

2026-10-06 在 Lima Ubuntu 26.04 ARM64 上复现。系统 Bash 5.3.9 与 macOS 系统
Bash 3.2.57 的原 `StopIsReverseOrdered` 各连续通过 20 次；这没有排除竞态。
Lima 的 `/tmp` 是接近满额的 tmpfs，首轮先因 MongoDB 的最低可用磁盘空间检查失败；
后续测试使用 `TMPDIR=/dev/shm`，数据库与 etcd 仍全部由隔离夹具启动。

从 Ubuntu 24.04 的 `bash_5.2.21-2ubuntu4_arm64.deb` 解包运行 Bash 5.2.21，
未替换系统 Bash。在原 Supervisor 上运行新增 `stop_during_process_checks` 用例，
只对管理 PID 连续发送 TERM、间隔 0.1 ms、最多 2 秒，捕捉到：

```text
Refusing to stop stale supervisor PID 2434112.
Stopping gateway
scripts/dev-services.sh: trap: line 2: unexpected EOF while looking for matching `)'
```

原始输出片段保存在 [Ubuntu Bash 5.2.21 失败日志](assets/dev-services-signal-stop/noble-original-failure.txt)。

原脚本与本次任务基点 `be2d795b7a1b2e19b9230cdbd38690f74c306b0c` 的文件逐字一致：

| 文件 | SHA-256 |
|---|---|
| `scripts/dev-services.sh` | `46d3cd9817cb7008b3a9060cc849a48bd387f9d99acca54dccfe45966a2741a7` |
| `scripts/lib/dev-process.sh` | `7a9d216c69f0c37d0f48884825f04302daeff361d25496716cb45f6f68d73913` |

新增回归用例在原实现上失败，再运行修复版本验证；没有重试 start/stop 或放宽顺序断言。

## 最小场景与根因

以下片段足以触发与原 CI 相同的解析错误，且不需要 MongoDB、etcd 或业务服务：

```bash
set -eu
stop=0
trap 'stop=1' TERM
printf 'READY\n'
while [[ $stop -eq 0 ]]; do
    value="$(<"$1")"
done
printf 'STOPPED\n'
```

文件 `$1` 只含 `12345`。驱动程序等待 `READY`，等待 3 ms 后向该 Bash PID 发送
**一次** TERM，收集退出码、stderr 与 `STOPPED`。Ubuntu 24.04 Bash 5.2.21
各 300 次对照的结果：

| 唯一替换的表达式 | 带解析错误的轮次 |
|---|---:|
| `$(printf %s ok)` | 9/300 |
| `$(<"$1")` | 87/300 |
| `$(cat "$1")` | 0/300 |

无错误的有限样本不证明表达式安全。错误发生在 trap 重入尚未完成的命令替换解析时；
不是停止顺序、存储连接或服务退出慢。单次信号、单层替换即可触发。
把 trap 改成函数调用，在另一个 Bash 5.2.0 对照中仍失败 2/300；
只把 trap 缩成赋值（原代码已经如此）也无法规避。
把命令替换放到没有信号 trap 的工作进程、管理进程只 `wait`，同一探针通过 300 次。

## 修复与回归边界

管理进程安装 INT/TERM trap 后只记录标志并 `wait` 工作进程，不执行命令替换。
信号打断 `wait` 时，由主流程发布 `.runtime/supervisor.shutdown`，忽略后续停止信号，
再 `wait` 回收工作进程。工作进程的启动、就绪等待与巡检从停止文件观察请求，
执行原有 `Gateway → Realm` 回收逻辑；管理进程随后删除停止文件。
就绪等待收到停止请求时，已启动的 Realm 被回收，Gateway 不会越过未就绪 Realm 启动。

测试都通过真实 `dev-services.sh` 与真实 `realm_mesh`，沿用 `integration`、
`RUN_SERIAL`、20 秒超时与隔离 etcd/MongoDB 夹具：

- `StopIsReverseOrdered`：原来的正常 stop 与顺序断言，并新增退出与文件清理断言。
- `StopDuringProcessChecks`：停止信号压力，仍要求相同顺序、无解析错误与全部进程退出。
- `StopDuringStartup`：Realm 就绪不可达时停止，启动调用返回失败，Realm 被回收，Gateway 未启动。

## 验证记录

2026-10-06，代码固定后运行：

| 环境 | 验证 | 结果 |
|---|---|---|
| macOS ARM64，Bash 3.2.57 | 三条停止用例各 `--repeat until-fail:20` | 60/60 通过 |
| Linux ARM64，Ubuntu 24.04 Bash 5.2.21 解包运行 | 三条停止用例各连续 20 次 | 60/60 通过 |
| Linux ARM64，Bash 5.2.0 | 停止信号压力用例连续 20 次 | 20/20 通过 |
| macOS，`./scripts/build.sh` | 完整构建与串行 CTest，含本机 MsQuic | 666/666 通过，393.42 秒 |
| Linux，当前分支隔离源码快照 | 完整构建与串行 CTest，Bash 5.2.21，epoll + QUIC | 666/666 通过，316.35 秒 |

两端配置均打印 `realm_network: QUIC transport enabled (MsQuic found)`。
结束后核查进程表，本次 Supervisor、工作进程、服务及 etcd/MongoDB 夹具均已退出。
macOS 进程表里另有 10 月 2 日启动的旧夹具，未将它们计入本次运行或修改。

Linux 使用已有依赖源码与工具，在独立目录构建当前分支快照；测试文件为本次最终版本，
系统仓库和系统 Bash 均未替换。这里的 Linux 结果是本地 ARM64 验证。

同日，修复提交 `fd55a80a92185a80b8a64f9b0126a735ece8272c` 的
[GitHub CI](https://github.com/lvivvde/RealmMesh/actions/runs/37473072687) 全部通过：
Ubuntu 24.04 x86_64 Linux 完整 CTest 666/666，包含 QUIC；macOS 完整 CTest 665/665，
按 CI 平台约定未编入 QUIC。三条停止回归在两端均通过，Linux 后续 M1–M4 登录链验收也成功。
[CI 原始输出摘录](assets/dev-services-signal-stop/ci-validation.txt) 保存测试数量与停止回归结果。
验收记录的后续提交只补文档与日志摘录；以上 CI 对应的是实际修复提交。

Standards 与 Spec 两路代码审查均为 0 项发现。语法检查与 `git diff --check` 通过。
