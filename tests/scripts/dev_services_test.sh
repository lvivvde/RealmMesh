#!/usr/bin/env bash

set -euo pipefail

realmmesh_case="${1:?test case is required}"
realmmesh_source_root="${2:?source root is required}"
realmmesh_mesh_binary="${3:?realm_mesh binary is required}"
realmmesh_tls_certificate="${4:?TLS certificate is required}"
realmmesh_tls_private_key="${5:?TLS private key is required}"
realmmesh_three_stage_test="${6:-}"

realmmesh_scratch="$(mktemp -d)"
realmmesh_test_root="${realmmesh_scratch}/RealmMesh"
realmmesh_script="${realmmesh_test_root}/scripts/dev-services.sh"

cleanup() {
    local realmmesh_status=$?
    if [[ "${realmmesh_status}" -ne 0 &&
        -f "${realmmesh_test_root}/.runtime/logs/supervisor/console.log" ]]; then
        printf '%s\n' '--- supervisor log ---' >&2
        sed -n '1,240p' \
            "${realmmesh_test_root}/.runtime/logs/supervisor/console.log" >&2
    fi
    if [[ "${realmmesh_status}" -ne 0 &&
        -d "${realmmesh_test_root}/configs/logs" ]]; then
        printf '%s\n' '--- structured logs ---' >&2
        find "${realmmesh_test_root}/configs/logs" -maxdepth 3 -type f \
            -print -exec sed -n '1,40p' {} \; >&2
    fi
    if [[ "${realmmesh_status}" -ne 0 &&
        -d "${realmmesh_test_root}/.runtime/logs" ]]; then
        printf '%s\n' '--- service console logs ---' >&2
        find "${realmmesh_test_root}/.runtime/logs" -maxdepth 3 -type f \
            -print -exec sed -n '1,80p' {} \; >&2
    fi
    if [[ "${realmmesh_status}" -ne 0 ]]; then
        local realmmesh_debug_pid_file
        for realmmesh_debug_pid_file in \
            "${realmmesh_test_root}"/.runtime/pids/*.pid; do
            [[ -f "${realmmesh_debug_pid_file}" ]] || continue
            local realmmesh_debug_pid
            realmmesh_debug_pid="$(<"${realmmesh_debug_pid_file}")"
            ps -o pid=,ppid=,stat=,args= -p "${realmmesh_debug_pid}" >&2 || true
        done
    fi
    if [[ -f "${realmmesh_script}" ]]; then
        bash "${realmmesh_script}" stop >/dev/null 2>&1 || true
    fi
    local realmmesh_pid_file
    for realmmesh_pid_file in "${realmmesh_test_root}"/.runtime/pids/*.pid; do
        [[ -f "${realmmesh_pid_file}" ]] || continue
        local realmmesh_pid
        realmmesh_pid="$(<"${realmmesh_pid_file}")"
        if [[ "${realmmesh_pid}" =~ ^[1-9][0-9]*$ ]]; then
            kill -TERM "${realmmesh_pid}" 2>/dev/null || true
        fi
    done
    rm -rf -- "${realmmesh_scratch}"
    return "${realmmesh_status}"
}
trap cleanup EXIT

mkdir -p "${realmmesh_test_root}/scripts" \
    "${realmmesh_test_root}/build/dev/bin" \
    "${realmmesh_test_root}/configs"
cp "${realmmesh_source_root}/scripts/dev-services.sh" "${realmmesh_script}"
cp -R "${realmmesh_source_root}/configs/common" \
    "${realmmesh_test_root}/configs/common"
cp -R "${realmmesh_source_root}/configs/services" \
    "${realmmesh_test_root}/configs/services"
cp "${realmmesh_source_root}/configs/main.config" \
    "${realmmesh_test_root}/configs/main.config"
ln -s "${realmmesh_mesh_binary}" \
    "${realmmesh_test_root}/build/dev/bin/realm_mesh"

mapfile -t realmmesh_ports < <(python3 - <<'PY'
import socket

sockets = []
for _ in range(6):
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    sockets.append(listener)
for listener in sockets:
    print(listener.getsockname()[1])
PY
)

realmmesh_realm_port="${realmmesh_ports[0]}"
realmmesh_login_port="${realmmesh_ports[1]}"
realmmesh_gateway_port="${realmmesh_ports[2]}"
realmmesh_realm_metrics_port="${realmmesh_ports[3]}"
realmmesh_login_metrics_port="${realmmesh_ports[4]}"
realmmesh_gateway_metrics_port="${realmmesh_ports[5]}"

sed -i \
    -e "s/listen_port = 7100/listen_port = ${realmmesh_realm_port}/" \
    -e "s/downstream_port = 8000/downstream_port = ${realmmesh_gateway_port}/" \
    -e "s/metrics_port = 9102/metrics_port = ${realmmesh_realm_metrics_port}/" \
    "${realmmesh_test_root}/configs/services/realm.lua"
sed -i \
    -e "s/listen_port = 7000/listen_port = ${realmmesh_login_port}/" \
    -e "s/downstream_port = 7100/downstream_port = ${realmmesh_realm_port}/" \
    -e "s/metrics_port = 9101/metrics_port = ${realmmesh_login_metrics_port}/" \
    "${realmmesh_test_root}/configs/services/login.lua"
sed -i \
    -e "s/listen_port = 8000/listen_port = ${realmmesh_gateway_port}/g" \
    -e "s/metrics_port = 9103/metrics_port = ${realmmesh_gateway_metrics_port}/" \
    "${realmmesh_test_root}/configs/services/gateway.lua"

export REALMMESH_TLS_CERTIFICATE_FILE="${realmmesh_tls_certificate}"
export REALMMESH_TLS_PRIVATE_KEY_FILE="${realmmesh_tls_private_key}"
export REALMMESH_SESSION_TICKET_KEY="0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"
export REALMMESH_REALM_METRICS_URL="http://127.0.0.1:${realmmesh_realm_metrics_port}/metrics"
export REALMMESH_LOGIN_METRICS_URL="http://127.0.0.1:${realmmesh_login_metrics_port}/metrics"
export REALMMESH_GATEWAY_METRICS_URL="http://127.0.0.1:${realmmesh_gateway_metrics_port}/metrics"

case "${realmmesh_case}" in
    start_uses_supervisor)
        bash "${realmmesh_script}" start
        realmmesh_supervisor_pid_file="${realmmesh_test_root}/.runtime/pids/supervisor.pid"
        [[ -s "${realmmesh_supervisor_pid_file}" ]]
        realmmesh_supervisor_pid="$(<"${realmmesh_supervisor_pid_file}")"
        kill -0 "${realmmesh_supervisor_pid}"
        ;;
    unready_realm_blocks_dependents)
        export REALMMESH_REALM_METRICS_URL="http://127.0.0.1:1/metrics"
        export REALMMESH_STARTUP_TIMEOUT_SECONDS=1
        if bash "${realmmesh_script}" start; then
            printf 'start unexpectedly succeeded with an unready Realm\n' >&2
            exit 1
        fi
        [[ ! -f "${realmmesh_test_root}/.runtime/pids/realm.pid" ]]
        [[ ! -f "${realmmesh_test_root}/.runtime/pids/login.pid" ]]
        [[ ! -f "${realmmesh_test_root}/.runtime/pids/gateway.pid" ]]
        ;;
    child_failure_stops_group)
        bash "${realmmesh_script}" start
        realmmesh_supervisor_pid="$(<"${realmmesh_test_root}/.runtime/pids/supervisor.pid")"
        realmmesh_realm_pid="$(<"${realmmesh_test_root}/.runtime/pids/realm.pid")"
        realmmesh_login_pid="$(<"${realmmesh_test_root}/.runtime/pids/login.pid")"
        realmmesh_gateway_pid="$(<"${realmmesh_test_root}/.runtime/pids/gateway.pid")"

        kill -KILL "${realmmesh_login_pid}"
        for realmmesh_attempt in {1..100}; do
            if ! kill -0 "${realmmesh_supervisor_pid}" 2>/dev/null &&
                ! kill -0 "${realmmesh_realm_pid}" 2>/dev/null &&
                ! kill -0 "${realmmesh_gateway_pid}" 2>/dev/null; then
                exit 0
            fi
            sleep 0.1
        done
        printf 'service group survived a Login process failure\n' >&2
        exit 1
        ;;
    stop_is_reverse_ordered)
        bash "${realmmesh_script}" start
        bash "${realmmesh_script}" stop >/dev/null
        realmmesh_supervisor_log="${realmmesh_test_root}/.runtime/logs/supervisor/console.log"
        realmmesh_gateway_line="$(grep -n '^Stopping gateway$' "${realmmesh_supervisor_log}" | tail -1 | cut -d: -f1)"
        realmmesh_login_line="$(grep -n '^Stopping login$' "${realmmesh_supervisor_log}" | tail -1 | cut -d: -f1)"
        realmmesh_realm_line="$(grep -n '^Stopping realm$' "${realmmesh_supervisor_log}" | tail -1 | cut -d: -f1)"
        [[ "${realmmesh_gateway_line}" -lt "${realmmesh_login_line}" ]]
        [[ "${realmmesh_login_line}" -lt "${realmmesh_realm_line}" ]]
        ;;
    commands_manage_service_group)
        bash "${realmmesh_script}" start
        realmmesh_status_output="$(bash "${realmmesh_script}" status)"
        grep -Eq '^manager +running' <<< "${realmmesh_status_output}"
        grep -Eq '^gateway +running' <<< "${realmmesh_status_output}"
        grep -Eq '^login +running' <<< "${realmmesh_status_output}"
        grep -Eq '^realm +running' <<< "${realmmesh_status_output}"

        realmmesh_old_supervisor="$(<"${realmmesh_test_root}/.runtime/pids/supervisor.pid")"
        bash "${realmmesh_script}" restart >/dev/null
        realmmesh_new_supervisor="$(<"${realmmesh_test_root}/.runtime/pids/supervisor.pid")"
        [[ "${realmmesh_new_supervisor}" != "${realmmesh_old_supervisor}" ]]
        bash "${realmmesh_script}" status >/dev/null

        bash "${realmmesh_script}" stop >/dev/null
        if bash "${realmmesh_script}" status >/dev/null; then
            printf 'status unexpectedly succeeded after stop\n' >&2
            exit 1
        fi
        ;;
    three_stage_flow_uses_service_group)
        [[ -x "${realmmesh_three_stage_test}" ]]
        bash "${realmmesh_script}" start
        REALMMESH_THREE_STAGE_EXTERNAL=1 \
        REALMMESH_THREE_STAGE_CONFIG_ROOT="${realmmesh_test_root}/configs" \
        REALMMESH_THREE_STAGE_LOGIN_PORT="${realmmesh_login_port}" \
        REALMMESH_THREE_STAGE_REALM_PORT="${realmmesh_realm_port}" \
        REALMMESH_THREE_STAGE_GATEWAY_PORT="${realmmesh_gateway_port}" \
            "${realmmesh_three_stage_test}" \
            --gtest_filter=ThreeStageFlowTest.LogsInSelectsACharacterAndEntersTheGateway
        bash "${realmmesh_script}" stop >/dev/null
        ;;
    *)
        printf 'Unknown test case: %s\n' "${realmmesh_case}" >&2
        exit 2
        ;;
esac
