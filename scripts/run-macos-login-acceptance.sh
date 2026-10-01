#!/usr/bin/env bash

# 用途：在 macOS 上复现完整 Login Chain 的 TLS/TCP 验收矩阵，并生成报告。
# 用法：./scripts/run-macos-login-acceptance.sh
# 可选：REALMMESH_ACCEPTANCE_REPEATS=5 ./scripts/run-macos-login-acceptance.sh

set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
repeat_count="${REALMMESH_ACCEPTANCE_REPEATS:-3}"
report_dir="${project_root}/build/dev/acceptance"
report_path="${report_dir}/macos-login-chain.md"
log_path="${report_dir}/macos-login-chain.log"

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "This acceptance profile is macOS-only (TLS/TCP fallback)." >&2
    exit 2
fi
if ! [[ "${repeat_count}" =~ ^[1-9][0-9]*$ ]]; then
    echo "REALMMESH_ACCEPTANCE_REPEATS must be a positive integer." >&2
    exit 2
fi

if command -v cmake >/dev/null 2>&1; then
    cmake_bin="$(command -v cmake)"
elif [[ -x "${project_root}/.tools/cmake/bin/cmake" ]]; then
    cmake_bin="${project_root}/.tools/cmake/bin/cmake"
else
    echo "CMake 3.20 or newer is required." >&2
    exit 1
fi
ctest_bin="$(dirname "${cmake_bin}")/ctest"

mkdir -p "${report_dir}"
: >"${log_path}"

revision="$(git -C "${project_root}" rev-parse HEAD)"
tree_state="clean"
if [[ -n "$(git -C "${project_root}" status --porcelain)" ]]; then
    tree_state="dirty"
fi
platform="$(sw_vers -productName) $(sw_vers -productVersion) ($(uname -m))"
started_at="$(date -u '+%Y-%m-%dT%H:%M:%SZ')"

{
    echo "# macOS Login Chain acceptance"
    echo
    echo "- Started: ${started_at}"
    echo "- Platform: ${platform}"
    echo "- Revision: \`${revision}\` (${tree_state} working tree)"
    echo "- Transport: TLS/TCP (repository clients carry no QUIC dialer)"
    echo "- Success-chain repetitions: ${repeat_count}"
    echo "- Raw log: \`macos-login-chain.log\`"
    echo
    echo "## Command"
    echo
    echo "\`REALMMESH_ACCEPTANCE_REPEATS=${repeat_count} ./scripts/run-macos-login-acceptance.sh\`"
    echo
    echo "## Source configuration"
    echo
    echo '```text'
    (
        cd "${project_root}"
        shasum -a 256 \
            configs/main.config \
            configs/common/player_data.lua \
            configs/services/login_verify.lua \
            configs/services/queue.lua \
            configs/services/gateway.lua \
            configs/services/realm.lua
    )
    echo '```'
    echo
    echo "## Results"
    echo
    echo "| Group | Runs | Result |"
    echo "|---|---:|---|"
} >"${report_path}"

run_logged() {
    local label="$1"
    shift
    echo "## ${label}" | tee -a "${log_path}"
    set +e
    "$@" 2>&1 | tee -a "${log_path}"
    local status="${PIPESTATUS[0]}"
    set -e
    return "${status}"
}

cd "${project_root}"
run_logged "Configure" "${cmake_bin}" --preset dev
run_logged "Build" "${cmake_bin}" --build --preset dev

# 装了 libmsquic 的 Mac 会编入 QUIC(ADR-0011),Gateway 随之多起一个 QUIC
# 监听;客户端仍只拨 TLS/TCP,报告记下监听状态以便对照。
gateway_quic_listener="not compiled in"
if grep -q 'realm_network: QUIC transport enabled' "${log_path}"; then
    gateway_quic_listener="compiled in (unused by the chain)"
fi

success_regex='^(WireLoginTransportIntegrationTest\.DrivesLoginChainOverRealTls|LoadgenIntegrationTest\.FullTargetRedeemsRealmSession)$'
external_success_regex='^DevServicesScriptTest\.FourProcessFullRepeats$'
recovery_regex='^(LoginChainTest\.AdmitTimeoutReturnsToIdle|NewChainFlowTest\.AttachesToGatewayAndEntersRealm|NewChainFlowTest\.RestartKeepsCommittedAdmissionConsumed|GatewayAdmissionTest\.TwoInstancesCommitOneIdentityAtMostOnce|GatewayAdmissionTest\.StoreOutageIsRetryableAndAffectsAvailability|GatewayAdmissionTest\.AmbiguousCommitCanNeverReleaseReservation|QueueProcessRestartTest\.RecoversAcknowledgedIssueFromAuthoritativeStore|LoadgenIntegrationTest\.GatewayReadinessRecoversAfterEtcdOutage|DevServicesScriptTest\.FourProcessFailureRecovery)$'
soak_regex='^LoadgenIntegrationTest\.L1GatewaySoakHoldsWaterLevelWithoutFdLeak$'

overall_status=0
if run_logged "Four-process Full Login Chain" \
    "${ctest_bin}" --test-dir build/dev -R "${external_success_regex}" \
    --verbose; then
    echo "| Four independent service processes + real etcd, Full chain | ${repeat_count} | PASS |" >>"${report_path}"
else
    echo "| Four independent service processes + real etcd, Full chain | ${repeat_count} | FAIL |" >>"${report_path}"
    overall_status=1
fi

if run_logged "Repeated full success" \
    "${ctest_bin}" --test-dir build/dev -R "${success_regex}" \
    --repeat "until-fail:${repeat_count}" --verbose; then
    echo "| Full Login Chain + Realm heartbeat/orderly close | ${repeat_count} | PASS |" >>"${report_path}"
else
    echo "| Full Login Chain + Realm heartbeat/orderly close | ${repeat_count} | FAIL |" >>"${report_path}"
    overall_status=1
fi

if run_logged "Failure and restart recovery" \
    "${ctest_bin}" --test-dir build/dev -R "${recovery_regex}" \
    --output-on-failure; then
    echo "| Timeout, replay, process restart, Queue recovery, etcd outage | 1 | PASS |" >>"${report_path}"
else
    echo "| Timeout, replay, process restart, Queue recovery, etcd outage | 1 | FAIL |" >>"${report_path}"
    overall_status=1
fi

if run_logged "L1 soak" \
    "${ctest_bin}" --test-dir build/dev -R "${soak_regex}" \
    --output-on-failure; then
    echo "| L1 Gateway water-level / fd soak | 1 | PASS |" >>"${report_path}"
else
    echo "| L1 Gateway water-level / fd soak | 1 | FAIL |" >>"${report_path}"
    overall_status=1
fi

phase_metrics="$(grep -E 'loadgen report|robots:|verify: attempts|tickets: attempts|poll: attempts|attach: attempts|handoff: attempts|realm: attempts' "${log_path}" || true)"
runtime_config="$(grep -E 'acceptance_runtime( |_)' "${log_path}" || true)"

{
    echo
    echo "## Resolved runtime configuration"
    echo
    if [[ -n "${runtime_config}" ]]; then
        echo '```text'
        echo "${runtime_config}"
        echo '```'
    else
        echo "No resolved runtime configuration was captured; inspect the raw log."
        overall_status=1
    fi
    echo
    echo "## Phase metrics"
    echo
    if [[ -n "${phase_metrics}" ]]; then
        echo '```text'
        echo "${phase_metrics}"
        echo '```'
    else
        echo "No Full-target phase metrics were captured; inspect the raw log."
        overall_status=1
    fi
    echo
    echo "## Transport"
    echo
    echo "- Client dial: TLS/TCP (QUIC candidates are rejected as unsupported)"
    echo "- Gateway QUIC listener: ${gateway_quic_listener}"
    echo
    echo "## Scope"
    echo
    echo "This profile starts the real Login Verifier, Queue, Gateway, Realm and a"
    echo "temporary real etcd. It exercises real HTTPS/TLS/TCP wire paths. QUIC wire paths,"
    echo "production capacity sizing and production account data remain out of scope."
} >>"${report_path}"

echo "Acceptance report: ${report_path}"
echo "Raw log: ${log_path}"
exit "${overall_status}"
