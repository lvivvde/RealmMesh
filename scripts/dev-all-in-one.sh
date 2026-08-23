#!/usr/bin/env bash

# 用途：以单进程 all-in-one 模式管理完整 RealmMesh 拓扑，并统一保存 PID 和控制台日志。
# 用法：./scripts/dev-all-in-one.sh [start|stop|restart|status]（默认 restart）

set -euo pipefail

realmmesh_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
realmmesh_runtime_dir="${realmmesh_root}/.runtime"
realmmesh_pid_file="${realmmesh_runtime_dir}/pids/all-in-one.pid"
realmmesh_log_file="${realmmesh_runtime_dir}/logs/all-in-one/console.log"
realmmesh_ticket_key_file="${realmmesh_runtime_dir}/session-ticket-key.hex"
realmmesh_mesh_binary="${realmmesh_root}/build/dev/bin/realm_mesh"
realmmesh_config_root="${realmmesh_root}/configs"
realmmesh_action="${1:-restart}"

read_pid() {
    [[ -f "${realmmesh_pid_file}" ]] || return 1

    local realmmesh_pid
    realmmesh_pid="$(<"${realmmesh_pid_file}")"
    [[ "${realmmesh_pid}" =~ ^[1-9][0-9]*$ ]] || return 1
    printf '%s\n' "${realmmesh_pid}"
}

is_expected_process() {
    local realmmesh_pid="$1"
    kill -0 "${realmmesh_pid}" 2>/dev/null || return 1
    [[ -r "/proc/${realmmesh_pid}/cmdline" ]] || return 1
    [[ "$(readlink -f "/proc/${realmmesh_pid}/cwd")" == \
        "${realmmesh_root}" ]] || return 1

    local -a realmmesh_arguments=()
    mapfile -d '' realmmesh_arguments < "/proc/${realmmesh_pid}/cmdline"
    [[ "${realmmesh_arguments[0]##*/}" == \
        "$(basename "${realmmesh_mesh_binary}")" ]] || return 1

    local realmmesh_index
    for realmmesh_index in "${!realmmesh_arguments[@]}"; do
        [[ "${realmmesh_arguments[realmmesh_index]}" != "--service" ]] ||
            return 1
    done
    return 0
}

show_status() {
    local realmmesh_pid
    if realmmesh_pid="$(read_pid)" &&
        is_expected_process "${realmmesh_pid}"; then
        printf 'all-in-one running (pid %s)\n' "${realmmesh_pid}"
        printf 'console log: %s\n' "${realmmesh_log_file}"
        return 0
    fi
    printf 'all-in-one stopped\n'
    return 1
}

stop_service() {
    local realmmesh_pid
    if ! realmmesh_pid="$(read_pid)"; then
        printf 'RealmMesh all-in-one is already stopped.\n'
        return 0
    fi
    if ! kill -0 "${realmmesh_pid}" 2>/dev/null; then
        rm -f -- "${realmmesh_pid_file}"
        printf 'Removed stale all-in-one PID file.\n'
        return 0
    fi
    if ! is_expected_process "${realmmesh_pid}"; then
        printf 'Refusing to stop stale all-in-one PID %s.\n' \
            "${realmmesh_pid}" >&2
        return 1
    fi

    kill -TERM "${realmmesh_pid}"
    local realmmesh_attempt
    for realmmesh_attempt in {1..50}; do
        if ! kill -0 "${realmmesh_pid}" 2>/dev/null; then
            rm -f -- "${realmmesh_pid_file}"
            printf 'RealmMesh all-in-one stopped.\n'
            return 0
        fi
        sleep 0.1
    done

    printf 'Process %s did not stop within 5 seconds.\n' \
        "${realmmesh_pid}" >&2
    return 1
}

load_environment() {
    export REALMMESH_TLS_CERTIFICATE_FILE="${REALMMESH_TLS_CERTIFICATE_FILE:-${realmmesh_runtime_dir}/tls/server-cert.pem}"
    export REALMMESH_TLS_PRIVATE_KEY_FILE="${REALMMESH_TLS_PRIVATE_KEY_FILE:-${realmmesh_runtime_dir}/tls/server-key.pem}"

    if [[ -z "${REALMMESH_SESSION_TICKET_KEY:-}" ]]; then
        if [[ ! -r "${realmmesh_ticket_key_file}" ]]; then
            mkdir -p "$(dirname "${realmmesh_ticket_key_file}")"
            umask 077
            openssl rand -hex 32 > "${realmmesh_ticket_key_file}"
        fi
        IFS= read -r REALMMESH_SESSION_TICKET_KEY < \
            "${realmmesh_ticket_key_file}"
        export REALMMESH_SESSION_TICKET_KEY
    fi

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
    if [[ ! "${REALMMESH_SESSION_TICKET_KEY}" =~ ^[0-9a-fA-F]{64}$ ]]; then
        printf 'REALMMESH_SESSION_TICKET_KEY must be 64 hex characters.\n' >&2
        return 1
    fi
}

start_service() {
    local realmmesh_pid
    if realmmesh_pid="$(read_pid)"; then
        if is_expected_process "${realmmesh_pid}"; then
            printf 'RealmMesh all-in-one is already running.\n'
            show_status
            return 0
        fi
        if kill -0 "${realmmesh_pid}" 2>/dev/null; then
            printf 'Stale all-in-one PID %s belongs to another process.\n' \
                "${realmmesh_pid}" >&2
            return 1
        fi
        rm -f -- "${realmmesh_pid_file}"
    fi

    if ! command -v setsid >/dev/null 2>&1; then
        printf 'setsid is required to detach all-in-one.\n' >&2
        return 1
    fi
    if [[ ! -x "${realmmesh_mesh_binary}" ]]; then
        printf 'Service binary is missing: %s\nRun ./scripts/build.sh first.\n' \
            "${realmmesh_mesh_binary}" >&2
        return 1
    fi
    load_environment

    mkdir -p "$(dirname "${realmmesh_pid_file}")" \
        "$(dirname "${realmmesh_log_file}")"
    cd "${realmmesh_root}"
    nohup setsid "${realmmesh_mesh_binary}" \
        --config "${realmmesh_config_root}" \
        >> "${realmmesh_log_file}" 2>&1 </dev/null &
    realmmesh_pid=$!
    printf '%s\n' "${realmmesh_pid}" > "${realmmesh_pid_file}"

    sleep 1
    if ! is_expected_process "${realmmesh_pid}"; then
        rm -f -- "${realmmesh_pid_file}"
        printf 'RealmMesh all-in-one failed to start; inspect %s\n' \
            "${realmmesh_log_file}" >&2
        return 1
    fi

    printf 'RealmMesh all-in-one started.\n'
    show_status
}

case "${realmmesh_action}" in
    start)
        start_service
        ;;
    stop)
        stop_service
        ;;
    restart)
        stop_service
        start_service
        ;;
    status)
        show_status
        ;;
    *)
        printf 'Usage: %s [start|stop|restart|status]\n' "$0" >&2
        exit 2
        ;;
esac
