# 第三方头文件与配置状态：首批隔离决策

关联[确定第三方头文件与配置状态的隔离策略](https://github.com/lvivvde/RealmMesh/issues/116)。用户已通过 Q1–Q5 确认来源范围、本机覆盖管理、旧缓存迁移、状态隔离和入口预设选择，并最终确认完整方案及正式记录。本文记录后续实施约束，尚未修改构建代码。

## 已选择的范围

首批保留现有固定版本源码包、SHA-256、静态 Mongo 驱动与平台包来源。修正头来源混用、配置串扰和覆盖参数的隐含要求；依赖包管理器、独立依赖构建和产物复用的取舍继续由“选择构建工具、并行缓存与第三方依赖复用策略”决定。与 ADR-0011/0012 一致，不为本票新增依赖来源或降低 QUIC 门槛。

## 头文件来源与发现顺序

- 在任何会查找 OpenSSL 的第三方子项目进入前，解析并验证统一的 OpenSSL 来源。保留标准 OPENSSL_ROOT_DIR/发现项；macOS 默认定位 OpenSSL 专用安装前缀，不能把 Homebrew 共享 include 根当成隔离完成。明确覆盖优先于自动发现，冲突或无效覆盖不静默回退。
- 同一依赖的头和库应来自相容版本与架构，验证 realpath 后的真实来源；不以 symlink 路径字符串不同直接判定混用。输出所选版本、关键路径和 QUIC 能力，便于定位。
- 使用目标表达编译/链接使用需求，保留各依赖原有明确名称；PUBLIC/PRIVATE 按消费者实际头契约设置。固定 Abseil 的消费者必须实际命中固定来源，而不是机器上其他 Abseil。禁止用全局 include 的 BEFORE 重排、把整个包前缀复制进仓库或把所有目录标成 SYSTEM 来替代来源隔离。
- Linux 标准 /usr/include 与架构配置头布局仍可合法使用；不能靠“一律禁止共享 include 根”的黑名单破坏系统开发包。按平台专用发现规则与代表性实际头命中验证。检测环境污染无法在配置阶段完全穷尽，验收必须包含真实预处理/依赖文件及链接。

[FindOpenSSL](https://cmake.org/cmake/help/v3.20/module/FindOpenSSL.html)提供 root、发现结果与 imported targets；[target_include_directories](https://cmake.org/cmake/help/v3.20/command/target_include_directories.html)定义使用需求传播。它们并不保证机器上的同名第三方头已经隔离。

## 旧缓存与配置状态

已识别的旧发现路径或依赖版本污染，在配置阶段明确失败，指出冲突项及精确迁移方法。由开发者执行定向迁移，不由普通构建入口静默改写本机选择或清空整个缓存。迁移后的生成头允许一次必要变化与重编译；之后相同输入应稳定。

OpenSSL 最小探针中，旧 OPENSSL_INCLUDE_DIR=/opt/homebrew/include 后，仅设置专用 OPENSSL_ROOT_DIR 不重新查找；定向移除该 include 缓存后才命中专用目录。此环境的修复示例是选定专用 root 后 `-UOPENSSL_INCLUDE_DIR`；其他库/架构缓存是否需迁移按实际诊断决定，不推广为通用全缓存清理命令。[find_path 缓存规则](https://cmake.org/cmake/help/v3.20/command/find_path.html)与[-U 定向移除](https://cmake.org/cmake/help/v3.20/manual/cmake.1.html)支持这一区分。

依赖封装分别拥有版本与选项：mongo-c-driver=2.5.5，mongo-cxx-driver=4.6.0，应与固定包信息一致，不能把一个共同 BUILD_VERSION 普通变量传给二者。普通变量保留在对应目录/函数作用域，通用共享缓存键另行定向隔离。第三方子目录不是独立 CMakeCache；仅增加函数或子目录不能解决 CACHE 串扰。

对审计确认的通用键，需要同时控制进入子项目时的有效普通值、缓存值和类型，以及退出时原缓存状态的恢复。若采用临时设置/恢复，保存原键是否存在、值、类型及相关属性；不存在则退出后仍不存在。只在末尾恢复，不能阻止子项目当次读错；仅 unset 普通变量可能重新暴露缓存。现有用户显式配置存在歧义或与固定版本冲突时，说明不受支持的通用覆盖并给迁移提示，不能把它悄悄当成另一驱动版本。

命名明确的依赖选项缓存可按所属依赖保留，不清扫所有上游缓存。源包只读，修补以仓库记录的补丁/封装表达，不修改已下载源码作为隐含步骤。每个依赖更新复查实际读写的普通变量、CACHE、PARENT_SCOPE 和策略，必要时扩展定向隔离清单。

[CMake 变量作用域](https://cmake.org/cmake/help/v3.20/manual/cmake-language.7.html#variables)区分普通变量与持久缓存；[CMP0077](https://cmake.org/cmake/help/latest/policy/CMP0077.html)和[CMP0126](https://cmake.org/cmake/help/latest/policy/CMP0126.html)分别涉及 option 与 set(CACHE)。CMP0126 是 3.21 才加入，不能用普通 shadow 在新 CMake 下奏效就承诺最低 3.20 也安全。首批维持最低版本要求，实施时验证最低支持语义和当前工具；不为绕过问题盲目设全局 policy 或升级最低版本。

## 覆盖参数与入口一致性

仓库维护默认选择、固定包元数据和参数说明；本机绝对路径放入不提交的 CMakeUserPresets.json，并在实施时补 .gitignore 与无本机路径的示例。CI 显式设置其所需参数。本机预设以独立名字继承 dev，配置/构建/测试各有匹配预设；不能同名覆盖 dev。[CMake Presets 3.20](https://cmake.org/cmake/help/v3.20/manual/cmake-presets.7.html)支持这种项目与用户分工。

作为对[入口决策](build-test-entry-decisions.md)的已确认补充：build.sh、test-fast.sh 与 test-watch.sh 接受同一 `--preset <name>`，默认 dev；watch 转交相同选择，配置、构建与 CTest 不混用其他目录。缺少匹配预设或不支持的配置明确失败。每次标准配置、Unit 聚焦、4 路可调与完整串行等原约定保持。

同一构建目录保持兼容工具链和依赖选择。切换编译器、宿主/目标架构、生成器或不兼容 ABI 时使用独立 binaryDir，不能复用旧产物伪装命中；普通路径迁移按明确诊断处理，不为每日构建引入复杂缓存指纹。

| 覆盖对象 | 首批约定 |
| --- | --- |
| OpenSSL | 标准 root/发现项，早于 Mongo 配置，验证实际头库及架构；旧结果冲突明确迁移 |
| MsQuic | 沿用 MSQUIC_ROOT 与发现项；Linux 必须、macOS 可选；保持 2.6 系列与既有版本告警政策 |
| protoc | 仓库已覆盖的宿主默认固定官方 35.0 包及校验；提供单一项目级显式外部可执行文件入口，优先于自动下载，验证宿主可执行性与匹配版本 |
| FetchContent 源覆盖 | 作为显式开发/实验覆盖记录，不能把临时源码目录当生产默认；固定构建验收使用声明的来源与补丁 |

protoc 是宿主工具，库是目标产物，验证不能混淆两者；本票不扩张交叉编译支持。外部入口可命名 REALMMESH_PROTOC_EXECUTABLE（实施命名），必须验证可执行与版本35.0，记录来源；无效显式选择失败，不静默下载替代。调查快照的 protobuf wrapper 对已覆盖宿主以普通 WITH_PROTOC 指定下载工具，会遮蔽命令行同名 CACHE，因此它不是可靠的全宿主外部覆盖入口；上游 WITH_PROTOC 留作内部映射，不增设多个同义公共参数。MsQuic 已有入口无需再造一套。

## 新证据与限制

调查事实快照 HEAD=cecb9375dcf27471378e2c20d73b0f76d793c187。整理时另观察到 c0652ce8800d00881a96d099e42bbcd523767e94，期间新增的是负载测试修正，上述构建文件未变化。与[冻结基线](build-baseline-2026-10-02.md)不同，调查快照已增加 Linux ARM64 官方 protoc 分支与 MsQuic 架构路径/安装适配；当年的临时覆盖属于旧快照条件，不能据旧基线推断其他分支缺少 ARM64 支持。发布时本地检出为14c19d08d42ea4653914c91a82d6b971541c6cb6，保持原检出不变；源码结论以这里记录的调查快照为准。

OpenSSL 最小 macOS configure 探针未构建项目：默认发现和 imported target 均导出 Homebrew 共享根；旧缓存仅加专用 root 无变化；定向清除 include 缓存后命中专用目录。库的共享路径与专用路径 realpath 同为 3.6.5，此探针不证明库版本不同。Mongo 上游更早 find_package(OpenSSL)，所以只在 Network 查找处追加 hint 太晚。

Mongo 最小反事实仅 project(LANGUAGES NONE)，使用真实上游版本解析模块/头模板，重放 CXX 相关语句，不是完整驱动 configure：

| 条件 | 结果 |
| --- | --- |
| 原共享缓存行为 | 首配 C2.5.5；二配数值0.0.0、完整字符串仍2.5.5，两头改变；三配稳定 |
| 父普通变量统一2.5.5 | C稳定，但CXX错误继承2.5.5 |
| 各 child 普通变量各自版本 | 当前 NEW 语义有效版本正确，头 hash/mtime三次稳定，仍遗留共享缓存0.0.0 |
| 原先没有缓存，CXX后删除该缓存 | 三次稳定；未证明保存/恢复用户原缓存的通用方案 |
| 各 child 版本＋CMP0126 OLD＋未类型化外部缓存9.9.9 | C正确，CXX有效版本却为9.9.9，普通 shadow不足 |

原行为生成的两头 hash 与冻结基线第一次和热配置后分别逐字节一致，版本污染机制已复现。尚未跑完整驱动反事实编译，不能声称这是 macOS265/Linux266次额外编译的唯一原因，或所有配置都会重复。OLD 负例在当前 CMake4.4.3强制策略，未实际运行3.20；两类探针均在 macOS，未重跑完整 Linux 配置。

[OpenSSL 探针资产](assets/dependency-isolation-2026-10-02/openssl-probe.tar.gz)、[Mongo 反事实资产](assets/dependency-isolation-2026-10-02/mongo-version-probe.tar.gz)、[逐文件校验清单](assets/dependency-isolation-2026-10-02/manifest.json)。仅精简 CMake、配置日志和结果，不含完整 cache/build/环境或驱动源码；Mongo 探针引用固定基线下载源码路径，跨机器须替换为相同版本源路径。

## 实施验收

1. macOS 同时安装其他 Abseil 的环境，新配置与旧污染缓存均按约定处理；实际固定 Abseil 头命中、命名空间宏、库符号和链接一致，不只查看 include 路径。
2. 两平台完整项目在相同输入下连续配置并构建至少三次：正确版本宏、相关生成头 hash/mtime稳定；无额外第三方编译。若仍有工作，保存实际事件与原因，不能因本探针通过就宣布验收完成。
3. 定向隔离含无缓存、已有类型化/未类型化缓存、外部冲突覆盖及最低支持策略；原状态正确恢复，其他依赖与项目测试/共享构建选项不受意外影响。
4. 版本或选项真实变更仍正确刷新相关头/产物，并仅影响真实消费者；来源变化须复查头库/ABI/包导出和工具。不能冻结错误生成头来获得零工作。
5. 支持的本机/CI覆盖走同一规则：默认、有效覆盖、无效路径、错误宿主工具/版本和旧缓存迁移都有明确结果；派生预设贯穿完整/快速/watch入口。
6. 保留 macOS 有/无 MsQuic、Linux x86_64/ARM64 的既有能力差异；实施后完成双平台完整 CTest及LinuxQUIC门槛，历史失败另行处理。实际未覆盖的平台组合明确记录。

量化耗时门槛由“锁定优化实施顺序与三类耗时验收标准”确定。本票首先要求来源正确与配置稳定，不凭最小探针承诺提速百分比。

## 源码与图证据

以 Verify 图做定位与覆盖核验；父起始 generation2026-10-02T11:59:19Z，补证时 generation2026-10-02T12:18:13Z/recorded12:20:16Z。根/Network/Proto/安装脚本等相关记录无缺口；third_party及冻结_deps为明确排除，均直接读取相关CMake补证。普通调用图不用于证明依赖搜索顺序或缓存完备性。

关键路径：根 CMakeLists.txt；third_party/mongo/CMakeLists.txt:25、:43；framework/network/CMakeLists.txt:84、:106；proto/CMakeLists.txt:20、:38；third_party/protobuf/CMakeLists.txt:30、:58；third_party/msquic/CMakeLists.txt:53；scripts/install-msquic-dev.sh:24。

固定下载源码：mongo-c-driver build/cmake/BuildVersion.cmake:19–25、CMakeLists.txt:5–19、src/libmongoc/CMakeLists.txt:217；mongo-cxx-driver CMakeLists.txt:99、:177–188。这些上游路径由固定版本来源确定，不属于本仓图覆盖。
