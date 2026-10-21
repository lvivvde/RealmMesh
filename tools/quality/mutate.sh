#!/usr/bin/env bash

# 用途：变异测试（mutator）。用 mutation 预设（clang-21 + Mull 插件）构建一个
# unit 测试目标，只变异路径匹配给定正则的源码，再用 mull-runner 逐个变异体跑
# 该测试。只在 Linux 上可用。步骤见 docs/agents/quality-checks.md。
#
# 用法：./tools/quality/mutate.sh <unit 测试目标> <源码路径正则> [gtest 参数...]
# 例：  ./tools/quality/mutate.sh character_name_test 'game/realm/src/character_name'
# 环境：MULL_GIT_DIFF_REF=main 时只变异相对该 ref 改过的行。

set -euo pipefail

if [[ $# -lt 2 ]]; then
    sed -n '7,9p' "$0" >&2
    exit 64
fi
target="$1"
include_regex="$2"
shift 2

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
# shellcheck source=../../scripts/lib/build-dir.sh
source "${project_root}/scripts/lib/build-dir.sh"
# shellcheck source=../../scripts/lib/build-jobs.sh
source "${project_root}/scripts/lib/build-jobs.sh"
command -v mull-runner-21 >/dev/null || { echo "mull-runner-21 not found; see docs/agents/quality-checks.md." >&2; exit 1; }

preset="mutation"
cd "${project_root}"
cmake_bin="$(realmmesh_find_cmake "${project_root}")"
realmmesh_resolve_build_jobs "" || exit 64

"${cmake_bin}" --preset "${preset}"
build_dir="$(realmmesh_resolve_build_dir "${project_root}" "${preset}")"

# 变异范围在编译期由 Mull 插件读取；范围变化时重建目标，避免沿用旧范围的目标文件。
config="${build_dir}/mull.yml"
new_config="$(cat <<EOF
mutators:
  - cxx_default
includePaths:
  - ${include_regex}
excludePaths:
  - .*/third_party/.*
  - .*/build/.*
  - .*/tests/.*
  - .*\.pb\..*
EOF
)"
if [[ -n "${MULL_GIT_DIFF_REF:-}" ]]; then
    new_config+=$'\n'"gitDiffRef: ${MULL_GIT_DIFF_REF}"$'\n'"gitProjectRoot: ${project_root}"
fi
clean_first=()
if [[ ! -f "${config}" || "$(cat "${config}")" != "${new_config}" ]]; then
    printf '%s\n' "${new_config}" >"${config}"
    clean_first=(--clean-first)
fi
export MULL_CONFIG="${config}"

"${cmake_bin}" --build "${build_dir}" --target "${target}" \
    --parallel "${realmmesh_build_jobs}" "${clean_first[@]}"

binary="$(find "${build_dir}" -type f -perm -u+x -name "${target}" | head -n 1)"
[[ -n "${binary}" ]] || { echo "Built binary for ${target} not found under ${build_dir}." >&2; exit 1; }

report_dir="${build_dir}/mull-report"
mkdir -p "${report_dir}"
LLVM_PROFILE_FILE="${build_dir}/mull-%p.profraw" \
    mull-runner-21 --reporters IDE --reporters Elements \
    --report-dir "${report_dir}" --report-name "${target}" \
    --allow-surviving --no-test-output \
    "${binary}" "$@"
echo "Elements HTML report: ${report_dir}/${target}.html"
