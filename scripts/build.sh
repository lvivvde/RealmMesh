#!/usr/bin/env bash

# 用途：按所选 CMake 预设配置并构建，随后运行完整 CTest 测试集。
# 用法:./scripts/build.sh [--preset NAME]（默认 dev）
# configure / build / test 用同一个预设名，派生的用户预设(CMakeUserPresets.json)
# 同样适用；构建目录取配置期记录的真实 binaryDir(scripts/build-dir.sh)。

set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
# shellcheck source=lib/build-dir.sh
source "${project_root}/scripts/lib/build-dir.sh"

usage() {
    echo "Usage: $0 [--preset NAME]"
    echo "Configures, builds and runs the full CTest suite with one preset"
    echo "(default: ${realmmesh_default_preset}); user presets derived in"
    echo "CMakeUserPresets.json work the same way."
}

preset="${realmmesh_default_preset}"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --preset)
            [[ $# -ge 2 ]] || { echo "--preset requires a name." >&2; exit 2; }
            preset="$2"
            shift 2
            ;;
        --preset=*)
            preset="${1#--preset=}"
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

if command -v cmake >/dev/null 2>&1; then
    cmake_bin="$(command -v cmake)"
elif [[ -x "${project_root}/.tools/cmake/bin/cmake" ]]; then
    cmake_bin="${project_root}/.tools/cmake/bin/cmake"
else
    echo "CMake 3.20 or newer is required." >&2
    echo "Install CMake or place a local distribution in .tools/cmake." >&2
    exit 1
fi

ctest_bin="$(dirname "${cmake_bin}")/ctest"

cd "${project_root}"
"${cmake_bin}" --preset "${preset}"
build_dir="$(realmmesh_resolve_build_dir "${project_root}" "${preset}")"

# 根目录的 compile_commands.json 是指向所选预设编译数据库的符号链接;切换
# 预设时跟着改指向。用户自己放的普通文件不动。
compile_commands="${build_dir}/compile_commands.json"
compile_commands_link="${project_root}/compile_commands.json"
if [[ -f "${compile_commands}" ]]; then
    if [[ "${compile_commands}" == "${project_root}/"* ]]; then
        compile_commands_target="${compile_commands#"${project_root}/"}"
    else
        compile_commands_target="${compile_commands}"
    fi
    if [[ -L "${compile_commands_link}" || ! -e "${compile_commands_link}" ]]; then
        if [[ "$(readlink "${compile_commands_link}" 2>/dev/null || true)" != \
            "${compile_commands_target}" ]]; then
            ln -sfn "${compile_commands_target}" "${compile_commands_link}"
        fi
    else
        echo "warning: ${compile_commands_link} is not a symlink; leaving it unchanged" \
            "(the ${preset} database is ${compile_commands})." >&2
    fi
fi

"${cmake_bin}" --build --preset "${preset}"
"${ctest_bin}" --preset "${preset}"
