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
