#!/usr/bin/env bash

# 供 dev 脚本使用的可移植进程查询工具。
#
# Linux 直接读 /proc;macOS 没有 /proc,改用 ps 与 lsof。所有函数只依赖 POSIX
# 工具与 bash 3.2 语法(刻意不用 mapfile / readlink -f)。
#
# 约定:调用方先设置 realmmesh_root(仓库根),cwd 校验与 detach 辅助程序
# 定位都基于它。

# 解析为绝对路径:macOS 的 readlink 不保证支持 -f,所以用 cd + pwd -P。
realmmesh_realpath() {
    local realmmesh_target="$1"
    local realmmesh_dir realmmesh_base
    if [[ -d "${realmmesh_target}" ]]; then
        (cd "${realmmesh_target}" && pwd -P)
        return 0
    fi
    realmmesh_dir="$(dirname "${realmmesh_target}")"
    realmmesh_base="$(basename "${realmmesh_target}")"
    printf '%s/%s\n' "$(cd "${realmmesh_dir}" && pwd -P)" "${realmmesh_base}"
}

# 进程状态首字母;进程已不存在时输出为空。
realmmesh_process_state() {
    ps -o state= -p "$1" 2>/dev/null | tr -d '[:space:]'
}

realmmesh_is_zombie_process() {
    local realmmesh_state
    realmmesh_state="$(realmmesh_process_state "$1")"
    [[ -n "${realmmesh_state}" && "${realmmesh_state:0:1}" == "Z" ]]
}

# 完整命令行(单行)。BSD ps 的宽度可能受终端影响,-ww 表示不限宽。
realmmesh_process_command_line() {
    ps -ww -o args= -p "$1" 2>/dev/null |
        sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//'
}

# argv[0]。命令行由空格连接,因此取第一个空白之前的部分。
realmmesh_process_executable() {
    local realmmesh_command
    realmmesh_command="$(realmmesh_process_command_line "$1")" || return 1
    [[ -n "${realmmesh_command}" ]] || return 1
    printf '%s\n' "${realmmesh_command%% *}"
}

# 命令行是否包含给定的连续参数串(用 ps 的单行形式做子串匹配)。
realmmesh_process_has_arguments() {
    local realmmesh_pid="$1"
    shift
    local realmmesh_command
    realmmesh_command="$(realmmesh_process_command_line "${realmmesh_pid}")" ||
        return 1
    [[ "${realmmesh_command}" == *"$*"* ]]
}

# 进程工作目录。
realmmesh_process_cwd() {
    local realmmesh_pid="$1"
    if [[ -d /proc ]]; then
        readlink "/proc/${realmmesh_pid}/cwd" 2>/dev/null
        return 0
    fi
    lsof -a -p "${realmmesh_pid}" -d cwd -Fn 2>/dev/null |
        sed -n 's/^n//p' | head -n 1
}

# 把子进程放进新会话的命令前缀:优先系统 setsid,否则用仓库内构建的
# realm_detach(macOS 没有 setsid)。失败时返回非零并说明如何构建。
realmmesh_detach_command() {
    if command -v setsid >/dev/null 2>&1; then
        printf 'setsid\n'
        return 0
    fi
    local realmmesh_helper="${realmmesh_root}/build/dev/bin/realm_detach"
    if [[ -x "${realmmesh_helper}" ]]; then
        printf '%s\n' "${realmmesh_helper}"
        return 0
    fi
    printf 'setsid is unavailable and the detach helper is missing: %s\n' \
        "${realmmesh_helper}" >&2
    printf 'Run ./scripts/build.sh first.\n' >&2
    return 1
}
