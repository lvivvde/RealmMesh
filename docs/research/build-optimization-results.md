# 构建优化：测量结果

关联[构建优化实施追踪](https://github.com/lvivvde/RealmMesh/issues/119)。口径以[实施顺序与验收约定](build-implementation-acceptance.md)与 [#115 决议](https://github.com/lvivvde/RealmMesh/issues/115#issuecomment-5954537886)为准；旧[build-optimization-rollout.md](build-optimization-rollout.md)只作历史。测量工具与场景定义见 [tools/build-bench](../../tools/build-bench/README.md)。各阶段按时间顺序追加；业务代码或测试合集变化产生新比较组，不拼接旧秒数。

## R0：冻结起点（[#120](https://github.com/lvivvde/RealmMesh/issues/120)）

### 条件

| 项 | 值 |
| --- | --- |
| 源码 | `4ffb58df9b8d20beec59f4b056d3e9c90859401f`，`git archive` 独立副本，无补丁 |
| 测量工具 | Mac：`e2df308`（measure.py sha256 `3ecb099c…79cf`）；Lima：`b4746f8`（`97acbbdd…425e`）。两者差别只在 CTest 4.x 汇总解析与 SIGTERM 收尾，不改变计时与场景；launcher.cpp 同为 `ac0a2b51…e0f1`，launcher 在各平台本机编译（Mac `64f28a77…`，Lima `b09628e8…`）。R0 之后 launcher.cpp 只修正了 `wait4` 非 EINTR 出错时的死循环，正常路径不变；measure.py 在 `024694c` 改为逐样本复位并新增私有头探针，R0 的改动类数据仍出自旧工具，见“异常、限制与历史对照” |
| 生成器 / preset | Unix Makefiles，`dev`（Debug），binaryDir `build/dev` |
| 并行 | 构建串行（不传 `--parallel`），CTest 串行；工具清除 `CMAKE_BUILD_PARALLEL_LEVEL`、`CTEST_PARALLEL_LEVEL`、`MAKEFLAGS` 等继承变量 |
| 编译缓存 | 无（ccache 未安装，未设 launcher 之外的 compiler launcher） |
| 来源准备 | FetchContent 源码由 `fetch` 场景一次获取后以 `FETCHCONTENT_SOURCE_DIR_<NAME>` 复用；libsodium 1.0.22 包预先下载并核对 SHA256 `adbdd8f1…3349`，冷入口前放入 ExternalProject 下载目录（hash 相符跳过下载），其 configure/make 仍在计时内 |
| 测试身份 | 649 个 CTest 用例，标签计时 unit 496、integration 153、lua 2（标签可重叠，以 CTest 实际 649 为准），两平台相同，无跳过；自动化只用夹具自起的隔离 etcd/MongoDB |
| 样本 | 冷入口、热完整验证各 3 组；短场景 1 次预热（不计）＋5 组；探针 token 变体 5 组＋注释变体 3 组＋恢复 1 次；`cpp-entry` 与探针的样本之间不复位（旧工具） |
| 能力 | Mac 与 Lima 都编入 QUIC（configure 行 `realm_network: QUIC transport enabled`） |

| 平台 | 机器 | 工具链 | 资源 |
| --- | --- | --- | --- |
| Mac（开发机） | arm64，15 逻辑核，48 GiB；macOS 27.0.1 | Apple clang 21.0.0，CMake/CTest 4.4.3，GNU Make 3.81，Python 3.13.15；Homebrew libmsquic、openssl@3 | 适配参数 `-DOPENSSL_INCLUDE_DIR=/opt/homebrew/opt/openssl@3/include`（前后一致） |
| Lima（同宿主 VM） | aarch64，8 CPU，7.73 GiB（无 cgroup 限额，VM `MemTotal` 即上限）；Ubuntu 26.04.1，Linux 7.0.0，glibc 2.43 | gcc 15.2.0，CMake/CTest 4.2.3，GNU Make 4.4.1，Python 3.14.4；MsQuic 取 `.tools/msquic` | 无 CMake 适配参数；`TMPDIR` 指向测量目录下的磁盘目录 |

Mac 与 Lima 共用宿主，按 Mac→Lima 先后测量，从未重叠。

### 正确性

两平台的 6 次完整 CTest（冷入口 3 次、热完整验证 3 次）都是 `100% tests passed out of 649`，没有失败或跳过；全部阶段退出码为 0。CTest 汇总行格式不同（Mac CTest 4.4 为 `100% tests passed out of 649`，Lima CTest 4.2 为 `100% tests passed, 0 tests failed out of 649`），工具 `d6039b7` 起两种都能解析，汇总由日志重新解析得出。

- HTTP 半关闭：`HttpServerTest.DeferredResponseSurvivesClientHalfClose` 在两平台 12 次完整运行中全部通过。修复提交 `14c19d0` 不是 `4ffb58d` 的祖先，但 `git diff 14c19d0 4ffb58d -- tests/cpp/framework/network/http/http_server_test.cpp` 为空，修复内容已在被测源码中。
- `RealmJourneyTest.CharacterLifecycleSurvivesReloginAndRealmRestart` 在两平台 12 次完整运行中全部通过，不需要开阻塞 bug。
- 两平台的 configure 都输出 `realm_network: QUIC transport enabled`，QUIC 路径在测试范围内。

### 结果

秒数是中位数（最小–最大）；噪声带为 max(极差÷中位数, 5%)。“总”是整个阶段端到端的墙钟，不由各步中位数相加；各步单独统计，只用于定位。

| 场景 | Mac n | Mac 秒，中位数（最小–最大） | Mac 噪声带 | Lima n | Lima 秒，中位数（最小–最大） | Lima 噪声带 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 首次获取：FetchContent 全新获取＋配置 | 1 | 251.99 | — | 1 | 125.93 | — |
| 首次获取：libsodium 包下载 | 1 | 15.30 | — | 1 | 15.38 | — |
| 完整冷入口 总 | 3 | 678.84（669.18–703.01） | 5.0% | 3 | 647.58（640.59–648.37） | 5.0% |
| 　configure | 3 | 4.98（4.81–6.12） | 26.4% | 3 | 2.08（2.00–2.18） | 8.4% |
| 　build（已备齐源码的全量构建） | 3 | 305.71（300.09–331.04） | 10.1% | 3 | 359.20（354.56–363.89） | 5.0% |
| 　test（完整 CTest） | 3 | 366.98（364.27–366.99） | 5.0% | 3 | 284.02（282.40–286.19） | 5.0% |
| 热完整验证 总 | 3 | 369.22（366.86–370.30） | 5.0% | 3 | 288.66（286.77–288.97） | 5.0% |
| 　build（无操作） | 3 | 7.26（7.19–7.39） | 5.0% | 3 | 2.25（2.17–2.54） | 16.8% |
| 　test（完整 CTest） | 3 | 360.51（358.31–361.71） | 5.0% | 3 | 285.69（283.93–285.76） | 5.0% |
| 默认 Unit，无改动 总 | 5 | 17.78（17.22–17.88） | 5.0% | 5 | 7.32（7.30–7.37） | 5.0% |
| 　build | 5 | 7.88（7.54–7.97） | 5.5% | 5 | 2.05（2.04–2.07） | 5.0% |
| 　test（`-L unit`） | 5 | 8.54（8.30–8.59） | 5.0% | 5 | 4.64（4.62–4.66） | 5.0% |
| 默认 Unit，`.cpp` token 改动 总 | 5 | 43.25（40.51–45.14） | 10.7% | 5 | 24.26（24.10–24.66） | 5.0% |
| 　build | 5 | 32.91（30.38–35.39） | 15.2% | 5 | 18.94（18.79–19.30） | 5.0% |
| 　test | 5 | 8.84（8.35–9.12） | 8.7% | 5 | 4.66（4.63–4.69） | 5.0% |

探针只计 build 步（之前是 ALL 构建的稳定状态）。token 变体是主指标，注释变体只用于和旧数据对照。旧工具每个变体都由原始字节派生，但样本之间不写回原始字节、不重建：token 第 N 次是从第 N−1 次变体的稳定状态改过来的（仍是真实 token 变化）；注释第 1 次是从 token 第 5 次改过来的，实际是“去掉 token、加注释”，第 2、3 次才是纯注释变化。编译/链接次数在两平台和所有样本中都一致。

| 探针（build 计时） | 变体 | Mac 秒 | Mac 噪声带 | Mac 编译/链接 | Lima 秒 | Lima 噪声带 | Lima 编译/链接 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `lua-cpp` | token（主） | 31.15（29.14–32.91） | 12.1% | 1/53 | 19.00（19.00–19.14） | 5.0% | 1/53 |
| `lua-cpp` | 注释（历史对照） | 32.96（29.43–35.08） | 17.2% | 1/53 | 19.02（19.00–19.05） | 5.0% | 1/53 |
| `lua-cpp` | 恢复 | 31.95 | — | 1/53 | 19.03 | — | 1/53 |
| `lua-hpp` | token（主） | 72.71（72.37–81.32） | 12.3% | 33/53 | 73.79（72.55–74.60） | 5.0% | 33/53 |
| `lua-hpp` | 注释（历史对照） | 76.36（74.94–83.79） | 11.6% | 33/53 | 72.41（72.40–74.36） | 5.0% | 33/53 |
| `lua-hpp` | 恢复 | 79.10 | — | 33/53 | 73.70 | — | 33/53 |
| `gateway-hpp` | token（主） | 57.88（55.89–60.61） | 8.1% | 28/20 | 64.67（64.53–64.78） | 5.0% | 28/20 |
| `gateway-hpp` | 注释（历史对照） | 57.18（56.13–57.21） | 5.0% | 28/20 | 64.95（64.14–65.38） | 5.0% | 28/20 |
| `gateway-hpp` | 恢复 | 58.03 | — | 28/20 | 64.90 | — | 28/20 |
| `player-data-hpp` | token（主） | 82.81（80.32–87.30） | 8.4% | 40/52 | 84.63（83.73–85.48） | 5.0% | 40/52 |
| `player-data-hpp` | 注释（历史对照） | 85.36（83.42–87.85） | 5.2% | 40/52 | 85.22（84.84–85.64） | 5.0% | 40/52 |
| `player-data-hpp` | 恢复 | 80.73 | — | 40/52 | 85.54 | — | 40/52 |
| `proto` | token（主） | 88.84（85.41–90.44） | 5.7% | 43/52 | 87.69（87.01–88.19） | 5.0% | 43/52 |
| `proto` | 注释（历史对照） | 87.06（86.36–87.88） | 5.0% | 43/52 | 88.78（88.00–88.91） | 5.0% | 43/52 |
| `proto` | 恢复 | 91.49 | — | 43/52 | 90.26 | — | 43/52 |

两平台在编译密集的探针（`lua-hpp`、`player-data-hpp`、`proto`）上相差不超过 3%；Mac 的完整构建短 15%，Lima 的 CTest 短 23%，Unit 入口 Lima 快一倍多。具体原因见下一节。

### 工作量与发现

**冷入口工作量**（cold-entry-1，3 次一致）：

| 项 | Mac | Lima |
| --- | ---: | ---: |
| 编译次数（依赖 / 项目） | 771（598 / 173） | 770（598 / 172） |
| 链接次数 | 74 | 73 |
| 编译墙钟合计 s | 215.1 | 314.3 |
| 链接墙钟合计 s | 3.6 | 19.6 |
| build 步内编译、链接之外 s | 81.4 | 25.3 |
| 单次编译 / 链接 RSS 峰值 MiB | 567 / 266 | 1280 / 561 |

项目源码差 1 个、链接差 1 个都来自平台源码：Mac 编 `event_loop_kqueue.cpp`、`tcp_platform_darwin.cpp`、`tools/detach/main.cpp`，并多链接 `realm_detach`；Lima 编 `event_loop_epoll.cpp`、`tcp_platform_linux.cpp`。

**编译、链接之外的时间。** build 步墙钟减去 launcher 记录的编译、链接合计：

| 阶段 | Mac build / 编译 / 链接 / 其余 s | Lima build / 编译 / 链接 / 其余 s |
| --- | --- | --- |
| `probe-lua-cpp-1` | 32.9 / 0.8 / 3.2 / 28.9 | 19.0 / 0.9 / 15.4 / 2.8 |
| `cpp-entry-1` | 30.4 / 0.7 / 3.0 / 26.7 | 18.8 / 0.8 / 15.1 / 2.9 |
| `probe-proto-1` | 88.8 / 53.3 / 3.2 / 32.4 | 87.7 / 69.2 / 15.3 / 3.2 |

- Lima 的单 `.cpp` 改动以 53 次链接为主（gcc 默认链接器约 0.3 s/次）。
- Mac 的单次链接只需约 0.06 s，但 53 次链接前后约有 27–32 s 花在编译、链接之外；无操作 Unit 构建（7.88 s 对 2.05 s）也是这样。候选来源包括测试可执行文件 POST_BUILD 的 `gtest_discover_tests` 用例枚举（`tests/cmake/test_helpers.cmake`）、Make 3.81 递归调度、`protoc` 等自定义命令，以及 libsodium 的 configure/make（只影响冷入口）。R0 没有逐项计时，归因留给 P2，这里只记录现象。

**链接扇出。** 只改 `lua_runtime.cpp` 一个文件也要重新链接 53 个产物（Mac 共 74 个），改 `player_data_store.hpp` 或 `envelope.proto` 要重链 52 个；`gateway_runtime.hpp` 的扇出最小，为 28 次编译 / 20 次链接。Lua 重头 `lua_runtime.hpp` 触发 33 次编译、53 次链接，名单见资产中 `probe-lua-hpp-*` 的 `compiled_sources` / `linked_outputs`：除 `lua_runtime.cpp` 与 `training_rule.cpp` 外，多数是各服务的配置加载、服务宿主与对应测试。哪些只用配置结果、应在 P3 后 0 次重编，由 P3 分类。

**token 与注释。** 两种变体的编译/链接名单相同，耗时差别都在噪声带内（注释组含上述 1 次非纯注释样本）。现行构建按文件时间戳失效，不区分是否只改了注释。

**生成版本头污染（#116）。** 冷入口后第一次重新配置会改写 mongo-c 的 `bson/version.h` 与 `mongoc-version.h`，随后第一次构建在 Mac 上重编 265 个依赖源码、重链 52 次，用时 75.01 s，在 Lima 上为 266 / 52，用时 70.49 s。Lima 多出的 1 个是 Linux 专用的 `mongoc-linux-distro-scanner.c`。第 2、3 轮重新配置不再改写，构建都是 0 编译（Mac 7.3–7.4 s，Lima 2.6 s）。热完整验证、Unit 入口与探针都发生在这次污染之后，不受它影响；但开发者首次重新配置时会付出这一代价。

**CTest 构成**（Mac cold-entry-1 标签计时）：integration 355.22 s / 153 个，unit 8.85 s / 496 个，lua 0.58 s / 2 个；Lima 分别为 281.37 s、4.68 s、0.01 s。完整验证的测试时间几乎全在 integration。

**Make 3.81 的整秒 mtime。** macOS `/usr/bin/make` 按整秒比较时间戳，与上次产物同一秒内的改动会漏编。工具在每次改写前等到下一个整秒；后续阶段若在 Mac 上测 Make 增量，也要这样处理。

### 资源

| 资源 | Mac | Lima |
| --- | ---: | ---: |
| 进程树 RSS 峰值 MiB（全部场景最大） | 638 | 1354 |
| 最低可用内存 MiB | 16058 | 5869 |
| swap 增长 MiB（最大） | 0 | 0 |
| 内存压力等级（最大） | 1 | — |
| OOM kill | — | 0 |

进程树 RSS 是每秒采样、整棵树合计的峰值，单次编译峰值见上一节。R0 串行构建下两平台都没有内存压力：Lima 最低可用 5869 MiB，高于 1 GiB 门槛，swap 无增长，无 OOM。并行度提高之后需要重新测量，不能用单次 RSS 乘 jobs 推算。

### CI（同提交，独立环境）

`4ffb58d` 只有一次 CI：[run 37014899940](https://github.com/lvivvde/RealmMesh/actions/runs/37014899940)（push 到 main，成功）。CI 用的也是 `dev` preset、Unix Makefiles、串行构建、串行 CTest、无缓存，但机器、工具链与负载都与本机不同，只作为独立环境记录，不与 Mac、Lima 配对。

| 步骤 | Linux（ubuntu-24.04，gcc，epoll，QUIC）s | macOS（macos-latest，clang，kqueue）s |
| --- | ---: | ---: |
| Install dependencies | 17 | 4 |
| Install MongoDB | 6 | 69 |
| Show toolchain | 0 | 11 |
| Configure（含 FetchContent 获取） | 14 | 70 |
| Build | 836 | 1034 |
| Test（完整 CTest） | 319 | 428 |
| Linux M1–M4 acceptance | 254 | — |
| job 总计 | 1451 | 1629 |

其他提交上的绿色运行（业务代码不同，只看量级）：

| run | 提交 | Linux Build / Test / M1–M4 / 总 | macOS Build / Test / 总 |
| --- | --- | --- | --- |
| 37011005130 | 不同提交 | 721 / 301 / 266 / 1329 | 794 / 342 / 1228 |
| 36979742757 | 不同提交 | 648 / 289 / 269 / 1254 | 684 / 313 / 1097 |

三次运行的 Build 在 Linux 上为 648–836 s（相差 29%），macOS 上为 684–1034 s。其中混有业务代码差异与 runner 波动，无法拆开。CI 的单次结果不能单独当作阶段收益或回归的证据。本 PR 只改工具与文档，构建输入与 `4ffb58d` 相同，它的 CI 分步耗时作为补充样本记在 [#130](https://github.com/lvivvde/RealmMesh/pull/130) 的评论中，并标明提交。

### 与已选目标的距离

门槛按各平台 R0 中位数（未四舍五入）换算并向严格方向截到 0.01 s，到时同平台、同条件配对比较；目标与判定口径以验收约定为准，下表只换算数字。

| 指标 | R0 Mac | Mac 门槛 | R0 Lima | Lima 门槛 |
| --- | ---: | ---: | ---: | ---: |
| 完整构建（冷入口 build 步）快 ≥20% | 305.71 | ≤244.56 | 359.20 | ≤287.36 |
| 默认 Unit 无改动（总）快 ≥30% | 17.78 | ≤12.44 | 7.32 | ≤5.12 |
| 默认 Unit `.cpp` 改动（总）快 ≥30% | 43.25 | ≤30.27 | 24.26 | ≤16.97 |
| Lua 重头（`lua-hpp` token）快 ≥30%，只用配置结果的消费者 0 次重编 | 72.71（33 编译） | ≤50.90 | 73.79（33 编译） | ≤51.65 |
| 热完整验证（总）回归 ≤5% | 369.22 | ≤387.68 | 288.66 | ≤303.09 |
| 兼容缓存重建 | 不适用（R0 无缓存，P5 建立） | — | — | — |

R0 的改动类数据来自不复位的旧工具；后续阶段在同平台取配对基线时用 `024694c` 起的逐样本复位工具，不拿 R0 改动类秒数直接判定。R0 的构建与测试都是串行。最终并行预算按验收约定为 Mac 8、Linux 2、CI 2，相应阶段在同平台以新条件重新取基线再配对，不拿串行 R0 直接比较。

### 异常、限制与历史对照

- **Mac 中途暂停。** Mac 测量在 `hot-full-3`（00:47）之后按用户要求暂停约 12 小时，次日 12:56 从 `unit-entry` 续跑。被打断的 `unit-entry-warmup` 只跑到 configure，半成品已删除；续跑重新预热，短场景都在续跑段内。暂停前后的机器、提交与源码副本相同。
- **Mac 的 `environment.json` 只有续跑段一条**（14:00:33，工具 `e2df308`）。第一段被 SIGTERM 中止时没有写出 environment，`b4746f8` 修复了这个问题，并补了测试。第一段的条件见各阶段日志与 `stages.json`，与续跑段相同。
- **Mac 负载。** 测量期间桌面应用照常运行（WindowServer、Claude、Codex、XprotectService 等），负载均值 1.5–6.3；还有一对 10 月 2 日 14:46 起的空闲 etcd、mongod 孤儿进程，不属于本次测量。Mac 的 configure（26.4%）与 `cpp-entry` build（15.2%）噪声带较宽，后续配对比较时以噪声带判定。Lima 负载约为 1，各项噪声带基本为 5%。
- **工具版本。** Mac 用 `e2df308`，Lima 用 `b4746f8`。两者计时与场景代码相同，差别只在 CTest 4.x 汇总解析（属后处理）与 SIGTERM 收尾。
- **改动类样本未逐样本复位（偏离验收约定第 5 条）。** 验收约定要求每次改动后恢复原始字节与稳定状态。R0 的 `cpp-entry` 与探针用的旧工具只在整组结束后恢复一次（`cpp-entry-restore`、`probe-*-restore`），影响见“结果”一节的说明。工具已在 `024694c` 修正；按新工具的重测只跑完 Mac 的 `cpp-entry` 一组就按决定取消，未采用，R0 保留旧数据。
- **私有头探针缺失。** 验收约定要求的私有头探针（`game/common/src/envelope_codec.hpp`，`edge_protocol.cpp`、`realm_protocol.cpp` 两个消费者）在 R0 没有数据，由后续阶段取同平台基线时首次测量。
- **未覆盖的组合。** 两平台本机都编入 QUIC；macOS 不带 QUIC（只有 TLS/TCP）的组合本机未测，由 CI 的 macOS job 覆盖构建与测试。Linux 的 M1–M4 验收不在本机测量范围，由同提交 CI 的 Linux job 覆盖（254 s，通过）。
- **libsodium。** libsodium 的 configure/make 不经 launcher，不计入编译次数，只体现在 build 步墙钟里。
- **历史对照（不可比）。** 旧协议下的 Mac 数据（同一提交，旧工具）为：冷构建 316.14 s、完整测试 369.83 s；注释探针 `lua-cpp` 26.86、`lua-hpp` 68.99、`gateway-hpp` 52.39、`player-data-hpp` 77.21、`proto` 78.44 s。旧工具的恢复步骤有缺陷，注释探针受到污染；一组 `cpp` 样本还被 Xcode 许可弹窗打断（exit 2/69）。这些数据只用来说明数量级，不参与任何判定。

### 资产

- [`r0-mac-samples.json`](assets/build-optimization-results/r0-mac-samples.json)、[`r0-lima-samples.json`](assets/build-optimization-results/r0-lima-samples.json)：逐次样本（含预热与恢复）、每步墙钟、编译/链接次数与合计、静态库归档次数、内存摘要、探针变体 hash、由日志重新解析的 CTest 结果、不超过 60 项时的编译源码与链接产物名单、`environment.json` 与 `summary.json`，由 `024694c` 的 `measure.py export` 从原始结果导出（计时与汇总和原始结果一致）。本机路径已替换为 `<bench>`。逐调用事件、原始日志与时间线体积较大，没有提交。
- 复现方法：按 [tools/build-bench](../../tools/build-bench/README.md) 的用法，用 `git archive 4ffb58d` 生成源码副本，加 `--scenario all`；Mac 附加上表的 OpenSSL 适配参数，Lima 附加 `--env TMPDIR=<磁盘目录>`。R0 不改代码，所以没有回滚步骤。

## P1：隔离第三方头来源与配置状态（[#121](https://github.com/lvivvde/RealmMesh/issues/121)）

P1 是正确性阶段，门槛是来源与配置状态，不设耗时目标；本节秒数都只作筛查，不参与判定。

### 条件

| 项 | 值 |
| --- | --- |
| 源码 | `fd4fa7721513104b6f75d0f0dc16858d19e63710`，`git archive` 独立副本；Lima 取推送后的分支（`git fetch` ＋ `git archive FETCH_HEAD`），不动 Lima 工作区 |
| 测量工具 | 同提交的 measure.py（sha256 `c272f92a…67b5`）、launcher.cpp（`1374833a…034b`）；launcher 在各平台本机编译（Mac `7c015b5f…`，Lima `19554d55…`） |
| 生成器 / preset | Unix Makefiles，`dev`（Debug），binaryDir `build/dev` |
| 并行 | 构建按验收约定的资源预算：Mac `--jobs 8`，Lima `--jobs 2`；CTest 串行 |
| 编译缓存 | 无 |
| 场景 | `fetch`（新目录全新获取＋配置）、`cold-entry` 1 组（全量构建＋完整 CTest）、`repro`（同目录连续 3 轮配置＋构建，首次重新配置计入） |
| 适配参数 | 两平台都没有 CMake 适配参数。Mac 测量进程去掉了 `OPENSSL_ROOT_DIR`，走 macOS 默认根；Lima 只附加 `--env TMPDIR=<磁盘目录>` |
| 测试身份 | 650 个 CTest 用例：R0 的 649 个加本阶段的 `DependencyIsolationTest`（integration） |

机器、操作系统与工具链同 R0。Mac 与 Lima 按 Mac→Lima 先后测量，从未重叠。

### 门槛验收

| 门槛 | Mac | Lima |
| --- | --- | --- |
| OpenSSL 来源 | `RealmMesh: OpenSSL 3.6.5 from /opt/homebrew/Cellar/openssl@3/3.6.5 (root /opt/homebrew/opt/openssl@3, macOS default (Homebrew openssl@3))`；编译命令只有 `-isystem /opt/homebrew/opt/openssl@3/include` | `RealmMesh: OpenSSL 3.5.5 from /usr (root none, FindOpenSSL search)` |
| macOS 实际使用固定版 Abseil | 是：`compile_commands.json` 中 `/opt/homebrew/include` 出现 0 次；依赖文件里的 `absl/base/config.h` 指向复用的 `absl-src`；开发目录 `nm` 只见 `lts_20250512` 符号（Homebrew 为 20260817） | 不适用（无系统 Abseil 混入，`-I/usr/include` 0 次） |
| 新目录连续 3 轮配置＋构建，第三方 0 次额外编译 | 3 轮编译均为 0，链接均为 0 | 3 轮编译均为 0；第 1 轮有 1 次静态库归档＋52 次重链，见“异常与限制” |
| 生成版本头 hash 与 mtime 稳定 | 7 个版本头 3 轮均未变（sha256 与 `mtime_ns`） | 同左 |
| Mongo 版本头 | `BSON_VERSION_S`、`MONGOC_VERSION_S` 均为 `"2.5.5"` | 同左 |
| 缓存不残留通用键 | `CMakeCache.txt` 中无 `BUILD_VERSION`、`WITH_PROTOC` | 同左 |
| 完整 CTest | `100% tests passed out of 650` | `100% tests passed, 0 tests failed out of 650` |
| QUIC | `realm_network: QUIC transport enabled` | 同左 |

R0 在冷入口后第一次重新配置会改写 mongo-c 版本头，随后重编 265（Mac）/ 266（Lima）个依赖源码；P1 下这一代价消失，Mac 第 1 轮构建 2.52 s、0 编译。

### 迁移验证（Mac 既有 `build/dev`）

在开发机原有构建目录上逐项确认旧缓存的处理（不计时）：

- 旧缓存带 `BUILD_VERSION:STRING=0.0.0`：配置失败并给出 `-UBUILD_VERSION` 命令；按提示重新配置后两个 Mongo 版本头为 2.5.5，此后重新配置＋构建反复为 0 编译。
- `-DOPENSSL_INCLUDE_DIR=/opt/homebrew/include`（共享根）：配置失败并点名该项；`-UOPENSSL_INCLUDE_DIR` 后恢复。
- 缓存中的 `WITH_PROTOC`：配置失败并给出 `-UWITH_PROTOC -DREALMMESH_PROTOC_EXECUTABLE=…`；`REALMMESH_PROTOC_EXECUTABLE` 指向 protoc 35.0 时通过，指向 `/usr/bin/true` 时被拒绝。

### 筛查耗时（不判定）

| 场景 | Mac（8 jobs）s | Lima（2 jobs）s |
| --- | ---: | ---: |
| 首次获取：FetchContent 全新获取＋配置 | 272.47 | 323.05 |
| 首次获取：libsodium 包下载 | 见下 | 5.82 |
| 冷入口 总 | 427.76 | 457.67 |
| 　configure | 5.83 | 1.83 |
| 　build | 54.38 | 171.85 |
| 　test（完整 CTest） | 367.54 | 283.99 |
| repro 配置（3 轮） | 1.49 / 1.44 / 1.48 | 0.63 / 0.63 / 0.62 |
| repro 构建（3 轮） | 2.52 / 2.60 / 2.47 | 10.23 / 1.16 / 1.11 |

冷入口编译 771 / 770 次（依赖 598，项目 173 / 172），链接 74 / 73 次，与 R0 相同。并行度与 R0 不同，build 步不与串行 R0 比较；按验收约定，并行条件下的同平台基线由后续阶段重新取。资源：Mac 进程树 RSS 峰值 2686 MiB，最低可用 20455 MiB；Lima 峰值 1717 MiB，最低可用 4359 MiB（高于 1 GiB 门槛）；两平台 swap 无增长，Lima 无 OOM。

### 异常与限制

- **Lima 第 1 轮构建重链。** `repro-build-1` 没有编译，但重新归档了 `libabsl_graphcycles_internal.a` 并重链 52 个产物（10.23 s）；第 2、3 轮完全为 0。归档的唯一输入 `graphcycles.cc.o` 仍是冷入口时的文件（mtime 未变），规则依赖的 `build.make`、`link.txt` 也未被重新配置改写，版本头不变，所以触发点是冷入口留下的归档比目标文件旧，不是配置状态。归档不经 launcher，冷入口时的归档 mtime 没有记录，具体原因未确认；Mac 未出现。R0 的 Lima 第 1 轮因版本头污染重编 266 个源码，掩盖了同类现象，无法对照。门槛（第三方 0 次额外编译、版本头稳定）不受影响，原因另行跟踪。
- **Mac libsodium 下载 503。** Mac 的 `fetch-sodium` 遇到 GitHub 503 失败；`fetch-configure` 已完成（272.47 s，退出码 0）。随后手动下载同一 URL、核对 SHA256 `adbdd8f1…3349` 与 `third_party/sodium` 的固定值一致，放回依赖目录，以 `--deps-dir` 复用这次获取的源码另跑冷入口与 repro。因此 Mac 资产只含冷入口与 repro，下载耗时无数据。
- **CMake 3.20 未实跑。** 最低版本 3.20 下 CMP0126 为 OLD；契约测试以 `cmake_policy` 分别设 OLD/NEW 重放上游语句，两平台本机与 CI 的 CMake 都高于 3.20。
- **Linux 不指定根时没有旧缓存选择检查。** 未设 `OPENSSL_ROOT_DIR` 时不知道“应在哪个根”，只检查 FindOpenSSL 结果的布局（专用头目录、头与库同一安装），混装仍会被拒绝。
- **macOS 默认根不进缓存。** 默认根是普通变量，`environment.json` 的缓存项里看不到；以配置日志中的 `RealmMesh: OpenSSL` 状态行为准。
- **macOS 不带 QUIC 的组合**本机未测，由 CI 的 macOS job 覆盖。

### 资产

- [`p1-mac-samples.json`](assets/build-optimization-results/p1-mac-samples.json)（冷入口＋repro）、[`p1-lima-samples.json`](assets/build-optimization-results/p1-lima-samples.json)（fetch＋冷入口＋repro）：由同提交的 `measure.py export` 导出，本机路径替换为 `<bench>`。
- 复现：`git archive fd4fa77` 生成源码副本，`--scenario fetch --scenario cold-entry --scenario repro --long-samples 1`，Mac 加 `--jobs 8`，Lima 加 `--jobs 2 --env TMPDIR=<磁盘目录>`；两平台都不需要 CMake 适配参数。

## P2a：统一预设、构建目录信息与 `--preset` 入口（[#122](https://github.com/lvivvde/RealmMesh/issues/122)）

P2a 是正确性阶段，门槛是“全部目录消费者迁移完毕，启动与验收不误用旧 `build/dev`”，不设耗时目标。生成器不变（`dev` 与新增的 `dev-make` 都是 Unix Makefiles），Ninja 与切默认留给 P2b/P4；本节秒数只用来确认入口没有引入额外开销。

### 条件

| 项 | 值 |
| --- | --- |
| 前版本 | `16d1a14`（main，P1 之后），入口固定读 `build/dev` |
| 后版本 | 分支 `feat/p2a-preset-build-dir`：Mac 冷入口 `e13bcf1`，此后各项 `448334f`（评审修正，只改脚本与测试）；Lima 冷入口 `448334f`，此后各项 `82fdab2`（只改测试匹配，见“异常与限制”） |
| 源码 | Mac 用开发仓库本身（新建 `build/dev-make`；`build/dev` 为既有目录）；Lima 从推送后的分支做本地 `git clone` 到独立目录，不动 Lima 工作区，`.tools` 以符号链接复用 |
| 预设 | `dev`（`build/dev`）、`dev-make`（`build/dev-make`），均继承隐藏基 `realmmesh-base`（Debug、导出编译数据库、`REALMMESH_PRESET=${presetName}`） |
| 并行 | `CMAKE_BUILD_PARALLEL_LEVEL`：Mac 8，Lima 2；CTest 串行 |
| 工具 | Mac CMake/CTest 4.4.3，Lima 4.2.3；编译缓存无；机器同 R0 |
| 测试身份 | 651 个 CTest 用例：P1 的 650 个加本阶段的 `BuildDirScriptTest.PresetEntriesResolveRecordedBuildDirs`（integration） |

Mac 与 Lima 先后进行，计时项从未重叠。

### 门槛验收

| 检查 | Mac | Lima |
| --- | --- | --- |
| 新目录 `./scripts/build.sh --preset dev-make`（配置＋全量构建＋完整 CTest） | 退出 0；`realm_build_dir: preset dev-make -> …/build/dev-make`；编译 771、链接与归档 190（与 P1 冷入口相同）；`100% tests passed out of 651` | 编译 770、链接与归档 189（与 P1 相同）；首轮 650/651，唯一失败是新测试自身的匹配问题，修正后整套重跑 `100% tests passed, 0 tests failed out of 651` |
| 登记与目录身份 | `build/.build-dirs/{dev,dev-make}.txt` 与各目录的 `realmmesh-build-dir.txt` 互相对应，source_dir 为本仓库 | 同左（source_dir 为独立副本） |
| 编译数据库 | 仓库根 `compile_commands.json` 随 `--preset dev-make` 指向 `build/dev-make/…`，随 `--preset dev` 改回 `build/dev/…` | 指向 `build/dev-make/…` |
| 既有 `build/dev` 迁移（`./scripts/build.sh --preset dev`） | 重新配置 1.7 s 写入登记，0 编译，完整 CTest 651/651 | 不适用（独立副本另配 `dev`：配置 68 s、构建 167 s、编译 770） |
| 接入验收按所选预设 | `run-macos-login-acceptance.sh --preset dev-make`：4 组 PASS，报告写到 `build/dev-make/acceptance/`，`build/dev/acceptance/` 仍是 10-01 的旧文件 | `REALMMESH_LINUX_ACCEPTANCE_SKIP_BUILD=1 run-linux-login-acceptance.sh --preset dev-make`：Transport、M1–M4 共 5 组 PASS，报告写到 `build/dev-make/acceptance/`，副本中不存在 `build/dev/acceptance/` |
| 服务启动、watch、派生用户预设、误用拒绝 | 由 `BuildDirScriptTest` 与更新后的 `DevServicesScriptTest` 覆盖，两平台都通过：未配置时 build-dir / test-watch / dev-services / dev-all-in-one 都失败并给出配置命令，不起进程；`dev-local`（继承 `dev`）经 `build.sh` 走完配置、构建、测试；目录身份不符、来自另一棵源码树、缓存缺失均被拒绝；两个预设共用 binaryDir 时配置期告警、解析失败 | 同左 |
| QUIC | `realm_network: QUIC transport enabled` | 同左 |

### 入口开销（前后版本）

同一 `build/dev`（无改动），旧版 `test-watch.sh --once`（`16d1a14`，固定 `build/dev`）与新版（按 `--preset dev` 解析）各预热 1 次后交替运行 5 次；工作量相同：无操作 ALL 构建＋`ctest -L unit`（496 用例）。

| 场景 | Mac 中位数 s（极差） | Lima 中位数 s（极差） |
| --- | ---: | ---: |
| 前：`test-watch --once` | 9.645（9.573–9.973） | 5.610（5.595–5.683） |
| 后：`test-watch --once`（默认 `--preset dev`） | 9.654（9.613–9.763） | 5.637（5.625–5.699） |
| 解析一次构建目录（`build-dir.sh --preset dev`） | 0.037（0.036–0.038） | 0.015（0.014–0.016） |

前后差 9 ms / 27 ms，在各自极差之内；解析本身是几十毫秒的 sed 读取。

### 筛查耗时（不判定）

| 场景 | Mac（8 jobs）s | Lima（2 jobs）s |
| --- | ---: | ---: |
| `build.sh --preset dev-make` 新目录 总（含 FetchContent 获取） | 510 | 504 |
| 　其中配置（CMake 自报） | 82.9 | 57.7 |
| 　其中完整 CTest | 371.51 | 286.88 |
| 既有 `build/dev` 上 `build.sh --preset dev` 总 | 377 | — |
| 接入验收 `--preset dev-make` | 126 | 222（跳过构建） |

### 异常与限制

- **Lima 首轮新测试失败。** CMake 按宽度折行警告文本，Lima 的临时目录路径更长，折行断点落在被匹配的句子里（`was previously` 后换行），Mac 未出现。`82fdab2` 改为比较前压缩空白，Lima 上单跑与整套重跑均通过；产品行为无变化。
- **Lima 报告标注 dirty。** 独立副本里 `.tools` 是符号链接，`.gitignore` 的 `.tools/` 只匹配目录，故 Linux 验收报告的工作树状态为 dirty；源码文件无改动。
- **`tools/build-bench/measure.py` 未迁移。** 它以显式 `--build-dir`（缺省 `build/<preset>`）测量，需要与 R0/P1 同口径；对仓库预设这一缺省与登记一致。
- **文档里的 `build/dev`。** `docs/operations` 运行手册与历史 `docs/plans` 仍写 `build/dev`，`dev` 预设仍映射到它，P4 切默认时一并复查。
- **只有继承 `realmmesh-base` 的预设会被登记。** 不继承它的用户预设可以配置，但脚本入口找不到其构建目录，解析失败时会说明原因。
- **CMake 3.20 未实跑**，两平台本机与 CI 的 CMake 都更高；`${presetName}` 宏为预设格式 v2 已有。

### 资产

- [`p2a-mac-samples.json`](assets/build-optimization-results/p2a-mac-samples.json)、[`p2a-lima-samples.json`](assets/build-optimization-results/p2a-lima-samples.json)：各入口的命令、提交、退出码、计数与 CTest 结果，以及前后入口与解析的逐次样本；本机路径替换为 `<repo>` / `<bench>`。

## P2b：快速入口与并行预算（[#125](https://github.com/lvivvde/RealmMesh/issues/125)）

P2b 新增日常反馈入口 `scripts/test-fast.sh`（配置＋只构建 Unit 聚合目标＋Unit 用例 4 路并行），统一编译 jobs 预算，并把完整验证与 CI 的 CTest 固定为 `-j 1`。门槛是主无操作与真实 `.cpp` 编辑两个反馈场景各快至少 30%。实际选择与失败行为见[入口决策的实施记录](build-test-entry-decisions.md#实施记录125)。生成器仍是 Unix Makefiles，Ninja 链接池留给 P4。

### 条件

| 项 | 值 |
| --- | --- |
| 前版本（before） | `06c75b4`（main，P2a 之后），R0 同款连续流程：`cmake --preset dev`＋ALL 构建＋串行 `ctest -L unit`（`unit-entry` / `cpp-entry` 场景） |
| 后版本（after） | `82b52ee`（分支 `feat/p2b-test-fast-budget`），`scripts/test-fast.sh --preset dev`，编译 jobs 与测试 jobs 都取脚本缺省（`fast-entry` / `fast-cpp-entry` 场景） |
| 编译并行 | 按验收约定在新条件下重新取基线，不与串行 R0 直接比较：before 显式 `--jobs`，Mac 8、Lima 2；after 由预算得出，同为 Mac 8（`min(15 CPUs, 8, 48 GiB)`）、Lima 2（`min(8 CPUs, 8, 7.73 GiB)`），每个样本日志的 `build jobs:` 行可查 |
| 测试并行 | before 串行；after 默认 4 |
| 工作量 | 两侧都是同一组 496 个 Unit 用例（与 R0、P2a 相同），全部通过、无跳过。`.cpp` 场景改 `framework/scripting/src/lua_runtime.cpp` 的 token，每样本 1 次编译；链接 before 53、after 40（见“工作量差异”） |
| 源码 | 两侧各一份 `git clone --no-checkout` 的独立副本，检出上表提交，`.tools` 以符号链接复用。Lima 从本机工作仓库克隆，再从 GitHub 取推送后的分支提交，不动 Lima 工作区 |
| 准备（不计时） | 同提交的 `measure.py --scenario fetch` 获取一次依赖，两侧都以 launcher 与 `FETCHCONTENT_SOURCE_DIR_*` 复用它配置，并先做一次 ALL 构建 |
| 测量工具 | `82b52ee` 的 measure.py（sha256 `8a24dbcc…acf2`），两侧共用；launcher.cpp `1374833a…034b`，本机编译（Mac `a1b6b957…`，Lima `19554d55…`） |
| 编译缓存 | 无 |
| 采样 | 每侧预热 1 次后交替 5 组：奇数组 before 先，偶数组 after 先；每次调用只跑 1 个样本（`--samples 1 --sample-start N --no-warmup`）。`.cpp` 场景每样本后复位原始字节并重建（`*-reset-N`，不计入判定） |
| 工具链 | Mac CMake/CTest 4.4.3、GNU Make 3.81、Apple clang 21.0.0；Lima CMake/CTest 4.2.3、GNU Make 4.4.1、GCC 15.2.0 |

Mac 与 Lima 先后进行：两侧准备完成后先测 Mac，此时 Lima 空闲；Mac 的完整验证结束后再测 Lima，此时 Mac 不跑构建与测试。计时从未重叠。

### 门槛验收

降幅为 `1 − 中位数(after) ÷ 中位数(before)`。

| 平台 | 场景 | before 中位数 s（极差） | after 中位数 s（极差） | 降幅 | after 更快的组数 | R0 目标（≥30%） |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| Mac | 无改动 | 13.21（12.56–13.42） | 6.50（6.23–6.59） | 50.8% | 5/5 | ≤12.44，达成 |
| Mac | `.cpp` token 改动 | 14.87（14.70–15.83） | 8.04（7.73–8.27） | 45.9% | 5/5 | ≤30.27，达成 |
| Lima | 无改动 | 6.51（6.38–6.85） | 2.96（2.95–3.05） | 54.5% | 5/5 | ≤5.12，达成 |
| Lima | `.cpp` token 改动 | 16.65（16.58–16.82） | 8.45（8.30–8.92） | 49.2% | 5/5 | ≤16.97，达成 |

逐组降幅如下（奇数组 before 先）：

| 组 | Mac 无改动 | Mac `.cpp` | Lima 无改动 | Lima `.cpp` |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 50.1% | 47.8% | 57.0% | 49.8% |
| 2 | 51.2% | 47.2% | 54.2% | 47.2% |
| 3 | 48.2% | 45.4% | 54.8% | 46.2% |
| 4 | 53.5% | 48.6% | 53.1% | 49.8% |
| 5 | 48.3% | 44.0% | 53.1% | 50.0% |

四个场景对同条件配对基线都降 44% 以上，5 组方向全部一致，差距远大于噪声带（5.0–7.6%）。after 的中位数也都低于 R0 换算出的绝对门槛。

### 分段与工作量差异

各段为中位数，单位 s。before 是 measure.py 的步骤墙钟，after 是 `test-fast.sh` 的自报分段。

| 平台 / 场景 | configure 前 → 后 | build 前 → 后 | test 前 → 后 |
| --- | --- | --- | --- |
| Mac 无改动 | 1.46 → 1.35 | 2.56 → 2.52 | 9.19 → 2.36 |
| Mac `.cpp` | 1.43 → 1.31 | 4.94 → 4.31 | 8.55 → 2.19 |
| Lima 无改动 | 0.63 → 0.65 | 1.18 → 1.09 | 4.68 → 1.18 |
| Lima `.cpp` | 0.67 → 0.64 | 11.29 → 6.44 | 4.73 → 1.18 |

- **测试 4 路并行是两个场景的共同来源。** Unit 用例由串行改为 4 路后，Mac 从约 9 s 降到约 2.3 s，Lima 从约 4.7 s 降到约 1.2 s。
- **只构建 Unit 范围主要节省链接。** `.cpp` 改动后，ALL 要重链 53 个产物；Unit 聚合目标只重链 40 个 Unit 测试。少掉的 13 个是 `realm_mesh`、`realm_mesh_loadgen` 和 11 个 integration 测试。Lima 只有 2 路编译，链接占比高，build 段因此由 11.29 s 降到 6.44 s；Mac 8 路并行下差别只有 0.6 s。
- **无改动时 build 段两侧相当。** 两侧都只做 Make 的时间戳检查，少检查 13 个非 Unit 产物省下的时间在 0.1 s 以内。
- **R0 是串行编译**（Mac 无改动 build 7.88 s，`.cpp` 32.91 s），所以 before 已经吃到了预算并行的收益。按验收约定，判定只用上表的同条件配对；R0 绝对门槛列只作对照。

### 正确性与稳定性

| 检查 | Mac | Lima |
| --- | --- | --- |
| 完整验证 `./scripts/build.sh`（after 副本，预算 jobs，ALL＋完整 CTest `-j 1`） | 退出 0，397 s；`100% tests passed out of 652`（P2a 的 651 加本阶段的 `TestFastScriptTest`，integration） | 退出 0，301 s；`100% tests passed, 0 tests failed out of 652` |
| Unit 4 路重复 20 次（`ctest --preset dev -L '^unit$' -j 4`） | 0 次失败；测试时间 2.14–2.32 s | 0 次失败；1.15–1.23 s |
| `TestFastScriptTest`（含于上面的完整 CTest） | 通过；本机有 fswatch，轮询与 fswatch 两种模式都跑 | 通过；本机没有 inotifywait，只跑轮询模式 |
| QUIC | `realm_network: QUIC transport enabled` | 同左 |
| 预算行 | `build jobs: 8 (from budget; budget 8 = min(15 CPUs, 8, memory 48.00 GiB))` | `build jobs: 2 (from budget; budget 2 = min(8 CPUs, 8, memory 7.73 GiB))` |

资源：进程树 RSS 峰值在 Mac 为 714 MiB（`.cpp` 复位重建），在 Lima 为 783 MiB。最低可用内存 Mac 为 18053 MiB，Lima 为 2646 MiB，高于 1 GiB 门槛。两平台 swap 都没有增长，Lima 无 OOM。

CI 的 workflow 同步改为 `REALMMESH_CI_BUILD_JOBS: 2`、`--parallel` 与 `ctest -j 1`，以本 PR 的 CI 结果为准。

### 异常与限制

- **4 路 Unit 不是完整验证。** 快速入口只覆盖 496 个 Unit 用例；完整验证仍是 `build.sh`，即 ALL 加 652 个用例串行。4 路 Unit 秒数不和任何完整测试秒数比较。
- **before 的 `ctest -L unit` 没有加锚点**，那是 R0 场景的原样命令。当前标签下它选中的仍是同一组 496 个用例，后版本入口一律用 `^unit$`。
- **after 侧 `environment.json` 的 `jobs` 记为 `serial`。** 这个字段只表示 measure.py 没有传 `--jobs`；实际 jobs 由脚本按预算决定，记在每个样本的 `build_jobs` 与日志里。
- **准备阶段的配置不计时。** `fast-*` 场景每次调用前先用标准参数配置一次（launcher、复用依赖），记为预热组的 `*-setup-*` 阶段，不进统计。原因是 `test-fast.sh` 只跑 `cmake --preset`，不带 launcher 参数，靠缓存保留。
- **Mac 负载。** 测量期间桌面应用照常运行。加上测量本身，负载均值为 5–15。各组噪声带为 5.0–7.6%。
- **Lima 环境。** 另有一个与本项目无关、空闲的 qemu 进程（CPU 约 0.7%），以及几乎写满的 tmpfs `/tmp`。准备与测量都把 `TMPDIR` 设到磁盘目录，没有动它们。
- **Ninja 与链接池未做**，留给 P4；外部 libsodium 构建固定 `make -j1`，并断开顶层 jobserver（见实施记录）。
- **CMake 3.20 未实跑**，两平台本机与 CI 的 CMake 都更高。3.20 下 `gtest_discover_tests` 拆平列表的问题，靠生成的标签脚本绕开。

### 资产

- [`p2b-mac-samples.json`](assets/build-optimization-results/p2b-mac-samples.json)、[`p2b-lima-samples.json`](assets/build-optimization-results/p2b-lima-samples.json)：内容包括：
  - `pairs`：逐组配对与降幅；
  - `groups`：四组 `measure.py export` 结果，含每次调用的 `environment`、逐样本的每步墙钟、`fast_times` 自报分段、`build_jobs`、编译与链接次数、链接产物名单、内存摘要与汇总。
  - 本机路径都替换为 `<bench>`；原始日志与时间线体积较大，没有提交。
- 复现：两侧按上表提交各做一份克隆并链接 `.tools`，准备一次 `--scenario fetch`，以复用依赖的配置做一次 ALL 构建，然后各预热 1 次，再交替调用 `measure.py run`。before 用 `--scenario unit-entry` 或 `cpp-entry` 加 `--jobs 8` / `--jobs 2`；after 用 `--scenario fast-entry` 或 `fast-cpp-entry`，参数为 `--samples 1 --sample-start N --no-warmup`（见 [tools/build-bench](../../tools/build-bench/README.md)）。Lima 另外附加 `TMPDIR=<磁盘目录>`。

## P3a：分离配置 DTO 与 Lua 解析入口（[#126](https://github.com/lvivvde/RealmMesh/issues/126)）

P3a 把各模块配置头里的 `parse(sol::table)` 移到显式的 Lua 解析入口 `*_config_lua.hpp`，普通配置头不再带 sol2，gateway、login_verify、queue、service_host 对 `realm_scripting` 改为 PRIVATE。做法与取舍见[接口边界决策的实施记录](interface-boundaries-proposal.md#实施记录126)。本阶段门槛是 `lua_runtime.hpp` 扰动的编译次数下降、列出真实直接 Lua 使用者，以及双平台完整验证；“快 ≥30%、只用配置结果的消费者 0 次重编”在整个 P3（P3a＋[#127](https://github.com/lvivvde/RealmMesh/issues/127) P3b＋[#128](https://github.com/lvivvde/RealmMesh/issues/128) P3c）结束时判定，本节的降幅只记录、不判定。

### 条件

| 项 | 值 |
| --- | --- |
| 前版本（before） | `4f2a8a9`（main，P2b 之后） |
| 后版本（after） | `0caeeb0`（分支 `feat/p3a-config-lua-parse`，本阶段的代码提交；其后的提交只改测量工具与文档） |
| 场景 | `probes --probe lua-hpp`：改 `framework/scripting/include/realmmesh/scripting/lua_runtime.hpp` 的 token，只计 build 步；每样本后复位原始字节并重建（`*-reset-N`，不计入）。注释变体不跑（`--comment-samples 0`） |
| 生成器 / preset | Unix Makefiles，`dev`（Debug），binaryDir `build/dev` |
| 并行 | 两侧相同：Mac `--jobs 8`，Lima `--jobs 2`，与 P2b 的预算一致 |
| 编译缓存 | 无（OFF） |
| 源码 | Mac 两侧各一份 `git archive` 副本；Lima 两侧各一份本地 `git clone` 并检出上表提交，不动 Lima 工作区。`.tools` 以符号链接复用 |
| 准备（不计时） | 每平台一次 `measure.py --scenario fetch` 获取依赖；两侧都以 launcher 与 `FETCHCONTENT_SOURCE_DIR_*` 复用它配置，再做一次 ALL 构建 |
| 测量工具 | `6d92abd` 的 measure.py（sha256 `368bd1c7…aac`），两侧共用；这一提交让 `probes` 场景也遵守 `--sample-start` / `--no-warmup`，P3a 起用于交替配对。launcher.cpp `1374833a…034b`，本机编译（Mac `7c015b5f…`，Lima `19554d55…`） |
| 采样 | 每侧预热 1 次后交替 5 组：奇数组 before 先，偶数组 after 先；每次调用 1 个样本（`--samples 1 --sample-start N --no-warmup`） |
| 工具链 | Mac CMake 4.4.3、GNU Make 3.81、Apple clang 21.0.0；Lima CMake 4.2.3、GNU Make 4.4.1、GCC 15.2.0 |

Mac 与 Lima 先后进行：两平台准备完成后先测 Mac，此时 Lima 空闲；Mac 的完整验证与重复测试结束后再测 Lima，此时 Mac 不跑构建与测试。计时从未重叠。

### 门槛验收

| 门槛 | Mac | Lima |
| --- | --- | --- |
| `lua_runtime.hpp` 扰动的编译次数下降 | 33 → 16，5 组一致 | 33 → 16，5 组一致 |
| 链接次数 | 53 → 54：多出的 1 次是本阶段新增的 `config_headers_test`，其余 53 个产物名单相同 | 同左 |
| 普通配置头不带 sol2 | `config_headers_test` 编译通过（含 `SOL_HPP` / `SOL_FORWARD_HPP` 的 `#error` 守卫） | 同左 |
| 完整验证 `./scripts/build.sh`（ALL＋完整 CTest `-j 1`） | 退出 0；`100% tests passed out of 653`（P2b 的 652 加本阶段的 `ConfigHeadersTest`，unit）。首轮有 1 例失败，见“异常与限制” | 退出 0；`100% tests passed, 0 tests failed out of 653`（CTest 293.27 s），`build jobs: 2` |
| 指名测试重复 3 次 | 3 次都是 `100% tests passed out of 45` | 3 次都是 `100% tests passed, 0 tests failed out of 45` |
| QUIC | `realm_network: QUIC transport enabled` | 同左 |

指名测试为 `LuaRuntimeTest`、`ConfigsLoadSmokeTest`、`GatewayLoginConfigTest`、`LayeredConfigLoaderTest`、`LayeredConfigTest`、`GatewayConfigLoaderTest`（`gateway_server_test`）、`TrainingRuleTest`、`TrainingRuleFileTest`、`ConfigHeadersTest` 九组，共 45 个用例。

### 重编名单

两平台名单相同。前版本的 33 次里，下列 17 个源码在后版本不再重编：

- 配置与服务实现：`player_data_config.cpp`、`login_verify_config.cpp`、`login_verify_service.cpp`、`queue_config.cpp`、`queue_service.cpp`、`realm_config.cpp`、`mesh_host.cpp`、`service_frame.cpp`；
- 测试：`layered_config_loader_test`、`mesh_host_test`、`mesh_host_e2e_test`、`mode2_test`、`service_host_test`、`gateway_server_test`、`login_verify_service_test`、`queue_service_test`、`loadgen_integration_test`。

后版本仍重编的 16 个分两类：

| 类别 | 源码 | 说明 |
| --- | --- | --- |
| 真实直接 Lua 使用者（11） | `framework/scripting/src/lua_runtime.cpp` | 运行时本身 |
| | `apps/mesh_host/main.cpp` | 拓扑装载直接驱动 LuaRuntime；装载下沉由 #127 处理 |
| | `framework/service_host/src/layered_config_loader.cpp` | 合并装载器：MergedLayers 持有 runtime 与根表，调用各 `parse_*_config` |
| | `game/gateway/src/gateway_config_loader.cpp` | `GatewayConfigLoader::load` 单文件装载 |
| | `game/common/src/account_store.cpp` | `ConfigAccountStore::load` 读取 Lua 账号配置 |
| | `game/common/src/player_data_store.cpp` | 首次导入实现读取 bootstrap 账号 Lua 文件 |
| | `game/realm/src/training_rule.cpp` | 规则实现 |
| | `lua_runtime_test`、`configs_load_smoke_test`、`gateway_login_config_test`、`training_rule_test` | 直接驱动 LuaRuntime 或解析入口的测试 |
| 经 `training_rule.hpp` 间接引入（5） | `service_host.cpp`、`realm_sessions.cpp`、`wire_login_transport_integration_test`、`service_frame_realm_enter_test`、`realm_sessions_test` | 公共头 `training_rule.hpp` 仍包含 `lua_runtime.hpp`，由 #127 前置声明后应归零 |

各模块的 `*_config_lua.cpp` 是 sol2 的直接使用者，但只包含 `<sol/sol.hpp>` 与解析入口头，不包含 `lua_runtime.hpp`，所以不在本探针的名单里；改 sol2 本身时它们会重编。

### 筛查耗时（不判定）

降幅为 `1 − 中位数(after) ÷ 中位数(before)`，只计 build 步。

| 平台 | before 中位数 s（极差） | after 中位数 s（极差） | 降幅 | after 更快的组数 |
| --- | ---: | ---: | ---: | ---: |
| Mac（8 jobs） | 14.20（13.96–14.52） | 11.03（10.85–11.06） | 22.4% | 5/5 |
| Lima（2 jobs） | 44.04（43.64–44.25） | 26.31（26.18–27.20） | 40.3% | 5/5 |

逐组降幅（奇数组 before 先）：

| 组 | Mac | Lima |
| ---: | ---: | ---: |
| 1 | 22.1% | 39.4% |
| 2 | 21.6% | 40.4% |
| 3 | 24.4% | 38.3% |
| 4 | 22.3% | 40.8% |
| 5 | 22.8% | 40.3% |

- 两平台 5 组方向一致，各侧极差都在 5% 噪声带内。
- 编译次数减半，降幅却差别很大：Lima 只有 2 路编译，少编 17 个源码几乎全数转成墙钟；Mac 8 路并行下，剩余的 16 次编译与 54 次链接决定了大部分时间。这只是推测，没有拆分关键路径。
- R0 的同名探针是串行构建（Mac 72.71 s、Lima 73.79 s），与本节的并行条件不可比；P3 结束时按同条件配对判定。

资源：进程树 RSS 峰值 Mac 为 2712 MiB（before）/ 1409 MiB（after），Lima 为 1989 / 1406 MiB；最低可用内存 Mac 约 17950 MiB，Lima 为 1449 MiB（before）/ 2044 MiB（after），都高于 1 GiB 门槛。两平台 swap 无增长，Lima 无 OOM。

### 异常与限制

- **Mac 首轮完整验证 1 例失败。** 第一次 `build.sh` 与 Lima 的不计时准备构建同时进行（Lima 虚拟机与 Mac 共用宿主 CPU），`LoadgenIntegrationTest.M3SmokeTenThousandTicketsAndConcurrentPolls` 的发票数 4991 低于下限 4995，属于吞吐阈值，与配置解析无关。在 Lima 空闲时重跑整套 `build.sh`，653 个用例全部通过（CTest 387.89 s）。上表记录的是重跑结果。
- **realm 仍 PUBLIC 传播 sol2。** `realm_game_realm` 的公共头 `training_rule.hpp` 还包含 `lua_runtime.hpp`，`realm_service_host` 经它仍间接获得 sol2。因此 service_host 改 PRIVATE 对 `lua_runtime.hpp` 探针的效果要到 #127 才完全体现，本节不把那 5 个间接重编算作已解决。
- **使用需求只写在注释里。** 解析入口“包含方须自行链接 RealmMesh::Scripting”写在头注释与 CMake 注释中，没有用 INTERFACE target 表达；现有两个跨目标使用方都已 PRIVATE 链接。
- **链接扇出未变。** PRIVATE 不消除静态库的最终链接，`lua_runtime.hpp` 扰动仍触发 53 个原有产物重链，与决策文档的预期一致；链接等待由 P4 处理。
- **重复链接告警。** `configs_load_smoke_test`、`gateway_login_config_test` 显式链接 `realm_scripting` 后，Apple ld 报 `ignoring duplicate libraries: librealm_scripting.a`，与既有的 `game_common` 同类告警一样无害。
- **完整验证用的源码。** Mac 在开发仓库 `6d92abd` 上跑；Lima 在 after 副本 `0caeeb0` 上跑，两者产品代码相同，`6d92abd` 只改测量工具。
- **CMake 3.20 未实跑**，两平台本机与 CI 的 CMake 都更高；本阶段没有用到新于 3.20 的 CMake 特性。

### 资产

- [`p3a-mac-samples.json`](assets/build-optimization-results/p3a-mac-samples.json)、[`p3a-lima-samples.json`](assets/build-optimization-results/p3a-lima-samples.json)：内容包括：
  - `pairs`：逐组配对、降幅与每组的编译 / 链接次数；
  - `groups`：before、after 两组 `measure.py export` 结果，含每次调用的 `environment`、逐样本（含预热与复位）的 build 墙钟、编译与链接次数、编译源码与链接产物名单、内存摘要与汇总。
  - 本机路径都替换为 `<bench>`；原始日志与时间线没有提交。
- 复现：两侧按上表提交各做一份源码副本并链接 `.tools`，准备一次 `--scenario fetch`，以复用依赖的配置做一次 ALL 构建。然后每侧以 `--scenario probes --probe lua-hpp --comment-samples 0 --samples 0` 预热 1 次，再按奇偶顺序交替调用 `--samples 1 --sample-start N --no-warmup`。Mac 加 `--jobs 8`，Lima 加 `--jobs 2 --env TMPDIR=<磁盘目录>`（见 [tools/build-bench](../../tools/build-bench/README.md)）。
