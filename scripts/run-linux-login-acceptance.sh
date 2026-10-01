#!/usr/bin/env bash

# 用途：在 Linux 生产基准上复现 Login Chain 的 QUIC/TLS 与 M1–M4
# 缩减验收矩阵，并生成可上传的 Markdown 报告和原始日志。
# 用法：./scripts/run-linux-login-acceptance.sh
# 可选：REALMMESH_ACCEPTANCE_REPEATS=5 ./scripts/run-linux-login-acceptance.sh
# CI 已构建时：REALMMESH_LINUX_ACCEPTANCE_SKIP_BUILD=1 ./scripts/run-linux-login-acceptance.sh

set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
repeat_count="${REALMMESH_ACCEPTANCE_REPEATS:-3}"
skip_build="${REALMMESH_LINUX_ACCEPTANCE_SKIP_BUILD:-0}"
report_dir="${project_root}/build/dev/acceptance"
report_path="${report_dir}/linux-login-chain-m1-m4.md"
log_path="${report_dir}/linux-login-chain-m1-m4.log"

print_plan() {
    cat <<'EOF'
Linux Login Chain M1-M4 acceptance plan (CI-scale)
Transport: real QUIC + verified TLS/TCP fallback
M1: concurrent multi-stage water, attach P99 drift, and fd soak
M2: 250 robots, >=248 completed, fetch failures <1%
M3: 10000 durable tickets + 2000 progress clients, >=99.9%
M4: budget convergence <=10s, cold Queue <=30s, released number monotonic
Production targets are reported but not claimed by the CI profile.
EOF
}

if [[ "${1:-}" == "--print-plan" ]]; then
    print_plan
    exit 0
fi
if [[ "${1:-}" == "--help" ]]; then
    cat <<'EOF'
Usage: ./scripts/run-linux-login-acceptance.sh [--print-plan]

Environment:
  REALMMESH_ACCEPTANCE_REPEATS=N          repeat transport/M1/M2 groups
  REALMMESH_LINUX_ACCEPTANCE_SKIP_BUILD=1 reuse build/dev
EOF
    exit 0
fi
if [[ $# -ne 0 ]]; then
    echo "Unknown argument: $1" >&2
    exit 2
fi
if [[ "$(uname -s)" != "Linux" ]]; then
    echo "This acceptance profile is Linux-only (QUIC + TLS/TCP baseline)." >&2
    exit 2
fi
if ! [[ "${repeat_count}" =~ ^[1-9][0-9]*$ ]]; then
    echo "REALMMESH_ACCEPTANCE_REPEATS must be a positive integer." >&2
    exit 2
fi
if [[ "${skip_build}" != "0" && "${skip_build}" != "1" ]]; then
    echo "REALMMESH_LINUX_ACCEPTANCE_SKIP_BUILD must be 0 or 1." >&2
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
linux_name="Linux"
if [[ -r /etc/os-release ]]; then
    # PRETTY_NAME is distribution-owned text, used only as report metadata.
    linux_name="$(sed -n 's/^PRETTY_NAME=//p' /etc/os-release | head -1 | tr -d '"')"
fi
platform="${linux_name}; kernel $(uname -r); $(uname -m)"
cpu_count="$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc)"
cpu_model="$(awk -F ': ' '/^model name/ {print $2; exit}' /proc/cpuinfo)"
memory_kib="$(awk '/^MemTotal:/ {print $2}' /proc/meminfo)"
fd_limit="$(ulimit -n)"
started_at="$(date -u '+%Y-%m-%dT%H:%M:%SZ')"

{
    echo "# Linux Login Chain M1–M4 acceptance"
    echo
    echo "- Started: ${started_at}"
    echo "- Platform: ${platform}"
    echo "- CPU: ${cpu_count} logical; ${cpu_model:-unknown}"
    echo "- Memory: ${memory_kib:-unknown} KiB"
    echo "- File-descriptor limit: ${fd_limit}"
    echo "- Revision: \`${revision}\` (${tree_state} working tree)"
    echo "- Topology: one Linux host; production service/loadgen paths in the dev-preset CI build; four independent service processes where required; temporary real etcd"
    echo "- Transport: real MsQuic server/client exchange plus production Login Chain TLS/TCP fallback"
    echo "- Repetitions: transport/M1/M2=${repeat_count}; M3/M4=1"
    echo "- Raw log: \`linux-login-chain-m1-m4.log\`"
    echo
    echo "## Command"
    echo
    echo "\`REALMMESH_ACCEPTANCE_REPEATS=${repeat_count} REALMMESH_LINUX_ACCEPTANCE_SKIP_BUILD=${skip_build} ./scripts/run-linux-login-acceptance.sh\`"
    echo
    echo "## Acceptance levels"
    echo
    echo "| Level | CI-scale input and gate | Production target from #32 |"
    echo "|---|---|---|"
    echo "| Transport | real QUIC verified stream; TLS/TCP staged fallback; certificate failure must not fall back | both Gateway and Realm candidate paths |"
    echo "| M1 | 100 Gateway sessions for 5s; concurrent multi-stage water; attach P99 <=5x same-load baseline; no fd growth | 100k mixed sessions for 30 min; <=3 GB; no P99 drift/fd/OOM |"
    echo "| M2 | 250 robots/64 concurrency/25s; >=248 complete; fetch failures <1% | 100k robots handed off in 60s |"
    echo "| M3 | 10k durable tickets + 2k progress clients; >=99.9% success | 1m tickets; origin <=5k QPS; cache hit >=99% |"
    echo "| M4 | Gateway/Realm budget-key convergence <=10s; Queue cold recovery <=30s; released number monotonic; no Grant replay | same gates on staged topology |"
    echo
    echo "> The CI profile is a deterministic regression baseline. It reports, but does not claim, the dedicated-host 100k/1m capacity targets. M5 remains out of scope."
    echo
    echo "## Source configuration"
    echo
    echo '```text'
    (
        cd "${project_root}"
        sha256sum \
            configs/main.config \
            configs/common/discovery.lua \
            configs/common/player_data.lua \
            configs/services/login_verify.lua \
            configs/services/queue.lua \
            configs/services/gateway.lua \
            configs/services/realm.lua \
            .github/workflows/ci.yml
    )
    echo '```'
    echo
    echo "## Results"
    echo
    echo "| Group | Runs | Elapsed | Result |"
    echo "|---|---:|---:|---|"
} >"${report_path}"

last_elapsed=0
run_logged() {
    local label="$1"
    shift
    local started
    started="$(date +%s)"
    echo "## ${label}" | tee -a "${log_path}"
    set +e
    "$@" 2>&1 | tee -a "${log_path}"
    local status="${PIPESTATUS[0]}"
    set -e
    last_elapsed="$(( $(date +%s) - started ))"
    return "${status}"
}

run_group() {
    local label="$1"
    local runs="$2"
    local regex="$3"
    local -a repeat_args=()
    if [[ "${runs}" -gt 1 ]]; then
        repeat_args=(--repeat "until-fail:${runs}")
    fi
    if run_logged "${label}" \
        env REALMMESH_ACCEPTANCE_REPEATS=1 \
        "${ctest_bin}" --test-dir build/dev -R "${regex}" \
        "${repeat_args[@]}" --verbose; then
        echo "| ${label} | ${runs} | ${last_elapsed}s | PASS |" >>"${report_path}"
        return 0
    fi
    echo "| ${label} | ${runs} | ${last_elapsed}s | FAIL |" >>"${report_path}"
    return 1
}

cd "${project_root}"
if [[ "${skip_build}" == "0" ]]; then
    run_logged "Configure" "${cmake_bin}" --preset dev
    run_logged "Build" "${cmake_bin}" --build --preset dev
elif [[ ! -x build/dev/bin/realm_mesh_loadgen ]]; then
    echo "REALMMESH_LINUX_ACCEPTANCE_SKIP_BUILD=1 requires build/dev." >&2
    exit 1
fi

transport_tests=(
    QuicTransportTest.ExchangesAFramedMessageOverOneVerifiedStream
    TlsTcpClientDialerTest.QuicCandidateIsUnsupportedAndFallbackPermitted
    PreferredTransportConnectorTest.StartsTlsAfterDelayAndFirstSecureWins
    PreferredTransportConnectorTest.DoesNotFallbackOnCertificateFailure
    WireLoginTransportIntegrationTest.DrivesLoginChainOverRealTls
    DevServicesScriptTest.FourProcessFullRepeats
)
m1_tests=(
    LoadgenIntegrationTest.L1VerifyDirectsTrafficAndCountersAgree
    LoadgenIntegrationTest.L1TicketsIssueTokensAllValidate
    LoadgenIntegrationTest.L1GatewaySoakHoldsWaterLevelWithoutFdLeak
    LoadgenIntegrationTest.FullTargetRedeemsRealmSession
)
m2_tests=(
    LoadgenIntegrationTest.M2ReducedChainCompletesWithLowFetchFailure
    QueueStoreEtcdIntegrationTest.MeasuresIssuanceWriteLoadAgainstTarget
)
m3_tests=(
    LoadgenIntegrationTest.M3SmokeTenThousandTicketsAndConcurrentPolls
)
m4_tests=(
    DevServicesScriptTest.FourProcessFailureRecovery
    QueueProcessRestartTest.RecoversAcknowledgedIssueFromAuthoritativeStore
    QueueServiceTest.RestartRecoversAcknowledgedIssueAndReleasePosition
    NewChainFlowTest.RestartKeepsCommittedAdmissionConsumed
    GatewayAdmissionTest.TwoInstancesCommitOneIdentityAtMostOnce
    GatewayAdmissionTest.StoreOutageIsRetryableAndAffectsAvailability
    GatewayAdmissionTest.AmbiguousCommitCanNeverReleaseReservation
    LoadgenIntegrationTest.GatewayReadinessRecoversAfterEtcdOutage
)

test_regex() {
    local regex=""
    local test_name
    for test_name in "$@"; do
        test_name="${test_name//./\\.}"
        regex="${regex:+${regex}|}${test_name}"
    done
    printf '^(%s)$' "${regex}"
}

registered_tests="$("${ctest_bin}" --test-dir build/dev -N |
    sed -n 's/^ *Test *#[0-9][0-9]*: //p')"
for required_test in \
    "${transport_tests[@]}" "${m1_tests[@]}" "${m2_tests[@]}" \
    "${m3_tests[@]}" "${m4_tests[@]}"; do
    if ! grep -Fxq "${required_test}" <<<"${registered_tests}"; then
        echo "Required acceptance test is not registered: ${required_test}" |
            tee -a "${log_path}" >&2
        exit 1
    fi
done

transport_regex="$(test_regex "${transport_tests[@]}")"
m1_regex="$(test_regex "${m1_tests[@]}")"
m2_regex="$(test_regex "${m2_tests[@]}")"
m3_regex="$(test_regex "${m3_tests[@]}")"
m4_regex="$(test_regex "${m4_tests[@]}")"

overall_status=0
run_group "Transport and topology" "${repeat_count}" "${transport_regex}" || overall_status=1
run_group "M1 directed soak" "${repeat_count}" "${m1_regex}" || overall_status=1
run_group "M2 admission and durable issuance" "${repeat_count}" "${m2_regex}" || overall_status=1
run_group "M3 ticket and progress load" 1 "${m3_regex}" || overall_status=1
run_group "M4 failure and recovery" 1 "${m4_regex}" || overall_status=1

measured_evidence="$(grep -E 'acceptance_(m1|m2|m3|fault|runtime)|queue issuance write load:|loadgen report|robots:|verify: attempts|tickets: attempts|poll: attempts|attach: attempts|handoff: attempts|realm: attempts' "${log_path}" || true)"
required_evidence=(
    'acceptance_runtime'
    'acceptance_m1 '
    'acceptance_m2 '
    'acceptance_m3 '
    'acceptance_fault gateway_budget_removal '
    'acceptance_fault gateway_budget_restore '
    'acceptance_fault realm_budget_removal '
    'acceptance_fault realm_budget_restore '
    'acceptance_fault queue_cold_restart '
    'acceptance_fault queue_released_number '
    'acceptance_fault etcd_outage_unready '
    'acceptance_fault etcd_recovery '
    'acceptance_fault queue_authoritative_recovery '
    'queue issuance write load:'
)
for evidence_pattern in "${required_evidence[@]}"; do
    if ! grep -Fq "${evidence_pattern}" "${log_path}"; then
        echo "Missing required measurement marker: ${evidence_pattern}" |
            tee -a "${log_path}" >&2
        overall_status=1
    fi
done
if grep -Fq '***Skipped' "${log_path}"; then
    echo "A required acceptance test was skipped." | tee -a "${log_path}" >&2
    overall_status=1
fi
{
    echo
    echo "## Measured evidence"
    echo
    if [[ -n "${measured_evidence}" ]]; then
        echo '```text'
        echo "${measured_evidence}"
        echo '```'
    else
        echo "No measurement markers were captured; inspect the raw log."
        overall_status=1
    fi
    echo
    echo "## Limitations"
    echo
    echo "- CI runs on one GitHub-hosted Ubuntu 24.04 machine; it does not prove cross-machine network or etcd TLS/authentication behavior."
    echo "- Accounts and characters come from MongoDB Player Data (ADR-0011): each case starts its own single-node \`rs0\` replica set on the runner, and Login Verifier authenticates, Gateway fetches and Realm rechecks against the same database. The replica set is local and unauthenticated, so this does not prove MongoDB TLS/authentication or multi-member failover. Loadgen capacity fixtures (thousands of robot accounts) use \`credential_hash_cost = \"minimum\"\`, so their Login Verify CPU cost is below a production Argon2 profile; single-account dev-services cases keep the default \`interactive\` cost."
    echo "- Results are not comparable with pre-#92 reports, whose Gateway fetch path was the \`DelayedAccountFetchPort\` controlled stub."
    echo "- CDN cache hit rate and the full 100k/1m production sizes require a separately approved dedicated environment; M5 remains separate."
} >>"${report_path}"

echo "Acceptance report: ${report_path}"
echo "Raw log: ${log_path}"
exit "${overall_status}"
