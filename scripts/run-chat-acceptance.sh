#!/usr/bin/env bash
# Chat C1–C5 本机多进程总验收：冻结源码/配置，构建必需目标并生成可校验归档。
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
source "${project_root}/scripts/lib/build-dir.sh"
source "${project_root}/scripts/lib/build-jobs.sh"

preset="${realmmesh_default_preset}"
jobs=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --preset|--jobs)
            [[ $# -ge 2 ]] || { echo "$1 requires a value" >&2; exit 64; }
            if [[ "$1" == --preset ]]; then preset="$2"; else jobs="$2"; fi
            shift 2
            ;;
        --preset=*) preset="${1#--preset=}"; shift ;;
        --jobs=*) jobs="${1#--jobs=}"; shift ;;
        -h|--help)
            echo "Usage: $0 [--preset NAME] [--jobs N]"
            exit 0
            ;;
        *) echo "Unknown argument: $1" >&2; exit 64 ;;
    esac
done

realmmesh_require_preset_name "$preset" || exit 64
realmmesh_resolve_build_jobs "$jobs" || exit 64
cmake_bin="$(realmmesh_find_cmake "$project_root")"
ctest_bin="$(dirname "$cmake_bin")/ctest"
cd "$project_root"

run_id="$(date -u '+%Y%m%dT%H%M%SZ')-$$"
build_dir="$(realmmesh_resolve_build_dir "$project_root" "$preset" 2>/dev/null || true)"
fallback_root="${REALMMESH_CHAT_ACCEPTANCE_FALLBACK_DIRECTORY:-${project_root}/.local/acceptance/chat-reference-loop}"
if [[ -n "$build_dir" ]]; then
    report_dir="${build_dir}/acceptance/chat-reference-loop/${run_id}"
else
    report_dir="${fallback_root}/${run_id}"
fi
mkdir -p "$report_dir"
python3 "$project_root/scripts/lib/chat-acceptance-report.py" \
    freeze "$project_root" "$report_dir" "$preset"

archive_report() {
    local archive="${report_dir}.tar.gz"
    tar -C "$(dirname "$report_dir")" -czf "$archive" "$(basename "$report_dir")"
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$archive" >"${archive}.sha256"
    else
        shasum -a 256 "$archive" >"${archive}.sha256"
    fi
    printf 'Chat acceptance archive: %s\n' "$archive"
    printf 'Chat acceptance SHA256: %s\n' "${archive}.sha256"
}

if ! "$cmake_bin" --preset "$preset" >"$report_dir/configure.log" 2>&1; then
    cat "$report_dir/configure.log" >&2
    python3 "$project_root/scripts/lib/chat-acceptance-report.py" \
        finish "$project_root" "$report_dir" configure-failed || true
    archive_report
    printf 'Chat acceptance report: %s/report.md\n' "$report_dir"
    exit 1
fi

build_dir="$(realmmesh_resolve_build_dir "$project_root" "$preset")"
actual_report_dir="${build_dir}/acceptance/chat-reference-loop/${run_id}"
if [[ "$report_dir" != "$actual_report_dir" ]]; then
    mkdir -p "$(dirname "$actual_report_dir")"
    mv "$report_dir" "$actual_report_dir"
    report_dir="$actual_report_dir"
fi
export REALMMESH_CHAT_ARTIFACT_DIRECTORY="$report_dir/services"
export REALMMESH_SCENE_ARTIFACT_DIRECTORY="$report_dir/services"

if ! "$cmake_bin" --build --preset "$preset" \
    --parallel "$realmmesh_build_jobs" \
    --target chat_protocol_test chat_channels_test chat_service_test \
        realm_chat_test chat_session_test chat_journey_test scene_journey_test \
    >"$report_dir/build.log" 2>&1; then
    cat "$report_dir/build.log" >&2
    python3 "$project_root/scripts/lib/chat-acceptance-report.py" \
        finish "$project_root" "$report_dir" build-failed || true
    archive_report
    printf 'Chat acceptance report: %s/report.md\n' "$report_dir"
    exit 1
fi

test_regex='^(ChatProtocolTest|ChatChannelsTest|ChatServiceTest|RealmChatTest|ChatSessionTest|ChatJourneyTest|DualSceneDualChatJourneyTest|ChatAcceptanceReportTest)\.'
if ! "$ctest_bin" --preset "$preset" --show-only=json-v1 \
    -R "$test_regex" >"$report_dir/expected-cases.json"; then
    python3 "$project_root/scripts/lib/chat-acceptance-report.py" \
        finish "$project_root" "$report_dir" discovery-failed || true
    archive_report
    printf 'Chat acceptance report: %s/report.md\n' "$report_dir"
    exit 1
fi

mkdir -p "$report_dir/gtest"
export GTEST_OUTPUT="xml:$report_dir/gtest/"
set +e
"$ctest_bin" --preset "$preset" -j 1 --output-on-failure \
    -R "$test_regex" \
    --output-junit "$report_dir/results.xml" >"$report_dir/ctest.log" 2>&1
result=$?
set -e
cat "$report_dir/ctest.log"

set +e
python3 "$project_root/scripts/lib/chat-acceptance-report.py" \
    finish "$project_root" "$report_dir" "$result"
report_result=$?
set -e
archive_report
printf 'Chat acceptance report: %s/report.md\n' "$report_dir"
if [[ "$result" -ne 0 ]]; then exit "$result"; fi
exit "$report_result"
