# RealmMesh

C++20 分布式游戏服务端框架：CMake 构建，Lua 写配置与业务规则，protobuf 线格式。项目文档（CONTEXT、ADR、spec、architecture）用中文写，新增内容也用中文。

## 动手前

- **领域词**：给代码、测试、issue、文档里的概念命名前查 `CONTEXT.md`，用词条原词，避开各词条 `_Avoid_` 所列同义词。
- **ADR**：读 `docs/adr/` 中涉及改动区域的条目；改动与某条 ADR 冲突时，在说明里点名冲突的那条。
- **代码放哪**：新文件、新模块、新服务、新 Lua 规则的落点查 `docs/agents/code-layout.md`。四层依赖只向下，`framework/` 只放与游戏无关的代码，`layering_guard_test` 守住任何反向依赖。目录与填充它的代码在同一个变更里出现（ADR-0003）。
- **改公共头、CMake、生成规则或测试夹具**：先读 `docs/agents/build-performance.md`。
- **跨服务流程**（登录链、Scene 转移）：设计在 `docs/specs/`，已实现结构在 `docs/architecture.md`，线格式在 `docs/protocol.md`。

## 平台

Linux（Ubuntu 24.04，WSL2 为本地参考）同时是生产与开发基线，始终编入 QUIC，完整验证以 Linux 结果为准。macOS 是可选开发平台，不阻塞主路径（平台差异一律用条件分支表达，不删 macOS 分支），装了 Homebrew `libmsquic` 才编入 QUIC，否则只有 TLS/TCP（ADR-0017，部分取代 ADR-0002、ADR-0012）。在 macOS 上采信 QUIC 路径的结果前，确认 configure 输出含 `realm_network: QUIC transport enabled`。CI 的 `macos` job 不编 QUIC，QUIC 的门槛是 `linux` job。Linux 开发环境见 `docs/operations/linux-arm64-development.md`。

### Linux SSH 开发环境（仅在 macOS 宿主上开发时适用）

- 本机 Lima 的 `ubuntu` 实例已配置 Git 环境，项目已完成构建；需要在 Linux 中开发或验证时，通过 `ssh lima-ubuntu` 进入。
- Linux 仓库位置：`/home/edwin.guest/code/RealmMesh`；连接后执行 `cd /home/edwin.guest/code/RealmMesh`。
- SSH 连接沿用本机 `~/.ssh/config` 与 Lima 生成的配置；认证材料及 Git 凭据保存在仓库外。此处只记录环境指引，密钥、令牌、口令及凭据内容不得写入或提交到仓库。

## 玩家数据

手动开发运行连远端共享 MongoDB（SSH 隧道 + TLS + 认证）：启动服务、排查 DB 连接或配置新机器前读 `docs/operations/shared-mongodb.md`，经 `scripts/with-shared-mongodb.sh` 启动。地址、凭据与 CA 留在仓库外，输出与提交中只写占位符。自动化测试只用夹具拉起的隔离本地 `mongod`。

## 测试与验证

- 添加测试前读 `tests/README.md`（位置、注册、标签、入口）。
- 无 sudo 的 Linux（如 WSL2）把 CMake/Ninja/OpenSSL/libnuma 放在仓库内 `.tools/`，按 README「构建」一节设置 `PATH`、`OPENSSL_ROOT_DIR`、`CMAKE_LIBRARY_PATH`、`LD_LIBRARY_PATH`；没有 Ninja 时用 `--preset dev-make`。
- 编辑循环用 `./scripts/test-fast.sh`（只建、只跑 `unit`）。阶段完成，或改了公共头、跨模块、网络、协议、存储行为时，`./scripts/build.sh` 全绿才算验证完成；未验证项在说明里列出。
- macOS CI 比开发机慢数倍：负载（连接数、迭代、载荷）在 `__APPLE__` 下缩小，Linux 保持全量。macOS CI 的 `Timeout` 也可能是测试崩溃后孤儿子进程占住 ctest 输出管道，先读日志再缩数据。

## 收尾

- **文档随代码**：改了已实现结构、线格式或领域词，同一个 PR 更新 `docs/architecture.md`、`docs/protocol.md`、`CONTEXT.md` 与 README 实施状态表。
- **提交**：Conventional Commits，带 scope 与 issue 号，如 `fix(queue): persist issued positions across restart (#89)`。Issue 操作与分诊标签见 `docs/agents/issue-tracker.md`、`docs/agents/triage-labels.md`。

## 代码检索

接入了 `codebase-memory-mcp` 时，用 `search_graph` / `trace_path` / `get_code_snippet` 查代码，用 Grep 查字符串字面量和 `third_party/`；拉取变更后跑 `index_repository`（增量）。
