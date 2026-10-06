#!/usr/bin/env bash

# 用途：按所选 CMake 预设配置并构建 ALL,随后运行完整 CTest 测试集(完整验证)。
# 用法:./scripts/build.sh [--preset NAME] [--jobs N]
# configure / build / test 用同一个预设名，派生的用户预设(CMakeUserPresets.json)
# 同样适用；构建目录取配置期记录的真实 binaryDir(scripts/build-dir.sh)。
# 编译并行按资源预算(scripts/lib/build-jobs.sh);完整 CTest 固定 -j 1,不继承
# CTEST_PARALLEL_LEVEL(#125)。日常编辑循环用 ./scripts/test-fast.sh。

set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
# shellcheck source=lib/build-dir.sh
source "${project_root}/scripts/lib/build-dir.sh"
# shellcheck source=lib/build-jobs.sh
source "${project_root}/scripts/lib/build-jobs.sh"

usage() {
    echo "  Compiler cache: AUTO; override REALMMESH_CCACHE=AUTO/ON/OFF in a user preset. Consult README for cache paths and limits."
    echo "Usage: $0 [--preset NAME] [--jobs N]"
    echo "Configures, builds ALL and runs the full CTest suite serially (ctest -j 1)"
    echo "with one preset (default: ${realmmesh_default_preset}); user presets derived"
    echo "in CMakeUserPresets.json work the same way."
    echo "  --jobs N   compile jobs (default: CMAKE_BUILD_PARALLEL_LEVEL if set, else"
    echo "             the CPU/memory budget)"
}

preset="${realmmesh_default_preset}"
jobs=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --preset|--jobs)
            [[ $# -ge 2 ]] || { echo "$1 requires a value." >&2; exit 2; }
            if [[ "$1" == "--preset" ]]; then preset="$2"; else jobs="$2"; fi
            shift 2
            ;;
        --preset=*)
            preset="${1#--preset=}"
            shift
            ;;
        --jobs=*)
            jobs="${1#--jobs=}"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done
realmmesh_require_preset_name "${preset}" || exit 2
realmmesh_resolve_build_jobs "${jobs}" || exit 2

cmake_bin="$(realmmesh_find_cmake "${project_root}")" || exit 1
ctest_bin="$(dirname "${cmake_bin}")/ctest"

cd "${project_root}"
echo "${realmmesh_build_jobs_report}"
"${cmake_bin}" --preset "${preset}"
build_dir="$(realmmesh_resolve_build_dir "${project_root}" "${preset}")"
realmmesh_link_compile_commands "${project_root}" "${build_dir}" "${preset}"

"${cmake_bin}" --build --preset "${preset}" --parallel "${realmmesh_build_jobs}"
"${ctest_bin}" --preset "${preset}" -j 1
