# macOS Login Chain acceptance

- Started: 2026-10-06T17:18:49Z
- Platform: macOS 27.0.1 (arm64)
- Revision: `51ad054bcb172f8e66995d9a450a1a03c6ea3085` (dirty working tree)
- Build: preset `dev`; directory `/private/tmp/realmmesh-p6/final/build/dev-ninja`
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
15: acceptance_runtime login_verify=127.0.0.1:21154 queue=127.0.0.1:24696 gateway=127.0.0.1:27367 realm=127.0.0.1:23286 etcd=http://127.0.0.1:20024 mongodb=127.0.0.1:21997
15: acceptance_runtime_config badf4cc0e32b5da4a7c84da777978704f2a37ff71b021332ae2f16b3e94bcfbb  configs/main.config
15: acceptance_runtime_config ece37203f23d3f14f068e5de9f80dc23ccc3189de0182a8b7abf596668891f9b  configs/common/discovery.lua
15: acceptance_runtime_config 69b5c275f09cbca512ae487c308a2b07accc71404e48b11c3dd5973212203b63  configs/common/accounts.lua
15: acceptance_runtime_config b0eb4383a1818a3f0dc9b6da02833c130bd0fefe385e34f53d89b3c74a53c6d7  configs/common/player_data.lua
15: acceptance_runtime_config a35147651948b3d485aeb3cf3a62a967f1e3df8f3aa23f85228bca6bd00f3487  configs/services/login_verify.lua
15: acceptance_runtime_config 376cea2c10018ddbe9eec27afc3b8d0af1206f2444af01dafc2a6aed3526fa4d  configs/services/queue.lua
15: acceptance_runtime_config 5fac1f55a5cc640592d6eee70bf37b5e5266558b9843a955159a09b918e52868  configs/services/gateway.lua
15: acceptance_runtime_config 2851ea864917d5ec96fb660089d7379c0ce540fb243545f58540bba1e1b2f5a8  configs/services/realm.lua
```

## Phase metrics

```text
15: loadgen report
15: robots: 1  completed: 1
15: verify: attempts=1 failures=0 p50_ms=174.466666 p99_ms=174.466666 max_ms=174.466666
15: tickets: attempts=1 failures=0 p50_ms=127.282375 p99_ms=127.282375 max_ms=127.282375
15: poll: attempts=17 failures=0 p50_ms=41.669834 p99_ms=51.782250 max_ms=51.782250
15: attach: attempts=1 failures=0 p50_ms=53.014292 p99_ms=53.014292 max_ms=53.014292
15: handoff: attempts=1 failures=0 p50_ms=89.891750 p99_ms=89.891750 max_ms=89.891750
15: realm: attempts=1 failures=0 p50_ms=7.409084 p99_ms=7.409084 max_ms=7.409084
15: loadgen report
15: robots: 1  completed: 1
15: verify: attempts=1 failures=0 p50_ms=249.738583 p99_ms=249.738583 max_ms=249.738583
15: tickets: attempts=1 failures=0 p50_ms=132.451292 p99_ms=132.451292 max_ms=132.451292
15: poll: attempts=17 failures=0 p50_ms=37.954167 p99_ms=59.501583 max_ms=59.501583
15: attach: attempts=1 failures=0 p50_ms=50.176083 p99_ms=50.176083 max_ms=50.176083
15: handoff: attempts=1 failures=0 p50_ms=81.146708 p99_ms=81.146708 max_ms=81.146708
15: realm: attempts=1 failures=0 p50_ms=11.515208 p99_ms=11.515208 max_ms=11.515208
15: loadgen report
15: robots: 1  completed: 1
15: verify: attempts=1 failures=0 p50_ms=163.322333 p99_ms=163.322333 max_ms=163.322333
15: tickets: attempts=1 failures=0 p50_ms=133.794667 p99_ms=133.794667 max_ms=133.794667
15: poll: attempts=17 failures=0 p50_ms=43.351958 p99_ms=57.365917 max_ms=57.365917
15: attach: attempts=1 failures=0 p50_ms=51.237583 p99_ms=51.237583 max_ms=51.237583
15: handoff: attempts=1 failures=0 p50_ms=80.328916 p99_ms=80.328916 max_ms=80.328916
15: realm: attempts=1 failures=0 p50_ms=12.447750 p99_ms=12.447750 max_ms=12.447750
659: loadgen report
659: robots: 1  completed: 1
659: verify: attempts=1 failures=0 p50_ms=20.152292 p99_ms=20.152292 max_ms=20.152292
659: tickets: attempts=1 failures=0 p50_ms=20.239417 p99_ms=20.239417 max_ms=20.239417
659: poll: attempts=36 failures=0 p50_ms=2.387917 p99_ms=5.880166 max_ms=5.880166
659: attach: attempts=1 failures=0 p50_ms=17.056000 p99_ms=17.056000 max_ms=17.056000
659: handoff: attempts=1 failures=0 p50_ms=22.059542 p99_ms=22.059542 max_ms=22.059542
659: realm: attempts=1 failures=0 p50_ms=4.694875 p99_ms=4.694875 max_ms=4.694875
659: loadgen report
659: robots: 1  completed: 1
659: verify: attempts=1 failures=0 p50_ms=20.081291 p99_ms=20.081291 max_ms=20.081291
659: tickets: attempts=1 failures=0 p50_ms=18.431250 p99_ms=18.431250 max_ms=18.431250
659: poll: attempts=35 failures=0 p50_ms=2.733250 p99_ms=7.292875 max_ms=7.292875
659: attach: attempts=1 failures=0 p50_ms=20.541583 p99_ms=20.541583 max_ms=20.541583
659: handoff: attempts=1 failures=0 p50_ms=24.019458 p99_ms=24.019458 max_ms=24.019458
659: realm: attempts=1 failures=0 p50_ms=4.694750 p99_ms=4.694750 max_ms=4.694750
659: loadgen report
659: robots: 1  completed: 1
659: verify: attempts=1 failures=0 p50_ms=18.844708 p99_ms=18.844708 max_ms=18.844708
659: tickets: attempts=1 failures=0 p50_ms=18.985458 p99_ms=18.985458 max_ms=18.985458
659: poll: attempts=36 failures=0 p50_ms=2.580917 p99_ms=5.979458 max_ms=5.979458
659: attach: attempts=1 failures=0 p50_ms=17.165125 p99_ms=17.165125 max_ms=17.165125
659: handoff: attempts=1 failures=0 p50_ms=20.840458 p99_ms=20.840458 max_ms=20.840458
659: realm: attempts=1 failures=0 p50_ms=7.195375 p99_ms=7.195375 max_ms=7.195375
```

## Transport

- Client dial: TLS/TCP (QUIC candidates are rejected as unsupported)
- Gateway QUIC listener: compiled in (unused by the chain)

## Scope

This profile starts the real Login Verifier, Queue, Gateway, Realm and a
temporary real etcd. It exercises real HTTPS/TLS/TCP wire paths. QUIC wire paths,
production capacity sizing and production account data remain out of scope.
