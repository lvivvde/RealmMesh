#!/usr/bin/env bash

# 用途：重复代码检查（dryer）。用 jscpd 扫 framework/ game/ apps/ tools/ 的 C++，
# 排除 third_party、构建目录与 protobuf 生成代码。控制台打印每处克隆和汇总，
# HTML 报告写到 build/quality/jscpd/。步骤见 docs/agents/quality-checks.md。
#
# 用法：./tools/quality/dry.sh [jscpd 额外参数，如 --min-tokens 100 或某个子目录]

set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
cd "${project_root}"
command -v jscpd >/dev/null || { echo "jscpd not found (npm install -g jscpd@5.4.2)." >&2; exit 1; }

paths=(framework game apps tools)
if [[ $# -gt 0 && "$1" != -* ]]; then
    paths=()
    while [[ $# -gt 0 && "$1" != -* ]]; do
        paths+=("$1")
        shift
    done
fi

jscpd --format cpp --formats-exts 'cpp:cpp,cc,hpp,h' --min-tokens 70 \
    --ignore '**/third_party/**,**/build/**,**/*.pb.*' \
    --reporters console,html --output build/quality/jscpd \
    "$@" "${paths[@]}"
