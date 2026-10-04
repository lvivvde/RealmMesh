#!/usr/bin/env bash

# 编译并行的资源预算(#125,决策见 docs/research/build-tool-cache-decisions.md)。
#
#   预算 = min(逻辑 CPU 数, 8, max(1, floor((有效内存 GiB - 2) / 2)))
#
# 有效内存取主机/VM 内存与本进程所在容器(cgroup)限额中的较小值，读不到就按
# 1 路。最终 jobs 的优先级:显式 --jobs > 非空 CMAKE_BUILD_PARALLEL_LEVEL > 预算;
# 非法值失败。编译 jobs 只管 cmake --build,与 CTest 的测试并行分开。
#
# 只依赖 POSIX 工具与 bash 3.2 语法(macOS 自带 bash)。

realmmesh_jobs_cap=8
realmmesh_gib=1073741824

realmmesh_is_positive_integer() {
    [[ "$1" =~ ^[1-9][0-9]*$ ]]
}

# 纯计算:realmmesh_compute_jobs_budget <逻辑CPU数> <有效内存字节数>;任一项
# 为空或 0 视为未知，结果为 1。
realmmesh_compute_jobs_budget() {
    local cpus="$1"
    local memory="$2"
    if ! realmmesh_is_positive_integer "${cpus}" ||
        ! realmmesh_is_positive_integer "${memory}"; then
        echo 1
        return 0
    fi
    local by_memory=$(((memory - 2 * realmmesh_gib) / (2 * realmmesh_gib)))
    # 内存不足 4GiB 时上式不足 1(bash 向零取整),一律保底 1 路。
    if [[ "${memory}" -lt $((4 * realmmesh_gib)) ]]; then
        by_memory=1
    fi
    local budget="${cpus}"
    [[ "${budget}" -le "${realmmesh_jobs_cap}" ]] || budget="${realmmesh_jobs_cap}"
    [[ "${budget}" -le "${by_memory}" ]] || budget="${by_memory}"
    echo "${budget}"
}

# Linux:本进程 cgroup 的内存限额(字节);无限额或读不到时输出空。
realmmesh_cgroup_memory_limit() {
    local cgroup_path limit=""
    # cgroup v2:/proc/self/cgroup 的 "0::<路径>"。
    cgroup_path="$(sed -n 's/^0:://p' /proc/self/cgroup 2>/dev/null | head -n 1)"
    if [[ -n "${cgroup_path}" && -r "/sys/fs/cgroup${cgroup_path}/memory.max" ]]; then
        limit="$(cat "/sys/fs/cgroup${cgroup_path}/memory.max")"
    elif [[ -r /sys/fs/cgroup/memory.max ]]; then
        limit="$(cat /sys/fs/cgroup/memory.max)"
    elif [[ -r /sys/fs/cgroup/memory/memory.limit_in_bytes ]]; then
        # cgroup v1:无限额时是一个接近 2^63 的数，与主机内存取小即可。
        limit="$(cat /sys/fs/cgroup/memory/memory.limit_in_bytes)"
    fi
    if realmmesh_is_positive_integer "${limit}"; then
        echo "${limit}"
    fi
}

# Linux:cgroup v2 的 CPU 配额折算成整数 CPU(向上取整);无配额时输出空。
realmmesh_cgroup_cpu_limit() {
    local cgroup_path quota period
    cgroup_path="$(sed -n 's/^0:://p' /proc/self/cgroup 2>/dev/null | head -n 1)"
    [[ -n "${cgroup_path}" && -r "/sys/fs/cgroup${cgroup_path}/cpu.max" ]] || return 0
    read -r quota period <"/sys/fs/cgroup${cgroup_path}/cpu.max" || return 0
    if realmmesh_is_positive_integer "${quota}" &&
        realmmesh_is_positive_integer "${period}"; then
        echo $(((quota + period - 1) / period))
    fi
}

# 探测本机资源，写入 realmmesh_detected_cpus / realmmesh_detected_memory(字节,
# 读不到为空)。
realmmesh_detect_build_resources() {
    realmmesh_detected_cpus=""
    realmmesh_detected_memory=""
    case "$(uname -s)" in
        Darwin)
            realmmesh_detected_cpus="$(sysctl -n hw.logicalcpu 2>/dev/null || true)"
            realmmesh_detected_memory="$(sysctl -n hw.memsize 2>/dev/null || true)"
            ;;
        Linux)
            realmmesh_detected_cpus="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || true)"
            local cpu_limit
            cpu_limit="$(realmmesh_cgroup_cpu_limit)"
            if [[ -n "${cpu_limit}" && -n "${realmmesh_detected_cpus}" &&
                "${cpu_limit}" -lt "${realmmesh_detected_cpus}" ]]; then
                realmmesh_detected_cpus="${cpu_limit}"
            fi
            local mem_kib
            mem_kib="$(sed -n 's/^MemTotal:[[:space:]]*\([0-9][0-9]*\) kB$/\1/p' /proc/meminfo 2>/dev/null)"
            if realmmesh_is_positive_integer "${mem_kib}"; then
                realmmesh_detected_memory=$((mem_kib * 1024))
            fi
            local mem_limit
            mem_limit="$(realmmesh_cgroup_memory_limit)"
            if [[ -n "${mem_limit}" ]] && { [[ -z "${realmmesh_detected_memory}" ]] ||
                [[ "${mem_limit}" -lt "${realmmesh_detected_memory}" ]]; }; then
                realmmesh_detected_memory="${mem_limit}"
            fi
            ;;
    esac
    realmmesh_is_positive_integer "${realmmesh_detected_cpus}" || realmmesh_detected_cpus=""
    realmmesh_is_positive_integer "${realmmesh_detected_memory}" || realmmesh_detected_memory=""
}

# 内存字节数折成两位小数的 GiB,只用于打印。
realmmesh_format_gib() {
    if [[ -z "$1" ]]; then
        echo "unknown"
    else
        awk -v bytes="$1" 'BEGIN { printf "%.2f GiB", bytes / 1073741824 }'
    fi
}

# realmmesh_resolve_build_jobs <--jobs 的值，可为空>
# 成功时写入 realmmesh_build_jobs、realmmesh_build_jobs_source 与
# realmmesh_build_jobs_report(一行说明，含预算与最终值);非法值返回非零。
realmmesh_resolve_build_jobs() {
    local explicit="$1"
    realmmesh_detect_build_resources
    local budget
    budget="$(realmmesh_compute_jobs_budget \
        "${realmmesh_detected_cpus}" "${realmmesh_detected_memory}")"

    if [[ -n "${explicit}" ]]; then
        if ! realmmesh_is_positive_integer "${explicit}"; then
            printf -- '--jobs must be a positive integer, got "%s".\n' "${explicit}" >&2
            return 1
        fi
        realmmesh_build_jobs="${explicit}"
        realmmesh_build_jobs_source="--jobs"
    elif [[ -n "${CMAKE_BUILD_PARALLEL_LEVEL:-}" ]]; then
        if ! realmmesh_is_positive_integer "${CMAKE_BUILD_PARALLEL_LEVEL}"; then
            printf 'CMAKE_BUILD_PARALLEL_LEVEL must be a positive integer, got "%s".\n' \
                "${CMAKE_BUILD_PARALLEL_LEVEL}" >&2
            return 1
        fi
        realmmesh_build_jobs="${CMAKE_BUILD_PARALLEL_LEVEL}"
        realmmesh_build_jobs_source="CMAKE_BUILD_PARALLEL_LEVEL"
    else
        realmmesh_build_jobs="${budget}"
        realmmesh_build_jobs_source="budget"
    fi
    realmmesh_build_jobs_report="build jobs: ${realmmesh_build_jobs} (from ${realmmesh_build_jobs_source}; budget ${budget} = min(${realmmesh_detected_cpus:-unknown} CPUs, ${realmmesh_jobs_cap}, memory $(realmmesh_format_gib "${realmmesh_detected_memory}")))"
}
