# Linux ARM64 开发环境

## 安装与构建

Linux x86_64 与 ARM64（`aarch64`）共用 dev preset。三个安装脚本按宿主架构
选择固定版本、校验 SHA-256，并安装到仓库内被 Git 忽略的 `.tools/`：

```bash
sudo apt install build-essential cmake curl python3 libssl-dev libnuma1
./scripts/install-msquic-dev.sh
./scripts/install-etcd.sh
./scripts/install-mongodb.sh
./scripts/test-fast.sh          # 配置 + 只构建 Unit 聚合目标 + 4 路跑 unit 用例
./scripts/build.sh              # 配置 + 构建 ALL + 串行全量测试
```

MsQuic 与 MongoDB 使用 Ubuntu 24.04 的上游二进制；ARM64 同样强制编入 QUIC。
CMake 通过 `CMAKE_LIBRARY_ARCHITECTURE` 查找 MsQuic 的原生库目录，并下载、校验
官方 protoc 35.0 的 Linux ARM64 包。此前未覆盖该架构时会进入 Protobuf 编译器
源码构建，因缺少 `google/protobuf/descriptor.upb.h` 而失败。

两个入口按 CPU/内存预算选编译并行（8 GiB 机器为 2 路，见 `scripts/lib/build-jobs.sh`），`--jobs N` 可覆盖。下文 2026-10-02 的验证早于这两个入口，当时手动用 4 路编译、`ctest -L unit --parallel 4`。CI 仍以 Ubuntu 24.04
x86_64 为 Linux 回归门槛；本次适配不改变 ADR-0001、ADR-0002、ADR-0011、
ADR-0012 的平台后端、QUIC 与数据库边界。

## 本机验证记录（2026-10-02）

验证代码基于 `7522433` 加当前 ARM64 适配工作树，未提交。

| 项目 | 本次环境或结果 |
|---|---|
| 系统 | Ubuntu 26.04.1 LTS，Linux 7.0.0-34-generic，aarch64 |
| 资源 | 8 个逻辑 CPU，约 7.7 GiB 内存，无 swap |
| 工具链 | GCC 15.2.0，CMake 4.2.3，OpenSSL 3.5.5 |
| 依赖 | MsQuic 2.6.1，etcd 3.6.14，MongoDB 8.0.32，mongosh 2.12.0，protoc 35.0 |
| 配置与构建 | 成功；QUIC 已启用，Event Loop Backend 为 epoll；生成原生 ARM64 `realm_mesh` 与 `realm_mesh_loadgen` |
| 单元测试 | 456/456 通过，包含真实 QUIC 消息交换用例 |
| 集成测试首次运行 | 142/143 通过；连接保持用例的 fd 回归断言失败，见下文 |
| 四进程运行 | `FourProcessFullRepeats` 与 `FourProcessFailureRecovery` 通过 |
| MongoDB 与 etcd | 隔离单节点副本集、账号与角色持久化、排队状态与准入消费恢复用例通过 |
| M2 缩减负载 | 250/250 完成，attach/handoff 失败为 0，fetch 重试与失败为 0 |
| M3 缩减负载 | 10000/10000 取号、2000/2000 轮询客户端完成 |

首次 `L1GatewaySoakHoldsWaterLevelWithoutFdLeak` 运行中，100/100 客户端完成，
attach/handoff 失败为 0，阶段与额度排空断言通过，但 fd 从预热基线 89 增至 90，
等待 2 秒后未回落。该用例随后单独复测 6 次均通过，fd 均为 89 → 89；再用
`ctest --preset dev --rerun-failed --repeat until-fail:3` 连续复测 3 次也均通过，
未调整测试阈值。最初多出的句柄尚未复现定位，因此本记录不宣称首次全量测试全绿，也不
据此宣称已消除资源泄漏风险或具备生产容量。完整 M1–M4 验收脚本的重复矩阵与跨机器
容量验证不在本次构建运行检查内。

构建和首次测试日志保存在本机 `build/dev/acceptance/linux-arm64-{configure,build,unit,integration}.log`，
该目录不提交。手动开发联调仍须按[共享库说明](shared-mongodb.md)配置仓库外私有
连接材料，并通过 `scripts/with-shared-mongodb.sh` 启动；本机尚未配置这些材料。
本次服务运行由自动化夹具驱动，使用隔离临时数据库，不访问共享开发库。
