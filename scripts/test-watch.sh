#!/usr/bin/env bash

# 用途:监听源码变更,自动重跑快速单测子集,构成 TDD 反馈环(决策:watch 快子集 + CI 全量兜底)。
# 全量回归仍走 scripts/build.sh;这里只跑 unit 标签用例,划分约定见 tests/cmake/test_helpers.cmake。
#
# 用法:./scripts/test-watch.sh [--once]
#   (缺省)  循环监听:启动即跑一轮,此后每次变更触发一轮,Ctrl-C 退出。
#   --once  跑一轮「构建 + 快速子集」后退出(验证用;构建失败退 1,测试失败退 2)。
#
# 监听工具优先级:fswatch(macOS 常用)→ inotifywait(Linux)→ 轮询(平台无关兜底)。
# 环境变量 REALMMESH_WATCH_TOOL=fswatch|inotify|poll 可强制指定,便于验证单一路径。
#
# 防漏变更:每轮周期开始前打时间戳(build/dev/.test-watch-stamp),周期结束后凡有
# 文件比时间戳新就立刻进入下一轮——构建/测试期间保存的文件不会丢。

set -uo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${project_root}"

watch_paths=(framework game tests configs proto CMakeLists.txt)
poll_interval=2
debounce_seconds=1
stamp_file="build/dev/.test-watch-stamp"

if [[ "${1:-}" == "--once" ]]; then
    once=1
elif [[ $# -eq 0 ]]; then
    once=0
else
    echo "用法: $0 [--once]" >&2
    exit 64
fi

# cmake/ctest 定位:与 scripts/build.sh 同一约定(系统优先,.tools/cmake 兜底)。
if command -v cmake >/dev/null 2>&1; then
    cmake_bin="$(command -v cmake)"
elif [[ -x "${project_root}/.tools/cmake/bin/cmake" ]]; then
    cmake_bin="${project_root}/.tools/cmake/bin/cmake"
else
    echo "CMake 3.20 or newer is required. Install CMake or place a local distribution in .tools/cmake." >&2
    exit 1
fi
ctest_bin="$(dirname "${cmake_bin}")/ctest"

if [[ ! -f build/dev/CMakeCache.txt ]]; then
    echo "build/dev 尚未配置:先跑一次 ./scripts/build.sh,再用本脚本做增量反馈。" >&2
    exit 1
fi

watch_tool="${REALMMESH_WATCH_TOOL:-}"
if [[ -z "${watch_tool}" ]]; then
    if command -v fswatch >/dev/null 2>&1; then
        watch_tool=fswatch
    elif command -v inotifywait >/dev/null 2>&1; then
        watch_tool=inotify
    else
        watch_tool=poll
    fi
fi
case "${watch_tool}" in
    fswatch|inotify|poll) ;;
    *) echo "REALMMESH_WATCH_TOOL 仅支持 fswatch|inotify|poll,得到:${watch_tool}" >&2; exit 64 ;;
esac

# 编辑器临时文件不触发;其余噪声由防漏时间戳自然兜底,不做复杂过滤。
noise_exclude='(\.swp$|\.swx$|~$|#.*#|\.DS_Store)'

stop() {
    pkill -P $$ 2>/dev/null
    echo
    echo "test-watch 已停止。"
    exit 0
}
trap stop INT TERM

changes_pending() {
    find "${watch_paths[@]}" -type f -newer "${stamp_file}" -print -quit | grep -q .
}

wait_for_change() {
    case "${watch_tool}" in
        fswatch)
            fswatch -1 -e "${noise_exclude}" "${watch_paths[@]}" >/dev/null
            ;;
        inotify)
            inotifywait -q -r -e modify,create,delete,move \
                --exclude "${noise_exclude}" "${watch_paths[@]}" >/dev/null
            ;;
        poll)
            while ! changes_pending; do
                sleep "${poll_interval}"
            done
            ;;
    esac
}

# 返回码:0 全绿;1 构建失败;2 测试失败。
run_cycle() {
    touch "${stamp_file}"
    echo
    echo "─── $(date '+%H:%M:%S') 增量构建 ───"
    if ! "${cmake_bin}" --build --preset dev; then
        echo "✗ 构建失败,等待下一次变更"
        return 1
    fi
    echo "─── $(date '+%H:%M:%S') ctest -L unit(快速子集)───"
    local rc=0
    "${ctest_bin}" --preset dev -L unit || rc=$?
    if [[ ${rc} -eq 0 ]]; then
        echo "✓ 快速子集全绿"
    else
        echo "✗ 快速子集有失败(退出码 ${rc}),失败用例见上"
    fi
    return "${rc}"
}

echo "test-watch:工具=${watch_tool} 监听=${watch_paths[*]}"
echo "快速子集 = ctest -L unit;全量回归用 ./scripts/build.sh。"

if [[ ${once} -eq 1 ]]; then
    run_cycle
    exit $?
fi

run_cycle || true
while :; do
    wait_for_change
    sleep "${debounce_seconds}"
    run_cycle || true
    # 周期内保存的文件不丢:凡比本轮时间戳新的待处理变更,立刻再来一轮。
    while changes_pending; do
        run_cycle || true
    done
done