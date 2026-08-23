#!/usr/bin/env bash

set -euo pipefail

realmmesh_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
realmmesh_runtime_dir="${realmmesh_root}/.runtime"
realmmesh_pid_dir="${realmmesh_runtime_dir}/pids"
realmmesh_action="${1:-restart}"
realmmesh_services=(gateway realm login)

service_binary() {
    case "$1" in
        gateway) printf '%s\n' "${realmmesh_root}/build/dev/bin/realm_gateway" ;;
        realm) printf '%s\n' "${realmmesh_root}/build/dev/bin/realm_character" ;;
        login) printf '%s\n' "${realmmesh_root}/build/dev/bin/realm_login" ;;
    esac
}

service_config() {
    printf '%s\n' "${realmmesh_root}/lua/config/services/$1.lua"
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

is_expected_service_process() {
    local realmmesh_service="$1"
    local realmmesh_pid="$2"
    kill -0 "${realmmesh_pid}" 2>/dev/null || return 1
    [[ -r "/proc/${realmmesh_pid}/cmdline" ]] || return 1

    local realmmesh_executable
    IFS= read -r -d '' realmmesh_executable \
        < "/proc/${realmmesh_pid}/cmdline" || true
    [[ "${realmmesh_executable##*/}" == "$(basename "$(service_binary "${realmmesh_service}")")" ]] ||
        return 1
    [[ "$(readlink -f "/proc/${realmmesh_pid}/cwd")" == "${realmmesh_root}" ]]
}

show_status() {
    local realmmesh_failed=0
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

stop_services() {
    local -a realmmesh_stop_pids=()
    local realmmesh_service
    for realmmesh_service in "${realmmesh_services[@]}"; do
        local realmmesh_pid
        if ! realmmesh_pid="$(read_service_pid "${realmmesh_service}")"; then
            continue
        fi
        if ! is_expected_service_process "${realmmesh_service}" "${realmmesh_pid}"; then
            printf 'Refusing to stop stale %s PID %s.\n' \
                "${realmmesh_service}" "${realmmesh_pid}" >&2
            return 1
        fi
        realmmesh_stop_pids+=("${realmmesh_pid}")
    done

    local realmmesh_pid
    for realmmesh_pid in "${realmmesh_stop_pids[@]}"; do
        kill -TERM "${realmmesh_pid}"
    done

    local realmmesh_attempt
    for realmmesh_attempt in {1..50}; do
        local realmmesh_alive=0
        for realmmesh_pid in "${realmmesh_stop_pids[@]}"; do
            if kill -0 "${realmmesh_pid}" 2>/dev/null; then
                realmmesh_alive=1
            fi
        done
        [[ "${realmmesh_alive}" -eq 0 ]] && break
        sleep 0.1
    done

    for realmmesh_pid in "${realmmesh_stop_pids[@]}"; do
        if kill -0 "${realmmesh_pid}" 2>/dev/null; then
            printf 'Process %s did not stop within 5 seconds.\n' \
                "${realmmesh_pid}" >&2
            return 1
        fi
    done
    rm -f -- "${realmmesh_pid_dir}/gateway.pid" \
        "${realmmesh_pid_dir}/realm.pid" \
        "${realmmesh_pid_dir}/login.pid"
    printf 'RealmMesh development services stopped.\n'
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
    if ! command -v setsid >/dev/null 2>&1; then
        printf 'setsid is required to detach development services.\n' >&2
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
    local -a realmmesh_started_pids=()
    for realmmesh_service in "${realmmesh_services[@]}"; do
        local realmmesh_binary realmmesh_config realmmesh_log realmmesh_pid
        realmmesh_binary="$(service_binary "${realmmesh_service}")"
        realmmesh_config="$(service_config "${realmmesh_service}")"
        realmmesh_log="$(service_log_file "${realmmesh_service}")"
        if [[ ! -x "${realmmesh_binary}" ]]; then
            printf 'Service binary is missing: %s\nRun ./scripts/build.sh first.\n' \
                "${realmmesh_binary}" >&2
            return 1
        fi

        mkdir -p "$(dirname "${realmmesh_log}")"
        nohup setsid "${realmmesh_binary}" --config "${realmmesh_config}" \
            >> "${realmmesh_log}" 2>&1 </dev/null &
        realmmesh_pid=$!
        printf '%s\n' "${realmmesh_pid}" \
            > "$(service_pid_file "${realmmesh_service}")"
        realmmesh_started_pids+=("${realmmesh_pid}")
    done

    sleep 1
    local realmmesh_failed=0
    for realmmesh_service in "${realmmesh_services[@]}"; do
        local realmmesh_pid
        if ! realmmesh_pid="$(read_service_pid "${realmmesh_service}")" ||
            ! is_expected_service_process "${realmmesh_service}" "${realmmesh_pid}"; then
            printf '%s failed to start; inspect %s\n' \
                "${realmmesh_service}" "$(service_log_file "${realmmesh_service}")" >&2
            realmmesh_failed=1
        fi
    done

    if [[ "${realmmesh_failed}" -ne 0 ]]; then
        local realmmesh_pid
        for realmmesh_pid in "${realmmesh_started_pids[@]}"; do
            kill -TERM "${realmmesh_pid}" 2>/dev/null || true
        done
        return 1
    fi

    printf 'RealmMesh development services started.\n'
    show_status
}

case "${realmmesh_action}" in
    start)
        start_services
        ;;
    stop)
        stop_services
        ;;
    restart)
        stop_services
        start_services
        ;;
    status)
        show_status
        ;;
    *)
        printf 'Usage: %s [start|stop|restart|status]\n' "$0" >&2
        exit 2
        ;;
esac
