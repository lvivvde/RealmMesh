#!/usr/bin/env bash

# 用途：输出某个 CMake 预设已配置的构建目录(配置期记录的真实 binaryDir,#122)。
# 用法:./scripts/build-dir.sh [--preset NAME]（默认 dev）
# 预设未配置或构建目录身份不符时以非零退出，并提示如何配置。

set -euo pipefail

realmmesh_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
# shellcheck source=lib/build-dir.sh
source "${realmmesh_root}/scripts/lib/build-dir.sh"

realmmesh_preset="${realmmesh_default_preset}"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --preset)
            [[ $# -ge 2 ]] || { echo "--preset requires a name." >&2; exit 2; }
            realmmesh_preset="$2"
            shift 2
            ;;
        --preset=*)
            realmmesh_preset="${1#--preset=}"
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [--preset NAME]"
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            echo "Usage: $0 [--preset NAME]" >&2
            exit 2
            ;;
    esac
done

realmmesh_resolve_build_dir "${realmmesh_root}" "${realmmesh_preset}"
