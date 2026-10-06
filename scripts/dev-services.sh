#!/usr/bin/env bash

# 用途：以分布式开发模式分别管理 realm、gateway 两个服务进程。
# 用法：./scripts/dev-services.sh [--preset NAME] [start|stop|restart|status]（默认 restart）
# --preset 选择 realm_mesh 所在的构建目录(默认 dev,取配置期记录的真实 binaryDir,
# #122);只有启动路径需要它,stop/status 只看 PID 文件。

set -euo pipefail

# pwd -P:macOS 上 /var 是 /private/var 的符号链接,进程 cwd 校验用 lsof
# 取到的是物理路径,这里必须同样物理化,否则同一目录会被判为不匹配。
realmmesh_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
# shellcheck source=lib/dev-process.sh
source "${realmmesh_root}/scripts/lib/dev-process.sh"
# shellcheck source=lib/build-dir.sh
source "${realmmesh_root}/scripts/lib/build-dir.sh"
realmmesh_script_path="$(realmmesh_realpath "${BASH_SOURCE[0]}")"
realmmesh_runtime_dir="${realmmesh_root}/.runtime"
realmmesh_pid_dir="${realmmesh_runtime_dir}/pids"
realmmesh_action="restart"
realmmesh_preset="${realmmesh_default_preset}"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --preset)
            if [[ $# -lt 2 ]]; then
                printf -- '--preset requires a name.\n' >&2
                exit 2
            fi
            realmmesh_preset="$2"
            shift 2
            ;;
        --preset=*)
            realmmesh_preset="${1#--preset=}"
            shift
            ;;
        *)
            realmmesh_action="$1"
            shift
            ;;
    esac
done
realmmesh_require_preset_name "${realmmesh_preset}" || exit 2
# 停止/状态顺序(启动序的反转);启动按 realmmesh_start_services 依赖序。
realmmesh_services=(gateway realm)
realmmesh_start_services=(realm gateway)
# 服务进程的 argv[0] 基名;二进制的完整路径在启动时按预设解析。
realmmesh_mesh_binary_name="realm_mesh"
realmmesh_mesh_binary=""
realmmesh_bin_dir=""
realmmesh_config_root="${realmmesh_root}/configs"
realmmesh_supervisor_pid_file="${realmmesh_pid_dir}/supervisor.pid"
realmmesh_supervisor_state_file="${realmmesh_runtime_dir}/supervisor.state"
realmmesh_supervisor_shutdown_file="${realmmesh_runtime_dir}/supervisor.shutdown"
realmmesh_supervisor_log_file="${realmmesh_runtime_dir}/logs/supervisor/console.log"
realmmesh_startup_timeout_seconds="${REALMMESH_STARTUP_TIMEOUT_SECONDS:-10}"

# 按预设解析构建目录,设置 realmmesh_bin_dir 与 realmmesh_mesh_binary。
resolve_mesh_binary() {
    realmmesh_mesh_binary="$(realmmesh_resolve_built_binary "${realmmesh_root}" \
        "${realmmesh_preset}" "${realmmesh_mesh_binary_name}")" || return 1
    realmmesh_bin_dir="$(dirname "${realmmesh_mesh_binary}")"
}

service_pid_file() {
    printf '%s\n' "${realmmesh_pid_dir}/$1.pid"
}

service_log_file() {
    printf '%s\n' "${realmmesh_runtime_dir}/logs/$1/console.log"
}

read_service_pid() {
    local realmmesh_pid_file
    realmmesh_pid_file="$(service_pid_file "$1")"
    [[ -f "${realmmesh_pid_file}" ]] || return 1

    local realmmesh_pid
    realmmesh_pid="$(<"${realmmesh_pid_file}")"
    [[ "${realmmesh_pid}" =~ ^[1-9][0-9]*$ ]] || return 1
    printf '%s\n' "${realmmesh_pid}"
}

read_supervisor_pid() {
    [[ -f "${realmmesh_supervisor_pid_file}" ]] || return 1
    local realmmesh_pid
    realmmesh_pid="$(<"${realmmesh_supervisor_pid_file}")"
    [[ "${realmmesh_pid}" =~ ^[1-9][0-9]*$ ]] || return 1
    printf '%s\n' "${realmmesh_pid}"
}

is_expected_supervisor_process() {
    local realmmesh_pid="$1"
    kill -0 "${realmmesh_pid}" 2>/dev/null || return 1
    realmmesh_is_zombie_process "${realmmesh_pid}" && return 1
    [[ "$(realmmesh_process_cwd "${realmmesh_pid}")" == "${realmmesh_root}" ]] ||
        return 1
    realmmesh_process_has_arguments \
        "${realmmesh_pid}" "${realmmesh_script_path}" supervise
}

is_expected_service_process() {
    local realmmesh_service="$1"
    local realmmesh_pid="$2"
    kill -0 "${realmmesh_pid}" 2>/dev/null || return 1
    realmmesh_is_zombie_process "${realmmesh_pid}" && return 1

    local realmmesh_executable
    realmmesh_executable="$(realmmesh_process_executable "${realmmesh_pid}")" ||
        return 1
    [[ "${realmmesh_executable##*/}" == "${realmmesh_mesh_binary_name}" ]] ||
        return 1
    [[ "$(realmmesh_process_cwd "${realmmesh_pid}")" == "${realmmesh_root}" ]] ||
        return 1

    # 两个服务共用 realm_mesh 二进制,以 --service <name> 参数区分。
    realmmesh_process_has_arguments \
        "${realmmesh_pid}" --service "${realmmesh_service}"
}

show_status() {
    local realmmesh_failed=0
    local realmmesh_supervisor_pid
    if realmmesh_supervisor_pid="$(read_supervisor_pid)" &&
        is_expected_supervisor_process "${realmmesh_supervisor_pid}"; then
        printf '%-7s running (pid %s)\n' \
            "manager" "${realmmesh_supervisor_pid}"
    else
        printf '%-7s stopped\n' "manager"
        realmmesh_failed=1
    fi
    local realmmesh_service
    for realmmesh_service in "${realmmesh_services[@]}"; do
        local realmmesh_pid
        if realmmesh_pid="$(read_service_pid "${realmmesh_service}")" &&
            is_expected_service_process "${realmmesh_service}" "${realmmesh_pid}"; then
            printf '%-7s running (pid %s)\n' "${realmmesh_service}" "${realmmesh_pid}"
        else
            printf '%-7s stopped\n' "${realmmesh_service}"
            realmmesh_failed=1
        fi
    done
    return "${realmmesh_failed}"
}

all_services_running() {
    local realmmesh_service
    for realmmesh_service in "${realmmesh_services[@]}"; do
        local realmmesh_pid
        realmmesh_pid="$(read_service_pid "${realmmesh_service}")" || return 1
        is_expected_service_process \
            "${realmmesh_service}" "${realmmesh_pid}" || return 1
    done
}

service_metrics_url() {
    case "$1" in
        realm)
            printf '%s\n' \
                "${REALMMESH_REALM_METRICS_URL:-http://127.0.0.1:9102/metrics}"
            ;;
        gateway)
            printf '%s\n' \
                "${REALMMESH_GATEWAY_METRICS_URL:-http://127.0.0.1:9103/metrics}"
            ;;
    esac
}

wait_for_service_ready() {
    local realmmesh_service="$1"
    local realmmesh_pid="$2"
    local realmmesh_url
    realmmesh_url="$(service_metrics_url "${realmmesh_service}")"
    local realmmesh_deadline=$((SECONDS + realmmesh_startup_timeout_seconds))
    while [[ "${SECONDS}" -lt "${realmmesh_deadline}" ]]; do
        if [[ -f "${realmmesh_supervisor_shutdown_file}" ]]; then
            return 1
        fi
        if ! is_expected_service_process \
            "${realmmesh_service}" "${realmmesh_pid}"; then
            if ! kill -0 "${realmmesh_pid}" 2>/dev/null; then
                return 1
            fi
            sleep 0.1
            continue
        fi
        if curl --silent --fail --connect-timeout 0.1 --max-time 0.2 \
            "${realmmesh_url}" 2>/dev/null |
            grep -Eq \
                "^realmmesh_service_ready\\{service_name=\"${realmmesh_service}\",service_instance=\"[^\"]+\"\\} 1$"; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

stop_services() {
    local realmmesh_failed=0
    local realmmesh_service
    for realmmesh_service in "${realmmesh_services[@]}"; do
        local realmmesh_pid
        if ! realmmesh_pid="$(read_service_pid "${realmmesh_service}")"; then
            continue
        fi
        if ! is_expected_service_process "${realmmesh_service}" "${realmmesh_pid}"; then
            if kill -0 "${realmmesh_pid}" 2>/dev/null; then
                if [[ -n "$(realmmesh_process_command_line "${realmmesh_pid}")" ]]; then
                    printf 'Refusing to stop stale %s PID %s.\n' \
                        "${realmmesh_service}" "${realmmesh_pid}" >&2
                    realmmesh_failed=1
                    continue
                fi
                wait "${realmmesh_pid}" 2>/dev/null || true
            fi
            rm -f -- "$(service_pid_file "${realmmesh_service}")"
            continue
        fi
        printf 'Stopping %s\n' "${realmmesh_service}"
        kill -TERM "${realmmesh_pid}"
        local realmmesh_attempt
        for realmmesh_attempt in {1..50}; do
            if ! is_expected_service_process \
                "${realmmesh_service}" "${realmmesh_pid}"; then
                wait "${realmmesh_pid}" 2>/dev/null || true
                break
            fi
            sleep 0.1
        done
        if is_expected_service_process \
            "${realmmesh_service}" "${realmmesh_pid}"; then
            printf '%s process %s did not stop within 5 seconds; killing it.\n' \
                "${realmmesh_service}" "${realmmesh_pid}" >&2
            kill -KILL "${realmmesh_pid}"
            wait "${realmmesh_pid}" 2>/dev/null || true
            realmmesh_failed=1
        fi
        rm -f -- "$(service_pid_file "${realmmesh_service}")"
    done
    printf 'RealmMesh development services stopped.\n'
    return "${realmmesh_failed}"
}

supervise_services() {
    local realmmesh_shutdown_requested=0
    local realmmesh_worker_status=0
    local realmmesh_worker_pid
    rm -f -- "${realmmesh_supervisor_shutdown_file}"
    # Bash 5.2 会在解析命令替换时执行信号 trap,破坏尚未完成的解析。
    # 管理进程装好 trap 后只等待工作进程,不执行任何命令替换。
    # 工作进程没有信号 trap,通过停止文件在启动/巡检的安全点收尾。
    trap 'realmmesh_shutdown_requested=1' INT TERM
    (
        trap - INT TERM
        supervise_service_group
    ) &
    realmmesh_worker_pid=$!
    if [[ "${realmmesh_shutdown_requested}" -eq 0 ]]; then
        wait "${realmmesh_worker_pid}" || realmmesh_worker_status=$?
    fi
    if [[ "${realmmesh_shutdown_requested}" -ne 0 ]]; then
        # wait 被信号打断并不代表子进程退出;只发布一次停止请求,然后真回收。
        trap '' INT TERM
        : > "${realmmesh_supervisor_shutdown_file}"
        realmmesh_worker_status=0
        wait "${realmmesh_worker_pid}" || realmmesh_worker_status=$?
    fi
    rm -f -- "${realmmesh_supervisor_shutdown_file}"
    return "${realmmesh_worker_status}"
}

supervise_service_group() {
    local realmmesh_child_failed=0
    printf 'Starting RealmMesh services with a %s second per-service timeout.\n' \
        "${realmmesh_startup_timeout_seconds}"

    if ! start_services; then
        printf '%s\n' failed > "${realmmesh_supervisor_state_file}"
        rm -f -- "${realmmesh_supervisor_pid_file}"
        return 1
    fi
    printf '%s\n' running > "${realmmesh_supervisor_state_file}"

    while [[ ! -f "${realmmesh_supervisor_shutdown_file}" ]]; do
        if ! all_services_running; then
            printf 'A RealmMesh service exited; stopping the service group.\n' >&2
            realmmesh_child_failed=1
            break
        fi
        sleep 0.1
    done

    stop_services || true
    rm -f -- "${realmmesh_supervisor_pid_file}" \
        "${realmmesh_supervisor_state_file}"
    return "${realmmesh_child_failed}"
}

start_supervisor() {
    local realmmesh_pid
    if realmmesh_pid="$(read_supervisor_pid)" &&
        is_expected_supervisor_process "${realmmesh_pid}"; then
        printf 'RealmMesh development services are already running.\n'
        show_status
        return 0
    fi
    if [[ -f "${realmmesh_supervisor_pid_file}" ]]; then
        rm -f -- "${realmmesh_supervisor_pid_file}"
    fi
    rm -f -- "${realmmesh_supervisor_state_file}"
    resolve_mesh_binary || return 1
    local realmmesh_detach
    if ! realmmesh_detach="$(realmmesh_detach_command "${realmmesh_bin_dir}")"; then
        return 1
    fi

    mkdir -p "${realmmesh_pid_dir}" \
        "$(dirname "${realmmesh_supervisor_log_file}")"
    cd "${realmmesh_root}"
    # supervise 紧跟脚本路径:is_expected_supervisor_process 按这对连续参数
    # 识别监督进程,与所选预设无关。
    nohup "${realmmesh_detach}" bash "${realmmesh_script_path}" supervise \
        --preset "${realmmesh_preset}" \
        >> "${realmmesh_supervisor_log_file}" 2>&1 </dev/null &
    realmmesh_pid=$!
    printf '%s\n' "${realmmesh_pid}" > "${realmmesh_supervisor_pid_file}"

    local realmmesh_supervisor_attempts=$((
        realmmesh_startup_timeout_seconds *
        ${#realmmesh_start_services[@]} *
        10 + 20))
    local realmmesh_attempt
    for ((realmmesh_attempt = 0;
         realmmesh_attempt < realmmesh_supervisor_attempts;
         ++realmmesh_attempt)); do
        if [[ -f "${realmmesh_supervisor_state_file}" ]] &&
            [[ "$(<"${realmmesh_supervisor_state_file}")" == "failed" ]]; then
            wait "${realmmesh_pid}" 2>/dev/null || true
            rm -f -- "${realmmesh_supervisor_pid_file}" \
                "${realmmesh_supervisor_state_file}"
            printf 'RealmMesh supervisor failed; inspect %s\n' \
                "${realmmesh_supervisor_log_file}" >&2
            return 1
        fi
        if ! kill -0 "${realmmesh_pid}" 2>/dev/null; then
            rm -f -- "${realmmesh_supervisor_pid_file}"
            printf 'RealmMesh supervisor failed; inspect %s\n' \
                "${realmmesh_supervisor_log_file}" >&2
            return 1
        fi
        if [[ -f "${realmmesh_supervisor_state_file}" ]] &&
            [[ "$(<"${realmmesh_supervisor_state_file}")" == "running" ]]; then
            printf 'RealmMesh development services started.\n'
            show_status
            return 0
        fi
        sleep 0.1
    done
    printf 'RealmMesh services did not become ready; inspect %s\n' \
        "${realmmesh_supervisor_log_file}" >&2
    kill -TERM "${realmmesh_pid}" 2>/dev/null || true
    return 1
}

stop_supervisor() {
    local realmmesh_pid
    if ! realmmesh_pid="$(read_supervisor_pid)"; then
        stop_services
        return
    fi
    if ! is_expected_supervisor_process "${realmmesh_pid}"; then
        printf 'Refusing to stop stale supervisor PID %s.\n' \
            "${realmmesh_pid}" >&2
        return 1
    fi

    kill -TERM "${realmmesh_pid}"
    local realmmesh_attempt
    for realmmesh_attempt in {1..100}; do
        if ! is_expected_supervisor_process "${realmmesh_pid}"; then
            rm -f -- "${realmmesh_supervisor_pid_file}" \
                "${realmmesh_supervisor_state_file}"
            printf 'RealmMesh development services stopped.\n'
            return 0
        fi
        sleep 0.1
    done
    printf 'RealmMesh supervisor %s did not stop within 10 seconds.\n' \
        "${realmmesh_pid}" >&2
    return 1
}

start_services() {
    local realmmesh_running=0
    local realmmesh_service
    for realmmesh_service in "${realmmesh_services[@]}"; do
        local realmmesh_pid
        if realmmesh_pid="$(read_service_pid "${realmmesh_service}")" &&
            is_expected_service_process "${realmmesh_service}" "${realmmesh_pid}"; then
            realmmesh_running=$((realmmesh_running + 1))
        fi
    done
    if [[ "${realmmesh_running}" -eq "${#realmmesh_services[@]}" ]]; then
        printf 'RealmMesh development services are already running.\n'
        show_status
        return 0
    fi
    if [[ "${realmmesh_running}" -ne 0 ]]; then
        printf 'Some RealmMesh services are already running; use restart.\n' >&2
        show_status || true
        return 1
    fi
    resolve_mesh_binary || return 1
    local realmmesh_detach
    if ! realmmesh_detach="$(realmmesh_detach_command "${realmmesh_bin_dir}")"; then
        return 1
    fi
    if ! command -v curl >/dev/null 2>&1; then
        printf 'curl is required to check service readiness.\n' >&2
        return 1
    fi
    if [[ ! "${realmmesh_startup_timeout_seconds}" =~ ^[1-9][0-9]*$ ]]; then
        printf 'REALMMESH_STARTUP_TIMEOUT_SECONDS must be a positive integer.\n' >&2
        return 1
    fi

    export REALMMESH_TLS_CERTIFICATE_FILE="${REALMMESH_TLS_CERTIFICATE_FILE:-${realmmesh_runtime_dir}/tls/server-cert.pem}"
    export REALMMESH_TLS_PRIVATE_KEY_FILE="${REALMMESH_TLS_PRIVATE_KEY_FILE:-${realmmesh_runtime_dir}/tls/server-key.pem}"
    export REALMMESH_SESSION_TICKET_KEY="${REALMMESH_SESSION_TICKET_KEY:-$(openssl rand -hex 32)}"

    local realmmesh_required_file
    for realmmesh_required_file in \
        "${REALMMESH_TLS_CERTIFICATE_FILE}" \
        "${REALMMESH_TLS_PRIVATE_KEY_FILE}"; do
        if [[ ! -r "${realmmesh_required_file}" ]]; then
            printf 'Required TLS file is missing or unreadable: %s\n' \
                "${realmmesh_required_file}" >&2
            return 1
        fi
    done

    mkdir -p "${realmmesh_pid_dir}"
    # 模式 2 单服务进程:以依赖序 realm → gateway 依次拉起,
    # cwd 固定为仓库根(is_expected_service_process 的 cwd 校验依赖它)。
    cd "${realmmesh_root}"
    for realmmesh_service in "${realmmesh_start_services[@]}"; do
        local realmmesh_log realmmesh_pid
        realmmesh_log="$(service_log_file "${realmmesh_service}")"

        mkdir -p "$(dirname "${realmmesh_log}")"
        nohup "${realmmesh_detach}" "${realmmesh_mesh_binary}" \
            --service "${realmmesh_service}" \
            --config "${realmmesh_config_root}" \
            >> "${realmmesh_log}" 2>&1 </dev/null &
        realmmesh_pid=$!
        printf '%s\n' "${realmmesh_pid}" \
            > "$(service_pid_file "${realmmesh_service}")"
        if ! wait_for_service_ready \
            "${realmmesh_service}" "${realmmesh_pid}"; then
            printf '%s failed to become ready within %s seconds; inspect %s\n' \
                "${realmmesh_service}" \
                "${realmmesh_startup_timeout_seconds}" \
                "${realmmesh_log}" >&2
            stop_services || true
            return 1
        fi
    done

    printf 'RealmMesh development services started.\n'
    show_status
}

case "${realmmesh_action}" in
    start)
        start_supervisor
        ;;
    stop)
        stop_supervisor
        ;;
    restart)
        stop_supervisor
        start_supervisor
        ;;
    status)
        show_status
        ;;
    supervise)
        supervise_services
        ;;
    *)
        printf 'Usage: %s [--preset NAME] [start|stop|restart|status]\n' "$0" >&2
        exit 2
        ;;
esac
