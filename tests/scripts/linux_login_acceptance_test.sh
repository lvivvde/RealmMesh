#!/usr/bin/env bash

set -euo pipefail

project_root="$1"
acceptance_script="${project_root}/scripts/run-linux-login-acceptance.sh"

plan="$(bash "${acceptance_script}" --print-plan)"

grep -Fq 'Transport: real QUIC + verified TLS/TCP fallback' <<<"${plan}"
grep -Fq 'M1: concurrent multi-stage water, attach P99 drift, and fd soak' \
    <<<"${plan}"
grep -Fq 'M2: 250 robots, >=248 completed, fetch failures <1%' <<<"${plan}"
grep -Fq 'M3: 10000 durable tickets + 2000 progress clients, >=99.9%' <<<"${plan}"
grep -Fq 'M4: budget convergence <=10s, cold Queue <=30s, released number monotonic' \
    <<<"${plan}"
grep -Fq 'Production targets are reported but not claimed by the CI profile.' <<<"${plan}"

# 构建目录随 --preset 选择(#122):帮助说明该选项与报告位置,--preset 与
# --print-plan 可组合,未知参数与非法预设名直接拒绝。
help="$(bash "${acceptance_script}" --help)"
grep -Fq -- '--preset NAME' <<<"${help}"
grep -Fq '<its build directory>/acceptance/' <<<"${help}"
grep -Fq 'Linux Login Chain M1-M4 acceptance plan' \
    <<<"$(bash "${acceptance_script}" --preset dev-make --print-plan)"
if bash "${acceptance_script}" --bogus >/dev/null 2>&1; then
    echo "unknown arguments must be rejected" >&2
    exit 1
fi
if bash "${acceptance_script}" --preset ../dev >/dev/null 2>&1; then
    echo "invalid preset names must be rejected" >&2
    exit 1
fi
