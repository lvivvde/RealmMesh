#!/usr/bin/env bash

# 用途：日常编辑循环的快速入口(#125,决策见 docs/research/build-test-entry-decisions.md)。
# 每次按所选预设配置，只构建 Unit 范围(聚合目标 realmmesh_unit_tests 或一个已登记
# 的 Unit 目标)及其真实依赖，再运行该范围内的 unit 用例。它不是完整验证：阶段完成
# 或公共头、跨模块、网络、协议、存储行为变化时跑 ./scripts/build.sh。
#
# 用法:./scripts/test-fast.sh [--preset NAME] [--jobs N] [--target TARGET]
#                             [--test-regex REGEX] [--test-jobs N]
#
# 退出码:0 所选用例全部通过(有跳过时另行说明);1 配置、构建失败或二进制缺失
# (构建失败不运行旧二进制);2 用例失败或所选范围内没有用例;64 用法错误或
# --target 不是已登记的 Unit 目标。

set -uo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
# shellcheck source=lib/build-dir.sh
source "${project_root}/scripts/lib/build-dir.sh"
# shellcheck source=lib/build-jobs.sh
source "${project_root}/scripts/lib/build-jobs.sh"

unit_aggregate_target="realmmesh_unit_tests"
default_test_jobs=4

usage() {
    cat <<EOF
Usage: $0 [--preset NAME] [--jobs N] [--target TARGET] [--test-regex REGEX] [--test-jobs N]
Configures the preset (default: ${realmmesh_default_preset}), builds only the Unit scope and
runs its unit tests. This is the edit-loop entry, not full verification
(./scripts/build.sh builds ALL and runs the whole suite).
  --target TARGET     build and test one registered Unit target (a GTest
                      executable, or realm_lua_cli for the Unit Lua suites)
  --test-regex REGEX  further narrow the selected tests by name (ctest -R);
                      an empty selection fails
  --test-jobs N       CTest parallelism (default: ${default_test_jobs}; 1 runs serially)
  --jobs N            compile jobs (default: CMAKE_BUILD_PARALLEL_LEVEL if set,
                      else the CPU/memory budget)
Exit: 0 passed, 1 configure/build failure or missing binary, 2 test failure
or no tests selected, 64 usage error or a target that is not a registered Unit
target.
EOF
}

usage_error() {
    printf '%s\n' "$*" >&2
    usage >&2
    exit 64
}

preset="${realmmesh_default_preset}"
jobs=""
target=""
test_regex=""
test_regex_set=0
test_jobs="${default_test_jobs}"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --preset|--jobs|--target|--test-regex|--test-jobs)
            [[ $# -ge 2 ]] || usage_error "$1 requires a value."
            option="$1"
            value="$2"
            shift 2
            ;;
        --preset=*|--jobs=*|--target=*|--test-regex=*|--test-jobs=*)
            option="${1%%=*}"
            value="${1#*=}"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage_error "Unknown argument: $1"
            ;;
    esac
    case "${option}" in
        --preset) preset="${value}" ;;
        --jobs) jobs="${value}" ;;
        --target) target="${value}" ;;
        --test-regex) test_regex="${value}"; test_regex_set=1 ;;
        --test-jobs) test_jobs="${value}" ;;
    esac
done
realmmesh_require_preset_name "${preset}" || exit 64
if [[ "${test_regex_set}" -eq 1 && -z "${test_regex}" ]]; then
    usage_error "--test-regex requires a non-empty regular expression."
fi
if [[ -n "${target}" ]] && ! realmmesh_is_valid_preset_name "${target}"; then
    usage_error "Invalid target name: \"${target}\"."
fi
if ! realmmesh_is_positive_integer "${test_jobs}"; then
    usage_error "--test-jobs must be a positive integer, got \"${test_jobs}\"."
fi
realmmesh_resolve_build_jobs "${jobs}" || exit 64

cmake_bin="$(realmmesh_find_cmake "${project_root}")" || exit 1
ctest_bin="$(dirname "${cmake_bin}")/ctest"

# 墙钟时间(秒，三位小数):bash 5 有 EPOCHREALTIME;macOS 的 bash 3.2 用 perl。
now() {
    if [[ -n "${EPOCHREALTIME:-}" ]]; then
        printf '%s\n' "${EPOCHREALTIME}"
    else
        perl -MTime::HiRes=time -e 'printf "%.3f\n", time'
    fi
}
elapsed() {
    awk -v start="$1" -v end="$2" 'BEGIN { printf "%.2f", end - start }'
}

configure_seconds="-"
build_seconds="-"
test_seconds="-"
started_at="$(now)"

# 打印各阶段耗时后以 <code> 退出;第二个参数是结论。
finish() {
    local code="$1"
    local verdict="$2"
    echo "test-fast: ${verdict}"
    echo "test-fast: time configure ${configure_seconds}s, build ${build_seconds}s," \
        "test ${test_seconds}s, total $(elapsed "${started_at}" "$(now)")s (exit ${code})"
    exit "${code}"
}

show_command() {
    printf '+'
    printf ' %q' "$@"
    printf '\n'
}

cd "${project_root}"

if [[ -n "${target}" ]]; then
    selection="Unit target ${target}"
else
    selection="all registered Unit targets"
fi
[[ -n "${test_regex}" ]] && selection="${selection}, tests matching /${test_regex}/"
echo "test-fast: preset ${preset}; ${selection}; test jobs ${test_jobs}"
echo "${realmmesh_build_jobs_report}"

# --- 配置：每次都跑，增量时 CMake 自己判断是否需要重新生成 ---
stage_start="$(now)"
show_command "${cmake_bin}" --preset "${preset}"
if ! "${cmake_bin}" --preset "${preset}"; then
    configure_seconds="$(elapsed "${stage_start}" "$(now)")"
    finish 1 "FAILED: configure failed; nothing was built or tested."
fi
configure_seconds="$(elapsed "${stage_start}" "$(now)")"
if ! build_dir="$(realmmesh_resolve_build_dir "${project_root}" "${preset}")"; then
    finish 1 "FAILED: the configured build directory could not be resolved."
fi
realmmesh_link_compile_commands "${project_root}" "${build_dir}" "${preset}"

registry="${build_dir}/realmmesh-unit-targets.txt"
if [[ ! -f "${registry}" ]]; then
    echo "No Unit target registry at ${registry}; is BUILD_TESTING OFF for preset ${preset}?" >&2
    finish 1 "FAILED: no Unit targets are registered."
fi
# 登记清单每行 <目标>=<产物路径>;按字面比较目标名，不当正则。
registered_binary() {
    awk -v name="$1" 'index($0, name "=") == 1 { print substr($0, length(name) + 2); exit }' \
        "${registry}"
}
registered_targets=()
while IFS= read -r line; do
    [[ -n "${line}" ]] && registered_targets+=("${line%%=*}")
done <"${registry}"
if [[ ${#registered_targets[@]} -eq 0 ]]; then
    finish 2 "FAILED: the registry lists no Unit targets."
fi

if [[ -n "${target}" ]]; then
    if [[ -z "$(registered_binary "${target}")" ]]; then
        printf '"%s" is not a registered Unit target (unknown, or not classified unit).\n' \
            "${target}" >&2
        printf 'Registered Unit targets: %s\n' "${registered_targets[*]}" >&2
        exit 64
    fi
    build_target="${target}"
    selected_targets=("${target}")
    # 先确认 Unit 身份，再按转义后的精确身份标签筛选；同一解释器上的非 Unit Lua
    # 套件由分类标签排除(分类标签恰好是 unit / integration 之一)。
    escaped_target="$(printf '%s' "${target}" | sed 's/[][\.^$*+?(){}|]/\\&/g')"
    label_args=(-L "^target=${escaped_target}\$" -LE '^integration$')
else
    build_target="${unit_aggregate_target}"
    selected_targets=("${registered_targets[@]}")
    label_args=(-L '^unit$')
fi
regex_args=()
[[ -n "${test_regex}" ]] && regex_args=(-R "${test_regex}")

# --- 构建：只构建所选范围；失败即止，不运行旧二进制 ---
stage_start="$(now)"
show_command "${cmake_bin}" --build --preset "${preset}" --target "${build_target}" \
    --parallel "${realmmesh_build_jobs}"
if ! "${cmake_bin}" --build --preset "${preset}" --target "${build_target}" \
    --parallel "${realmmesh_build_jobs}"; then
    build_seconds="$(elapsed "${stage_start}" "$(now)")"
    finish 1 "FAILED: build failed; tests were not run (no stale binaries)."
fi
build_seconds="$(elapsed "${stage_start}" "$(now)")"

missing=()
for selected in "${selected_targets[@]}"; do
    binary="$(registered_binary "${selected}")"
    [[ -x "${binary}" ]] || missing+=("${selected} (${binary})")
done
if [[ ${#missing[@]} -gt 0 ]]; then
    printf 'Missing test binary after the build: %s\n' "${missing[@]}" >&2
    finish 1 "FAILED: test binaries are missing; tests were not run."
fi

# --- 测试：显式 -j 覆盖 CTEST_PARALLEL_LEVEL;--no-tests=error 让空选择失败 ---
test_log="$(mktemp "${TMPDIR:-/tmp}/realmmesh-test-fast.XXXXXX")"
trap 'rm -f "${test_log}"' EXIT
ctest_command=("${ctest_bin}" --preset "${preset}" "${label_args[@]}")
[[ ${#regex_args[@]} -gt 0 ]] && ctest_command+=("${regex_args[@]}")
ctest_command+=(-j "${test_jobs}" --no-tests=error --no-label-summary)
stage_start="$(now)"
show_command "${ctest_command[@]}"
"${ctest_command[@]}" 2>&1 | tee "${test_log}"
test_status="${PIPESTATUS[0]}"
test_seconds="$(elapsed "${stage_start}" "$(now)")"

# 汇总行在 3.20 是 "100% tests passed, 0 tests failed out of N",新版无失败时省去
# 中间一段("100% tests passed out of N")。
summary="$(grep -E '^[0-9]+% tests passed' "${test_log}" | tail -n 1)"
total="$(printf '%s' "${summary}" | sed -n 's/.* out of \([0-9][0-9]*\).*/\1/p')"
failed="$(printf '%s' "${summary}" | sed -n 's/.* \([0-9][0-9]*\) tests failed .*/\1/p')"
failed="${failed:-0}"
skipped="$(grep -c '(Skipped)$' "${test_log}")"
disabled="$(grep -c '(Disabled)$' "${test_log}")"
if [[ -z "${total}" ]]; then
    if grep -q 'No tests were found' "${test_log}"; then
        finish 2 "FAILED: no tests matched the selection (${selection})."
    fi
    finish 2 "FAILED: ctest exited ${test_status} without a test summary."
fi
not_run=$((skipped + disabled))
passed=$((total - failed - not_run))
counts="${passed} passed, ${failed} failed, ${not_run} skipped of ${total} selected"

if [[ "${test_status}" -ne 0 || "${failed}" -ne 0 ]]; then
    finish 2 "FAILED: ${counts}."
fi
if [[ "${not_run}" -gt 0 ]]; then
    finish 0 "passed with skips: ${counts}; the skipped tests were not verified."
fi
finish 0 "PASSED: ${counts}."
