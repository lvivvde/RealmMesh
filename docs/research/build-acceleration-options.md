# 调研：构建工具与编译缓存的适用条件及取舍

- 日期：2026-10-02。
- 决策票：[调查构建工具与编译缓存的适用条件及取舍](https://github.com/lvivvde/RealmMesh/issues/111)。
- 地图：[构建提速与接口边界优化决策地图](https://github.com/lvivvde/RealmMesh/issues/109)。
- 状态：适用条件调查完成；候选工具、并行度和代码边界尚未选定。
- 范围：C++20、项目声明的 CMake 3.20 下限、macOS 与 Linux；覆盖增量构建、干净构建、首次配置及依赖准备。只调查，没有安装工具、修改构建配置或执行性能基线。

## 可以据此决定什么

这些手段作用于不同成本，必须分阶段比较。Ninja 改变依赖调度，并行度改变同时运行的任务数；编译缓存复用相同编译结果；PCH 和 Unity 减少重复解析；依赖产物复用可以跳过已完成的依赖构建。后续票应依据实测的阶段占比选择组合。本调查不给加速比例，也不把任何组合设为默认方案。

下表的“可能受益”是根据各工具机制推导的候选场景，未在 RealmMesh 上测量。“首次”指既无构建产物也无兼容缓存；恢复远端缓存后的新 checkout 属于另一种场景。

| 候选 | 增量构建 | 干净构建 | 首次配置、下载与依赖准备 | 需先确认 |
|---|---|---|---|---|
| Ninja | 调度与判断开销可能减少 | 调度与默认并行可能有效 | 不复用下载或编译结果 | 外部 Make 构建、生成文件依赖、内存 |
| 显式并行 | 有多个独立失效任务时有效 | 独立任务多时可能有效 | 独立依赖可并行时可能有效 | 串行关键路径、峰值内存、嵌套并行 |
| ccache / sccache | 重复命中时有效；修改本身通常需要编译 | 删除 build 后缓存仍在，可复用命中项 | 空缓存不省首次编译；可恢复兼容缓存 | 命中率、不可缓存原因、路径、工具链 |
| target PCH | 公共重头稳定时可能有效 | 先建 PCH，再减少重复解析 | 不省下载；增加 PCH 建立步骤 | 解析占比、失效范围、flags、缓存组合 |
| target Unity | 可能扩大一次修改后的重编译批次 | 重复头解析多时可能有效 | 不省下载；可改变源码依赖的编译成本 | ODR、宏泄漏、内存、编译数据库 |
| 固定依赖产物复用 | 已有依赖正常不重建时收益有限 | 可跳过兼容依赖的重建 | 新 checkout 恢复兼容产物可能有效 | ABI、来源、配置键、路径、生成器版本 |

机制来源分别见下面各节；表中工程收益均为推断。

## 仓库观察：来源与工具文档分开

这些是本次读到的配置事实，不是性能结论，也不是全仓库依赖审计。

| 已核实的事实 | 本地来源 |
|---|---|
| 项目声明 CMake 最低 3.20；dev 使用 Unix Makefiles、Debug、导出编译数据库；build preset 未填写 jobs。 | [根 CMakeLists.txt:1](../../CMakeLists.txt#L1)、[CMakePresets.json:12–24](../../CMakePresets.json#L12-L24) |
| 日常脚本每次先 configure，再 build，最后完整 CTest；该 build 命令没有显式 parallel 参数。整个脚本耗时不能直接当编译耗时。 | [scripts/build.sh:23–32](../../scripts/build.sh#L23-L32) |
| 两个 CI job 的 build 命令也未显式传 parallel；这里的有限观察不代表实际环境一定单线程。 | [.github/workflows/ci.yml:59–66](../../.github/workflows/ci.yml#L59-L66)、[105–112](../../.github/workflows/ci.yml#L105-L112) |
| libsodium 1.0.22 用源码包与 SHA256，经 Autotools 外部项目构建静态库，build/install 命令为 `$(MAKE)` / `$(MAKE) install`。 | [third_party/sodium/CMakeLists.txt:6–21](../../third_party/sodium/CMakeLists.txt#L6-L21) |
| Protobuf runtime 35.0 与 Abseil 20250512.1 使用固定源码包；支持的主机下载同版本 protoc；full runtime 排除默认构建，项目 Protocol 链接 lite。已有这些裁剪，不能把它们作为尚未实施的优化。 | [third_party/protobuf/CMakeLists.txt:8–14](../../third_party/protobuf/CMakeLists.txt#L8-L14)、[24–59](../../third_party/protobuf/CMakeLists.txt#L24-L59)、[61–75](../../third_party/protobuf/CMakeLists.txt#L61-L75)、[104–108](../../third_party/protobuf/CMakeLists.txt#L104-L108)、[proto/CMakeLists.txt:47–50](../../proto/CMakeLists.txt#L47-L50) |
| Mongo C 2.5.5 / C++ 4.6.0 驱动固定源码包与 SHA256、静态构建；tests/examples 已关闭。改变依赖分发方式需另作决策。 | [third_party/mongo/CMakeLists.txt:16–34](../../third_party/mongo/CMakeLists.txt#L16-L34)、[36–52](../../third_party/mongo/CMakeLists.txt#L36-L52) |
| `.pb.cc` 与 `.pb.h` 是 custom command 输出，依赖 protoc target 与 `.proto` 输入，再组成 Protocol target。 | [proto/CMakeLists.txt:18–38](../../proto/CMakeLists.txt#L18-L38) |

证据层级为 Verify。父会话提供的符号图上下文用于范围定位；本笔记的上述配置事实均直接读取原文件。图项目 `Users-edwin-Projects-RealmMesh`，generation `2026-10-02T06:49:29Z`，metadata recorded `2026-10-02T07:42:43Z`；本次 index_status 为 ready。对表中全部 8 个文件调用 check_index_coverage：非 third_party 路径为 metadata_match / no_recorded_issue；3 个 third_party 文件为 excluded / not_tracked，已全文读取补证。未对全仓库做负面或穷尽断言；干净覆盖信号也不保证完整。

## Ninja 与显式并行

1. Ninja 支持 macOS/Linux，默认根据 CPU 并行，并有面向增量依赖判断的设计；它不会替换编译器。必须先比较“已有 Make + 相同 jobs”与“Ninja + 相同 jobs”，才能区分生成器与并行的贡献。[Ninja 官方手册](https://ninja-build.org/manual.html)
2. `CMAKE_BUILD_PARALLEL_LEVEL` 从 CMake 3.12 提供，可限制 `cmake --build` 并发；3.20 下限满足。任务数应是可配置参数。按机器内存、单任务峰值和链接峰值选 jobs 是工程推断，不能仅按核心数许诺收益；CTest 并发需要独立评估。[CMake 3.20 并行环境变量](https://cmake.org/cmake/help/v3.20/envvar/CMAKE_BUILD_PARALLEL_LEVEL.html)
3. Ninja 并行要求依赖准确；Ninja 1.11 起的 `missingdeps` 可检查生成文件缺少生成目标依赖的问题。protobuf 应验证新 build 目录中的生成顺序，而不能只相信已有 `.pb.h` 的成功构建。[Ninja 官方手册：头依赖、missingdeps](https://ninja-build.org/manual.html)
4. 项目的 `$(MAKE)` 绑定是切换前置核查项，尚未执行 Ninja 兼容测试。ExternalProject 可覆盖 build/install 命令；不能推断父 jobs 自动传入任意外部命令。Ninja 1.13 的 GNU jobserver 支持有额外条件，POSIX 需 GNU Make 4.4+ 的 FIFO 模式；不能把它假定为旧工具或 macOS 系统 Make 已具备的能力。[CMake 3.20 ExternalProject](https://cmake.org/cmake/help/v3.20/module/ExternalProject.html)、[Ninja 官方手册：GNU Jobserver](https://ninja-build.org/manual.html)

## ccache

1. 复用相同编译结果；空缓存首次不省编译。要记录命中与失效原因。[ccache 手册](https://ccache.dev/manual/latest.html)
2. 编译器、选项和路径影响命中；`base_dir` 改写路径可能影响 depfile，不能盲目设置。[ccache 手册：不同目录编译](https://ccache.dev/manual/latest.html#_compiling_in_different_directories)
3. 新生成文件的时间戳可能触发安全性禁用缓存；protobuf 应观测重生成后的命中，不随意放宽检查。[ccache 手册：新文件](https://ccache.dev/manual/latest.html#_handling_of_newly_created_source_files)
4. GCC/Clang PCH 支持有限，需要特定 flags 与 `pch_defines,time_macros` 放宽项；组合实验必须评估宏变化正确性。[ccache 手册：PCH](https://ccache.dev/manual/latest.html#_precompiled_headers)

本次手册标识 4.14.1；这不是本机安装版本或推荐最低版本。适配候选安装版本时须复查相关节。

## sccache 与接入边界

1. sccache 支持 GCC/Clang，默认本地磁盘缓存，也支持远端存储；有本地后台服务。远端模式增加传输和运维成本，是否值得需测命中后端到端时间。[sccache 官方 README](https://github.com/mozilla/sccache)
2. C/C++ 缓存键包含预处理结果、编译器与若干 flags/environment；文档的多 `-arch` 缓存有开关条件。路径默认需要匹配，当前 main 文档提供 `SCCACHE_BASEDIRS`，需确认所选 release 已有该能力。[sccache 缓存机制](https://github.com/mozilla/sccache/blob/main/docs/Caching.md)、[sccache 官方 README](https://github.com/mozilla/sccache)
3. 通过 C / CXX compiler launcher 接入两种缓存，Makefile 与 Ninja 均支持；该属性从 CMake 3.4 提供。变量在创建 target 时初始化属性，事后设置不等于所有 target 已生效。launcher 的 generator expression 要 3.25，不能用于 3.20 兼容方案。[CMake compiler launcher](https://cmake.org/cmake/help/v3.26/prop_tgt/LANG_COMPILER_LAUNCHER.html)
4. CMake launcher 不自动覆盖任意 Autotools 编译命令；libsodium 是否进入缓存需核实真实编译调用。sccache 与 PCH 的候选 release / 编译器组合尚未验证，不能套用 ccache 的配置。这里读的是官方 main 文档，不宣称某个历史 release 已支持全部特性。

## target PCH

1. `target_precompile_headers` 从 CMake 3.16 提供。候选应是同一 target 多次解析且很少变化的重头；建立 PCH 也有成本，净收益待测。[CMake 3.20 PCH](https://cmake.org/cmake/help/v3.20/command/target_precompile_headers.html)
2. CMake 强制包含生成的 PCH 包装头；可能掩盖源码自身缺失 include。评估期需保留禁用 PCH 的构建作为正确性对照，这是机制带来的工程验证要求。[CMake 3.20 PCH](https://cmake.org/cmake/help/v3.20/command/target_precompile_headers.html)
3. `REUSE_FROM` 要求两 target 的 compiler options / flags / definitions 相同；各目标的平台宏、告警、语言配置与调试选项应逐项比较。消费者应控制 PCH，一般不把 PCH 作为安装库的 PUBLIC usage requirement。[CMake 3.20 PCH](https://cmake.org/cmake/help/v3.20/command/target_precompile_headers.html)
4. 可按 target 或 source 禁用。工程推断：频繁变化的公共接口或生成 protobuf 头放入 PCH 会扩大失效范围；应实测，不因为头“大”就纳入。跨机器恢复 PCH 产物也需验证编译器与路径兼容，不默认通用。

## target Unity

1. `UNITY_BUILD` 从 CMake 3.16 提供，CMake 3.20 可用 BATCH / GROUP 合并 C / CXX 源文件；官方要求保持开发者可选，不直接全局强开。[CMake 3.20 Unity](https://cmake.org/cmake/help/v3.20/prop_tgt/UNITY_BUILD.html)
2. 合并可能违反 ODR；匿名命名空间、文件静态标识符、宏状态需检查。可按 source 排除，或使用 unique id / include 前后钩子；不能把这些机制当成自动修复。[CMake 3.20 Unity](https://cmake.org/cmake/help/v3.20/prop_tgt/UNITY_BUILD.html)
3. 合并单位更大，修改一个 `.cpp` 会重新编译整个批次；内存和并行任务数也变化。这是机制推断，须同时测干净构建与单文件修改，不以一项覆盖另一项。[CMake 3.20 Unity](https://cmake.org/cmake/help/v3.20/prop_tgt/UNITY_BUILD.html)
4. CMake 官方明确编译数据库与 Unity 组合工作不佳；当前 dev 依赖编译数据库，必须验证编辑器、clang tooling 与源码索引，或独立保留非 Unity 配置。[CMake 3.20 编译数据库](https://cmake.org/cmake/help/v3.20/variable/CMAKE_EXPORT_COMPILE_COMMANDS.html)

## 依赖复用与 CI 缓存

1. 分开讨论三种对象：下载源码缓存只省下载；编译器缓存只复用命中的编译；依赖安装产物复用可跳过该依赖的编译。能否真正跳过步骤取决于接入实现，不能把打包整个 build 目录等同于安全复用。[ExternalProject 构建步骤](https://cmake.org/cmake/help/v3.20/module/ExternalProject.html)、[GitHub 缓存机制](https://docs.github.com/en/actions/reference/workflows-and-actions/dependency-caching)
2. 产物键至少应区分 OS、架构、编译器及版本、SDK / sysroot、标准库与 ABI、Debug / Release、静态/PIC、依赖版本校验和、补丁及 CMake options；这是工程约束，具体字段由依赖审核确定。不能把跨 OS archive 能解包当作二进制兼容。搬移含绝对路径的 build 状态也需要单独验证。[sccache 缓存输入](https://github.com/mozilla/sccache/blob/main/docs/Caching.md)、[GitHub 缓存键与路径](https://docs.github.com/en/actions/reference/workflows-and-actions/dependency-caching)
3. Protobuf C++ 要求生成代码版本与 runtime 精确匹配，官方不承诺任何 release 间 ABI 稳定；复用 `.pb.*` 或 runtime 时同时锁定 protoc、runtime、`.proto` 内容与生成参数。现有输出依赖声明不替代恢复产物的版本验证。[Protobuf 官方兼容保证](https://protobuf.dev/support/cross-version-runtime-guarantee/)
4. GitHub cache 的内容不签名或验证，缓存路径不能含秘密；应从可信触发写入，限制不可信代码写入共享产物。编译缓存命中只是复用判定，不能成为恶意写入产物的认证。实际设计应明确读写者与隔离键，并保留 cache miss 的完整构建路径。[GitHub 缓存安全要求](https://docs.github.com/en/actions/reference/workflows-and-actions/dependency-caching)

## 后续决策的最小输入

- macOS 与 Linux 各自记录 configure / download / compile / archive / link / test 时间；CPU、内存、工具版本、编译 flags 与 QUIC 是否启用随报告保存。先把脚本总等待拆开。
- 对每个候选单独比较：空产物且空缓存、空产物且兼容缓存、无修改、单 `.cpp`、私有头、公共接口头、`.proto` 修改。后续依赖边界审计决定代表性文件，不在本调查指定。
- 同时记录峰值内存、缓存命中/失效/不可缓存、实际运行编译任务数。编译数据库条目数不能代替执行工作量。
- 留待决策：合理 jobs；Ninja 的 libsodium 适配；缓存工具及版本、存储位置与容量；PCH / Unity 目标范围与工具兼容；依赖产物复用是否保持当前源码版本、校验和和平台承诺。所有候选需要独立禁用或缓存缺失时的可用路径。

## 来源口径

全文使用 12 份官方来源，引用已放在对应结论旁。CMake 尽量引用 3.20 文档以核对声明下限；compiler launcher 的 3.20 页面本次无法读取，使用 3.26 官方页的版本注记，仅采用 3.4 的 C / CXX 能力并明确排除 3.25 的扩展。Ninja 手册标识 1.13.1；ccache 手册标识 4.14.1；sccache 为读取日的 main 文档。文档能力与本地实际版本必须分别验证。没有引用第三方性能数字，也没有据此推荐默认方案。
