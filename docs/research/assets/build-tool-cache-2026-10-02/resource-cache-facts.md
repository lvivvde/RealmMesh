# 构建资源与依赖复用事实（只读补充）

当前工作区 HEAD 为 14c19d08d42ea4653914c91a82d6b971541c6cb6；RSS 数据来自已发布冻结基线，不能当作当前 HEAD 的全新测量。没有安装、构建、测试或服务启动。

## RSS 与并行度候选

| 平台 | 编译事件数 | 编译最大 / P95 / 中位 MiB | 链接最大 MiB |
|---|---:|---:|---:|
| macOS | 771 | 565.3 / 263.4 / 92.8 | 264.6 |
| Linux | 770 | 1279.4 / 518.5 / 126.6 | 560.6 |

最大的编译均为 loadgen_integration_test.cpp；最大链接均为 service_host_test。事件记录 launcher 对编译器调用 wait4 的 ru_maxrss，不是整个并行进程树的内存总峰值，也未捕获 libsodium Autotools 编译、静态归档与新缓存进程。GCC 子进程、同时链接与页面缓存要另外测量。

macOS 原值按 bytes 转 MiB；Linux 按 KiB 转 MiB。Apple 旧网页写 kilobytes，但当前官方 XNU man 写 bytes，应引用后者；Linux 官方 man 同时解释 child max 并非整个树的聚合峰值。[Apple 官方 XNU man](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/man/man2/getrusage.2)，[Linux 官方 man](https://man7.org/linux/man-pages/man2/getrusage.2.html)。

工程候选：48 GiB Mac 初始上限 8；8 GiB Linux/Lima 初始 2；CI 未在当前 runner 上测量，初始 2 后调优。8 个 Mac 单编译最大约 4.4 GiB，4 个 Linux 最大约 5.0 GiB；Linux 冻结构建还在无 swap 的 tmpfs 上，约 2.8 GB build 数据与编译内存竞争。默认 4 并不能凭 8 CPU 保证安全。候选还需保留 OS、宿主/VM 和其他任务余量、限制同时间链接和嵌套 make，并实测 wall/RSS/压力后调优。这是上限的保守起点，不是速度或峰值保证。

## 缓存对象与最低成本

当前 CI 未配置 actions/cache 或 compiler launcher，完整构建和测试仍分别调用 preset；Linux 还跑 M1–M4。优先编译缓存可以跨新 checkout 复用命中对象，且不恢复 CMake 状态。候选本地项目缓存设 5 GiB，CI 每平台候选 1–2 GiB；容量未按 Linux 新缓存实测。记录命中/不可缓存/淘汰/大小后调整，不设无限容量。ccache 可配置 max_size、自动近似 LRU、默认压缩；当前手册默认 5 GiB。[ccache 官方手册](https://ccache.dev/manual/latest.html#_cache_size_management)。

GitHub 当前默认仓库缓存额度是 10 GB，管理员可增大并产生额外费用；不是每个 job 各有 10 GB。多个不可变键快照可能造成淘汰抖动，不能把本地上限与远端总额视为同一上限。[GitHub 官方缓存额度](https://docs.github.com/en/actions/reference/workflows-and-actions/dependency-caching#usage-limits-and-eviction-policy)。

不建议首批恢复整个 build 或整个 _deps。冻结 CMakeCache 保存 generator、compiler、源目录、launcher、FETCHCONTENT_SOURCE_DIR 的绝对路径；_deps 同时含 src/build/subbuild，不能当纯源码缓存，也不能用共享 FETCHCONTENT_BASE_DIR 自动证明跨 generator/profile 安全。编译缓存只解决编译，不解决链接或网络安装步骤。

下载归档可以后续独立缓存，来源仍是当前固定 URL/SHA。冻结 FetchContent 12 包约 32.5 MiB（不含 sodium）；名称既有同名 v1.17.0.tar.gz，也有两个 archive.tar，不能扁平以 basename 做键。归档目前位于 _deps/<name>-subbuild/<name>-populate-prefix/src，sodium 在 third_party/sodium/sodium_external-prefix/src，不是稳定独立缓存目录。

最低成本可验证方案是单独稳定下载目录白名单、按 SHA256/依赖身份区分原包，恢复后再次校验、提取到本 build，再执行受版本控制的补丁。仅包可以跨 generator；protoc 等平台包按各自 URL/SHA 区分。不要恢复 stamp、CMakeCache、已构建库或可写源码树。现有生成 download 脚本会对现存归档复查 SHA256；这证明当前工具生成的行为，尚未验证新缓存接入。

sol2 wrapper 明确有 PATCH_COMMAND（third_party/sol2/CMakeLists.txt:8–11）；原包 URL_HASH 并不认证已补丁/可修改的源码树。FETCHCONTENT_SOURCE_DIR override 则绕过下载/更新，不能把它的可复用源码树说成已经受原包 hash 验证。[CMake 3.20 FetchContent](https://cmake.org/cmake/help/v3.20/module/FetchContent.html)。

首批建议仍以 ccache 为主，把下载归档层作为下一次 CI 数据门槛：编译缓存已命中后，独立确认下载准备仍是主要等待，再接入该层；如果本轮要纳入，则必须为其另测归档命中/缺失/损坏三条路径、重新提取补丁正确性及实际下载+传输净耗时。冻结首次 configure 100/125 秒包含检查与下载，不能全算可节省下载。不能仅恢复几个 _deps 子目录并宣称依赖复用完成。

## 证据范围

RSS 来源：macos/adapted-build-cold-events、Linux results/build-cold-events；compact JSON 记录统计与前五项，不保留全部 argv。冻结 Mac .o 908 个总约 348.7 MiB，是整个树文件大小，含未捕获外部构建，不等于 ccache 容量预测。

项目图 Tier Verify，generation 2026-10-02T12:27:02Z、recorded12:30:32Z complete；CI metadata_match 无记录缺口；third_party/build excluded，已直接读取 sol2/sodium wrapper、冻结 CMakeCache 与生成下载脚本。当前 CI 引用源为 .github/workflows/ci.yml。未审计所有源码可缓存性，未运行性能实验，未确定 CI 实际硬件上限。
