#!/usr/bin/env bash

# 用途：监听源码变更，每轮调用快速入口 scripts/test-fast.sh(#125),构成 TDD 反馈环。
# 配置、选择范围与退出约定都沿用 test-fast.sh;全量回归仍走 scripts/build.sh。
#
# 用法:./scripts/test-watch.sh [--once] [test-fast.sh 的选项]
#   (缺省)   启动即跑一轮，此后每次变更顺序触发下一轮,Ctrl-C 退出;失败只报告，
#            继续等待新变更。
#   --once   跑一轮后以 test-fast.sh 的退出码退出。
#   其余选项(--preset、--jobs、--target、--test-regex、--test-jobs)原样传给
#   test-fast.sh,每轮相同。
#
# 监听工具优先级:fswatch(macOS 常用)→ inotifywait(Linux)→ 轮询(平台无关兜底)。
# 环境变量 REALMMESH_WATCH_TOOL=fswatch|inotify|poll 可强制指定。
#
# 是否有变更以文件快照为准(路径 + 修改时间 + 大小，另加比本轮时间戳新的文件),
# 所以新增、删除、移动都能识别；监听工具只负责尽快唤醒，并有定时复查兜住它
# 启动前后的空档。每轮开始前记快照，轮次结束后与之比较：配置、构建、测试期间
# (含第一轮)保存的变更都会再触发一轮。轮次在前台顺序执行，不会重叠。

set -uo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "${project_root}"
# shellcheck source=lib/build-dir.sh
source "${project_root}/scripts/lib/build-dir.sh"

# 生产/测试源码、第三方构建输入、CMake 模块与相关脚本，以及根目录的 CMake 文件。
watch_dirs=(framework game apps tools tests configs proto third_party cmake scripts)
watch_root_files=(CMakeLists.txt CMakePresets.json CMakeUserPresets.json)
poll_interval="${REALMMESH_WATCH_POLL_INTERVAL:-2}"
# 事件工具模式下的定时复查间隔(秒)。
recheck_interval="${REALMMESH_WATCH_RECHECK_INTERVAL:-5}"
debounce_seconds="${REALMMESH_WATCH_DEBOUNCE:-1}"

usage() {
    echo "Usage: $0 [--once] [--preset NAME] [--jobs N] [--target TARGET] [--test-regex REGEX] [--test-jobs N]"
    echo "Runs ./scripts/test-fast.sh now and again after every change; --once runs one round"
    echo "and exits with its status. Options other than --once go to test-fast.sh."
}

once=0
preset="${realmmesh_default_preset}"
fast_args=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --once)
            once=1
            shift
            ;;
        --preset|--jobs|--target|--test-regex|--test-jobs)
            [[ $# -ge 2 ]] || { echo "$1 requires a value." >&2; usage >&2; exit 64; }
            [[ "$1" == "--preset" ]] && preset="$2"
            fast_args+=("$1" "$2")
            shift 2
            ;;
        --preset=*|--jobs=*|--target=*|--test-regex=*|--test-jobs=*)
            [[ "$1" == --preset=* ]] && preset="${1#--preset=}"
            fast_args+=("$1")
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 64
            ;;
    esac
done
realmmesh_require_preset_name "${preset}" || exit 64

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
    *) echo "REALMMESH_WATCH_TOOL must be fswatch, inotify or poll, got: ${watch_tool}" >&2; exit 64 ;;
esac
if [[ "${watch_tool}" != poll ]] && ! command -v "${watch_tool/inotify/inotifywait}" >/dev/null 2>&1; then
    echo "${watch_tool} is not installed; use REALMMESH_WATCH_TOOL=poll." >&2
    exit 64
fi

# 会话临时目录放快照与时间戳：第一轮配置前构建目录可能还不存在。
state_dir="$(mktemp -d "${TMPDIR:-/tmp}/realmmesh-test-watch.XXXXXX")"
stamp_file="${state_dir}/stamp"
snapshot_file="${state_dir}/snapshot"
current_file="${state_dir}/current"
watcher_pid=""
timer_pid=""

stop_watcher() {
    if [[ -n "${timer_pid}" ]]; then
        pkill -P "${timer_pid}" 2>/dev/null
        kill "${timer_pid}" 2>/dev/null
        wait "${timer_pid}" 2>/dev/null
        timer_pid=""
    fi
    if [[ -n "${watcher_pid}" ]]; then
        kill "${watcher_pid}" 2>/dev/null
        wait "${watcher_pid}" 2>/dev/null
        watcher_pid=""
    fi
}
cleanup() {
    stop_watcher
    rm -rf -- "${state_dir}"
}
trap cleanup EXIT
# 停止提示写到启动时保存的标准输出(fd 3):信号到达时主 shell 可能正处在
# take_snapshot >快照文件 这类重定向里，陷阱的 echo 会跟着写进快照文件。
exec 3>&1
trap 'echo >&3; echo "test-watch stopped." >&3; exit 0' INT TERM

existing_watch_paths() {
    local path
    for path in "${watch_dirs[@]}" "${watch_root_files[@]}"; do
        [[ -e "${path}" ]] && printf '%s\n' "${path}"
    done
}

# 构建目录若落在监听范围内(用户预设的 binaryDir 可以指到任何地方),排除它，
# 免得自己的构建触发下一轮。第一轮配置之前解析不到，就先不排除。
build_dir_prune=""
refresh_build_dir_prune() {
    local build_dir
    build_dir="$(realmmesh_resolve_build_dir "${project_root}" "${preset}" 2>/dev/null)" ||
        return 0
    case "${build_dir}" in
        "${project_root}"/*) build_dir_prune="./${build_dir#"${project_root}/"}" ;;
        *) build_dir_prune="" ;;
    esac
}

# 快照：每个受监听文件一行「修改时间(秒) 大小 路径」,按路径排序。
# 排除 .git、__pycache__、构建目录与编辑器临时文件。
take_snapshot() {
    local -a paths=()
    local path
    while IFS= read -r path; do
        paths+=("./${path}")
    done < <(existing_watch_paths)
    local -a prune=(-name .git -o -name __pycache__ -o -name node_modules)
    [[ -n "${build_dir_prune}" ]] && prune+=(-o -path "${build_dir_prune}")
    find "${paths[@]}" \( "${prune[@]}" \) -prune -o -type f \
        ! -name '*.swp' ! -name '*.swx' ! -name '*~' ! -name '.#*' ! -name '#*#' \
        ! -name '.DS_Store' ! -name '4913' -print0 2>/dev/null |
        if [[ "$(uname -s)" == Darwin ]]; then
            xargs -0 stat -f '%m %z %N' 2>/dev/null
        else
            xargs -0 stat -c '%Y %s %n' 2>/dev/null
        fi | LC_ALL=C sort -k3
}

# 轮次开始：记快照与时间戳(同一秒内的再次保存靠时间戳识别)。
mark_round_start() {
    touch "${stamp_file}"
    take_snapshot >"${snapshot_file}"
}

changes_pending() {
    take_snapshot >"${current_file}"
    if ! cmp -s "${snapshot_file}" "${current_file}"; then
        return 0
    fi
    local -a paths=()
    local path
    while IFS= read -r path; do
        paths+=("./${path}")
    done < <(existing_watch_paths)
    local newer
    newer="$(find "${paths[@]}" -type f -newer "${stamp_file}" \
        ! -path "${build_dir_prune:-/nonexistent}/*" ! -path '*/.git/*' \
        ! -name '*.swp' ! -name '*.swx' ! -name '*~' ! -name '.#*' ! -name '#*#' \
        ! -name '.DS_Store' ! -name '4913' -print 2>/dev/null | head -n 1)"
    [[ -n "${newer}" ]]
}

# 列出相对本轮开始快照的变更(+ 新增、- 删除、~ 修改，最多 5 条),便于看清
# 下一轮因何触发；同一秒内的再次保存只由时间戳识别，不在列表里。
describe_changes() {
    awk '
        { path = $0; sub(/^[^ ]* [^ ]* /, "", path) }
        NR == FNR { old[path] = $0; next }
        !(path in old) { print "  + " path; next }
        old[path] != $0 { print "  ~ " path }
        { delete old[path] }
        END { for (path in old) print "  - " path }
    ' "${snapshot_file}" "${current_file}" | head -n 5
}

start_watcher() {
    local -a paths=()
    local path
    while IFS= read -r path; do
        paths+=("${path}")
    done < <(existing_watch_paths)
    local exclude='(\.git/|__pycache__|\.swp$|\.swx$|~$|/\.#|#[^/]*#$|\.DS_Store$|/4913$)'
    if [[ -n "${build_dir_prune}" ]]; then
        exclude="(${exclude}|${build_dir_prune#./}/)"
    fi
    case "${watch_tool}" in
        fswatch)
            fswatch -1 -r -E -e "${exclude}" "${paths[@]}" >/dev/null 2>&1 &
            ;;
        inotify)
            inotifywait -q -r -e modify,create,delete,move,attrib \
                --exclude "${exclude}" "${paths[@]}" >/dev/null 2>&1 &
            ;;
    esac
    watcher_pid=$!
}

# 阻塞到快照出现变化。事件工具先启动再检查，检查之后的变更必然唤醒它;
# 它启动完成前的空档由定时复查兜底。
wait_for_change() {
    if [[ "${watch_tool}" == poll ]]; then
        until changes_pending; do
            sleep "${poll_interval}"
        done
        return 0
    fi
    while :; do
        start_watcher
        if changes_pending; then
            stop_watcher
            return 0
        fi
        (sleep "${recheck_interval}"; kill "${watcher_pid}" 2>/dev/null) &
        timer_pid=$!
        wait "${watcher_pid}" 2>/dev/null
        watcher_pid=""
        stop_watcher
        if changes_pending; then
            return 0
        fi
    done
}

round=0
run_round() {
    round=$((round + 1))
    echo
    echo "─── test-watch round ${round} start $(date '+%H:%M:%S') ───"
    mark_round_start
    local status=0
    "${project_root}/scripts/test-fast.sh" "${fast_args[@]+"${fast_args[@]}"}" || status=$?
    refresh_build_dir_prune
    echo "─── test-watch round ${round} end $(date '+%H:%M:%S'): exit ${status} ───"
    return "${status}"
}

echo "test-watch: tool ${watch_tool}; watching $(existing_watch_paths | tr '\n' ' ')"
echo "test-watch: each round runs ./scripts/test-fast.sh ${fast_args[*]+"${fast_args[*]}"}"

status=0
run_round || status=$?
if [[ "${once}" -eq 1 ]]; then
    exit "${status}"
fi
# 用法错误每轮都会重复，直接退出。
[[ "${status}" -eq 64 ]] && exit 64

while :; do
    if ! changes_pending; then
        echo "test-watch: waiting for changes (Ctrl-C to stop)"
        wait_for_change
    fi
    echo "test-watch: change detected:"
    describe_changes
    sleep "${debounce_seconds}"
    run_round || true
done
