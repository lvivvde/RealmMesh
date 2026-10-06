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

## P3b：TrainingRule 前置声明与拓扑装载下沉（[#127](https://github.com/lvivvde/RealmMesh/issues/127)）

P3b 让 `training_rule.hpp` 只前置声明 `LuaRuntime`，完整的 Lua 包含与全部调用留在 `training_rule.cpp`，`realm_game_realm` 对 `realm_scripting` 改为 PRIVATE；`apps/mesh_host/main.cpp` 的 `main.config` 解析下沉为 service_host 的装载入口 `load_topology(config_root)`（实现在 `startup_topology_lua.cpp`），入口只拿 `ServiceSpec` 列表或异常，`realm_mesh` 不再链接 `realm_scripting`。做法与取舍见[接口边界决策的实施记录](interface-boundaries-proposal.md#实施记录127)。本阶段门槛是 `lua_runtime.hpp` 扰动的编译次数下降与双平台完整验证；“快 ≥30%、只用配置结果的消费者 0 次重编”仍在整个 P3 结束（[#128](https://github.com/lvivvde/RealmMesh/issues/128) P3c 之后）时判定，本节的降幅只记录、不判定。

### 条件

| 项 | 值 |
| --- | --- |
| 前版本（before） | `733d149`（main，P3a 之后） |
| 后版本（after） | `618f612`（分支 `feat/p3b-training-rule-topology-load`，本阶段的代码提交；其后的提交只改测试归置、文档与资产，产品代码与 `lua_runtime.hpp` 探针的重编集合不变） |
| 场景 | `probes --probe lua-hpp`：改 `framework/scripting/include/realmmesh/scripting/lua_runtime.hpp` 的 token，只计 build 步；每样本后复位原始字节并重建（`*-reset-N`，不计入）。注释变体不跑（`--comment-samples 0`） |
| 生成器 / preset | Unix Makefiles，`dev`（Debug），binaryDir `build/dev` |
| 并行 | 两侧相同：Mac `--jobs 8`，Lima `--jobs 2`，与 P3a 一致 |
| 编译缓存 | 无（OFF） |
| 源码 | 两平台两侧各一份 `git archive` 副本（Lima 从本地 clone 导出，不动 Lima 工作区）。`.tools` 以符号链接复用 |
| 准备（不计时） | 每平台一次 `measure.py --scenario fetch` 获取依赖；两侧都以 launcher 与 `FETCHCONTENT_SOURCE_DIR_*` 复用它配置，再做一次 ALL 构建 |
| 测量工具 | 与 P3a 相同：measure.py sha256 `368bd1c7…aac`，launcher.cpp `1374833a…034b`，本机编译（Mac `7c015b5f…`，Lima `19554d55…`） |
| 采样 | 每侧预热 1 次后交替 5 组：奇数组 before 先，偶数组 after 先；每次调用 1 个样本（`--samples 1 --sample-start N --no-warmup`） |
| 工具链 | Mac CMake 4.4.3、GNU Make 3.81、Apple clang 21.0.0；Lima CMake 4.2.3、GNU Make 4.4.1、GCC 15.2.0 |

Mac 与 Lima 先后进行：先测 Mac（Lima 空闲），Mac 的完整验证与重复测试结束后再测 Lima（Mac 不跑构建与测试），计时从未重叠。

### 门槛验收

| 门槛 | Mac | Lima |
| --- | --- | --- |
| `lua_runtime.hpp` 扰动的编译次数下降 | 16 → 10，5 组一致 | 16 → 10，5 组一致 |
| 链接次数 | 54 → 55：多出的 1 次是本阶段新增的 `load_topology_test`，其余 54 个产物名单相同 | 同左 |
| `training_rule.hpp` 不带 sol2 | `config_headers_test` 增加包含 `training_rule.hpp` 后编译通过（`SOL_HPP` / `SOL_FORWARD_HPP` 的 `#error` 守卫） | 同左 |
| 完整验证 `./scripts/build.sh`（ALL＋完整 CTest `-j 1`） | 退出 0；`100% tests passed out of 659`（P3a 的 653 加本阶段的 6 例：`load_topology_test` 5 例与 `ConfigsLoadSmokeTest` 新增 1 例，unit；CTest 387.62 s），`build jobs: 8`。首轮有 1 例失败，见“异常与限制” | 退出 0；`100% tests passed, 0 tests failed out of 659`（CTest 298.77 s），`build jobs: 2` |
| 指名测试重复 3 次 | 3 次都是 `100% tests passed out of 77` | 3 次都是 `100% tests passed, 0 tests failed out of 77` |
| QUIC | `realm_network: QUIC transport enabled` | 同左 |

指名测试为 `TrainingRuleTest`、`TrainingRuleFileTest`、`RealmTrainingRule`（规则脚本的 Lua 侧测试）、`RealmSessionsTest`、`ServiceFrameRealmEnterTest`、`WireLoginTransportIntegrationTest`、`ConfigHeadersTest`、`LoadTopologyFileTest`、`StartupTopologyTest`、`ConfigsLoadSmokeTest`、`LuaRuntimeTest`、`MeshHostE2ETest`、`Mode2Test`、`RealmJourneyTest` 十四组，共 77 个用例。`ConfigsLoadSmokeTest.MainConfigTopologyLoadsInDeclarationOrder` 经新入口装载随包的 `main.config`；后三组启动 `realm_mesh`，经同一入口读取拓扑。

### 重编名单

两平台名单相同。前版本的 16 次里，下列 7 个源码在后版本不再重编：

- P3a 记为“经 `training_rule.hpp` 间接引入”的 5 个：`service_host.cpp`、`realm_sessions.cpp`、`wire_login_transport_integration_test`、`service_frame_realm_enter_test`、`realm_sessions_test`，按预期归零；
- `training_rule_test`：只通过 `TrainingRule` 的公共接口使用规则，不再经头文件引入运行时；
- `apps/mesh_host/main.cpp`：拓扑装载移走，由下表的 `startup_topology_lua.cpp` 取代。

后版本仍重编的 10 个都是真实直接 Lua 使用者：

| 源码 | 说明 |
| --- | --- |
| `framework/scripting/src/lua_runtime.cpp` | 运行时本身 |
| `framework/service_host/src/startup_topology_lua.cpp` | 本阶段新的拓扑装载入口 |
| `framework/service_host/src/layered_config_loader.cpp` | 合并装载器 |
| `game/gateway/src/gateway_config_loader.cpp` | `GatewayConfigLoader::load` 单文件装载 |
| `game/common/src/account_store.cpp` | `ConfigAccountStore::load` 读取 Lua 账号配置 |
| `game/common/src/player_data_store.cpp` | 首次导入实现读取 bootstrap 账号 Lua 文件 |
| `game/realm/src/training_rule.cpp` | 规则实现 |
| `lua_runtime_test`、`configs_load_smoke_test`、`gateway_login_config_test` | 直接驱动 LuaRuntime 或解析入口的测试 |

### 筛查耗时（不判定）

降幅为 `1 − 中位数(after) ÷ 中位数(before)`，只计 build 步。

| 平台 | before 中位数 s（极差） | after 中位数 s（极差） | 降幅 | after 更快的组数 |
| --- | ---: | ---: | ---: | ---: |
| Mac（8 jobs） | 11.24（10.83–11.57） | 8.93（8.89–9.01） | 20.5% | 5/5 |
| Lima（2 jobs） | 25.81（25.56–26.02） | 18.99（18.97–19.21） | 26.4% | 5/5 |

逐组降幅（奇数组 before 先）：

| 组 | Mac | Lima |
| ---: | ---: | ---: |
| 1 | 18.8% | 26.2% |
| 2 | 20.7% | 26.1% |
| 3 | 20.8% | 25.8% |
| 4 | 22.8% | 26.8% |
| 5 | 18.0% | 26.0% |

- 两平台 5 组方向一致。Lima 两侧与 Mac after 的极差都在 2% 以内；Mac before 极差 0.74 s，为中位数的 6.6%，超出 5% 噪声带，但每组 before 都比同组 after 慢 1.9 s 以上，方向不受影响。
- 本节 before（`733d149`，P3a 的合入提交）与 P3a 的 after（`0caeeb0`）产品代码相同。两次测得的中位数 Mac 为 11.24 / 11.03 s，Lima 为 25.81 / 26.31 s，都相差 2% 以内，两次测量可以互相印证。按本文件口径，两段降幅不相乘，也不跨比较组拼接秒数；整个 P3 的判定在 #128 之后，以 `4f2a8a9` 对 P3c 做同条件配对。
- Lima 只有 2 路编译，少编 7 个源码基本都转成了墙钟，降幅 26.4%；Mac 是 8 路并行，降幅 20.5%，与 P3a 少编 17 个源码时的 22.4% 接近。少掉的 `service_host.cpp`、`realm_sessions.cpp` 以及服务帧、线协议、会话测试是重型源码，这可能是原因，但没有拆分关键路径，只是推测。

资源：进程树 RSS 峰值 Mac 为 1441 MiB（before）/ 904 MiB（after），Lima 为 1325 / 1018 MiB；最低可用内存 Mac 约 18087 MiB，Lima 为 2068 MiB（before）/ 2473 MiB（after），都高于 1 GiB 门槛。两平台 swap 都没有增长（Lima 没有 swap 分区），Lima 没有 OOM，所有工具调用都以 0 退出。

### 异常与限制

- **测试归置在测量之后调整。** 代码审查后，随包 `main.config` 的装载用例从 `load_topology_test` 移到 `configs_load_smoke_test`（`4faf6dd`，按 [tests/README](../../tests/README.md) 的归置约定），只改测试。`configs_load_smoke_test` 原本就在重编名单里，`load_topology_test` 不包含 Lua 头，所以探针的重编集合不变，没有重测。两平台的完整验证与指名测试都在 `4faf6dd` 上跑：Mac 在开发仓库，Lima 在本地 clone 里，不动 Lima 工作区。
- **Mac 首轮完整验证 1 例失败。** 在 `4faf6dd` 上第一次跑 `build.sh` 时，`LoadgenIntegrationTest.M3SmokeTenThousandTicketsAndConcurrentPolls` 发出的票数为 4989，低于下限 4995。这与 P3a 记录的是同一个吞吐阈值，当时 Lima 空闲，与本阶段改动无关。该用例单独重跑通过；随后整套 `build.sh` 重跑，659 个用例全部通过，上表记录的是重跑结果。
- **Lima 上有一个外部空闲进程。** 测量期间 Lima 上一直有一个与本仓库无关的 `qemu-system-aarch64`，已运行约 12 小时，在串口上等待，平均 CPU 约 1.4%。before、after 两侧同样受影响，没有处理。
- **Lima `/tmp` 接近满。** tmpfs `/tmp` 共 3.9 GiB，已用 3.5 GiB（89%），主要是本测量之外创建的目录。按“只清理由测量创建并明确拥有的目录”，这些目录没有动；测量与完整验证都用 `TMPDIR=<磁盘目录>` 把临时文件放到磁盘上。tmpfs 的占用计入共享内存（约 3.4 GiB），所以 Lima 开始测量时可用内存只有约 3.3 GiB，测得的最低可用内存（2068 MiB）仍高于门槛。
- **拓扑装载里的重复形状原样保留。** `startup_topology_lua.cpp` 逐字搬移自 `main.cpp` 的旧函数。审查指出，可选字段检查与按序遍历的写法各出现两次；本阶段只做下沉，不重写。
- **使用需求仍只写在注释里。** 与 P3a 相同，“包含 `*_config_lua.hpp` 的目标须自行链接 `realm_scripting`”没有构建期检查。现有包含方都已显式链接。
- **链接扇出未变。** `lua_runtime.hpp` 扰动仍触发 54 个原有产物重链，加上新测试共 55 个；链接等待由 P4 处理。
- **重复链接告警。** 与 P3a 相同：Apple ld 对显式链接 `realm_scripting` 的测试报 `ignoring duplicate libraries`，无害。
- **CMake 3.20 未实跑**，两平台本机与 CI 的 CMake 都更高；本阶段没有用到新于 3.20 的 CMake 特性。

### 资产

- [`p3b-mac-samples.json`](assets/build-optimization-results/p3b-mac-samples.json)、[`p3b-lima-samples.json`](assets/build-optimization-results/p3b-lima-samples.json)：结构同 P3a（`pairs` 逐组配对与降幅，`groups` 为两侧 `measure.py export` 结果），本机路径都替换为 `<bench>`；原始日志与时间线没有提交。
- 复现：同 P3a，把两侧提交换成上表的 `733d149` / `618f612`。

## P3c：Gateway 轻量事件与启动配置头（[#128](https://github.com/lvivvde/RealmMesh/issues/128)）

P3c 把 `GatewayEvent`/`GatewayEventKind` 移到 `gateway_event.hpp`，把 `GatewayConfig`/`GatewayRuntimeOptions` 移到 `gateway_config.hpp`；`gateway_primary_transport.hpp` 只前置声明 `GatewayRuntime`，配置装载头与 `mesh_host.cpp` 改含配置头，`gateway_runtime.hpp` 只留给真正构造或驱动 runtime 的代码。没有做 runtime PIMPL，也没有新增虚接口。做法与取舍见[接口边界决策的实施记录](interface-boundaries-proposal.md#实施记录128)。本阶段门槛是 `gateway_runtime.hpp` 扰动的编译次数下降与双平台完整验证；P3 至此结束，本节同时以 `4f2a8a9` 对 P3c 做 `lua-hpp` 同条件配对，判定整个 P3 的“快 ≥30%、只用配置结果的消费者 0 次重编”。

### 条件

| 项 | 值 |
| --- | --- |
| 版本 | `gateway-hpp`：before `b25f6dc`（main，P3b 之后），after `288e847`（分支 `feat/p3c-gateway-light-headers`，本阶段代码含审查修正；其后的提交只改文档与资产）。`lua-hpp`：before `4f2a8a9`（main，P3a 之前，即 P2b 之后），after 同为 `288e847` |
| 场景 | `probes --probe gateway-hpp`（改 `game/gateway/include/realmmesh/game/gateway/gateway_runtime.hpp`）与 `probes --probe lua-hpp`（改 `framework/scripting/include/realmmesh/scripting/lua_runtime.hpp`）的 token 变体，只计 build 步；每样本后复位原始字节并重建（`*-reset-N`，不计入）。注释变体不跑（`--comment-samples 0`） |
| 生成器 / preset | Unix Makefiles，`dev`（Debug），binaryDir `build/dev` |
| 并行 | 两侧相同：Mac `--jobs 8`，Lima `--jobs 2`，与 P3a、P3b 一致 |
| 编译缓存 | 无（OFF） |
| 源码 | 每平台三份 `git archive` 副本（`b25f6dc`、`4f2a8a9`、`288e847`）；Lima 从 `git bundle` 克隆的本地仓库导出，不动 Lima 工作区。`.tools` 以符号链接复用。after 副本先测 `gateway-hpp`，再测 `lua-hpp` |
| 准备（不计时） | 每平台一次 `measure.py --scenario fetch` 获取依赖；三份副本都以 launcher 与 `FETCHCONTENT_SOURCE_DIR_*` 复用它配置，再以 `unit-entry --samples 0`（配置＋ALL＋Unit）做一次 ALL 构建 |
| 测量工具 | 与 P3a、P3b 相同：measure.py sha256 `368bd1c7…aac`，launcher.cpp `1374833a…034b`，本机编译（Mac `7c015b5f…`，Lima `19554d55…`） |
| 采样 | 每个探针每侧预热 1 次后交替 5 组：奇数组 before 先，偶数组 after 先；每次调用 1 个样本（`--samples 1 --sample-start N --no-warmup`） |
| 工具链 | Mac CMake 4.4.3、GNU Make 3.81、Apple clang 21.0.0；Lima CMake 4.2.3、GNU Make 4.4.1、GCC 15.2.0 |

Mac 与 Lima 先后进行：先测 Mac（Lima 空闲），Mac 的完整验证与重复测试结束后再测 Lima（Mac 不跑构建与测试），计时从未重叠。

### 门槛验收

| 门槛 | Mac | Lima |
| --- | --- | --- |
| `gateway_runtime.hpp` 扰动的编译次数下降 | 30 → 14，5 组一致 | 30 → 14，5 组一致 |
| 链接次数（`gateway-hpp`） | 22 → 23：多出的 1 次是本阶段新增的 `gateway_headers_test`（链接 `realm_gateway_service`），其余 22 个产物名单相同 | 同左 |
| 轻量头不带完整 runtime | `gateway_headers_test` 与 `config_headers_test` 编译通过（`static_assert(!CompleteType<GatewayRuntime>)`）；在两测试最前面临时加入 `gateway_runtime.hpp` 时都在编译期失败 | 同左 |
| 完整验证 `./scripts/build.sh`（ALL＋完整 CTest `-j 1`） | 退出 0；`100% tests passed out of 660`（P3b 的 659 加本阶段的 `GatewayHeadersTest`，unit；CTest 392.77 s），`build jobs: 8` | 退出 0；`100% tests passed out of 660`（CTest 300.20 s），`build jobs: 2`（budget 2 = min(8 CPUs, 8, memory 7.73 GiB)） |
| 指名测试重复 3 次 | 3 次都是 `100% tests passed out of 134` | 3 次都是 `100% tests passed out of 134`（每次约 13.7 s） |
| QUIC | `realm_network: QUIC transport enabled` | 同左 |

指名测试为 `GatewayHeadersTest`、`ConfigHeadersTest`、`GatewayRuntimePrimaryTransportTest`、`InMemoryGatewayPrimaryTransportTest`、`GatewayRuntimeTest`、`GatewayLoginPipelineTest`、`GatewayAdmissionTest`、`GatewayConfigLoaderTest`（`gateway_server_test`）、`GatewayLoginConfigTest`、`LayeredConfigLoaderTest`、`ConfigsLoadSmokeTest`、`ServiceFrameEdgeBudgetTest`、`ServiceFrameRealmEnterTest`、`RealmSessionsTest`、`WireLoginTransportIntegrationTest`、`ServiceHostTest`、`MeshHostTest`、`MeshHostE2ETest`、`Mode2Test` 十九组，共 134 个用例：覆盖拆出的事件与配置头、主传输两种适配器、仍包含 runtime 的全部源码，以及启停与销毁顺序（`ServiceHostTest`、`MeshHostTest`、`Mode2Test`、`MeshHostE2ETest` 启动并停止真实 runtime）。

### 重编名单（`gateway-hpp`）

两平台名单相同。前版本的 30 次里，下列 16 个源码在后版本不再重编：

- 经 `gateway_config_loader.hpp`、`gateway_config_lua.hpp` 间接引入的（含 `layered_config_loader.hpp`、`mesh_host.hpp` 的包含方）：`layered_config_loader.cpp`、`mesh_host.cpp`、`apps/mesh_host/main.cpp`、`gateway_config_loader.cpp`、`gateway_config_lua.cpp`，以及 `config_headers_test`、`configs_load_smoke_test`、`layered_config_loader_test`、`gateway_login_config_test`、`gateway_server_test`、`loadgen_integration_test`；
- 经 `gateway_primary_transport.hpp` 间接引入的：`gateway_login_pipeline.cpp`、`gateway_admission.cpp`，以及 `gateway_login_pipeline_test`、`gateway_admission_test`、`gateway_ingress_throttle_test`。

后版本仍重编的 14 个都真实构造或驱动 `GatewayRuntime`，属于“稳定接口变化仍重编真实消费者”：

| 源码 | 说明 |
| --- | --- |
| `game/gateway/src/gateway_runtime.cpp`、`gateway_primary_transport.cpp` | runtime 本身与生产主传输适配器 |
| `framework/service_host/src/service_host.cpp` | 创建并持有 runtime |
| `framework/service_host/src/service_frame.cpp` | 帧循环 `drain_events`、`try_send`、`try_decline`、`stats` |
| `game/realm/src/realm_sessions.cpp` | 向会话 `try_send` |
| `gateway_runtime_test`、`gateway_primary_transport_test`、`wire_login_transport_integration_test` | 构造真实 runtime |
| `service_frame_edge_budget_test`、`service_frame_realm_enter_test` | 持有 `optional<GatewayRuntime>` |
| `service_host_test`、`mesh_host_test`、`mode2_test`、`mesh_host_e2e_test` | 经 `ServiceHost::runtime()` 调用 `running()`、`local_port()` |

### 筛查耗时（`gateway-hpp`，不判定）

降幅为 `1 − 中位数(after) ÷ 中位数(before)`，只计 build 步。

| 平台 | before 中位数 s（极差） | after 中位数 s（极差） | 降幅 | after 更快的组数 |
| --- | ---: | ---: | ---: | ---: |
| Mac（8 jobs） | 11.45（11.10–11.74） | 9.05（8.91–9.14） | 21.0% | 5/5 |
| Lima（2 jobs） | 36.13（35.92–38.70） | 22.46（22.35–22.50） | 37.8% | 5/5 |

逐组降幅（奇数组 before 先）：

| 组 | Mac | Lima |
| ---: | ---: | ---: |
| 1 | 23.8% | 38.6% |
| 2 | 22.2% | 37.5% |
| 3 | 22.2% | 38.0% |
| 4 | 17.7% | 37.7% |
| 5 | 19.4% | 42.0% |

### 整个 P3 的判定（`lua-hpp`，`4f2a8a9` → `288e847`）

| 门槛 | Mac | Lima |
| --- | --- | --- |
| build 中位耗时下降 ≥30%（同 jobs、cache OFF） | 13.52（13.21–14.17）→ 9.05（8.64–9.22）s，**33.1%**，5/5 组 after 更快 | 45.66（43.40–46.21）→ 19.87（19.33–20.46）s，**56.5%**，5/5 组 after 更快 |
| 只用配置结果的消费者 0 次重编 | 33 → 10，5 组一致；剩下 10 个都是真实直接 Lua 使用者（见下） | 33 → 10，5 组一致，名单同左 |
| 链接次数 | 53 → 56：多出的 3 次是 P3 新增的 `config_headers_test`、`load_topology_test`、`gateway_headers_test`，其余 53 个产物名单相同。链接扇出未承诺消除 | 同左 |

逐组降幅（奇数组 before 先）：

| 组 | Mac | Lima |
| ---: | ---: | ---: |
| 1 | 35.9% | 56.1% |
| 2 | 33.4% | 56.9% |
| 3 | 34.5% | 55.3% |
| 4 | 36.1% | 57.7% |
| 5 | 30.2% | 52.8% |

前版本的 33 次里，下列 24 个源码在后版本不再重编：配置与服务实现 `player_data_config.cpp`、`login_verify_config.cpp`、`login_verify_service.cpp`、`queue_config.cpp`、`queue_service.cpp`、`realm_config.cpp`、`mesh_host.cpp`、`service_frame.cpp`、`service_host.cpp`、`realm_sessions.cpp`、`apps/mesh_host/main.cpp`，以及 `layered_config_loader_test`、`gateway_server_test`、`login_verify_service_test`、`queue_service_test`、`realm_sessions_test`、`training_rule_test`、`service_host_test`、`service_frame_realm_enter_test`、`wire_login_transport_integration_test`、`mesh_host_test`、`mesh_host_e2e_test`、`mode2_test`、`loadgen_integration_test`。后版本的 10 个与 P3b 的 after 名单相同：`lua_runtime.cpp`、`startup_topology_lua.cpp`（P3b 新增的拓扑装载入口）、`layered_config_loader.cpp`、`gateway_config_loader.cpp`、`account_store.cpp`、`player_data_store.cpp`、`training_rule.cpp` 与 `lua_runtime_test`、`configs_load_smoke_test`、`gateway_login_config_test`，都创建或驱动 `LuaRuntime`、读取 Lua 文件或调用解析入口，不是“只用配置结果”的消费者。P3c 没有改变这份名单：`gateway_runtime.hpp` 本就不含 Lua 头，P3c 减少的是 Gateway 布局的传播。

- 两平台判定：**通过**。同 jobs、cache OFF 下 `lua-hpp` build 中位耗时 Mac 降 33.1%、Lima 降 56.5%，都不低于 30%，且两平台 5/5 组 after 更快；只用配置结果的消费者在两平台都是 0 次重编。Lima 降幅更大，因为 2 jobs 下被省掉的 23 个编译单元几乎全部串行排队，而 Mac 8 jobs 时它们与剩下的 10 个并行，墙钟收益被压缩。
- Mac 两侧极差为中位数的 7.1%（before）与 6.4%（after），超出 5% 噪声带；每组 after 都比同组 before 快 4 s 以上，方向不受影响，最低的第 5 组为 30.2%。Lima 两侧极差为 6.2% 与 5.7%，同样略超噪声带；每组 after 都比同组 before 快 23 s 以上，最低的第 5 组为 52.8%。
- `gateway-hpp` 只作筛查：Lima before 极差 7.7%，来自第 5 组的 38.70 s（其余四组 35.92–36.48 s），after 极差 0.7%；Mac 为 5.6% 与 2.5%。两平台 5/5 组方向一致。
- 本节 before 的 33 次与 P3a 记录的 `4f2a8a9` 名单一致；after 的 10 次与 P3b after 一致。两次比较组不同，秒数不跨组拼接。

### 异常与限制

- **after 副本 `lua-hpp` 预热多编 14 个（两平台相同）。** after 副本先跑 `gateway-hpp`、再跑 `lua-hpp`。`measure.py` 退出时会再写一次探针文件的原始字节（`edited()` 的收尾），使 `gateway_runtime.hpp` 的 mtime 晚于最后一次复位构建；于是下一次构建（`lua-hpp` 预热）先补编这 14 个 runtime 消费者，记为 24 次。预热不计入统计；随后的预热复位与 5 组正式样本都是 10 次。同一探针连续调用不受影响，因为下一次调用写的正是同一个文件。before 两份副本各只测一个探针，没有这个现象。
- **资源。** 进程树 RSS 峰值 Mac `gateway-hpp` 为 2239 MiB（before）/ 1685 MiB（after），`lua-hpp` 为 2575 / 1258 MiB；最低可用内存约 17806 MiB，内存压力等级一直为 1，swap 没有增长。Lima `gateway-hpp` 为 1974 / 1283 MiB，`lua-hpp` 为 2009 / 1379 MiB；最低可用内存 1443 MiB，内存压力等级为 0，swap 没有增长，没有 OOM。Lima 的 `/tmp`（tmpfs）测前已用 89%，与 P3b 记录相同，不是本次所致；构建临时文件经 `TMPDIR` 放在磁盘上。 所有工具调用都以 0 退出。
- **守卫与测试取舍。** 审查后删去 `gateway_headers_test` 中与 `config_headers_test` 重复的配置缺省值用例，以及对 `GatewayEvent::established` 的断言（`established` 在 [CONTEXT.md](../../CONTEXT.md) 是要避免的旧称；字段改名不在本票范围）。两个守卫各写一份 `CompleteType` 概念，没有为一行代码新建测试支持头。
- **链接扇出未变。** `gateway_runtime.hpp` 扰动仍触发 22 个原有产物重链，`lua_runtime.hpp` 扰动仍触发 53 个原有产物重链；链接等待由 P4 处理。
- **CMake 3.20 未实跑**；本阶段的 CMake 改动只有新增一个 `realm_add_gtest`，没有用到新于 3.20 的特性。

### 资产

- [`p3c-mac-samples.json`](assets/build-optimization-results/p3c-mac-samples.json)、[`p3c-lima-samples.json`](assets/build-optimization-results/p3c-lima-samples.json)：结构同 P3a、P3b，`pairs` 下分 `gateway-hpp` 与 `lua-hpp` 两组（各带 before/after 提交、逐组配对与降幅），`groups` 为四侧 `measure.py export` 结果（含预热与 reset 样本），本机路径都替换为 `<bench>`；原始日志与时间线没有提交。
- 复现：同 P3a，`gateway-hpp` 两侧为 `b25f6dc` / `288e847`，`lua-hpp` 两侧为 `4f2a8a9` / `288e847`；after 副本先测 `gateway-hpp` 再测 `lua-hpp` 时，以预热吸收上条所述的补编。


## P4：Ninja 默认与 Make 回退（[#123](https://github.com/lvivvde/RealmMesh/issues/123)）

P4 把 `dev` 预设切到 Ninja（≥1.11，binaryDir `build/dev-ninja`），新增 `dev-make` 预设（Unix Makefiles，`build/dev-make`）作为回退。Ninja 下库与生产可执行文件的链接进深度 1 的池；libsodium 的外部构建仍是 `make -j1`，安装出的库与全部头都声明为构建步产物，供 Ninja 恢复用。做法与取舍见[工具与缓存决策的实施记录](build-tool-cache-decisions.md#实施记录123)。本阶段门槛是：双平台新目录构建、删除单个生成产物后的恢复、完整测试和编译数据库都有效，之后才切默认；耗时以同 jobs、cache OFF 的 Make/Ninja 配对做生成器归因。没有净收益或出现回归时，回退 Make 或重新决策。

**两项偏离约定，已于 2026-10-05 在 [#140](https://github.com/lvivvde/RealmMesh/pull/140) 中确认：**测试可执行文件的链接不进池（偏离“Ninja 链接 1”的预算，见“全部进池的一轮”）；`measure.py` 自行解析预设的 binaryDir（与决策文档“不要自行简化解析”相抵，见实施记录）。默认已在分支上切换，合并以本节结果为依据。

### 条件

| 项 | 值 |
| --- | --- |
| 版本 | 正式配对与验证：`ca4b978`（分支 `feat/p4-ninja-default`，测试链接放出池外）。“全部进池”一轮：`1d96663`（同分支，所有链接进池）。两轮都只比较同一提交的两个预设，不与其他阶段跨组拼接 |
| 两侧 | Make：`--preset dev-make`（Unix Makefiles，`build/dev-make`）；Ninja：`--preset dev`（Ninja，`build/dev-ninja`）。同一提交、同一份依赖、同一 launcher |
| 场景 | `cold-entry`（删除构建目录后配置＋ALL＋完整 CTest `-j 1`）、`fast-entry`（`scripts/test-fast.sh` 无改动）、`fast-cpp-entry`（`test-fast.sh`，改 `lua_runtime.cpp` 的 token）、`probes --probe lua-hpp`、`probes --probe proto`（改 `edge.proto`），后两者只计 build 步。改动类每样本后复位原始字节并重建（不计入）；注释变体不跑 |
| 并行 | 两侧相同：Mac `--jobs 8`，Lima `--jobs 2`，即当前预算（CI 同为 2）。`fast-entry` 与 `fast-cpp-entry` 由 `test-fast.sh` 按预算取编译 jobs（Mac 8、Lima 2），Unit 用例 4 路并行 |
| 编译缓存 | 无（OFF） |
| 源码 | 每平台两份不带 `.git` 的源码副本（`make`、`ninja`），内容为上表提交；Lima 的副本由本机打包传入，不动 Lima 工作区。`.tools` 以符号链接复用 |
| 准备（不计时） | 每平台一次 `measure.py --scenario fetch` 获取依赖；两份副本都以 launcher 与 `FETCHCONTENT_SOURCE_DIR_*` 复用它配置 |
| 测量工具 | 本分支的 measure.py（sha256 `bd54f4b5…27b6`，`ca4b978` 与工作树相同），launcher.cpp `1374833a…034b`，本机编译（Mac `7c015b5f…`，Lima `19554d55…`）。本分支对 measure.py 的改动：构建命令总是带 `--parallel N`（缺省 1，因为 Ninja 不传时按核数并行，Make 不传即串行）；`--build-dir` 缺省按副本自己的预设文件解析 binaryDir，不再由预设名推导。两项都不改计时与计数口径 |
| 采样 | `cold-entry` 交替 3 组，短场景每侧预热 1 次后交替 5 组：奇数组 Make 先，偶数组 Ninja 先；每次调用 1 个样本（`--samples 1 --sample-start N --no-warmup`） |
| 工具链 | Mac CMake 4.4.3、GNU Make 3.81、Ninja 1.13.2、Apple clang 21.0.0（15 CPU，48 GiB）；Lima CMake 4.2.3、GNU Make 4.4.1、Ninja 1.13.2、GCC 15.2.0（8 vCPU，7.73 GiB） |

Mac 与 Lima 先后进行：先测 Mac（Lima 空闲），Mac 的验证结束后再测 Lima（Mac 不跑构建与测试），计时从未重叠。测量期间 Mac 以 `caffeinate` 阻止空闲睡眠。

### 门槛验收

| 门槛 | Mac | Lima |
| --- | --- | --- |
| 新目录高并行构建 | `--parallel 16` 从空目录配置并构建 ALL，57.85 s，退出 0；`ninja -t missingdeps` 检查 1379 个节点，无缺失 | `--parallel 4`，86.34 s，退出 0；`missingdeps` 1375 个节点，无缺失 |
| 删除单个产物后恢复（Ninja，一次构建内） | 见下表，全部在下一次构建中重建并退出 0；再构建一次为 0 编译 0 链接 | 同 Mac，计数一致；删除 `sodium.h` 后的再构建有 56 次多余重链（见“异常与限制”） |
| 完整验证 `./scripts/build.sh`（ALL＋完整 CTest `-j 1`） | `--preset dev`：退出 0，`100% tests passed out of 661`，CTest 391.39 s，`build jobs: 8`，之后 `missingdeps` 无缺失；`--preset dev-make`：退出 0，`100% tests passed out of 661`，CTest 388.03 s | `--preset dev`：退出 0，`100% tests passed out of 661`，CTest 293.07 s，`build jobs: 2`；`--preset dev-make`：退出 0，`100% tests passed out of 661`，CTest 295.73 s，`build jobs: 2` |
| QUIC | `realm_network: QUIC transport enabled`（本机装有 `libmsquic`） | 同左 |
| 编译数据库 | `build/dev-ninja` 与 `build/dev-make` 的 `compile_commands.json` 都是 1094 条，所列源码全部存在；以库中命令对 8 个依赖生成 protobuf 头或 libsodium 头的项目源码做 `-fsyntax-only`，全部通过。仓库根的链接随 `build.sh`／`test-fast.sh` 改指向所选预设 | 同一套检查只在 Mac 做；Lima 的入口与 Mac 相同 |
| 无改动零工作 | 正式样本两侧 `fast-entry` 都是 0 编译 0 链接（各 12 次，含预热与准备） | 正式样本 0 编译 0 链接；每侧紧接冷入口后的那次预热重链 42 个 Unit 测试（见“异常与限制”） |
| 资源 | 进程树 RSS 峰值 2178 MiB；最低可用内存 17022 MiB；内存压力等级一直为 1；swap 3.8 MiB，测量前后不变 | 进程树 RSS 峰值 1782 MiB；最低可用内存 1635 MiB（≥1 GiB）；swap 0；`oom_kill` 计数测前测后都是 22（测前已有），无新增 OOM |

删除恢复（Mac 与 Lima 计数相同，编译/链接次数）：

| 删除的文件 | 产出者 | 恢复构建 | 再构建 |
| --- | --- | ---: | ---: |
| `third_party/sodium/install/lib/libsodium.a` | libsodium 外部构建步 | 8 / 72 | 0 / 0 |
| `third_party/sodium/install/include/sodium.h` | 同上 | 8 / 72 | 0 / 0（Lima 0 / 56，复测 3 次为 0 / 0） |
| `third_party/sodium/install/include/sodium/crypto_box.h` | 同上 | 8 / 72 | 0 / 0 |
| `proto/generated/realmmesh/edge/v1/edge.pb.h` | `protoc` 自定义命令 | 44 / 61 | 0 / 0 |
| `proto/generated/realmmesh/edge/v1/edge.pb.cc` | 同上 | 44 / 61 | 0 / 0 |
| `proto/librealm_protocol.a` | 静态库归档 | 0 / 56 | 0 / 0 |
| `framework/cluster/CMakeFiles/realm_cluster.dir/src/budget_publisher.cpp.o` | 编译 | 1 / 36 | 0 / 0 |

libsodium 的三项都会重跑外部构建步并重装，随后重编 8 个源码、重链全部下游；protobuf 的两项重跑 `edge.proto` 的 `protoc` 命令，重编 44 个源码。

### 同 jobs 配对（`ca4b978`，cache OFF）

降幅为 `1 − 中位数(Ninja) ÷ 中位数(Make)`；`cold-entry` 计整次调用（配置＋构建＋完整 CTest），`fast-entry`、`fast-cpp-entry` 计整次 `test-fast.sh`，探针只计 build 步。两侧每组的编译/链接次数相同（Mac 冷入口 777/77，Lima 776/76；`fast-cpp-entry` 1/43；`lua-hpp` 10/56；`proto` 44/55）。

**Mac（8 jobs）**

| 场景 | Make 中位数 s（极差） | Ninja 中位数 s（极差） | 降幅 | Ninja 更快的组数 | 逐组降幅（奇数组 Make 先） |
| --- | ---: | ---: | ---: | ---: | --- |
| `cold-entry` | 453.69（452.77–455.60） | 459.85（458.39–460.60） | −1.4% | 0/3 | −1.2%、−1.1%、−1.4% |
| `fast-entry` | 6.30（6.22–6.32） | 3.40（3.36–3.42） | 46.1% | 5/5 | 46.0%、46.1%、46.3%、45.8%、45.8% |
| `fast-cpp-entry` | 8.38（8.21–8.83） | 6.29（6.23–7.24） | 24.9% | 5/5 | 23.8%、24.0%、25.6%、27.0%、14.4% |
| `lua-hpp` | 8.62（8.47–9.12） | 4.69（4.63–5.47） | 45.5% | 5/5 | 40.0%、47.2%、38.1%、44.6%、45.4% |
| `proto` | 14.83（14.39–15.28） | 10.40（10.31–10.84） | 29.9% | 5/5 | 30.4%、27.6%、32.1%、26.9%、28.4% |

**Lima（2 jobs）**

| 场景 | Make 中位数 s（极差） | Ninja 中位数 s（极差） | 降幅 | Ninja 更快的组数 | 逐组降幅（奇数组 Make 先） |
| --- | ---: | ---: | ---: | ---: | --- |
| `cold-entry` | 469.35（468.57–473.87） | 465.77（465.14–467.12） | 0.8% | 3/3 | 0.5%、0.7%、1.7% |
| `fast-entry` | 3.03（2.99–3.22） | 1.81（1.80–1.83） | 40.2% | 5/5 | 40.5%、39.6%、44.1%、39.6%、39.6% |
| `fast-cpp-entry` | 8.85（8.80–9.14） | 7.74（7.66–8.11） | 12.5% | 5/5 | 14.5%、12.9%、12.5%、11.3%、12.2% |
| `lua-hpp` | 19.33（19.32–20.55） | 17.31（16.86–17.79） | 10.5% | 5/5 | 17.2%、11.6%、8.8%、8.0%、12.7% |
| `proto` | 52.89（48.96–54.63） | 46.88（45.19–47.70） | 11.4% | 5/5 | 3.7%、4.9%、15.5%、11.4%、15.9% |

冷入口分步（三组的 configure / build / test，s）：

| 平台 | Make | Ninja |
| --- | --- | --- |
| Mac | configure 6.38、6.00、5.64；build 64.54、59.82、60.35；test 381.84、389.78、387.68 | configure 约 4.7；build 63.98、65.05、64.12；test 389.73、390.86、391.00 |
| Lima | configure 约 1.9；build 169.12、170.25、177.04；test 约 296 | configure 约 1.6；build 165.54、165.43、163.63；test 约 300 |

- **短场景：两平台都有净收益。** 四个短场景两平台都是 5/5 组 Ninja 更快。收益来自调度而非工作量（两侧编译、链接次数相同）：无操作时 Ninja 只读一份 `build.ninja` 与 `.ninja_deps`，Make 3.81／4.4 要递归进每个目录重新判定；改动时 Ninja 一边链接一边开始下一个可运行的边，Make 按目录与目标逐层推进。Lima 2 jobs 下改动类的编译与链接本身占大头，可省的只有调度，所以降幅（10–13%）小于 Mac（25–46%）。
- **Mac 冷入口慢 1.4%，在可解释范围内，不判回归（[#140](https://github.com/lvivvde/RealmMesh/pull/140) 中确认）。** 6 s 的差距里，build 步中位数 Make 60.35、Ninja 64.12 s（+3.8 s），test 步 Ninja 高约 3 s，configure 步 Ninja 少约 1.4 s。test 步跑的是同一套 CTest，差异属于桌面负载的波动。build 步剩下的差距来自 libsodium：它的串行 configure＋make 是冷构建的关键路径，Ninja 下这一步比 Make 晚约 2.4 s 开始，configure 在并发编译的争用下又慢约 2 s。池的影响已在放出测试链接时消除：全部进池的一轮里 Ninja 冷构建 build 步为 92.90–94.42 s，放出后为 63.98–65.05 s。Lima 冷入口 3/3 组 Ninja 更快（build 步 −2.8%），因为 2 jobs 下关键路径是编译本身而不是 libsodium。冷构建不是日常反馈路径；把 libsodium 移出关键路径（例如预构建或缓存安装树）不在 P4 范围。
- **完整验证。** 热完整验证没有作为配对场景重跑；同一会话内两预设的完整 CTest 为 Mac 391.39 s（Ninja）对 388.03 s（Make）、Lima 293.07 s 对 295.73 s，CTest 本身与生成器无关。热完整验证按 R0 的门槛在 P6 复测。

### 全部进池的一轮（`1d96663`，Mac 8 jobs）

最初的实现让所有链接（含测试可执行文件）进深度 1 的池。同条件配对显示短改动场景大幅回归：

| 场景 | Make 中位数 s | Ninja 中位数 s | 降幅 |
| --- | ---: | ---: | ---: |
| `cold-entry` | 460.77 | 488.51 | −6.0%（build 步 64.66 → 93.12） |
| `fast-entry` | 6.37 | 3.45 | 45.8% |
| `fast-cpp-entry` | 8.22 | 19.67 | −139.4% |
| `lua-hpp` | 8.27 | 22.82 | −176.1% |
| `proto` | 14.41 | 29.31 | −103.4% |

原因是 `gtest_discover_tests` 的 POST_BUILD 用例发现与链接同在一条 Ninja 边里：macOS 上新链接的二进制首次执行约等 0.5 s（user/sys 为 0），池深 1 把 43–56 个测试的链接加发现串成一列。Make 不受池约束，8 路并行跑这些 POST_BUILD。只把测试可执行文件放出池后（`ca4b978` 之前在 `1d96663` 上筛查，每场景 3 个样本），Ninja 中位数降为 `fast-cpp-entry` 6.20、`lua-hpp` 4.57、`proto` 10.41 s，与上面正式配对一致。放出后的最大单次链接内存为 263 MiB（`service_host_test`），生产链接 `realm_mesh` 为 239 MiB，同一量级；整体资源见上表。按验收约定，这一回归本应回退 Make 或重新决策；放出测试链接是对“Ninja 链接 1”预算的调整，作为范围决定记在实施记录与 PR 中，已获确认。

这一轮的 Ninja 冷入口第 3 组退出 8：`TestWatchScriptTest` 偶发失败，原因是停止提示被写进快照文件，已由 `fb0e18f` 修复（只改脚本的输出去向）。失败只影响退出码，不影响该样本的构建与计时；正式配对在修复之后，全部退出 0。

### 相对 R0 的累计变化（仅供参考）

R0 是串行构建（`4ffb58d`，Make），这里把它与 P4 后的 Ninja 默认配置放在一起，展示 P1–P4 合计后的日常体验；并行预算、目标选择、接口拆分与生成器的贡献混在其中，不能归因给 Ninja，也不作为 R0 门槛的判定（判定在 P6 按验收约定复测）。

| 指标 | Mac R0 → P4 Ninja | Mac 降幅 | Lima R0 → P4 Ninja | Lima 降幅 |
| --- | --- | ---: | --- | ---: |
| 冷入口 build 步 | 305.71 → 64.12 | 79.0% | 359.20 → 165.43 | 53.9% |
| 默认 Unit 无改动（总） | 17.78 → 3.40 | 80.9% | 7.32 → 1.81 | 75.3% |
| 默认 Unit `.cpp` 改动（总） | 43.25 → 6.29 | 85.5% | 24.26 → 7.74 | 68.1% |
| `lua-hpp` token（build 步） | 72.71 → 4.69 | 93.5% | 73.79 → 17.31 | 76.5% |

### 回退

```bash
./scripts/build.sh --preset dev-make
```

`test-fast.sh`、`test-watch.sh` 与 `build-dir.sh` 同样接受 `--preset dev-make`；两个预设的构建目录互不干扰，切换后仓库根的 `compile_commands.json` 随入口改指向。Ninja 缺失或低于 1.11 时，`dev` 在 `project()` 之前停下并提示改用这个预设，不会悄悄换生成器。

### 旧路径 `build/dev` 的复查

P2a 把文档里 `build/dev` 的复查留给 P4（见 P2a“异常与限制”）。本阶段已把运行手册中的启动命令改为 `build/dev-ninja`（`admission-cutover.md`、`shared-mongodb.md`、M1–M4 规格）。余下的出现都不改：`linux-arm64-development.md` 第 52 行与 `docs/plans` 下两份计划记录的是当时的日志位置与步骤；`admission-cutover.md` 的“旧 `build/dev` 产物不构成证据”本就指旧目录，仍然成立；`docs/research` 下的调研、决策文档与历史资产记录的是当时的路径；README 提到 `build/dev` 只是说明旧目录不再读写。

### 异常与限制

- **Lima 上的多余重链（两侧相同，不影响正式样本）。** 每侧冷入口之后的第一次 `fast-entry` 预热都重链了同样 42 个 Unit 测试（0 编译），正式样本与准备样本全是 0/0；Mac 没有出现。Ninja 侧的 `.ninja_log` 显示，那次构建里只重跑了 3 个 absl 静态库的归档边（`libabsl_int128.a`、`libabsl_string_view.a`、`libabsl_decode_rust_punycode.a`），没有编译，42 个测试经 protobuf 依赖它们而重链。命令未变时 Ninja 只在产物比输入旧时重跑一条边；归档紧跟最后一个目标文件之后几毫秒内完成，Lima 的时钟若在两者之间向后步进约 335 ms（见下条），归档的 mtime 就早于目标文件。Make 侧没有留下逐边记录，它同样按 mtime 判定，重链的也是这 42 个经 protobuf 依赖 absl 的测试。恢复验证里删 `sodium.h` 那一例的第二次构建重链 56 个（0 编译），也符合这一机制：第一次构建重编并重新归档了 `librealm_game_common.a`，而它下游恰好是 56 条链接边（另行 touch 其一个目标文件实测）；当时的第二次构建日志已被后续用例覆盖，无法逐边确认。之后在同一目录重做 3 次删 `sodium.h`，第二次构建均为 0/0。预热不计入统计，这一现象不影响配对结果；Lima 上的日常构建同样可能偶发一次这样的多余归档与重链，这不是依赖表达错误。
- **Lima 时钟步进。** Lima 的 guest agent 每约 10 s 按宿主时间步进一次系统时钟（约 335 ms），与 chronyd 互相拉扯。墙钟样本由单调时钟计时，不受影响；但文件 mtime 跟着步进，是 Lima 上偶发多余重编、重链的候选来源之一。测量期间没有修改 VM 的时间设置。
- **Mac 空闲睡眠。** 第一次尝试的 Mac 测量被系统空闲睡眠打断，作废重测；正式数据全部在 `caffeinate` 下取得。
- **负载与环境。** Mac 测量期间桌面应用照常运行。Lima 上有一个与本项目无关的空闲 qemu 进程（CPU 约 1%），`/tmp`（tmpfs）测前已用 89%，构建临时文件经 `TMPDIR` 放在磁盘上。
- **编译数据库的不存在目录。** 两预设的编译数据库都带有 `_deps/protobuf-build/src` 这一 `-I` 目录，它来自 protobuf 上游目标的接口包含路径，目录不存在，编译器忽略；与生成器无关。
- **CMake 3.20 未实跑**；两平台本机与 CI 的 CMake 都更高。`JOB_POOLS`、`CMAKE_JOB_POOL_LINK`、`JOB_POOL_LINK` 与 `BUILD_BYPRODUCTS` 在 3.20 都已可用；不用 3.26 的 `INSTALL_BYPRODUCTS`（见实施记录）。
- **macOS 不带 QUIC 的组合**本机未测，由 CI 的 macOS job 覆盖构建与测试；CI 两个 job 都改用 Ninja。

### 资产

- [`p4-mac-samples.json`](assets/build-optimization-results/p4-mac-samples.json)、[`p4-lima-samples.json`](assets/build-optimization-results/p4-lima-samples.json)：`pairs` 下 `final`（`ca4b978`）为逐场景的逐组配对、两侧耗时、降幅、工作量与退出码，Mac 另有 `pool-all`（`1d96663`）；`groups` 为各侧 `measure.py export` 结果（含预热、准备与复位样本，以及每个样本的编译源码与链接产物名单）。本机路径都替换为 `<bench>`；原始日志、时间线与验证日志没有提交。
- 复现：按上表提交做两份源码副本并链接 `.tools`，准备一次 `--scenario fetch`。每侧以 `--preset dev-make`（Make）或 `--preset dev`（Ninja）调用 `measure.py run`，加 `--cache-mode OFF` 与平台 jobs（Mac `--jobs 8`，Lima `--jobs 2 --env TMPDIR=<磁盘目录>`）。冷入口按奇偶顺序交替 `--scenario cold-entry --long-samples 1` 三组；短场景每侧先 `--samples 0` 预热，再按奇偶顺序交替 `--samples 1 --sample-start N --no-warmup` 五组（见 [tools/build-bench](../../tools/build-bench/README.md)）。删除恢复：在 Ninja 构建目录里删除上表的单个文件，构建两次并统计编译与链接行。

## P5：原生 ccache 与 CI 可信快照（#124）

审查起点 `73a6de0f6cc2406291101c48af9cd0f88ee14cb7`；发现并经用户确认纳入 M3 帧尾指标读取竞争后，先独立提交测试修复 `cf567594e6064ac88fca420f3720e3527b697bfc`，作为最终采样父版本；阶段代码是该版本加本票缓存补丁。下列 OFF/ON 对照都在同一阶段源码上，新增两个 integration 契约不参与构建等待计时。逐文件 SHA 清单、测量工具 SHA、逐次结果与日志 SHA 保存在本节资产；提交号由 Git 历史定位。**本机兼容缓存重建的结果与 CI 净成本分开判定；尚未测得真实 CI restore/save，不能宣布 P5 整体验收完成。** P6 的 R0 累计比较不在本节替代。

### 环境与采样

| 项目 | Mac | Lima Ubuntu |
| --- | --- | --- |
| 架构 / 内存 / CPU | arm64 / 48 GiB / 15 核 | aarch64 / 7.73 GiB / 8 核 |
| 编译器 | Apple clang 21.0.0 | GCC 15.2.0 |
| CMake / Ninja / ccache | 4.4.3 / 1.13.2 / 4.14.1 | 4.2.3 / 1.13.2 / 4.12.3 |
| preset / binaryDir | `dev` / `<bench>/source/build/dev-ninja` | 同左 |
| 构建预算 / 链接池 / 外部 Make | 8 / 1（GTest 链接沿用 P4 放出）/ 1 | 2 / 1（同左）/ 1 |
| QUIC | Homebrew libmsquic，开启 | 开启 |
| 缓存 | `<bench>/source/.cache/ccache`，5GiB | 同左 |

两平台分别在拥有的隔离源码副本运行，同一宿主不同时测量；不改 VM 配置。依赖源码预先备齐、复用版本一致的 `.tools`；libsodium 1.0.22 的已校验原包 SHA256 `adbdd8f16149e81ac6078a03aca6fc03b592b89ef7b5ed83841c086191be3349` 在计时前准备。每个 `cold-build` 删除整个 binaryDir 再配置与构建 ALL，libsodium 的 configure/build/install 和全部外部编译产物都重做。ON 的兼容缓存仅由同路径同 flags 的构建预热，不使用路径映射或宽松校验。两边各三组，奇数组 OFF→ON、偶数组 ON→OFF；主计时包含来源定位、标准 configure、ALL build，不包含 CTest、依赖下载或工具首次安装。OS 页缓存没有清空。

Linux 构建与隔离 fixture 使用磁盘 `TMPDIR`，避开 tmpfs；开测前可用磁盘约 58 GiB，满足源码、产物、日志与 5GiB 缓存余量。Mac 用 `caffeinate`，桌面应用照常运行。宏观压力来自采样中的整个测量进程树与平台数据，不以单个 compiler 峰值乘 jobs。

### 三组兼容缓存重建

| 平台 / 模式 | 三次总等待 s | 中位 s | build 三次 s | 总等待降幅 |
| --- | --- | ---: | --- | ---: |
| Mac OFF | 68.56 / 68.36 / 68.04 | 68.36 | 64.02 / 63.89 / 63.63 | — |
| Mac ON 热兼容缓存 | 41.13 / 41.05 / 41.40 | 41.13 | 36.67 / 36.59 / 36.94 | **39.8%** |
| Lima OFF | 174.54 / 173.56 / 182.40 | 174.54 | 172.96 / 172.07 / 180.73 | — |
| Lima ON 热兼容缓存 | 29.27 / 28.40 / 30.18 | 29.27 | 27.71 / 26.90 / 28.45 | **83.2%** |

Mac 六个正式样本均退出 0，每个都是 **777 次原生编译请求、77 次观测链接、137 次未缓存 libsodium C 编译**；ON 三轮各 777 次 direct hit、0 miss，与 OFF 相同源码及链接产物名单。配对降幅 40.0% / 39.9% / 39.2%，3/3 同向。请求数是缓存外 observer 的调用数，不能称为真实 compiler 执行数；hit/miss 取实际项目工具、目录与配置的计数差。链接计数来自 C/CXX linker launcher，静态归档不经过它；ALL 中的归档及外部 Make 步骤仍执行，原始构建日志另存。

Mac 正式六轮进程树 RSS 峰值 1910.19 MiB，最低可用内存 16891.52 MiB；压力等级最高 1（正常），swap 3.75 MiB 且零增长，未观察到持续换页或内存异常。缓存上限 5GiB，最终 cache 约 121 MiB（测量前清空自有缓存，不保留旧 flags 版本）。

Lima 六轮均退出 0，每轮 **776 次原生编译请求、76 次观测链接**，ON 每轮 776 direct hit、0 miss；配对降幅 83.2% / 83.6% / 83.5%，3/3 同向。本机两平台各自超过 20% 的兼容缓存净重建目标。Lima 的 libsodium 每轮编译同样 137 个独立源码；编译请求为 137–139 次，多出的一至两次发生在 `make install` 的重检查中并伴随归档，未接缓存，ON/OFF 两侧都出现。该波动原样计时、名单与重复项保留，未剔除或省掉外部产物；具体 mtime 原因本轮未诊断。

Lima 正式样本整个进程树 RSS 峰值 1522.03 MiB，最低可用内存 1757.87 MiB（高于 1GiB）；swap 增长为 0、OOM kill 增量为 0。VM 可用内存按 7.73GiB 实际内存及 cgroup 无限额核实，没有借宿主 48GiB 推断；缓存约 146MiB，上限 5GiB。

### 首次填充与异常保留

修复前的 Mac 最初沙箱运行的 build 成功（79.35 s，775 miss + 2 preprocessing hit），但工具在读取 `sysctl hw.memsize`/进程资源时被沙箱拒绝，驱动最终退出 1；该环境还漏掉 `-arch arm64`。之后解除沙箱的第一组 ON 因 flags 不兼容重新填充（78.01 s，775 miss + 2 preprocessing hit；同组 OFF 68.12 s）。这些是首次填充/失败的筛查记录，不能算热兼容缓存收益。修正环境并确认相同 flags 后重新取得完整三组正式对照；所有筛查样本与失败原因保留在资产的 `previous_snapshot`，最终空缓存填充另列于 `pilot_rows`，没有挑最快的三次拼组。空缓存有额外哈希/存储开销，收益只针对已兼容预热的缓存。

最终冻结后的 Mac 首次填充为 77.95 s（build 73.12 s，775 miss + 2 preprocessing hit）；Lima 首次填充单列为 203.19 s（build 201.45 s，774 miss + 2 preprocessing hit），成功退出 0；不计入三组热缓存统计。它与正式组使用同一源码、工具链和完整空产物复位条件，不包含来源首次获取或真实 CI 首次上传。

Linux 首轮完整 CTest 为 662/663，唯一失败是新增损坏夹具按十六进制文件名找不到 ccache 4.12 的 base32 条目；该轮失败日志保留。修复只把条目识别改为公开 `ccache --inspect` 接口，损坏后的实际重编译、程序结果和诊断断言未降低；两平台原生六项均通过后，重新运行完整验证。它不改变测量场景、compiler、flags 或缓存配置；修复前的 Linux 采样清单完整保留于 `previous_snapshot`；最终冻结后的组包含该修复。

Mac 最后一次复测出现 M3 指标断言失败：5000 个取号机器人和 1000 个轮询机器人全部完成，立即读取的 `tickets_issued_total` 为 4985（原门槛 4995）。该轮完整 CTest 为 662/663，日志保留；此前同一阶段完整 ON/OFF 各 663/663，失败后二进制不变的独立 M3 为通过（50.83 s）。源码证据显示 QueueService 在 `tick()` 帧尾发布指标，现有测试在 `run_loadgen` 请求完成后立即读取，读取时机竞争与指标的帧尾发布顺序相符。用户确认后，现有用例改为最多等待 2 秒、每 5ms 读取该公开指标，再执行原门槛断言；机器人、成功率、运行时代码未改。修复后连续三轮 M3 全通过（47.53 / 46.99 / 46.13 s，遇失败即停），独立提交 `cf56759` 冻结，再重新取得全部三组配对。修复前的正式样本（Mac 40.4%、Lima 82.7%）完整保存在资产的 `previous_snapshot`，不拼进最终组；测试修复本身不计入缓存收益。

随后 Mac OFF 全量在 L1 的同一公开指标读取边界失败（662/663，402.51 s）：300 次请求全部成功，帧尾 `tickets_issued_total` 立即读取为 253 而非原等式要求的 300。L1 沿用已确认的指标等待边界，最多等 2 秒，每 5ms 读取，最终仍 `EXPECT_EQ(..., 300)`；300 次请求、全部成功、验签与 30 秒吞吐断言原样保留，吞吐计时在等待前结束。最初重复验证前八轮通过，第九轮遭系统空闲睡眠 594 秒并超时（电源日志对应），失败原样保留；补回防睡眠包装后连续十轮通过（40.04 s，遇失败即停）。这一后续同步补丁单独提交 `675d5c7`；上表性能样本仍冻结在 `cf56759` 加缓存补丁，没有把 L1 修复并入缓存收益。最终正确性验证按下表区分版本与范围。

### 正确性、关闭与恢复

`CompilerCacheConfigTest` 通过公共 CMake 配置/CTest 注册与 CI 键 CLI 覆盖 AUTO/ON/OFF、工具缺失/错误版本/无效显式路径、已有 launcher 冲突、ON→OFF 仅清项目 launcher、自动发现工具移除后的重新检测、显式工具路径下 ON/OFF 测试一致、目录/容量约束，以及固定依赖元数据/ccache 版本改变兼容桶。`CompilerCacheNativeTest` 使用独立 C/C++ 小工程和真实程序输出对照冷/热/OFF；源码、普通头、宏、flags、compiler 内容、生成头改变或删除均正确失效；损坏条目回到真实编译、1KiB 容量淘汰后重建仍得到同一结果；故意编译错误原样失败，未做无条件重试。个人宽松环境配置不能覆盖逐调用的严格设置；测量统计也不会误读个人缓存或另一个 PATH 工具。测试只操作隔离目录。

Linux L1 定向复测一度未继承原磁盘 `TMPDIR`，默认 tmpfs 仅余 142888960 字节，低于 MongoDB 的 524288000 字节启动门槛，故服务未启动；该失败与指标修复无关。恢复原验证目录后，ON/OFF 各连续三轮通过（7.91 / 7.00 s），不修改 MongoDB 门槛或测试规模。该环境失败的日志与校验值也保留于最终验证资产。

配置契约 7 个、原生契约 6 个均通过；开发机工具都可用，因此完整 CTest 的 ON/OFF 两种模式都包含这两个 integration target，没有以关闭缓存缩小合集。测量工具既有 58 个测试通过，Python 编译检查通过。自动化均使用隔离 etcd/MongoDB，没有访问共享开发库。

| 平台 / preset / 缓存 | 全量 CTest | CTest 总时间 | 源码范围 |
| --- | --- | ---: | --- |
| Mac `dev` / ON | 663/663，0 跳过 | 396.35 s | `cf56759` 加缓存补丁；L1 后续同步变更另复测 |
| Mac `dev-make` / OFF | 663/663，0 跳过 | 382.26 s | 含 `675d5c7` 的最终源码，失败后完整重跑 |
| Lima `dev` / ON | 663/663，0 跳过 | 322.55 s | `cf56759` 加缓存补丁；L1 后续同步变更另复测 |
| Lima `dev-make` / OFF | 663/663，0 跳过 | 321.22 s | 同上 |

`675d5c7` 之后唯一变化的 L1 用例在四种组合均重建验证：Mac ON 三轮（12.65 s）、OFF 十轮（40.04 s），Lima ON/OFF 各三轮（7.91 / 7.00 s），遇失败即停且全部通过。没有把另三种组合在该补丁前完成的 663 个测试写成补丁后完整重跑。两平台 ON 各连续三轮配置后 `ninja: no work to do.`，8 个版本头的内容及时间戳均不变。最终材料代码与性能冻结清单逐文件校验，仅该测试文件因 L1 同步补丁不同，运行时及缓存代码相同。Mac 主检出恢复 `dev` AUTO。

Lima ON 的 Linux 登录链验收使用真实 QUIC，报告版本 `cf56759` 加缓存补丁；M1/M2 与传输各三轮，M3/M4 各一轮，结果如下。M2 中包含的 L1 后续补丁已另做上述定向复测；不声称专用宿主的生产容量门槛已覆盖。

| 验收组 | 轮数 | 时间 | 结果 |
| --- | ---: | ---: | --- |
| QUIC 传输与拓扑 | 3 | 13 s | PASS |
| M1 定向水位/浸泡 | 3 | 66 s | PASS |
| M2 准入与持久取号 | 3 | 35 s | PASS |
| M3 取号与进度负载 | 1 | 59 s | PASS |
| M4 故障与恢复 | 1 | 62 s | PASS |

全量、稳定配置、L1 定向验证、失败和 M1–M4 报告的校验值保存在资产的 `final_validation`；它与性能 `rows` 独立，保留各自源码范围。

### CI 与验收结论的边界

CI 两平台显式安装 ccache、ON/2GiB，以 OS/架构、工作路径、compiler 内容/版本/目标、SDK/sysroot/标准库宏、工具版本、Debug 及固定依赖元数据分桶；restore 不跨桶，weekly key 限制不可变快照增长。成功 main push/手动维护运行在全部测试（Linux 含 M1–M4）之后保存，PR 只恢复；恢复/保存允许缓存服务失败，正常源码编译与同一测试流程继续。日志分别记录键计算＋检查＋restore、save 的秒数，以及命中、未命中、大小、清理和 action 结果。YAML 语法与配置键 CLI 测试已通过，发布条件也经代码审查。

**真实 GitHub runner 的空/热缓存、首次上传、恢复不可用及包含 restore/save 的三组净等待尚未运行，P5 的 CI 门槛未通过验收（缺证据，非实测失败）。** 本机收益不能抵扣 CI 传输成本。后续只在两个实际 job 的相同 runner/编译器/SDK/路径、空完整产物状态下交替 OFF 与兼容 ON，至少三组（有界扩展至五组），包含键检查、恢复、构建和需要发生的保存；未达 20% 或不明确时记录未通过/无法判定，按约定在缓存配置/OFF 回退内重决策。不以本票结果宣布 P6 或首批全部优化验收完成。

本机测试均开启 QUIC；macOS CI 的 TLS/TCP-only 组合及 Linux x86_64 实际 runner 本轮未运行。CMake 3.20 与最低允许 ccache 4.8 未实跑，已测版本见环境表；版本/配置契约覆盖最低版本判定，但不代替真实最低版本构建。最终采样包含 AUTO 重新发现、显式 OFF 测试登记、公开缓存条目识别、统计工具解析、CI 元数据/计时与独立 M3 测试修复。两平台隔离副本的 Git 元数据均指向固定父版本 `cf56759`（缓存补丁为 dirty），避免验收脚本把无 `.git` 副本或另一检出的 HEAD 当阶段版本；最初无 Git 的验收入口失败也保留于日志。两平台资源和收益分别判定，不混合秒数。

### 回退与复现

```bash
cmake --preset dev -DREALMMESH_CCACHE=OFF
./scripts/build.sh --preset dev
# 同时使用 P4 的生成器回退：
cmake --preset dev-make -DREALMMESH_CCACHE=OFF
./scripts/build.sh --preset dev-make
```

OFF 不删专用 cache，ON/AUTO 可重新开启；空的 `REALMMESH_CCACHE_EXECUTABLE` 表示自动查找。用户自定义 launcher 用 OFF 保留。缓存删除、损坏、淘汰或 CI 恢复缺失只增加重新编译工作；真实 compiler 错误不重试。

隔离副本链接相同 `.tools`，一次准备 `*-src` 与已校验 sodium 原包；编译 `tools/build-bench/launcher.cpp`。先以 ON 执行一次 `--scenario cold-build` 填充兼容缓存，再交替 OFF/ON，每侧 `--long-samples 1 --sample-start N` 三组，Mac `--jobs 8`、Lima `--jobs 2 --env TMPDIR=<磁盘目录>`，其余 tool/preset/source/deps/路径不变。每次命令都由 `measure.py` 保存于样本的 `steps`。

### 资产

[`p5-mac-samples.json`](assets/build-optimization-results/p5-mac-samples.json)、[`p5-lima-samples.json`](assets/build-optimization-results/p5-lima-samples.json) 保留逐次原始结果、前后计数、编译源码/链接产物名单、外部 sodium 请求数、资源与环境、工具及采样源码 SHA、日志 SHA、总等待中位数/min/max 与配对差值；失败/初次填充另列，`previous_snapshot` 保存 M3 修复前的完整组。路径归一为 `<bench>`/`<checkout>`；完整原始日志和资源时间线留在本机拥有的隔离测量目录，不提交。
