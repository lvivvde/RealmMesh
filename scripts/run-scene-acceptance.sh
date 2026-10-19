#!/usr/bin/env bash
# Scene S2 本机多进程验收:总是先构建,只使用隔离测试数据库与服务证书。
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
            shift 2 ;;
        --preset=*) preset="${1#--preset=}"; shift ;;
        --jobs=*) jobs="${1#--jobs=}"; shift ;;
        -h|--help) echo "Usage: $0 [--preset NAME] [--jobs N]"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 64 ;;
    esac
done
realmmesh_require_preset_name "$preset" || exit 64
realmmesh_resolve_build_jobs "$jobs" || exit 64
cmake_bin="$(realmmesh_find_cmake "$project_root")"
ctest_bin="$(dirname "$cmake_bin")/ctest"
cd "$project_root"
configure_log="$(mktemp)"
trap 'rm -f -- "$configure_log"' EXIT
if ! "$cmake_bin" --preset "$preset" >"$configure_log" 2>&1; then
    cat "$configure_log" >&2; exit 1
fi
build_dir="$(realmmesh_resolve_build_dir "$project_root" "$preset")"
report_dir="${build_dir}/acceptance/scene-reference-loop/$(date -u '+%Y%m%dT%H%M%SZ')-$$"
mkdir -p "$report_dir"
cp "$configure_log" "$report_dir/configure.log"
export REALMMESH_SCENE_ARTIFACT_DIRECTORY="$report_dir/services"
python3 "$project_root/scripts/lib/scene-acceptance-report.py" freeze "$project_root" "$report_dir" "$preset"
if ! "$cmake_bin" --build --preset "$preset" --parallel "$realmmesh_build_jobs" \
    --target scene_journey_test realm_scenes_test scene_shard_test tls_mutual_auth_test scene_session_test \
    >"$report_dir/build.log" 2>&1; then
    cat "$report_dir/build.log" >&2
    python3 "$project_root/scripts/lib/scene-acceptance-report.py" finish "$project_root" "$report_dir" build-failed
    exit 1
fi
set +e
"$ctest_bin" --preset "$preset" -j 1 --output-on-failure \
    -R '^(SceneJourneyTest|RealmScenesTest|SceneShardTest|MutualTlsTest|SceneSessionTest)\.' \
    --output-junit "$report_dir/results.xml" >"$report_dir/ctest.log" 2>&1
result=$?
set -e
cat "$report_dir/ctest.log"
python3 "$project_root/scripts/lib/scene-acceptance-report.py" finish "$project_root" "$report_dir" "$result"
printf 'Scene acceptance report: %s/report.md\n' "$report_dir"
exit "$result"
