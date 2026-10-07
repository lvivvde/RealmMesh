# macOS Login Chain acceptance

- Started: 2026-10-07T07:26:59Z
- Platform: macOS 27.0.1 (arm64)
- Revision: `7858a925e10a85f6d190e34356cf8bfeb02ff12b` (dirty working tree)
- Build: preset `dev`; directory `/private/tmp/realmmesh-p6-fixed/final/build/dev-ninja`
- Transport: TLS/TCP (repository clients carry no QUIC dialer)
- Success-chain repetitions: 3
- Raw log: `macos-login-chain.log`

## Command

`REALMMESH_ACCEPTANCE_REPEATS=3 ./scripts/run-macos-login-acceptance.sh --preset dev`

## Source configuration

```text
badf4cc0e32b5da4a7c84da777978704f2a37ff71b021332ae2f16b3e94bcfbb  configs/main.config
f34f4b853456c2fbab99c51a12cd2e9428bb31cfb76b93d8ad9d370026101068  configs/common/player_data.lua
9219d59f3fc2b25daab02b01a575dcd942e79f2c39821931384b29177a91d8b4  configs/services/login_verify.lua
8b1bf0b7741e3b2edf320c601662a9419ad8a1079a0f2963e0605f783044e559  configs/services/queue.lua
4be72b8bbec6b7c58dcff27cac3f673b5f9366ad85f5ce806b4a5008d8cbd371  configs/services/gateway.lua
d2096415e096a6fa731d4b533473af9f8071fac66c98304e9f77b96e84c773ac  configs/services/realm.lua
```

## Results

| Group | Runs | Result |
|---|---:|---|
| Four independent service processes + real etcd, Full chain | 3 | PASS |
| Full Login Chain + Realm heartbeat/orderly close | 3 | PASS |
| Timeout, replay, process restart, Queue recovery, etcd outage | 1 | PASS |
| L1 Gateway water-level / fd soak | 1 | PASS |

## Resolved runtime configuration

```text
16: acceptance_runtime login_verify=127.0.0.1:26154 queue=127.0.0.1:26239 gateway=127.0.0.1:25707 realm=127.0.0.1:24086 etcd=http://127.0.0.1:21564 mongodb=127.0.0.1:24455
16: acceptance_runtime_config badf4cc0e32b5da4a7c84da777978704f2a37ff71b021332ae2f16b3e94bcfbb  configs/main.config
16: acceptance_runtime_config 05e356d41b021de7b279ea900d038051c170fc6cf986bba3fd034c743b1df0b8  configs/common/discovery.lua
16: acceptance_runtime_config 69b5c275f09cbca512ae487c308a2b07accc71404e48b11c3dd5973212203b63  configs/common/accounts.lua
16: acceptance_runtime_config 700c4c0faa57bed37df50a7a5b26360208e0656d03e54b3e1e289d7fe6b35b10  configs/common/player_data.lua
16: acceptance_runtime_config 643382ccf6d5fad624eb7e93f9b35e740abf8cfd563d101b1e27083d63fddd61  configs/services/login_verify.lua
16: acceptance_runtime_config 912b3187dbdf315684a33e34506fe4581d1429f73c7bc3901d34e4b76bb8d080  configs/services/queue.lua
16: acceptance_runtime_config 7c757b03f6080fe08ed47e8b6e333ad0fd30a19a86d2ffbc9c179c61e72afe54  configs/services/gateway.lua
16: acceptance_runtime_config 7fd8658a9a6775c1a7a9ec967f81079ddf3adbe272b94ded94eec2e7c5e6abc1  configs/services/realm.lua
```

## Phase metrics

```text
16: loadgen report
16: robots: 1  completed: 1
16: verify: attempts=1 failures=0 p50_ms=234.123792 p99_ms=234.123792 max_ms=234.123792
16: tickets: attempts=1 failures=0 p50_ms=122.165000 p99_ms=122.165000 max_ms=122.165000
16: poll: attempts=17 failures=0 p50_ms=40.611834 p99_ms=51.073791 max_ms=51.073791
16: attach: attempts=1 failures=0 p50_ms=72.362125 p99_ms=72.362125 max_ms=72.362125
16: handoff: attempts=1 failures=0 p50_ms=86.028375 p99_ms=86.028375 max_ms=86.028375
16: realm: attempts=1 failures=0 p50_ms=7.671667 p99_ms=7.671667 max_ms=7.671667
16: loadgen report
16: robots: 1  completed: 1
16: verify: attempts=1 failures=0 p50_ms=231.932250 p99_ms=231.932250 max_ms=231.932250
16: tickets: attempts=1 failures=0 p50_ms=110.419792 p99_ms=110.419792 max_ms=110.419792
16: poll: attempts=17 failures=0 p50_ms=40.037000 p99_ms=48.213625 max_ms=48.213625
16: attach: attempts=1 failures=0 p50_ms=19.533417 p99_ms=19.533417 max_ms=19.533417
16: handoff: attempts=1 failures=0 p50_ms=91.989333 p99_ms=91.989333 max_ms=91.989333
16: realm: attempts=1 failures=0 p50_ms=10.878750 p99_ms=10.878750 max_ms=10.878750
16: loadgen report
16: robots: 1  completed: 1
16: verify: attempts=1 failures=0 p50_ms=233.123500 p99_ms=233.123500 max_ms=233.123500
16: tickets: attempts=1 failures=0 p50_ms=115.960709 p99_ms=115.960709 max_ms=115.960709
16: poll: attempts=39 failures=0 p50_ms=41.500625 p99_ms=53.039500 max_ms=53.039500
16: attach: attempts=1 failures=0 p50_ms=27.870834 p99_ms=27.870834 max_ms=27.870834
16: handoff: attempts=1 failures=0 p50_ms=84.880416 p99_ms=84.880416 max_ms=84.880416
16: realm: attempts=1 failures=0 p50_ms=15.139042 p99_ms=15.139042 max_ms=15.139042
660: loadgen report
660: robots: 1  completed: 1
660: verify: attempts=1 failures=0 p50_ms=20.055541 p99_ms=20.055541 max_ms=20.055541
660: tickets: attempts=1 failures=0 p50_ms=19.779584 p99_ms=19.779584 max_ms=19.779584
660: poll: attempts=36 failures=0 p50_ms=2.423709 p99_ms=6.118209 max_ms=6.118209
660: attach: attempts=1 failures=0 p50_ms=20.666250 p99_ms=20.666250 max_ms=20.666250
660: handoff: attempts=1 failures=0 p50_ms=21.707042 p99_ms=21.707042 max_ms=21.707042
660: realm: attempts=1 failures=0 p50_ms=12.254584 p99_ms=12.254584 max_ms=12.254584
660: loadgen report
660: robots: 1  completed: 1
660: verify: attempts=1 failures=0 p50_ms=20.185709 p99_ms=20.185709 max_ms=20.185709
660: tickets: attempts=1 failures=0 p50_ms=19.140833 p99_ms=19.140833 max_ms=19.140833
660: poll: attempts=37 failures=0 p50_ms=1.272875 p99_ms=5.705042 max_ms=5.705042
660: attach: attempts=1 failures=0 p50_ms=16.250083 p99_ms=16.250083 max_ms=16.250083
660: handoff: attempts=1 failures=0 p50_ms=21.349250 p99_ms=21.349250 max_ms=21.349250
660: realm: attempts=1 failures=0 p50_ms=5.472833 p99_ms=5.472833 max_ms=5.472833
660: loadgen report
660: robots: 1  completed: 1
660: verify: attempts=1 failures=0 p50_ms=20.428041 p99_ms=20.428041 max_ms=20.428041
660: tickets: attempts=1 failures=0 p50_ms=20.903833 p99_ms=20.903833 max_ms=20.903833
660: poll: attempts=37 failures=0 p50_ms=1.715958 p99_ms=5.825458 max_ms=5.825458
660: attach: attempts=1 failures=0 p50_ms=19.923458 p99_ms=19.923458 max_ms=19.923458
660: handoff: attempts=1 failures=0 p50_ms=22.003042 p99_ms=22.003042 max_ms=22.003042
660: realm: attempts=1 failures=0 p50_ms=11.880250 p99_ms=11.880250 max_ms=11.880250
```

## Transport

- Client dial: TLS/TCP (QUIC candidates are rejected as unsupported)
- Gateway QUIC listener: compiled in (unused by the chain)

## Scope

This profile starts the real Login Verifier, Queue, Gateway, Realm and a
temporary real etcd. It exercises real HTTPS/TLS/TCP wire paths. QUIC wire paths,
production capacity sizing and production account data remain out of scope.
