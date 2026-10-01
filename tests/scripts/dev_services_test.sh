#!/usr/bin/env bash

set -euo pipefail

realmmesh_case="${1:?test case is required}"
realmmesh_source_root="${2:?source root is required}"
realmmesh_mesh_binary="${3:?realm_mesh binary is required}"
realmmesh_tls_certificate="${4:?TLS certificate is required}"
realmmesh_tls_private_key="${5:?TLS private key is required}"
realmmesh_new_chain_test="${6:-}"
realmmesh_loadgen_binary="${7:-}"

# dev-services.sh 用 ps 认服务进程(判断"这个 pid 还是不是我的那个服务")。
# 拿不到 ps 的环境里(受限沙箱会把 /bin/ps 直接拒掉)它既判不了就绪、也停不掉
# 子进程:用例只会在超时后失败,还会漏下 supervisor/realm_mesh 孤儿。这种
# "环境不让跑"和"代码坏了"必须区分开——报 Skipped 并说明原因(ctest 侧配
# SKIP_RETURN_CODE 77),而不是留一条看起来像缺陷的假红。CI 上 ps 可用,用例
# 照常真跑。
if ! ps -o pid= -p "$$" >/dev/null 2>&1; then
    printf '%s\n' \
        'SKIP: ps is unavailable in this environment, but dev-services.sh needs it to identify service processes' >&2
    exit 77
fi

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
    # 用例自带的 etcd:必须在 pid 文件清理之前收掉,否则下一条用例会看到
    # 上一轮残留的准入消费记录(重放断言会因此误判)。
    if [[ -n "${realmmesh_etcd_pid:-}" ]]; then
        kill -CONT "${realmmesh_etcd_pid}" 2>/dev/null || true
        kill -TERM "${realmmesh_etcd_pid}" 2>/dev/null || true
        wait "${realmmesh_etcd_pid}" 2>/dev/null || true
    fi
    # 用例自带的 mongod 同理：账号与角色事实只属于这一轮。
    if [[ -n "${realmmesh_mongod_pid:-}" ]]; then
        kill -TERM "${realmmesh_mongod_pid}" 2>/dev/null || true
        wait "${realmmesh_mongod_pid}" 2>/dev/null || true
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
# 超时路径必须自己接住信号:bash 被 TERM/INT 打断时不会执行 EXIT trap,
# 用例自带的 etcd 就会变成孤儿(实测泄漏过 9 个 etcd)。
trap 'exit 130' INT
trap 'exit 143' TERM

mkdir -p "${realmmesh_test_root}/scripts/lib" \
    "${realmmesh_test_root}/build/dev/bin" \
    "${realmmesh_test_root}/configs"
cp "${realmmesh_source_root}/scripts/dev-services.sh" "${realmmesh_script}"
cp "${realmmesh_source_root}/scripts/lib/dev-process.sh" \
    "${realmmesh_test_root}/scripts/lib/dev-process.sh"
cp -R "${realmmesh_source_root}/configs/common" \
    "${realmmesh_test_root}/configs/common"
cp -R "${realmmesh_source_root}/configs/services" \
    "${realmmesh_test_root}/configs/services"
cp "${realmmesh_source_root}/configs/main.config" \
    "${realmmesh_test_root}/configs/main.config"
ln -s "${realmmesh_mesh_binary}" \
    "${realmmesh_test_root}/build/dev/bin/realm_mesh"

# 没有 setsid 的开发平台(macOS)由 realm_detach 承担新会话隔离,它在
# build/dev/bin 中与 realm_mesh 同目录。
if ! command -v setsid >/dev/null 2>&1; then
    realmmesh_detach_binary="$(dirname "${realmmesh_mesh_binary}")/realm_detach"
    if [[ ! -x "${realmmesh_detach_binary}" ]]; then
        printf 'realm_detach is required when setsid is unavailable: %s\n' \
            "${realmmesh_detach_binary}" >&2
        exit 2
    fi
    ln -s "${realmmesh_detach_binary}" \
        "${realmmesh_test_root}/build/dev/bin/realm_detach"
fi

# macOS 自带 bash 3.2,没有 mapfile;用逐行读取保持同一行为。
# 在临时端口范围(49152-65535)之外选端口:supervisor 的就绪探测用 curl
# 反复建出站连接,其源端口恰好从这个池子分配,刚释放的临时端口可能在
# 服务 bind 前被抢走,服务会以 EADDRINUSE 启动失败(CI 上偶发)。
realmmesh_ports=()
while IFS= read -r realmmesh_port; do
    realmmesh_ports+=("${realmmesh_port}")
done < <(python3 - <<'PY'
import random
import socket

chosen = []
sockets = []
while len(chosen) < 11:
    port = random.randint(20000, 30000)
    if port in chosen:
        continue
    listener = socket.socket()
    try:
        listener.bind(("127.0.0.1", port))
    except OSError:
        continue
    chosen.append(port)
    sockets.append(listener)
for port in chosen:
    print(port)
PY
)

realmmesh_realm_port="${realmmesh_ports[0]}"
realmmesh_gateway_port="${realmmesh_ports[1]}"
realmmesh_realm_metrics_port="${realmmesh_ports[2]}"
realmmesh_gateway_metrics_port="${realmmesh_ports[3]}"
realmmesh_etcd_client_port="${realmmesh_ports[4]}"
realmmesh_etcd_peer_port="${realmmesh_ports[5]}"
realmmesh_login_verify_port="${realmmesh_ports[6]}"
realmmesh_queue_port="${realmmesh_ports[7]}"
realmmesh_login_verify_metrics_port="${realmmesh_ports[8]}"
realmmesh_queue_metrics_port="${realmmesh_ports[9]}"
realmmesh_mongod_port="${realmmesh_ports[10]}"
realmmesh_etcd_endpoint="http://127.0.0.1:${realmmesh_etcd_client_port}"

# 网关的准入消费存储是线性一致存储(ADR-0009):服务组必须有**真** etcd,
# 而且必须是本用例自己的实例——挂在开发者的 2379 上会让消费记录跨用例
# 存活,重放/单次消费断言会读到上一轮的状态。二进制位置同
# scripts/run-etcd-dev.sh,缺失即以清晰信息失败而不是跳过链路。
realmmesh_etcd_binary="${REALMMESH_ETCD_BINARY:-${realmmesh_source_root}/.tools/etcd-v3.6.14/etcd}"
if [[ ! -x "${realmmesh_etcd_binary}" ]]; then
    printf 'etcd is required by the gateway admission store; run ./scripts/install-etcd.sh first (looked at %s)\n' \
        "${realmmesh_etcd_binary}" >&2
    exit 2
fi
realmmesh_etcd_data_dir="${realmmesh_scratch}/etcd-data"
mkdir -p "${realmmesh_etcd_data_dir}"
"${realmmesh_etcd_binary}" \
    --name realmmesh-dev-services-test \
    --data-dir "${realmmesh_etcd_data_dir}" \
    --listen-client-urls "${realmmesh_etcd_endpoint}" \
    --advertise-client-urls "${realmmesh_etcd_endpoint}" \
    --listen-peer-urls "http://127.0.0.1:${realmmesh_etcd_peer_port}" \
    --initial-advertise-peer-urls "http://127.0.0.1:${realmmesh_etcd_peer_port}" \
    --initial-cluster "realmmesh-dev-services-test=http://127.0.0.1:${realmmesh_etcd_peer_port}" \
    --log-level error > "${realmmesh_scratch}/etcd.log" 2>&1 &
realmmesh_etcd_pid=$!

# Player Data 的权威来源是 MongoDB 副本集(ADR-0011):同样每条用例自带一个
# 单节点 rs0,与 etcd 并行启动以省掉一段等待。二进制查找顺序见
# scripts/lib/mongodb-tools.sh(macOS 即 Homebrew 安装的 mongod/mongosh)。
realmmesh_mongodb_root="${realmmesh_source_root}"
# shellcheck source=../../scripts/lib/mongodb-tools.sh
source "${realmmesh_source_root}/scripts/lib/mongodb-tools.sh"
if ! realmmesh_mongod_bin="$(realmmesh_mongod_binary)" ||
    ! realmmesh_mongosh_bin="$(realmmesh_mongosh_binary)"; then
    realmmesh_mongodb_install_hint >&2
    exit 2
fi
realmmesh_mongod_data_dir="${realmmesh_scratch}/mongod-data"
mkdir -p "${realmmesh_mongod_data_dir}"
"${realmmesh_mongod_bin}" \
    --replSet rs0 \
    --bind_ip 127.0.0.1 \
    --port "${realmmesh_mongod_port}" \
    --dbpath "${realmmesh_mongod_data_dir}" \
    --logpath "${realmmesh_scratch}/mongod.log" \
    --nounixsocket \
    --wiredTigerCacheSizeGB 0.25 > /dev/null 2>&1 &
realmmesh_mongod_pid=$!
realmmesh_mongod_member="127.0.0.1:${realmmesh_mongod_port}"
# 客户端端口可连 ≠ etcd 已能服务:选举窗口内它会直接关闭连接(实测
# "Failed to read connection"),而网关的就绪探测正好会踩进去。用一次真实
# 写入当就绪判据,而不是靠 sleep 猜——写入的 key 与用例无关,只证明
# linearizable 写路径已可用。
realmmesh_etcd_probe_body="$(python3 - <<'PY'
import base64
import json

print(json.dumps({
    "key": base64.b64encode(b"/realmmesh/test/readiness").decode(),
    "value": base64.b64encode(b"1").decode(),
}))
PY
)"
realmmesh_etcd_ready=0
# 预算收紧到秒级:etcd 正常时 1~2 秒内即可写;真起不来就尽快失败,不要
# 把 ctest 的 20s 用例预算耗在重试上。
for _ in {1..20}; do
    if curl --silent --fail --connect-timeout 0.3 --max-time 0.5 \
        --header 'Content-Type: application/json' \
        --request POST --data "${realmmesh_etcd_probe_body}" \
        "${realmmesh_etcd_endpoint}/v3/kv/put" > /dev/null; then
        realmmesh_etcd_ready=1
        break
    fi
    sleep 0.25
done
if [[ "${realmmesh_etcd_ready}" -ne 1 ]]; then
    printf 'etcd did not accept writes; see %s\n' \
        "${realmmesh_scratch}/etcd.log" >&2
    exit 1
fi
if ! "${realmmesh_source_root}/scripts/mongodb-init-replset.sh" \
    "${realmmesh_mongosh_bin}" "${realmmesh_mongod_member}"; then
    printf 'mongod did not become the rs0 primary; see %s\n' \
        "${realmmesh_scratch}/mongod.log" >&2
    exit 1
fi

# 回读用例 mongod 里的 Player Data,用 print(...) 输出需要比对的值。
player_data_eval() {
    "${realmmesh_mongosh_bin}" --quiet --norc \
        "mongodb://${realmmesh_mongod_member}/realmmesh?replicaSet=rs0" \
        --eval "$1"
}

etcd_range_body() {
    python3 - "$1" <<'PY'
import base64
import json
import sys

print(json.dumps({"key": base64.b64encode(sys.argv[1].encode()).decode()}))
PY
}
realmmesh_gateway_budget_range_body="$(etcd_range_body \
    /realmmesh/budgets/service/gateway/gateway-dev-01/budget)"
realmmesh_realm_budget_range_body="$(etcd_range_body \
    /realmmesh/budgets/service/realm/realm-dev-01/budget)"
realmmesh_queue_registration_range_body="$(etcd_range_body \
    /realmmesh/services/queue/queue-dev-01)"
realmmesh_login_verify_registration_range_body="$(etcd_range_body \
    /realmmesh/services/login_verify/login-verify-dev-01)"

# BSD sed 的 -i 需要一个后缀参数,多段 -e 会被当成文件名("sed: -e: No such
# file or directory")。用重定向 + mv 重写配置,在 GNU/BSD sed 上行为一致。
rewrite_config() {
    local realmmesh_config_file="$1"
    shift
    sed "$@" "${realmmesh_config_file}" > "${realmmesh_config_file}.tmp"
    mv "${realmmesh_config_file}.tmp" "${realmmesh_config_file}"
}

rewrite_config "${realmmesh_test_root}/configs/services/realm.lua" \
    -e "s/listen_port = 7100/listen_port = ${realmmesh_realm_port}/" \
    -e "s/downstream_port = 8000/downstream_port = ${realmmesh_gateway_port}/" \
    -e "s/metrics_port = 9102/metrics_port = ${realmmesh_realm_metrics_port}/"
# gateway 的静态兜底下游就是 realm:必须一起改写,否则 handoff 签发的
# 端点会指向配置里的固定 7100。
rewrite_config "${realmmesh_test_root}/configs/services/gateway.lua" \
    -e "s/listen_port = 8000/listen_port = ${realmmesh_gateway_port}/g" \
    -e "s/downstream_port = 7100/downstream_port = ${realmmesh_realm_port}/" \
    -e "s/metrics_port = 9103/metrics_port = ${realmmesh_gateway_metrics_port}/"
rewrite_config "${realmmesh_test_root}/configs/services/login_verify.lua" \
    -e "s/listen_port = 0/listen_port = ${realmmesh_login_verify_port}/" \
    -e "s/metrics_port = 9104/metrics_port = ${realmmesh_login_verify_metrics_port}/"
rewrite_config "${realmmesh_test_root}/configs/services/queue.lua" \
    -e "s/listen_port = 0/listen_port = ${realmmesh_queue_port}/" \
    -e "s/metrics_port = 9105/metrics_port = ${realmmesh_queue_metrics_port}/"

# 服务发现的 endpoint 由 common 层提供,gateway 与 realm 共用;网关的
# 准入消费存储正是拿 discovery_config_.endpoint 建 etcd 客户端,所以
# 这里必须换成用例自己的 etcd,而不是 127.0.0.1:2379。
rewrite_config "${realmmesh_test_root}/configs/common/discovery.lua" \
    -e "s|endpoint = \"http://127.0.0.1:2379\"|endpoint = \"${realmmesh_etcd_endpoint}\"|"
if [[ "${realmmesh_case}" == four_process_* ]]; then
    rewrite_config "${realmmesh_test_root}/configs/common/discovery.lua" \
        -e "s/enabled = false/enabled = true/"
fi
# Queue 的 etcd 存取(额度/快照/放行账本)走自己的配置键。
rewrite_config "${realmmesh_test_root}/configs/services/queue.lua" \
    -e "s|etcd_endpoint = \"http://127.0.0.1:2379\"|etcd_endpoint = \"${realmmesh_etcd_endpoint}\"|"
# sed 不匹配时静默成功;显式核对,避免"以为指到用例 etcd、其实还在用
# 开发者 2379"这类看不出错的错误。
grep -q "endpoint = \"${realmmesh_etcd_endpoint}\"" \
    "${realmmesh_test_root}/configs/common/discovery.lua" || {
    printf 'failed to point service discovery at the test etcd\n' >&2
    exit 1
}
grep -q "etcd_endpoint = \"${realmmesh_etcd_endpoint}\"" \
    "${realmmesh_test_root}/configs/services/queue.lua" || {
    printf 'failed to point the queue store at the test etcd\n' >&2
    exit 1
}
# 三个服务共享的 Player Data 也换成用例自己的 mongod。
rewrite_config "${realmmesh_test_root}/configs/common/player_data.lua" \
    -e "s|mongodb://127.0.0.1:27017/|mongodb://${realmmesh_mongod_member}/|"
grep -q "uri = \"mongodb://${realmmesh_mongod_member}/?replicaSet=rs0\"" \
    "${realmmesh_test_root}/configs/common/player_data.lua" || {
    printf 'failed to point player data at the test mongod\n' >&2
    exit 1
}

export REALMMESH_TLS_CERTIFICATE_FILE="${realmmesh_tls_certificate}"
export REALMMESH_TLS_PRIVATE_KEY_FILE="${realmmesh_tls_private_key}"
export REALMMESH_SESSION_TICKET_KEY="0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"
# 新链端到端用例要在测试内直接签身份 Token 与 Admission Grant:种子/公钥
# 必须与各服务验签用的同一份,经环境传进服务组与用例进程。凭据角色不共材
# (Queue 侧 validate() 会拒绝共享签名密钥),所以三个种子互不相同。
export REALMMESH_IDENTITY_KEY_SEED="9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"
export REALMMESH_QUEUE_NUMBER_KEY_SEED="4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb"
export REALMMESH_ADMISSION_GRANT_KEY_SEED="0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
# 上行种子配对的 Ed25519 公钥,网关键环按 kid 索引它验签。
export REALMMESH_ADMISSION_GRANT_PUBLIC_KEY="207a067892821e25d770f1fba0c47c11ff4b813e54162ece9eb839e076231ab6"
# 准入消费记录的摘要键(非可逆):每次用例自己的 etcd 从空开始。
export REALMMESH_ADMISSION_CONSUMPTION_DIGEST_KEY="fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210"
export REALMMESH_REALM_METRICS_URL="http://127.0.0.1:${realmmesh_realm_metrics_port}/metrics"
export REALMMESH_GATEWAY_METRICS_URL="http://127.0.0.1:${realmmesh_gateway_metrics_port}/metrics"

write_acceptance_account() {
    printf '%s\n' \
        'return {' \
        '    accounts = {' \
        '        { account = "robot-0", credential = "loadgen-credential", whitelisted = true },' \
        '    },' \
        '}' > "${realmmesh_test_root}/configs/common/accounts.lua"
}

standalone_pid_file() {
    printf '%s\n' "${realmmesh_test_root}/.runtime/pids/$1.pid"
}

start_standalone_service() {
    local realmmesh_service="$1"
    local realmmesh_pid_file
    realmmesh_pid_file="$(standalone_pid_file "${realmmesh_service}")"
    local realmmesh_log_dir="${realmmesh_test_root}/.runtime/logs/${realmmesh_service}"
    mkdir -p "$(dirname "${realmmesh_pid_file}")" "${realmmesh_log_dir}"
    (
        cd "${realmmesh_test_root}"
        exec "${realmmesh_mesh_binary}" \
            --config "${realmmesh_test_root}/configs" \
            --service "${realmmesh_service}"
    ) >> "${realmmesh_log_dir}/console.log" 2>&1 &
    printf '%s\n' "$!" > "${realmmesh_pid_file}"
}

stop_standalone_service() {
    local realmmesh_service="$1"
    local realmmesh_pid_file
    realmmesh_pid_file="$(standalone_pid_file "${realmmesh_service}")"
    [[ -f "${realmmesh_pid_file}" ]] || return 0
    local realmmesh_pid
    realmmesh_pid="$(<"${realmmesh_pid_file}")"
    kill -TERM "${realmmesh_pid}" 2>/dev/null || true
    (
        sleep 5
        kill -KILL "${realmmesh_pid}" 2>/dev/null || true
    ) &
    local realmmesh_shutdown_watchdog=$!
    wait "${realmmesh_pid}" 2>/dev/null || true
    kill -TERM "${realmmesh_shutdown_watchdog}" 2>/dev/null || true
    wait "${realmmesh_shutdown_watchdog}" 2>/dev/null || true
    rm -f -- "${realmmesh_pid_file}"
}

crash_standalone_service() {
    local realmmesh_service="$1"
    local realmmesh_pid_file
    realmmesh_pid_file="$(standalone_pid_file "${realmmesh_service}")"
    [[ -f "${realmmesh_pid_file}" ]]
    local realmmesh_pid
    realmmesh_pid="$(<"${realmmesh_pid_file}")"
    kill -KILL "${realmmesh_pid}"
    wait "${realmmesh_pid}" 2>/dev/null || true
    rm -f -- "${realmmesh_pid_file}"
}

service_metrics_port() {
    case "$1" in
        login_verify) printf '%s\n' "${realmmesh_login_verify_metrics_port}" ;;
        queue) printf '%s\n' "${realmmesh_queue_metrics_port}" ;;
        realm) printf '%s\n' "${realmmesh_realm_metrics_port}" ;;
        gateway) printf '%s\n' "${realmmesh_gateway_metrics_port}" ;;
    esac
}

wait_for_standalone_ready() {
    local realmmesh_service="$1"
    local realmmesh_metrics_port
    realmmesh_metrics_port="$(service_metrics_port "${realmmesh_service}")"
    local realmmesh_attempt
    for realmmesh_attempt in {1..150}; do
        if curl --silent --fail --connect-timeout 0.1 --max-time 0.2 \
            "http://127.0.0.1:${realmmesh_metrics_port}/metrics" 2>/dev/null |
            grep -Eq \
                "^realmmesh_service_ready\\{service_name=\"${realmmesh_service}\",service_instance=\"[^\"]+\"\\} 1$"; then
            return 0
        fi
        sleep 0.1
    done
    printf '%s did not become ready\n' "${realmmesh_service}" >&2
    return 1
}

wait_for_standalone_unready() {
    local realmmesh_service="$1"
    local realmmesh_metrics_port
    realmmesh_metrics_port="$(service_metrics_port "${realmmesh_service}")"
    local realmmesh_attempt
    for realmmesh_attempt in {1..100}; do
        if ! curl --silent --fail --connect-timeout 0.1 --max-time 0.2 \
            "http://127.0.0.1:${realmmesh_metrics_port}/metrics" 2>/dev/null |
            grep -Eq \
                "^realmmesh_service_ready\\{service_name=\"${realmmesh_service}\",service_instance=\"[^\"]+\"\\} 1$"; then
            return 0
        fi
        sleep 0.1
    done
    printf '%s stayed ready after dependency outage\n' \
        "${realmmesh_service}" >&2
    return 1
}

wait_for_etcd_key_state() {
    local realmmesh_range_body="$1"
    local realmmesh_expected_state="$2"
    local realmmesh_attempt
    for realmmesh_attempt in {1..100}; do
        local realmmesh_range_response
        if realmmesh_range_response="$(
            curl --silent --fail --connect-timeout 0.1 --max-time 0.2 \
                --header 'Content-Type: application/json' \
                --request POST --data "${realmmesh_range_body}" \
                "${realmmesh_etcd_endpoint}/v3/kv/range" 2>/dev/null
        )"; then
            local realmmesh_key_state="absent"
            if grep -Eq '"count":"?[1-9][0-9]*"?' \
                <<<"${realmmesh_range_response}"; then
                realmmesh_key_state="present"
            fi
            if [[ "${realmmesh_key_state}" == "${realmmesh_expected_state}" ]]; then
                return 0
            fi
        fi
        sleep 0.1
    done
    printf 'etcd key did not become %s\n' "${realmmesh_expected_state}" >&2
    return 1
}

queue_released_number() {
    curl --silent --fail --connect-timeout 0.1 --max-time 0.2 \
        "http://127.0.0.1:${realmmesh_queue_metrics_port}/metrics" |
        awk '$1 == "released_number" { print int($2); found = 1 }
             END { if (!found) exit 1 }'
}

start_four_services() {
    write_acceptance_account
    local realmmesh_service
    for realmmesh_service in login_verify queue realm gateway; do
        start_standalone_service "${realmmesh_service}"
    done
    for realmmesh_service in login_verify realm gateway queue; do
        wait_for_standalone_ready "${realmmesh_service}"
    done
}

run_full_login() {
    "${realmmesh_loadgen_binary}" \
        --phase full \
        --robots 1 \
        --concurrency 1 \
        --duration 8 \
        --poll-interval-ms 50 \
        --login-verify "127.0.0.1:${realmmesh_login_verify_port}" \
        --queue "127.0.0.1:${realmmesh_queue_port}" \
        --gateway "127.0.0.1:${realmmesh_gateway_port}" \
        --account-prefix robot \
        --credential loadgen-credential
}

expect_full_login_failure() {
    if run_full_login; then
        printf 'Full Login Chain unexpectedly succeeded during injected failure\n' >&2
        return 1
    fi
}

print_acceptance_runtime() {
    printf 'acceptance_runtime login_verify=127.0.0.1:%s queue=127.0.0.1:%s gateway=127.0.0.1:%s realm=127.0.0.1:%s etcd=%s mongodb=%s\n' \
        "${realmmesh_login_verify_port}" \
        "${realmmesh_queue_port}" \
        "${realmmesh_gateway_port}" \
        "${realmmesh_realm_port}" \
        "${realmmesh_etcd_endpoint}" \
        "${realmmesh_mongod_member}"
    if command -v shasum >/dev/null 2>&1; then
        (cd "${realmmesh_test_root}" && shasum -a 256 \
            configs/main.config configs/common/discovery.lua \
            configs/common/accounts.lua configs/common/player_data.lua \
            configs/services/login_verify.lua \
            configs/services/queue.lua configs/services/gateway.lua \
            configs/services/realm.lua) |
            sed 's/^/acceptance_runtime_config /'
    else
        (cd "${realmmesh_test_root}" && sha256sum \
            configs/main.config configs/common/discovery.lua \
            configs/common/accounts.lua configs/common/player_data.lua \
            configs/services/login_verify.lua \
            configs/services/queue.lua configs/services/gateway.lua \
            configs/services/realm.lua) |
            sed 's/^/acceptance_runtime_config /'
    fi
}

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
        [[ ! -f "${realmmesh_test_root}/.runtime/pids/gateway.pid" ]]
        ;;
    child_failure_stops_group)
        bash "${realmmesh_script}" start
        realmmesh_supervisor_pid="$(<"${realmmesh_test_root}/.runtime/pids/supervisor.pid")"
        realmmesh_realm_pid="$(<"${realmmesh_test_root}/.runtime/pids/realm.pid")"
        realmmesh_gateway_pid="$(<"${realmmesh_test_root}/.runtime/pids/gateway.pid")"

        kill -KILL "${realmmesh_realm_pid}"
        for realmmesh_attempt in {1..100}; do
            if ! kill -0 "${realmmesh_supervisor_pid}" 2>/dev/null &&
                ! kill -0 "${realmmesh_realm_pid}" 2>/dev/null &&
                ! kill -0 "${realmmesh_gateway_pid}" 2>/dev/null; then
                exit 0
            fi
            sleep 0.1
        done
        printf 'service group survived a Realm process failure\n' >&2
        exit 1
        ;;
    stop_is_reverse_ordered)
        bash "${realmmesh_script}" start
        bash "${realmmesh_script}" stop >/dev/null
        realmmesh_supervisor_log="${realmmesh_test_root}/.runtime/logs/supervisor/console.log"
        realmmesh_gateway_line="$(grep -n '^Stopping gateway$' "${realmmesh_supervisor_log}" | tail -1 | cut -d: -f1)"
        realmmesh_realm_line="$(grep -n '^Stopping realm$' "${realmmesh_supervisor_log}" | tail -1 | cut -d: -f1)"
        [[ "${realmmesh_gateway_line}" -lt "${realmmesh_realm_line}" ]]
        ;;
    commands_manage_service_group)
        bash "${realmmesh_script}" start
        realmmesh_status_output="$(bash "${realmmesh_script}" status)"
        grep -Eq '^manager +running' <<< "${realmmesh_status_output}"
        grep -Eq '^gateway +running' <<< "${realmmesh_status_output}"
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
    new_chain_flow_uses_service_group)
        [[ -x "${realmmesh_new_chain_test}" ]]
        bash "${realmmesh_script}" start
        REALMMESH_NEW_CHAIN_EXTERNAL=1 \
        REALMMESH_NEW_CHAIN_CONFIG_ROOT="${realmmesh_test_root}/configs" \
        REALMMESH_NEW_CHAIN_REALM_PORT="${realmmesh_realm_port}" \
        REALMMESH_NEW_CHAIN_GATEWAY_PORT="${realmmesh_gateway_port}" \
            "${realmmesh_new_chain_test}" \
            --gtest_filter=NewChainFlowTest.AttachesToGatewayAndEntersRealm
        bash "${realmmesh_script}" stop >/dev/null
        ;;
    four_process_full_repeats)
        [[ -x "${realmmesh_loadgen_binary}" ]]
        start_four_services
        print_acceptance_runtime
        realmmesh_repeat_count="${REALMMESH_ACCEPTANCE_REPEATS:-3}"
        [[ "${realmmesh_repeat_count}" =~ ^[1-9][0-9]*$ ]]
        for ((realmmesh_repeat = 0;
             realmmesh_repeat < realmmesh_repeat_count;
             ++realmmesh_repeat)); do
            run_full_login
        done
        ;;
    four_process_failure_recovery)
        [[ -x "${realmmesh_loadgen_binary}" ]]
        start_four_services

        wait_for_etcd_key_state \
            "${realmmesh_gateway_budget_range_body}" present
        realmmesh_recovery_started="${SECONDS}"
        crash_standalone_service gateway
        wait_for_etcd_key_state \
            "${realmmesh_gateway_budget_range_body}" absent
        realmmesh_recovery_elapsed="$((SECONDS - realmmesh_recovery_started))"
        [[ "${realmmesh_recovery_elapsed}" -le 10 ]]
        printf 'acceptance_fault gateway_budget_removal elapsed_s=%s threshold_s=10 result=PASS\n' \
            "${realmmesh_recovery_elapsed}"
        expect_full_login_failure
        realmmesh_recovery_started="${SECONDS}"
        start_standalone_service gateway
        wait_for_standalone_ready gateway
        wait_for_etcd_key_state \
            "${realmmesh_gateway_budget_range_body}" present
        run_full_login
        realmmesh_recovery_elapsed="$((SECONDS - realmmesh_recovery_started))"
        [[ "${realmmesh_recovery_elapsed}" -le 10 ]]
        printf 'acceptance_fault gateway_budget_restore elapsed_s=%s threshold_s=10 result=PASS\n' \
            "${realmmesh_recovery_elapsed}"

        wait_for_etcd_key_state \
            "${realmmesh_realm_budget_range_body}" present
        realmmesh_recovery_started="${SECONDS}"
        stop_standalone_service realm
        wait_for_etcd_key_state \
            "${realmmesh_realm_budget_range_body}" absent
        realmmesh_recovery_elapsed="$((SECONDS - realmmesh_recovery_started))"
        [[ "${realmmesh_recovery_elapsed}" -le 10 ]]
        printf 'acceptance_fault realm_budget_removal elapsed_s=%s threshold_s=10 result=PASS\n' \
            "${realmmesh_recovery_elapsed}"
        expect_full_login_failure
        realmmesh_recovery_started="${SECONDS}"
        start_standalone_service realm
        wait_for_standalone_ready realm
        wait_for_etcd_key_state \
            "${realmmesh_realm_budget_range_body}" present
        wait_for_standalone_ready gateway
        run_full_login
        realmmesh_recovery_elapsed="$((SECONDS - realmmesh_recovery_started))"
        [[ "${realmmesh_recovery_elapsed}" -le 10 ]]
        printf 'acceptance_fault realm_budget_restore elapsed_s=%s threshold_s=10 result=PASS\n' \
            "${realmmesh_recovery_elapsed}"

        # Gateway、Realm 已各自冷启动过；再重启 Login Verifier，证明同一
        # 账号在三个进程都重新连上 MongoDB Player Data 后仍走通全链路。
        crash_standalone_service login_verify
        wait_for_etcd_key_state \
            "${realmmesh_login_verify_registration_range_body}" absent
        start_standalone_service login_verify
        wait_for_standalone_ready login_verify
        run_full_login
        [[ "$(player_data_eval \
            'print(db.accounts.countDocuments({account_name: "robot-0"}))')" == 1 ]]
        printf 'acceptance_data player_data_restart source=mongodb database=realmmesh restarted=gateway,realm,login_verify result=PASS\n'

        realmmesh_released_before="$(queue_released_number)"
        [[ "${realmmesh_released_before}" =~ ^[0-9]+$ ]]
        [[ "${realmmesh_released_before}" -gt 0 ]]
        realmmesh_recovery_started="${SECONDS}"
        crash_standalone_service queue
        wait_for_etcd_key_state \
            "${realmmesh_queue_registration_range_body}" absent
        start_standalone_service queue
        wait_for_standalone_ready queue
        realmmesh_released_after="$(queue_released_number)"
        [[ "${realmmesh_released_after}" =~ ^[0-9]+$ ]]
        [[ "${realmmesh_released_after}" -ge "${realmmesh_released_before}" ]]
        printf 'acceptance_fault queue_released_number before=%s after=%s result=PASS\n' \
            "${realmmesh_released_before}" "${realmmesh_released_after}"
        run_full_login
        realmmesh_recovery_elapsed="$((SECONDS - realmmesh_recovery_started))"
        [[ "${realmmesh_recovery_elapsed}" -le 30 ]]
        printf 'acceptance_fault queue_cold_restart elapsed_s=%s threshold_s=30 result=PASS\n' \
            "${realmmesh_recovery_elapsed}"

        realmmesh_recovery_started="${SECONDS}"
        kill -STOP "${realmmesh_etcd_pid}"
        wait_for_standalone_unready gateway
        realmmesh_recovery_elapsed="$((SECONDS - realmmesh_recovery_started))"
        [[ "${realmmesh_recovery_elapsed}" -le 10 ]]
        printf 'acceptance_fault etcd_outage_unready elapsed_s=%s threshold_s=10 result=PASS\n' \
            "${realmmesh_recovery_elapsed}"
        expect_full_login_failure
        realmmesh_recovery_started="${SECONDS}"
        kill -CONT "${realmmesh_etcd_pid}"
        realmmesh_etcd_ready=0
        for _ in {1..40}; do
            if curl --silent --fail --connect-timeout 0.3 --max-time 0.5 \
                --header 'Content-Type: application/json' \
                --request POST --data "${realmmesh_etcd_probe_body}" \
                "${realmmesh_etcd_endpoint}/v3/kv/put" >/dev/null; then
                realmmesh_etcd_ready=1
                break
            fi
            sleep 0.25
        done
        [[ "${realmmesh_etcd_ready}" -eq 1 ]]
        wait_for_standalone_ready gateway
        realmmesh_recovery_elapsed="$((SECONDS - realmmesh_recovery_started))"
        [[ "${realmmesh_recovery_elapsed}" -le 10 ]]
        printf 'acceptance_fault etcd_recovery elapsed_s=%s threshold_s=10 result=PASS\n' \
            "${realmmesh_recovery_elapsed}"
        # outage 中撞上不确定 etcd 写入的 Queue 会按 #89 fail-closed；重启后
        # 从权威快照恢复，才允许再次发号。
        realmmesh_recovery_started="${SECONDS}"
        stop_standalone_service queue
        start_standalone_service queue
        wait_for_standalone_ready queue
        run_full_login
        realmmesh_recovery_elapsed="$((SECONDS - realmmesh_recovery_started))"
        [[ "${realmmesh_recovery_elapsed}" -le 30 ]]
        printf 'acceptance_fault queue_authoritative_recovery elapsed_s=%s threshold_s=30 result=PASS\n' \
            "${realmmesh_recovery_elapsed}"
        ;;
    *)
        printf 'Unknown test case: %s\n' "${realmmesh_case}" >&2
        exit 2
        ;;
esac
