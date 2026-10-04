# 双平台构建与验证基线（2026-10-02）

本报告为[建立双平台三类构建耗时与重编译范围基线](https://github.com/lvivvde/RealmMesh/issues/110)提供调查结果，供[构建提速与接口边界优化决策地图](https://github.com/lvivvde/RealmMesh/issues/109)作后续决策。未实施构建优化或接口重构。

主要证据是：两边串行冷构建约 5.5 分钟；代表性公共头修改都触发 33 次项目编译和 53 次链接；同模块实现修改只触发 1 次编译，仍有 53 次链接。依赖获取、首次重新配置后的第三方重编译、稳定增量构建和完整测试应分别计量。单纯移动文件目录无法消除这些工作。

## 快照与控制条件

两边使用同一个冻结快照：提交 `8252967c20ba8e061ff7d278eb1ca8fac611d88a`，叠加开始测量时的已跟踪工作区补丁。补丁 SHA-256 为 `19bfb4589d0f23d21b85a493427b100dfce75e594868bd0d23c0f7439867dbae`；发送给 Linux 的快照 tar SHA-256 为 `5b8a043832f0b0e89da220fa2a21fc9650fd17e2d036ce7acb0966f0810a61d6`。补丁涉及 AGENTS.md、TLS 客户端/平台实现、TLS 测试和 RealmJourney 测试，包含当时的调试代码。这不是纯提交版本；不能将测试失败归因于纯 HEAD，也不代表测量完成时工作区的新状态。

Linux 原检出为另一提交，未在该检出测量、拉取或覆盖改动；从本机传送冻结源码到独立临时目录。macOS 同样使用独立源码和构建树。增量探针向文件末尾追加不同注释，确实改变内容，测完恢复原始字节。原工作区和已有缓存未清理。测试只使用自动创建的隔离 etcd/MongoDB，未连接共享开发数据库。

| 条件 | macOS | Linux |
| --- | --- | --- |
| 环境 | macOS 27.0.1，arm64，Mac17,9 | Lima Ubuntu，aarch64，内核 7.0.0-34 |
| CPU / 内存 | 15 逻辑核，48 GiB | 8 vCPU，约 7.7 GiB，无 swap |
| 编译器 | Apple Clang 21.0.0 | GCC 15.2.0 |
| CMake / Make | 4.4.3 / 3.81 | 4.2.3 / 4.4.1 |
| 配置 | dev / Debug / Unix Makefiles | dev / Debug / Unix Makefiles |
| 并行与编译缓存 | 未传 --parallel，清除并行环境参数；无 ccache/sccache | 同左 |
| QUIC | 已启用，Homebrew libmsquic 2.6.2 | 已启用，MsQuic 2.6.1，显式指定 ARM64 路径 |
| OpenSSL | 3.6.5 | 3.5.5 |
| 隔离 fixture 工具 | etcd 3.6.14 / mongod 8.3.11 / mongosh 2.12.0 | etcd 3.6.14 / mongod 8.0.32 / mongosh 2.12.0 |
| 构建存储 | 本机临时目录，磁盘 | /tmp 为约 3.9 GiB tmpfs |
| 完整测试临时数据 | 本机临时目录 | 重试与脚本使用磁盘上的专用 TMPDIR |

两个环境共享物理宿主，编译与增量探针顺序运行；首次 Linux 依赖下载/配置与 macOS 失败冷构建有重叠，macOS 适配配置曾短暂与 Linux 构建重叠。硬件、编译器和存储不同，数字用于各平台后续同条件比较，不能据此判定平台性能优劣。

## 三类耗时

单位为秒。三次样本报告中位数及最小–最大值；冷构建、首次获取和完整验证各为一次，不能提供稳定性结论。所有时长包含记录工具开销。

| 场景 | macOS | Linux | 实际 CMake 编译 / 链接次数 |
| --- | ---: | ---: | --- |
| 新 FetchContent 源码获取 + 首次配置 | 100.97 | 125.33 | 0 / 0（launcher 捕获），获取与配置交织 |
| 已获取 FetchContent 源码、新构建产物的冷构建 | 335.76 | 328.40 | 771 / 74；770 / 73 |
| 热配置，3 次 | 1.37（1.34–1.71） | 0.98（0.98–1.01） | 0 / 0 |
| 首次热配置后的“无改动”构建 | 79.69 | 63.31 | 265 / 52；266 / 52，第三方重编译 |
| 稳定无操作构建，3 次 | 6.20（5.96–6.47） | 1.85（1.82–1.94） | 0 / 0 |
| 修改 lua_runtime.cpp，3 次 | 30.17（29.45–31.04） | 19.29（19.13–19.29） | 两边均 1 / 53 |
| 修改 lua_runtime.hpp，3 次 | 67.95（65.12–69.49） | 75.71（75.42–76.20） | 两边均 33 / 53 |
| Unit Test，3 次 | 8.50（8.35–9.59） | 4.37（4.37–4.39） | 独立测试，不编译 |
| 完整 CTest | 362.80 | 281.74 | 退出状态见下文 |
| 一键脚本：热配置 + 无操作构建 + 全部测试 | 350.68 | 289.27 | 退出状态见下文 |

“冷构建”仅保证构建产物为空、无编译缓存；没有清空 OS 文件页缓存。libsodium 通过 ExternalProject 在 build 阶段首次下载、配置、编译，故两边冷构建数字仍含它的获取成本。macOS 成功冷构建前的适配配置为 5.66 秒，复用失败构建已下载的 FetchContent 源码。未单独做“已获取 libsodium 的全量重建”，也没有安装编译缓存来测缓存命中；稳定无操作不是编译缓存命中重建。

## 编译、链接与等待范围

用 C/C++ compiler launcher 和 linker launcher 在实际调用时记录 argv、退出码与墙钟时间，不使用 compile_commands 条目数估算工作量。串行运行下可将记录的时间相加，但不等于整个 build 的全部阶段。

| 冷构建阶段的记录总时长 | macOS | Linux |
| --- | ---: | ---: |
| 第三方 CMake 编译 | 109.13 | 107.29 |
| 项目及测试 CMake 编译 | 128.20 | 173.22 |
| CMake 编译合计 | 237.32 | 280.51 |
| 捕获的可执行/共享库链接 | 3.90 | 17.70 |
| 总 build 墙钟 | 335.76 | 328.40 |

未捕获 libsodium 自己的 autotools 编译及静态库 ar 命令；build 还含下载/配置、目标遍历、归档和链接后的测试发现。macOS 代表性实现修改仅约 0.8 秒编译、3.1 秒被捕获链接，却有约 30 秒 build 墙钟，不能把余量全算作链接。生成的 service_host_test build.make 显示链接后执行 GoogleTest discovery（资产 postlink-discovery-evidence.json）；当前仅确认其存在，未独立计时归因。Linux 实现修改的捕获链接约 15.6 秒，确实占其约 19.3 秒等待的大部分。

冷构建中较慢的五个编译调用（单次）：

| 文件 | macOS | Linux |
| --- | ---: | ---: |
| `tests/cpp/tools/loadgen/loadgen_integration_test.cpp` | 2.49 | 5.15 |
| `tests/cpp/framework/service_host/service_host_test.cpp` | 2.16 | 4.24 |
| `framework/cluster/src/etcd_service_registry.cpp` | 2.13 | 4.43 |
| `framework/observability/src/logger.cpp` | 2.12 | 3.99 |
| `tests/cpp/framework/observability/logger_test.cpp` | 2.11 | 4.19 |

增量代表文件是 `framework/scripting/src/lua_runtime.cpp` 与 `framework/scripting/include/realmmesh/scripting/lua_runtime.hpp`。公共头包含 sol2，并暴露模板/sol 类型；实测头改动带来 33 个编译调用。具体重编译源码名单和全部 argv 在 stages.json 与逐调用 events 中；代码图使用 Verify 层级，相关 LuaRuntime 图与源码已核验；相关路径覆盖无记录缺口，慢文件名单直接来自编译调用而非图穷举；这只证明该代表头的传播范围，不是全库包含关系的完整审计。后续接口票需判断哪些消费者真正需要 sol2/模板，哪些只需稳定接口，同时分析 53 个链接消费者。不为每个类制造虚接口，也不预设一律 PIMPL。

## 环境失败与适配

macOS 默认首次配置成功，但首次 build 在约 205 秒后因 Abseil 链接符号失败，退出 2；678 次编译、2 次链接只是失败过程，不能作为成功冷构建。OpenSSL 检测提供 `/opt/homebrew/include`，该系统搜索路径在仓库固定 Abseil 头之前；预处理取到 Homebrew 的 `lts_20260817`，仓库构建库为 `lts_20250512`。只在测量目录覆盖 OPENSSL_INCLUDE_DIR 为 `/opt/homebrew/opt/openssl@3/include` 后，预处理选择正确版本并完成新产物冷构建。失败日志、独立重现、宏/符号/depfile 探针均保留，未修改项目源码来绕过。

Linux aarch64 用显式 MsQuic 路径和原机器已有 ARM64 protoc 35.0（WITH_PROTOC 覆盖）；冻结源码的 protoc 预编译分支未覆盖该 ARM64 环境。不是 CI x86_64 的原样验证，适配参数有记录。

首次重新配置后的无源码改动构建，两边分别额外编译 265/266 次，随后无操作和一键脚本不再编译。Linux 260 个去重源码的冷构建/额外编译 argv 全部相同（266 调用含跨目标重复），排除这些命令参数变化。生成 BSON/Mongo 版本头的 mtime 落在首次热配置，整数版本变成 0/0/0，VERSION_S 仍为 2.5.5；mongo-cxx-driver 将共享 BUILD_VERSION 缓存设为 0.0.0，mongo-c-driver 在已定义时不重新推导，提供了强的状态污染线索。尚未做最小反事实实验，不能声称这是唯一根因，也不能说每次配置都重编译。新决策票[确定第三方头文件与配置状态的隔离策略](https://github.com/lvivvde/RealmMesh/issues/116)据此建立。

Linux 首轮完整测试受 /tmp tmpfs 空间影响：构建目录约 2.8 GiB，Mongo 可用约 351 MB，低于所需约 524 MB，etcd 也报空间不足；该轮 56 项失败、约 347 秒作为环境失败保留，不纳入正常完整验证耗时。只将后续测试临时数据定向到独立磁盘 TMPDIR，构建目录仍在原 tmpfs。确认测试所有权后结束了三个失败遗留 mongod，未处理用户进程或删除用户缓存。

## 验证结果与解释边界

Linux：647 项测试，494 项 unit 连续三次全通过。磁盘临时数据完整运行与一键脚本各有两项失败：`HttpServerTest.DeferredResponseSurvivesClientHalfClose`、`RealmJourneyTest.CharacterLifecycleSurvivesReloginAndRealmRestart`。HTTP 单项三次复测为失败/通过/通过，存在波动；RealmJourney 原因尚未确定。本报告保存失败证据，不借测量任务修改网络/业务行为。完整 CTest 约 282 秒与脚本约 289 秒是执行结束耗时，不能作为全绿验收。脚本 0 编译 / 0 链接；当次 CTest 自报约 286 秒，余量为配置/无操作构建等步骤。

macOS：同一快照发现 647 项测试，494 项 unit 连续三次全通过；独立完整 CTest 的 647 项全部通过，约 363 秒。脚本结果如下。

macOS 原始输出：

```text
100% tests passed out of 494
Total Test time (real) =   9.59 sec
100% tests passed out of 647
Total Test time (real) = 362.79 sec
100% tests passed out of 647
Total Test time (real) = 339.25 sec
```

Linux 原始输出：

```text
271/647 Test #271: HttpServerTest.DeferredResponseSurvivesClientHalfClose .................................................***Failed    0.17 sec
620/647 Test #620: RealmJourneyTest.CharacterLifecycleSurvivesReloginAndRealmRestart ......................................***Failed   16.86 sec
99% tests passed, 2 tests failed out of 647
Total Test time (real) = 281.73 sec
271/647 Test #271: HttpServerTest.DeferredResponseSurvivesClientHalfClose .................................................***Failed    0.17 sec
620/647 Test #620: RealmJourneyTest.CharacterLifecycleSurvivesReloginAndRealmRestart ......................................***Failed   18.46 sec
99% tests passed, 2 tests failed out of 647
Total Test time (real) = 286.02 sec
```

完整验证保留所有现有测试入口；既有 CTest 跳过条件未改，实际结果以日志为准。未独立验证 CI x86_64 或 macOS QUIC 禁用配置；首次依赖下载受网络/共同宿主负载影响，不能精确分离下载与 CMake 检测。对冷构建和全套测试未重复三次；优化验收前应在选定环境和修复后的测试状态复测。

记录器的小编译探针中位附加约 2.5 ms/次，未开启事件写盘且曾与 Linux 测试重叠，只能说明量级，未扣除开销，也不是未插桩冷构建对照。maxrss_raw 在两平台单位不同，不直接比较；本报告未由它推断内存瓶颈。

## 可复现资产与后续决策输入

压缩资产以平台分别保存原始日志、每次实际编译/链接事件、配置适配、探针、快照元数据和测量脚本，附开始时的工作区补丁；不含源码构建树、数据库数据、TLS 私钥或认证材料。Linux environment.json 的 repository/head 描述原检出；实际被测版本应看 linux-adaptations.json 的快照指纹与本节快照说明，不能混用原检出的 599 项测试数。

- [macOS 原始资产](assets/build-baseline-2026-10-02/macos.tar.gz)
- [Linux 原始资产](assets/build-baseline-2026-10-02/linux.tar.gz)
- [文件指纹清单](assets/build-baseline-2026-10-02/manifest.json)

复现步骤：新目录解压资产，在另一个空目录用 git archive 取得本报告 HEAD，应用 working-tree.patch；编译 launcher.cpp（`c++ -O2 launcher.cpp -o launcher`）。运行 measure.py 的 configure、cold、quick、noop、tests、script 模式，提供 --source/--out/--launcher。按平台 adaptation JSON 替换绝对路径；macOS FETCHCONTENT_SOURCE_DIR 参数是失败诊断后复用源码的条件，干净初次配置可先只覆盖专用 OpenSSL include；Linux 提供 ARM64 protoc/MsQuic 和独立 fixture 路径，并将测试 TMPDIR 放在有充足空间的磁盘。三方版本仍由该提交固定。Linux harness 增加了 full-only/http 复测模式；日志列明实际命令。从新的 out 开始，避免重名事件混入。测试使用隔离数据库。

本票完成的是可复查基线和失败分类。后续票据分别决定工具/并行/缓存、第三方依赖隔离、公共头与实现边界、日常/完整验证入口，最后确定实施顺序及量化验收。候选包括减少公共头传播、缩小消费者链接范围、利用可用并行和依赖复用；收益不能在本票假定，也未因此启用 Unity/PCH 或拆目录。
