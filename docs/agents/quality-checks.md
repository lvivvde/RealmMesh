# 代码质量检查：CRAP、重复代码、变异测试

三项检查衡量测试与结构质量，都不进 CI，也不判定通过/失败，只输出报告供人或 agent 判断。脚本在 `tools/quality/`，产物写到 `build/` 下。

| 检查 | 回答的问题 | 平台 | 入口 |
|---|---|---|---|
| CRAP（crapper） | 哪些函数又复杂又缺测试 | macOS（AppleClang）；Linux 需 clang-21 | `./tools/quality/crap.sh` |
| 重复代码（dryer） | 哪些代码复制粘贴了 | 任意 | `./tools/quality/dry.sh` |
| 变异测试（mutator） | 测试能否发现被改坏的代码 | 仅 Linux（Lima `ubuntu`） | `./tools/quality/mutate.sh` |

## 一次性安装

macOS：

```bash
brew install pipx && pipx install lizard
npm install -g jscpd@5.4.2
```

`llvm-cov`/`llvm-profdata` 用 Xcode 自带的（`xcrun`）。Homebrew 的 `lizard` 公式是压缩库，不是复杂度工具，复杂度工具要用 pipx 装。

Linux（`ssh lima-ubuntu`）：

```bash
sudo apt-get install -y clang-21 llvm-21 lld-21 libclang-rt-21-dev pipx
pipx install lizard
# Mull 与 clang 主版本必须一致；从 GitHub release 取 ubuntu-<arch>-26.04 的 Mull-21 包
curl -fsSLO https://github.com/mull-project/mull/releases/download/0.34.1/Mull-21-0.34.1-LLVM-21.1.8-ubuntu-aarch64-26.04.deb
sudo apt-get install -y ./Mull-21-0.34.1-LLVM-21.1.8-ubuntu-aarch64-26.04.deb
```

## CRAP

`CRAP = CCN² × (1 − 覆盖率)³ + CCN`。CCN 来自 lizard，覆盖率是该函数在 unit 用例下的 region 覆盖率（llvm-cov）。分数超过 30 的函数值得先补测试或拆分。

```bash
./tools/quality/crap.sh            # 默认列前 30 名
./tools/quality/crap.sh --top 50 --threshold 20
```

- 用 `coverage` 预设（`build/coverage/`）构建 `realmmesh_unit_tests`，只跑 `unit` 标签用例。integration 路径（etcd、MongoDB、子进程）不计入覆盖，排行里网络/存储代码的覆盖率因此偏低，读结果时要考虑这一点。
- llvm-cov 的 `functions have mismatched data` 警告来自同一内联/模板函数在不同测试二进制里的编译差异，可忽略。
- 覆盖数据中完全没出现的文件跳过并在末行计数；出现了但函数没有数据（未被实例化的模板/内联）按 0 覆盖计。
- Linux 首次配置：`CC=clang-21 CXX=clang++-21 ./tools/quality/crap.sh`（GCC 不支持 `-fprofile-instr-generate`）。

## 重复代码

```bash
./tools/quality/dry.sh                       # framework game apps tools
./tools/quality/dry.sh game/realm --min-tokens 50
```

默认阈值是 70 个 token。控制台列出每对克隆的位置，HTML 报告在 `build/quality/jscpd/jscpd-report.html`。默认不扫 `tests/`；需要时把它作为路径参数传入。

## 变异测试

Mull 在编译期把变异体植入目标代码，运行时逐个打开变异体跑测试：测试失败记为 killed，测试仍然通过记为 survived。survived 的变异体说明对应行为没有被断言约束。

每次只针对一个 unit 测试目标和它要测的源码目录跑，全仓库一次跑完不现实：

```bash
ssh lima-ubuntu
cd /home/edwin.guest/code/RealmMesh
./tools/quality/mutate.sh <unit 测试目标> <源码路径正则> [gtest 过滤]
# 例：./tools/quality/mutate.sh character_name_test 'game/realm/src/character_name'
```

- 用 `mutation` 预设（`build/mutation/`，clang-21 + Mull 插件，关闭 ccache）。源码路径正则写入 `build/mutation/mull.yml` 的 `includePaths`；它变化时脚本会 `--clean-first` 重建该目标，因此换范围后的第一次运行较慢。
- 只有被所选测试覆盖到的变异体才会运行；报告里的 survived 行就是需要补断言的位置，常见是边界比较（`<` 改成 `<=` 仍通过 = 缺边界值用例）。
- 报告：控制台 IDE 格式；`build/mutation/mull-report/<目标>.html` 是 Mutation Testing Elements 页面。
- 只看本分支改动：`MULL_GIT_DIFF_REF=origin/main ./tools/quality/mutate.sh …`，只变异相对该 ref 改过的行。
- `mull-runner-21`、`/usr/lib/mull-ir-frontend-21` 的版本必须与 `clang++-21` 一致；升级其中一个时三者一起换。
