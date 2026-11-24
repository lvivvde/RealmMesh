#!/usr/bin/env bash

# 用途：CRAP 检查（crapper）。用 coverage 预设构建 Unit 范围并跑 unit 用例收集
# 覆盖率，再用 lizard 算圈复杂度，按函数合并成 CRAP 排行。只覆盖 unit 用例走到
# 的代码；integration 路径不在内。步骤与工具安装见 docs/agents/quality-checks.md。
#
# 用法：./tools/quality/crap.sh [--top N] [--threshold T]
# 需要 Clang 系编译器；Linux 首次配置前设 CC=clang-21 CXX=clang++-21。
# LLVM_COV / LLVM_PROFDATA 可覆盖工具路径（默认 macOS 用 xcrun，Linux 用 -21 后缀）。

set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
# shellcheck source=../../scripts/lib/build-dir.sh
source "${project_root}/scripts/lib/build-dir.sh"
# shellcheck source=../../scripts/lib/build-jobs.sh
source "${project_root}/scripts/lib/build-jobs.sh"

preset="coverage"
if [[ "$(uname -s)" == "Darwin" ]]; then
    llvm_cov=(${LLVM_COV:-xcrun llvm-cov})
    llvm_profdata=(${LLVM_PROFDATA:-xcrun llvm-profdata})
else
    llvm_cov=(${LLVM_COV:-llvm-cov-21})
    llvm_profdata=(${LLVM_PROFDATA:-llvm-profdata-21})
fi
command -v lizard >/dev/null || { echo "lizard not found (pipx install lizard)." >&2; exit 1; }
command -v "${llvm_cov[0]}" >/dev/null || { echo "${llvm_cov[0]} not found; see docs/agents/quality-checks.md." >&2; exit 1; }
command -v "${llvm_profdata[0]}" >/dev/null || { echo "${llvm_profdata[0]} not found; see docs/agents/quality-checks.md." >&2; exit 1; }

cd "${project_root}"
# 配置期检测程序也带插桩；把它们的 profraw 引到构建目录，不落在仓库根。
mkdir -p "${project_root}/build/coverage"
export LLVM_PROFILE_FILE="${project_root}/build/coverage/configure-%p.profraw"
cmake_bin="$(realmmesh_find_cmake "${project_root}")"
ctest_bin="$(dirname "${cmake_bin}")/ctest"
realmmesh_resolve_build_jobs "" || exit 64

"${cmake_bin}" --preset "${preset}"
build_dir="$(realmmesh_resolve_build_dir "${project_root}" "${preset}")"
"${cmake_bin}" --build "${build_dir}" --target realmmesh_unit_tests \
    --parallel "${realmmesh_build_jobs}"

profile_dir="${build_dir}/profraw"
rm -rf "${profile_dir}"
mkdir -p "${profile_dir}"
LLVM_PROFILE_FILE="${profile_dir}/%p-%m.profraw" \
    "${ctest_bin}" --test-dir "${build_dir}" -L '^unit$' -j 4 --output-on-failure \
    || echo "warning: some unit tests failed; coverage still reported." >&2

profiles=("${profile_dir}"/*.profraw)
if [[ ! -e "${profiles[0]}" ]]; then
    echo "No coverage profiles were produced under ${profile_dir}." >&2
    exit 1
fi
"${llvm_profdata[@]}" merge -sparse -o "${build_dir}/unit.profdata" "${profiles[@]}"

objects=()
while IFS= read -r binary; do
    objects+=(-object "${binary}")
done < <("${ctest_bin}" --test-dir "${build_dir}" -L '^unit$' --show-only=json-v1 \
    | python3 -c 'import json, sys, os
seen = set()
for t in json.load(sys.stdin)["tests"]:
    exe = (t.get("command") or [""])[0]
    if exe and os.path.isfile(exe) and exe not in seen:
        seen.add(exe)
        print(exe)')
if [[ ${#objects[@]} -eq 0 ]]; then
    echo "No unit test executables were found in ${build_dir}." >&2
    exit 1
fi

"${llvm_cov[@]}" export -instr-profile="${build_dir}/unit.profdata" -skip-expansions \
    -ignore-filename-regex='(third_party|/build/|/tests/|\.pb\.)' \
    "${objects[@]:1}" >"${build_dir}/unit-coverage.json"

lizard -l cpp --csv -x '*/third_party/*' framework game apps >"${build_dir}/lizard.csv"

python3 "${project_root}/tools/quality/crap_report.py" \
    --lizard-csv "${build_dir}/lizard.csv" \
    --llvm-cov-json "${build_dir}/unit-coverage.json" "$@"
