# 构建工具、并行缓存与依赖复用策略

关联[选择构建工具、并行缓存与第三方依赖复用策略](https://github.com/lvivvde/RealmMesh/issues/112)。用户已通过 Q1–Q6 确认各项选择，并通过 Q7 最终确认完整方案与正式记录。本文是后续实施约束。Ninja 默认与 Make 回退已由 [#123](https://github.com/lvivvde/RealmMesh/issues/123)（P4）落地，见文末[实施记录（#123）](#实施记录123)；ccache 尚未实施。

## 首批选择与适用场景

| 对象 | 首批选择 | 生效条件与限制 |
| --- | --- | --- |
| 构建器 | 开发与 CI 目标默认 Ninja，保留显式 Make 回退 | 双平台正确性、生成文件恢复和工具兼容验证后切换；使用独立目录 |
| 编译并行 | CPU/内存预算，当前 Mac 8、Linux 2、CI 2 路起步 | 初值可调整，需实测整体内存压力；不声称最优 |
| 编译缓存 | ccache，本地磁盘与 CI GitHub 缓存复用 | 本机 AUTO；CI 显式启用；不部署跨机器共享缓存服务 |
| 第三方复用 | 现有 build 内正常增量复用，加 ccache 的兼容编译命中 | 保留固定源码、校验与静态构建；不恢复整个 build/_deps 或预编译依赖包 |
| 下载包缓存 | 首批不新增独立层 | CI 分离下载成本后再决定；首次配置耗时不能全部算下载 |
| PCH / Unity | 首批默认关闭 | 边界与工具策略复测后，依据重复解析证据逐目标评估 |

Ninja 与并行可能减少调度和独立任务等待；ccache 只省命中的编译，不能省链接、测试或空缓存首次编译。日常入口减少不必要的目标和测试工作、接口边界减少依赖传播，分别由已确认的[入口决策](build-test-entry-decisions.md)与[接口决策](interface-boundaries-proposal.md)约束。优化实施顺序和三类量化门槛由“锁定优化实施顺序与三类耗时验收标准”决定。

## Ninja、预设和目录迁移

规划 dev 使用 Ninja，binaryDir 为 build/dev-ninja；dev-make 使用 Unix Makefiles，binaryDir 为 build/dev-make。二者均有匹配的 configure/build/test 预设。旧 build/dev 保留，不自动删除、搬移或在其 Cache 上换生成器；不兼容选择按[隔离决策](dependency-isolation-decisions.md)明确诊断。Ninja 缺失时提示安装或显式选择 dev-make，不悄悄改生成器。

build.sh、test-fast.sh、test-watch.sh 保留已确认的统一 --preset，默认 dev。本机派生预设也使用实际 binaryDir。目录不能再由各脚本独立写死：配置阶段提供实际 CMAKE_BINARY_DIR 与所选预设的目录信息，公共入口读取同一信息；不要自行简化解析 CMake 的继承、宏和用户预设规则。跳过构建的启动/验收路径须检查所选目录已配置且身份匹配，缺失时给明确配置指引。

同步迁移的消费者包含编译数据库链接、watch 时间戳、dev-all-in-one/dev-services、realm_detach 的定位、macOS/Linux 接入验收、CI 报告上传路径及相关文档/脚本检查。启动与验收选择同一预设/目录，不能只迁移 build.sh 而继续运行旧 build/dev 的二进制。验收报告位置采用同一目录信息；CI 显式传递选择，不靠本机状态。

保留项目 CMake 3.20 下限。规划要求 Ninja 至少1.11，以便使用 missingdeps 检查；ccache 采用经实施验证的4.x版本，安装来源和实际版本随报告记录，不把文档 latest 版本当本机版本。构建入口不自动安装工具。采用前核实最低支持 CMake 与实际工具组合；不能引入仅新版本可用的选项而隐含提高下限。

[Ninja 官方手册](https://ninja-build.org/manual.html)说明生成依赖、并行和 missingdeps；[CMake 3.20 Presets](https://cmake.org/cmake/help/v3.20/manual/cmake-presets.7.html)定义预设关系。Ninja 默认并行不能代替本项目的资源预算。

## 外部构建与生成文件

libsodium 保留当前源码与 Autotools 构建。显式发现它所需的 Make，不能把 Ninja 的 CMAKE_MAKE_PROGRAM 当作 Make；build/install 首批明确串行。核实其真实 CC、SDK/架构和安装产物与主项目一致，不直接复制全部 CMake flags。

首批 ccache 覆盖通过原生 CMake C/CXX target 编译的项目和依赖；libsodium 外部编译暂不额外接缓存 wrapper，作为明示例外，记录其耗时，不声称全部依赖编译可缓存。它已有源码构建目录内的增量复用。若以后独立测量证明值得接入，再验证 compiler wrapper、CC/CCAS、选项、缓存命中和关闭路径，不把原生 launcher 配置当作它已经生效。

Ninja 默认切换的前提是明确产物生产者与顺序：libsodium 的安装库/头和 imported target 之间必须正确依赖；现有 BUILD_BYPRODUCTS 标注不等于安装步骤与删除恢复已验证。实施采用兼容3.20的表达；不依赖3.26的 INSTALL_BYPRODUCTS、3.28的 BUILD_JOB_SERVER_AWARE 等新增特性。

protobuf 的生成源码/头、输入与 protoc 依赖须在新目录与高并行下正确；修改输入、删除生成文件、替换明确工具路径/版本均按契约重生。现有三份 proto 不含 import，未来增加 import 时须补真实依赖，不能推断现有列表能覆盖未来输入。

[CMake 3.20 ExternalProject](https://cmake.org/cmake/help/v3.20/module/ExternalProject.html)允许明确构建命令；[compiler launcher](https://cmake.org/cmake/help/v3.20/variable/CMAKE_LANG_COMPILER_LAUNCHER.html)只初始化相应 target 的编译入口。Ninja 的 GNU jobserver 支持不意味着其任意 Make 子构建自动共享顶层预算。

## 编译资源预算

默认建议采用 min(逻辑CPU数, 8, max(1, floor((有效内存GiB − 2) / 2)))；有效内存取主机/VM与已知进程容器限额中的较小值，未知则1路。这是每路约2GiB、保留约2GiB的工程起点，不能当作内存保证。读取实际平台资源并打印预算与最终 jobs，不把 macOS 宿主内存拿来替代 Linux VM 限额。

入口统一 --jobs 正整数；优先级为显式 --jobs、非空 CMAKE_BUILD_PARALLEL_LEVEL、资源预算，空值走预算，非法值失败。CI 首批显式2路；若 runner 限额不足，调整其显式配置并记录，而不是默许超配。本机覆盖可提高或降低并行，但应保留调优报告。watch 向快速入口传递相同值。

Ninja 对原生链接使用容量1的 job pool；它是总体 jobs 的子集，不额外增加一组任务。Make 回退不能保证同样的链接池，须按实际内存压力使用安全 jobs，未验证时可 --jobs 1。外部 Make 的 build/install 明确 -j1，不将顶层 jobs 再乘入外部构建。Ninja job pool 也不自动覆盖任意外部命令，整体并行进程树仍须测量。

编译 jobs 与 CTest jobs 分开：Unit 默认4路可调，完整验证与CI CTest显式1路，沿用入口决策。不能因编译调优改变测试覆盖或已选择的串并行行为。

[CMake 并行参数](https://cmake.org/cmake/help/v3.20/envvar/CMAKE_BUILD_PARALLEL_LEVEL.html)与[Ninja job pools](https://cmake.org/cmake/help/v3.20/prop_gbl/JOB_POOLS.html)支持相应控制。

## ccache 接入、存储与失效行为

项目提供 AUTO/ON/OFF 三种模式：AUTO 发现 ccache 则启用，缺失则正常构建并显示关闭；ON 缺失明确失败；OFF 恢复直接编译，清除本项目管理的 C/CXX launcher，不能残留旧缓存启动器。已有用户自定义 launcher 要验证组合或明确报冲突，不能静默覆盖。配置在所有相关原生 target 创建前完成，日志显示模式、工具路径/版本和实际覆盖。

本机采用 build 目录外的项目专用缓存，默认上限5GiB，允许显式调整；CI 每平台/兼容工具链桶先以2GiB为候选上限。使用 ccache 正常压缩/淘汰，记录大小、命中、未命中、不可缓存和淘汰；这些容量未实测为最优，也不等于远端仓库总配额。构建清理不会顺带删除编译缓存。

首批保持正确性检查：compiler_check 使用 content；不设置 sloppiness、不忽略系统头、不关闭调试目录哈希、不用 compiler_check=none、不启用硬链接。首批不强设 base_dir；稳定 CI 工作路径优先。跨不同 checkout 路径的复用需以后验证路径映射、depfile、调试信息和命中净收益，不能为命中率放宽检查。新生成头导致暂不可缓存属于观测结果，不能忽略时间戳校验来掩盖。

缓存未命中、空目录或远端恢复不可用时走正常源码构建，仍运行相同测试；损坏项须验证正常重新编译与可诊断恢复。编译器错误仍失败，不能无条件绕过 launcher 重试把错误藏起来。缓存容量满时正常淘汰；无效显式工具或配置按契约失败。OFF 与完全空缓存提供独立对照。

[ccache 官方手册](https://ccache.dev/manual/latest.html)说明容量、compiler_check、路径与正确性设置。缓存失效是否正确还需项目场景验证；这些配置不是已经测到命中的证据。

## CI 复用与来源政策

CI 显式安装经过验证的 Ninja/ccache 并选择预设、jobs、缓存模式。只保存专用 ccache 目录；键的兼容前缀区分 OS/架构、编译器版本/身份、SDK/sysroot/标准库ABI、构建配置与依赖固定元数据/补丁/工具缓存格式。追加版本/提交标识用于更新不可变快照；restore 前缀不能越过兼容桶。快照数量、传输耗时和淘汰抖动随报告控制，不无限保存每次运行。

首批只由成功完成完整验证的可信默认分支或明确可信维护运行发布缓存；PR 可恢复可访问的兼容缓存并在本 job 内更新，但不发布供默认分支复用的共享快照。沿用普通 PR 工作流，不使用 pull_request_target 执行未审阅代码后写共享缓存。GitHub 缓存作用域不能当作产物认证，路径不包含环境文件或秘密。

缓存 miss、下载失败或服务不可用不减少测试、也不阻断本可完成的源码构建；日志标明是否恢复、实际缓存统计及恢复/保存耗时。临时网络失败导致固定依赖源码本身不可取得，仍属于来源准备失败，不能由缓存政策伪装成功。

继续固定版本/URL/SHA与静态 Mongo；不引入包管理器、独立依赖发布、跨机缓存服务或来源变更，不要求重审 ADR-0011/0012。不同生成器/工具链使用独立 build；复用 ccache 兼容对象不等于恢复生成器状态。

下载包层的后续入口已明确：先在 CI 单列下载、检查/提取/补丁、配置和缓存传输成本；若下载准备仍是主要等待且预计净收益成立，再评估独立原始包白名单目录，按依赖身份/URL/SHA区分，恢复后重新校验、提取与应用仓库补丁，并验证命中、缺失、损坏。现在不缓存可写源码树、stamp、整个_deps 或安装产物，也不另建下载层实现票。

[GitHub 缓存机制](https://docs.github.com/en/actions/reference/workflows-and-actions/dependency-caching)说明键、作用域和配额；[FetchContent 3.20](https://cmake.org/cmake/help/v3.20/module/FetchContent.html)说明 source override 会绕过下载更新，因此不能把可写源码覆盖视为通过原包校验的缓存。

## 事实与测量限制

源码事实快照为14c19d08d42ea4653914c91a82d6b971541c6cb6。当前 PATH 没有 Ninja/ccache/sccache；本项没有安装工具或执行 RealmMesh 全量构建。兼容性小探针在 macOS、CMake4.4.3/Apple clang21/Make3.81执行，只证明原生 launcher 不自动覆盖外部 Make。最低版本能力依据官方文档，未实际执行 CMake3.20/Ninja。

下面的 RSS 从[已发布冻结基线](build-baseline-2026-10-02.md)重新统计，不是当前 HEAD 新测量：

| 平台 | 编译事件数 | 单次编译最大/P95/中位 MiB | 链接最大 MiB |
| --- | ---: | ---: | ---: |
| macOS | 771 | 565.3 / 263.4 / 92.8 | 264.6 |
| Linux | 770 | 1279.4 / 518.5 / 126.6 | 560.6 |

最大编译均为 loadgen_integration_test.cpp，最大链接均为 service_host_test。launcher 的 wait4/ru_maxrss 不是并行进程树总峰值，未捕获 libsodium、归档和新缓存进程。Mac 原值按字节，Linux 按KiB；单位依据[当前 Apple XNU 官方手册](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/man/man2/getrusage.2)与[Linux getrusage](https://man7.org/linux/man-pages/man2/getrusage.2.html)。不能用单次最大值乘 jobs 宣布资源验收通过。

Linux 基线约8GiB、无swap，build位于tmpfs且约2.8GB；编译内存与文件数据竞争，因此初值不用8CPU直接开8路。48GiB Mac从8路、Linux从2路、CI从2路是保守选择；CI真实runner和新的并行峰值均未测量。

固定 FetchContent 的12份原包约32.5MiB，不含sodium。首次配置约101/125秒交织下载与检查，不能全算下载潜在节省。基线的265/266次配置后额外编译由隔离决策先解决；缓存命中不能作为配置污染已修复的证据。

[兼容性最小探针](assets/build-tool-cache-2026-10-02/external-launcher-probe.tar.gz)、[探针清单](assets/build-tool-cache-2026-10-02/external-launcher-manifest.json)、[RSS精简统计](assets/build-tool-cache-2026-10-02/memory-summary.json)、[事实笔记](assets/build-tool-cache-2026-10-02/resource-cache-facts.md)、[资产校验清单](assets/build-tool-cache-2026-10-02/manifest.json)。探针含原始临时路径，重放须替换前缀；不含完整cache/build/环境或第三方源码。

## 采用门槛、对照与退出

1. 先满足来源正确、配置状态稳定；双平台相同输入连续配置/构建无非预期第三方重编译。不能用缓存掩盖生成头污染。
2. Make/Ninja在相同源码、工具链、jobs和缓存模式下比较；再单独改变jobs；最后比较cache OFF、空cache与兼容cache命中。记录首次准备与缓存传输，区分空产物空缓存、空产物有缓存、稳定无操作、单.cpp/私有头/公共头/.proto变更，避免混算收益。
3. 测配置到完整测试结束总耗时，保留阶段时间、实际编译/链接工作量、整体内存压力、失败与缓存统计。稳定无操作不调用编译器，不把“零编译事件”记作缓存命中。
4. 双平台从新目录完成必要原生/Unit目标、全部构建与完整CTest；Linux必须启用QUIC并通过既有接入验收，macOS记录有/无QUIC差异。历史Linux失败仍需在验收前处理，不能因旧基线非绿降低门槛。最终量化目标另票确定。
5. 高并行验证libsodium库/头、protobuf生成顺序；删除单个生成库/头/源码后正确恢复，不依靠删除整个build修复。missingdeps及实际依赖检查用于辅助，不能代替行为验收。
6. 缓存对照包含默认发现、显式ON缺工具、OFF切换、冷/热命中、源码/头/宏/flags/编译器与生成文件变化、缺失/损坏与容量淘汰。命中路径与不命中路径通过相同正确性检查；CI验证隔离桶及读写/故障行为。
7. 编译数据库与编辑器/clang tooling/索引能使用实际所选目录；所有入口、启动服务和接入报告不误用旧产物。Make回退的正常构建与完整测试也须保持。
8. 生成顺序、产物恢复、最低工具兼容或运行行为不通过，则Ninja不成为默认，使用明确Make预设归因；jobs出现内存压力则降低并复测；缓存净收益不成立或正确性不通过则关闭缓存并保留证据。不给尚未实测的提速百分比。

后续下载层、PCH/Unity、sccache共享服务与libsodium专门缓存均有测量后重新决策的入口，不是本轮默认采用或承诺的收益。

## 图与源码证据范围

Tier Verify：Users-edwin-Projects-RealmMesh，主体证据generation2026-10-02T12:27:02Z、recorded12:30:32Z；watch补证generation2026-10-02T12:54:16Z。根CMake/预设/CI/Proto及入口脚本按相关路径核验覆盖；third_party与冻结build为明确排除，相关wrapper、缓存与下载脚本直接读取。dev-services.sh:265–266、Linux验收:224部分解析范围已源码补证；watch入口为scripts/test-watch.sh。

路径迁移事实来自CMakePresets.json、scripts/build.sh、scripts/test-watch.sh、scripts/lib/dev-process.sh、scripts/dev-all-in-one.sh、scripts/dev-services.sh、scripts/run-linux-login-acceptance.sh、scripts/run-macos-login-acceptance.sh与.github/workflows/ci.yml的build/dev字面量。图信号无记录缺口不保证全仓完整性；本票未做全仓缓存可用性或ABI审计。

## 实施记录（#123）

以下是 P4 的实际选择；上文约定未改动的部分不再重复。耗时、恢复与资源数据见[构建优化结果的 P4 节](build-optimization-results.md#p4ninja-默认与-make-回退123)。

### 预设与工具门槛

- `dev` 为 Ninja、binaryDir `build/dev-ninja`；`dev-make` 为 Unix Makefiles、binaryDir `build/dev-make`，显示名改为 “Development (Make fallback)”。两者都有同名 build/test 预设。旧 `build/dev` 不读、不删、不迁移。
- 入口与验收脚本按 #122 的配置期登记解析目录，本阶段不改它们；仓库根 `compile_commands.json` 随所选预设指向 `build/dev-ninja/` 或 `build/dev-make/`。
- `cmake/RealmMeshNinja.cmake` 的 `realmmesh_require_ninja()` 在 `project()` 之前运行：按 CMake 的查找名（`ninja-build`、`ninja`）解析 `CMAKE_MAKE_PROGRAM` 并缓存，版本低于 1.11、解析不出或执行失败都以配置错误停下，提示安装 Ninja 或显式改用 `--preset dev-make`。不自动换生成器，也不自动安装。
- CI 两个 job 显式安装 Ninja（`brew install ninja`、`apt-get install ninja-build`）并打印版本，构建仍显式 `--parallel 2`。
- `tools/build-bench/measure.py` 不再由预设名推导目录，改为按副本自己的 `CMakePresets.json`／`CMakeUserPresets.json` 沿 `inherits` 解析 binaryDir（只展开 `${sourceDir}`、`${presetName}`，其他宏要求显式 `--build-dir`）；构建命令总是带 `--parallel N`，缺省 1，因为 Ninja 不传时按核数并行。
- **与上文“不要自行简化解析 CMake 的继承、宏和用户预设规则”相抵，提请重审。** 测量对象是尚未配置的 `git archive` 副本，冷入口要在配置之前删掉构建目录，此时还没有 #122 的配置期登记可读，CMake 也没有不配置就报告 binaryDir 的命令。这一简化解析只在测量工具内，遇到其他宏即要求显式 `--build-dir`；`scripts/` 下的入口仍只读配置期登记，不受影响。若不接受，替代是测量调用一律显式传 `--build-dir`。

### 链接池

- Ninja 下根 CMakeLists 在第一个目标之前调用 `realmmesh_use_ninja_link_pool()`：声明 `JOB_POOLS realmmesh_link=1`，并以 `CMAKE_JOB_POOL_LINK` 让此后创建的原生目标（含静态库归档与 FetchContent 依赖）链接进这个池。它是总 jobs 的子集；Make 下不生效。
- **偏离硬门槛预算“Ninja 链接 1”：GTest 可执行文件不进池，提请确认。** `realm_add_gtest` 对每个测试可执行文件调用 `realmmesh_link_outside_pool()`（`JOB_POOL_LINK` 置空）。原因是 `gtest_discover_tests` 的 POST_BUILD 用例发现与链接同在一条 Ninja 边里，而 macOS 上新链接的二进制首次执行约等 0.5 s（user/sys 为 0），池深 1 把这些等待串成一列：公共头或 `.proto` 改动要重链 43–73 个测试，全部进池时 Ninja 反比 Make 慢 2–2.8 倍（结果节“全部进池”一轮）。按验收约定，这一回归本应回退 Make 或重新决策；放出测试链接是在保留 Ninja 收益的前提下对预算的调整，不只是“修正依赖表达”，因此作为范围决定写进 PR 请人确认，而不是自行认定。
- 放出后的边界：测试链接仍受总 `--jobs` 约束（Mac 8、Linux/CI 2），不会超出编译预算的进程数；Linux/CI 2 路下最多两个链接同时运行。测试链接的单次内存峰值（Mac 最高 263 MiB）与生产链接同量级（`realm_mesh` 239 MiB），放出后两平台的整体压力（进程树峰值、最低可用内存、swap、OOM）见结果节，均在门槛内。库与生产可执行文件仍在池内；`BuildGraphTest` 在 Ninja 构建下检查本工程 `build.ninja` 的池归属，防止回退。
- 考虑过而未采用：另设一个深度 N 的测试链接池（N 取多少都只是另一个预算值，且仍要人定）；改用 PRE_TEST 用例发现（见下条）；整体回退 Make（短场景在两平台都失去 10–46% 的收益）。
- 不重写用例发现：GoogleTest 模块在 3.20、3.31、4.x 之间的 POST_BUILD/PRE_TEST 实现不同；改用 PRE_TEST 只是把同样的串行等待移到 ctest 启动时。

### libsodium 与生成文件

- `make install` 并入 ExternalProject 的构建步，安装步置空；装出的全部头与 `lib/libsodium.a` 都列为该步的 `BUILD_BYPRODUCTS`。Ninja 由此知道这些文件的产出者，删除任一文件都会重跑这一步并在同一轮重编、重链下游。单独安装步的产物声明要 3.26 的 `INSTALL_BYPRODUCTS`，最低 3.20 不可用，所以采用并步。
- 安装文件清单写在 `third_party/sodium/sodium-install-manifest.cmake`，构建步末尾按清单核对安装树：多出未声明的头或缺少声明的文件都使构建失败，升级 libsodium 时漏改清单不会悄悄让 Ninja 不认识新头。`.la` 与 pkg-config 文件不被消费，不在清单内。
- 外部构建仍是 `make -j1` 并断开顶层 jobserver（#125），Ninja 池不覆盖它。
- protobuf 生成规则未改：现有 `add_custom_command` 的 OUTPUT/DEPENDS 在 Ninja 下已能从删除的 `.pb.h`／`.pb.cc` 恢复，`ninja -t missingdeps` 未报缺失。

### 测试覆盖

`BuildGraphTest`（integration）覆盖：版本门槛对各种 `ninja --version` 输出与执行失败的判定；`project()` 之前停下并给出提示；用真实 Ninja 生成的临时工程里库与可执行文件的链接边在深度 1 的池内、用 `realmmesh_link_outside_pool` 放出的不在池内；Ninja 构建下本工程 `build.ninja` 里库与 `realm_mesh` 在池内、`realm_add_gtest` 注册的测试可执行文件不在池内；libsodium 清单核对对多出与缺少文件报错。
