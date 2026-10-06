# Linux Login Chain M1–M4 acceptance

- Started: 2026-10-06T10:43:23Z
- Platform: Ubuntu 24.04.5 LTS; kernel 6.17.0-1022-azure; x86_64
- CPU: 4 logical; AMD EPYC 9V45 96-Core Processor
- Memory: 16373452 KiB
- File-descriptor limit: 65536
- Revision: `40b75b69ac1f4ad1018a37be9a1caae3b401a95f` (clean working tree)
- Build: preset `dev`; directory `/home/runner/work/RealmMesh/RealmMesh/build/dev-ninja`
- Topology: one Linux host; production service/loadgen paths in the dev-preset build; four independent service processes where required; temporary real etcd
- Transport: real MsQuic server/client exchange plus production Login Chain TLS/TCP fallback
- Repetitions: transport/M1/M2=3; M3/M4=1
- Raw log: `linux-login-chain-m1-m4.log`

## Command

`REALMMESH_ACCEPTANCE_REPEATS=3 REALMMESH_LINUX_ACCEPTANCE_SKIP_BUILD=1 ./scripts/run-linux-login-acceptance.sh --preset dev`

## Acceptance levels

| Level | CI-scale input and gate | Production target from #32 |
|---|---|---|
| Transport | real QUIC verified stream; TLS/TCP staged fallback; certificate failure must not fall back | both Gateway and Realm candidate paths |
| M1 | 100 Gateway sessions for 5s; concurrent multi-stage water; attach P99 <=5x same-load baseline; no fd growth | 100k mixed sessions for 30 min; <=3 GB; no P99 drift/fd/OOM |
| M2 | 250 robots/64 concurrency/25s; >=248 complete; fetch failures <1% | 100k robots handed off in 60s |
| M3 | 10k durable tickets + 2k progress clients; >=99.9% success | 1m tickets; origin <=5k QPS; cache hit >=99% |
| M4 | Gateway/Realm budget-key convergence <=10s; Queue cold recovery <=30s; released number monotonic; no Grant replay | same gates on staged topology |

> The CI profile is a deterministic regression baseline. It reports, but does not claim, the dedicated-host 100k/1m capacity targets. M5 remains out of scope.

## Source configuration

```text
badf4cc0e32b5da4a7c84da777978704f2a37ff71b021332ae2f16b3e94bcfbb  configs/main.config
c2a7f81fc5fbe8598978ad82f0b7fef37f3325d21813bdb889c06a2f0e3d6eb7  configs/common/discovery.lua
f34f4b853456c2fbab99c51a12cd2e9428bb31cfb76b93d8ad9d370026101068  configs/common/player_data.lua
9219d59f3fc2b25daab02b01a575dcd942e79f2c39821931384b29177a91d8b4  configs/services/login_verify.lua
8b1bf0b7741e3b2edf320c601662a9419ad8a1079a0f2963e0605f783044e559  configs/services/queue.lua
4be72b8bbec6b7c58dcff27cac3f673b5f9366ad85f5ce806b4a5008d8cbd371  configs/services/gateway.lua
d2096415e096a6fa731d4b533473af9f8071fac66c98304e9f77b96e84c773ac  configs/services/realm.lua
f5d7d56f439a65029b9b2b9a54f3a6a85723feaadda9c8b1810e49205b3f7ba0  .github/workflows/ci.yml
```

## Results

| Group | Runs | Elapsed | Result |
|---|---:|---:|---|
| Transport and topology | 3 | 15s | PASS |
| M1 directed soak | 3 | 65s | PASS |
| M2 admission and durable issuance | 3 | 34s | PASS |
| M3 ticket and progress load | 1 | 52s | PASS |
| M4 failure and recovery | 1 | 61s | PASS |

## Measured evidence

```text
13: acceptance_runtime login_verify=127.0.0.1:22862 queue=127.0.0.1:22234 gateway=127.0.0.1:22267 realm=127.0.0.1:22389 etcd=http://127.0.0.1:26185 mongodb=127.0.0.1:22781
13: acceptance_runtime_config badf4cc0e32b5da4a7c84da777978704f2a37ff71b021332ae2f16b3e94bcfbb  configs/main.config
13: acceptance_runtime_config 0008f161291f2493b8feef54f3de5f7c3d90c370ddde2be764870e951c0ab99a  configs/common/discovery.lua
13: acceptance_runtime_config 69b5c275f09cbca512ae487c308a2b07accc71404e48b11c3dd5973212203b63  configs/common/accounts.lua
13: acceptance_runtime_config 2b8c54ef77ffd8e6aae2e3b52bdd3b33902bd52f337f8c5e85e28d465c76f18a  configs/common/player_data.lua
13: acceptance_runtime_config 93f89873dfccc0ff11032d6e834de823459a9740d956e948bdba1e901817b9f0  configs/services/login_verify.lua
13: acceptance_runtime_config ac611b1612b3f18badec1c6206d6f1c38db75820bf122e38b4974d868fc3147b  configs/services/queue.lua
13: acceptance_runtime_config b240cb982ef26e586b1213b6dbbb9810e22210ec7e4fbd35e72ee04484afcd18  configs/services/gateway.lua
13: acceptance_runtime_config a2b7d648247f7b8ce4fe0e98b4203fd74bb64608d73cd92021f36dd5263ced71  configs/services/realm.lua
13: loadgen report
13: robots: 1  completed: 1
13: verify: attempts=1 failures=0 p50_ms=179.899810 p99_ms=179.899810 max_ms=179.899810
13: tickets: attempts=1 failures=0 p50_ms=178.423897 p99_ms=178.423897 max_ms=178.423897
13: poll: attempts=31 failures=0 p50_ms=0.111436 p99_ms=49.727087 max_ms=49.727087
13: attach: attempts=1 failures=0 p50_ms=45.654092 p99_ms=45.654092 max_ms=45.654092
13: handoff: attempts=1 failures=0 p50_ms=97.427157 p99_ms=97.427157 max_ms=97.427157
13: realm: attempts=1 failures=0 p50_ms=42.236604 p99_ms=42.236604 max_ms=42.236604
13: acceptance_runtime login_verify=127.0.0.1:27894 queue=127.0.0.1:23005 gateway=127.0.0.1:24029 realm=127.0.0.1:24316 etcd=http://127.0.0.1:23447 mongodb=127.0.0.1:29062
13: acceptance_runtime_config badf4cc0e32b5da4a7c84da777978704f2a37ff71b021332ae2f16b3e94bcfbb  configs/main.config
13: acceptance_runtime_config cccab091128c2b4ad665a557fcc39d737dee517435e34a6611402c32f45c511d  configs/common/discovery.lua
13: acceptance_runtime_config 69b5c275f09cbca512ae487c308a2b07accc71404e48b11c3dd5973212203b63  configs/common/accounts.lua
13: acceptance_runtime_config a519454c0ecc736a3ac75ad6076e2c6d4b80e2e1dca26111e8813ebb92f506cc  configs/common/player_data.lua
13: acceptance_runtime_config 6f24e3306ac1fbcd0c542a95868d7866c15f6edaddef6305cd8bbe11506e6d7e  configs/services/login_verify.lua
13: acceptance_runtime_config 59831b7b1f1795dc78801b9bd2f4b76eb6324debe2b6d1462ef08e73a6d514c5  configs/services/queue.lua
13: acceptance_runtime_config c71c6aa603762e849695915d1ab1f56e7003ae3c04c315eb9dc91ea85a18d39a  configs/services/gateway.lua
13: acceptance_runtime_config 49a51fd06c65ccc9df283ded153c38e787f6578c85c0d212edac585e2a8a9355  configs/services/realm.lua
13: loadgen report
13: robots: 1  completed: 1
13: verify: attempts=1 failures=0 p50_ms=177.085584 p99_ms=177.085584 max_ms=177.085584
13: tickets: attempts=1 failures=0 p50_ms=180.194393 p99_ms=180.194393 max_ms=180.194393
13: poll: attempts=31 failures=0 p50_ms=0.107270 p99_ms=49.552255 max_ms=49.552255
13: attach: attempts=1 failures=0 p50_ms=45.373557 p99_ms=45.373557 max_ms=45.373557
13: handoff: attempts=1 failures=0 p50_ms=97.369462 p99_ms=97.369462 max_ms=97.369462
13: realm: attempts=1 failures=0 p50_ms=41.098098 p99_ms=41.098098 max_ms=41.098098
13: acceptance_runtime login_verify=127.0.0.1:25567 queue=127.0.0.1:23135 gateway=127.0.0.1:24327 realm=127.0.0.1:22684 etcd=http://127.0.0.1:24868 mongodb=127.0.0.1:23549
13: acceptance_runtime_config badf4cc0e32b5da4a7c84da777978704f2a37ff71b021332ae2f16b3e94bcfbb  configs/main.config
13: acceptance_runtime_config a2787136b51b5ac2334f0d0fe72fcafc346380e222ded162d3dff2f213f752c8  configs/common/discovery.lua
13: acceptance_runtime_config 69b5c275f09cbca512ae487c308a2b07accc71404e48b11c3dd5973212203b63  configs/common/accounts.lua
13: acceptance_runtime_config 498876c04d193c0cef39938ec45473e70961ff6858da25c512f3524b6950a119  configs/common/player_data.lua
13: acceptance_runtime_config b26619494a739d6cc9dd07d19d6ba06972c56cab02d022dc46fa27a85695a72e  configs/services/login_verify.lua
13: acceptance_runtime_config ebee761a82297fdf961ac39479513512dc55fc3b1a48579e1e96f7d7689a9e6c  configs/services/queue.lua
13: acceptance_runtime_config c399b5de5998c8bd001d3a36d9d77ce3e22aa054a720513ccb92ecddf9becc67  configs/services/gateway.lua
13: acceptance_runtime_config 5f838958419c5e89ea0101bff5a7de0235abbcc07597f1e346e5e34c87a40ce2  configs/services/realm.lua
13: loadgen report
13: robots: 1  completed: 1
13: verify: attempts=1 failures=0 p50_ms=181.384160 p99_ms=181.384160 max_ms=181.384160
13: tickets: attempts=1 failures=0 p50_ms=175.560786 p99_ms=175.560786 max_ms=175.560786
13: poll: attempts=31 failures=0 p50_ms=0.106669 p99_ms=49.618934 max_ms=49.618934
13: attach: attempts=1 failures=0 p50_ms=49.508694 p99_ms=49.508694 max_ms=49.508694
13: handoff: attempts=1 failures=0 p50_ms=97.427288 p99_ms=97.427288 max_ms=97.427288
13: realm: attempts=1 failures=0 p50_ms=42.016085 p99_ms=42.016085 max_ms=42.016085
655: acceptance_m1 gateway_soak completed=100 attach_failures=0 handoff_failures=0 capacity=20000 max_pending=0 max_fetching=84 max_handed_off=100 mixed_water_sample=1 water_samples=778 baseline_attach_p50_ms=191.253 main_attach_p50_ms=195.748 baseline_attach_p99_ms=218.117 main_attach_p99_ms=301.294 fd_before=48 fd_after=48
655: acceptance_m1 gateway_soak completed=100 attach_failures=0 handoff_failures=0 capacity=20000 max_pending=0 max_fetching=72 max_handed_off=100 mixed_water_sample=1 water_samples=719 baseline_attach_p50_ms=243.205 main_attach_p50_ms=178.325 baseline_attach_p99_ms=300.941 main_attach_p99_ms=260.455 fd_before=48 fd_after=48
655: acceptance_m1 gateway_soak completed=100 attach_failures=0 handoff_failures=0 capacity=20000 max_pending=0 max_fetching=73 max_handed_off=100 mixed_water_sample=1 water_samples=707 baseline_attach_p50_ms=196.775 main_attach_p50_ms=186.516 baseline_attach_p99_ms=231.767 main_attach_p99_ms=251.731 fd_before=48 fd_after=48
657: loadgen report
657: robots: 1  completed: 1
657: verify: attempts=1 failures=0 p50_ms=50.312042 p99_ms=50.312042 max_ms=50.312042
657: tickets: attempts=1 failures=0 p50_ms=51.997431 p99_ms=51.997431 max_ms=51.997431
657: poll: attempts=37 failures=0 p50_ms=3.780298 p99_ms=6.371516 max_ms=6.371516
657: attach: attempts=1 failures=0 p50_ms=40.820952 p99_ms=40.820952 max_ms=40.820952
657: handoff: attempts=1 failures=0 p50_ms=0.028403 p99_ms=0.028403 max_ms=0.028403
657: realm: attempts=1 failures=0 p50_ms=41.910885 p99_ms=41.910885 max_ms=41.910885
657: loadgen report
657: robots: 1  completed: 1
657: verify: attempts=1 failures=0 p50_ms=50.524852 p99_ms=50.524852 max_ms=50.524852
657: tickets: attempts=1 failures=0 p50_ms=51.980289 p99_ms=51.980289 max_ms=51.980289
657: poll: attempts=37 failures=0 p50_ms=3.787840 p99_ms=5.388133 max_ms=5.388133
657: attach: attempts=1 failures=0 p50_ms=41.282600 p99_ms=41.282600 max_ms=41.282600
657: handoff: attempts=1 failures=0 p50_ms=0.028282 p99_ms=0.028282 max_ms=0.028282
657: realm: attempts=1 failures=0 p50_ms=41.931937 p99_ms=41.931937 max_ms=41.931937
657: loadgen report
657: robots: 1  completed: 1
657: verify: attempts=1 failures=0 p50_ms=50.471277 p99_ms=50.471277 max_ms=50.471277
657: tickets: attempts=1 failures=0 p50_ms=51.993378 p99_ms=51.993378 max_ms=51.993378
657: poll: attempts=37 failures=0 p50_ms=3.793519 p99_ms=5.413060 max_ms=5.413060
657: attach: attempts=1 failures=0 p50_ms=41.762364 p99_ms=41.762364 max_ms=41.762364
657: handoff: attempts=1 failures=0 p50_ms=0.029414 p99_ms=0.029414 max_ms=0.029414
657: realm: attempts=1 failures=0 p50_ms=41.921512 p99_ms=41.921512 max_ms=41.921512
592: queue issuance write load: 1395.65 committed numbers/s
592: queue issuance write load: 1394.76 committed numbers/s
592: queue issuance write load: 1367.16 committed numbers/s
659: acceptance_m2 robots=250 completed=250 skipped=0 attach_attempts=250 attach_failures=0 handoff_attempts=250 handoff_failures=0 fetch_count=250 fetch_retries=0 fetch_failure_rate=0
659: acceptance_m2 robots=250 completed=250 skipped=0 attach_attempts=250 attach_failures=0 handoff_attempts=250 handoff_failures=0 fetch_count=250 fetch_retries=0 fetch_failure_rate=0
659: acceptance_m2 robots=250 completed=250 skipped=0 attach_attempts=250 attach_failures=0 handoff_attempts=250 handoff_failures=0 fetch_count=250 fetch_retries=0 fetch_failure_rate=0
660: acceptance_m3 tickets_robots=10000 tickets_completed=10000 tickets_skipped=0 poll_robots=2000 poll_completed=2000 poll_skipped=0 poll_attempts=29855
14: acceptance_fault gateway_budget_removal elapsed_s=5 threshold_s=10 result=PASS
14: loadgen report
14: robots: 1  completed: 0
14: verify: attempts=1 failures=0 p50_ms=173.335022 p99_ms=173.335022 max_ms=173.335022
14: tickets: attempts=1 failures=0 p50_ms=185.231767 p99_ms=185.231767 max_ms=185.231767
14: poll: attempts=149 failures=7 p50_ms=0.103544 p99_ms=48.038445 max_ms=48.041200
14: loadgen report
14: robots: 1  completed: 1
14: verify: attempts=1 failures=0 p50_ms=198.423533 p99_ms=198.423533 max_ms=198.423533
14: tickets: attempts=1 failures=0 p50_ms=185.272953 p99_ms=185.272953 max_ms=185.272953
14: poll: attempts=14 failures=0 p50_ms=0.106309 p99_ms=50.247275 max_ms=50.247275
14: attach: attempts=1 failures=0 p50_ms=41.042972 p99_ms=41.042972 max_ms=41.042972
14: handoff: attempts=1 failures=0 p50_ms=72.187531 p99_ms=72.187531 max_ms=72.187531
14: realm: attempts=1 failures=0 p50_ms=41.756996 p99_ms=41.756996 max_ms=41.756996
14: acceptance_fault gateway_budget_restore elapsed_s=1 threshold_s=10 result=PASS
14: acceptance_fault realm_budget_removal elapsed_s=1 threshold_s=10 result=PASS
14: loadgen report
14: robots: 1  completed: 0
14: verify: attempts=1 failures=0 p50_ms=188.429539 p99_ms=188.429539 max_ms=188.429539
14: tickets: attempts=1 failures=0 p50_ms=185.272267 p99_ms=185.272267 max_ms=185.272267
14: poll: attempts=153 failures=11 p50_ms=0.108572 p99_ms=48.042572 max_ms=48.044214
14: loadgen report
14: robots: 1  completed: 1
14: verify: attempts=1 failures=0 p50_ms=159.651254 p99_ms=159.651254 max_ms=159.651254
14: tickets: attempts=1 failures=0 p50_ms=185.269476 p99_ms=185.269476 max_ms=185.269476
14: poll: attempts=20 failures=0 p50_ms=0.121080 p99_ms=48.945306 max_ms=48.945306
14: attach: attempts=1 failures=0 p50_ms=41.726241 p99_ms=41.726241 max_ms=41.726241
14: handoff: attempts=1 failures=0 p50_ms=72.502065 p99_ms=72.502065 max_ms=72.502065
14: realm: attempts=1 failures=0 p50_ms=41.440381 p99_ms=41.440381 max_ms=41.440381
14: acceptance_fault realm_budget_restore elapsed_s=1 threshold_s=10 result=PASS
14: loadgen report
14: robots: 1  completed: 1
14: verify: attempts=1 failures=0 p50_ms=151.951585 p99_ms=151.951585 max_ms=151.951585
14: tickets: attempts=1 failures=0 p50_ms=151.973818 p99_ms=151.973818 max_ms=151.973818
14: poll: attempts=1 failures=0 p50_ms=9.999971 p99_ms=9.999971 max_ms=9.999971
14: attach: attempts=1 failures=0 p50_ms=40.955938 p99_ms=40.955938 max_ms=40.955938
14: handoff: attempts=1 failures=0 p50_ms=72.287786 p99_ms=72.287786 max_ms=72.287786
14: realm: attempts=1 failures=0 p50_ms=41.658685 p99_ms=41.658685 max_ms=41.658685
14: acceptance_fault queue_released_number before=5 after=5 result=PASS
14: loadgen report
14: robots: 1  completed: 1
14: verify: attempts=1 failures=0 p50_ms=175.387106 p99_ms=175.387106 max_ms=175.387106
14: tickets: attempts=1 failures=0 p50_ms=158.971330 p99_ms=158.971330 max_ms=158.971330
14: poll: attempts=32 failures=0 p50_ms=0.111657 p99_ms=49.305983 max_ms=49.305983
14: attach: attempts=1 failures=0 p50_ms=41.394597 p99_ms=41.394597 max_ms=41.394597
14: handoff: attempts=1 failures=0 p50_ms=65.559147 p99_ms=65.559147 max_ms=65.559147
14: realm: attempts=1 failures=0 p50_ms=41.379360 p99_ms=41.379360 max_ms=41.379360
14: acceptance_fault queue_cold_restart elapsed_s=7 threshold_s=30 result=PASS
14: acceptance_fault etcd_outage_unready elapsed_s=1 threshold_s=10 result=PASS
14: loadgen report
14: robots: 1  completed: 0
14: verify: attempts=1 failures=0 p50_ms=188.525011 p99_ms=188.525011 max_ms=188.525011
14: tickets: attempts=1 failures=1 p50_ms=4425.630683 p99_ms=4425.630683 max_ms=4425.630683
14: acceptance_fault etcd_recovery elapsed_s=1 threshold_s=10 result=PASS
14: loadgen report
14: robots: 1  completed: 1
14: verify: attempts=1 failures=0 p50_ms=159.991027 p99_ms=159.991027 max_ms=159.991027
14: tickets: attempts=1 failures=0 p50_ms=179.963358 p99_ms=179.963358 max_ms=179.963358
14: poll: attempts=32 failures=0 p50_ms=0.110995 p99_ms=49.080657 max_ms=49.080657
14: attach: attempts=1 failures=0 p50_ms=41.463644 p99_ms=41.463644 max_ms=41.463644
14: handoff: attempts=1 failures=0 p50_ms=95.044854 p99_ms=95.044854 max_ms=95.044854
14: realm: attempts=1 failures=0 p50_ms=41.895757 p99_ms=41.895757 max_ms=41.895757
14: acceptance_fault queue_authoritative_recovery elapsed_s=2 threshold_s=30 result=PASS
```

## Limitations

- CI runs on one GitHub-hosted Ubuntu 24.04 machine; it does not prove cross-machine network or etcd TLS/authentication behavior.
- Accounts and characters come from MongoDB Player Data (ADR-0011): each case starts its own single-node `rs0` replica set on the runner, and Login Verifier authenticates, Gateway fetches and Realm rechecks against the same database. The replica set is local and unauthenticated, so this does not prove MongoDB TLS/authentication or multi-member failover. Loadgen capacity fixtures (thousands of robot accounts) use `credential_hash_cost = "minimum"`, so their Login Verify CPU cost is below a production Argon2 profile; single-account dev-services cases keep the default `interactive` cost.
- Results are not comparable with pre-#92 reports, whose Gateway fetch path was the `DelayedAccountFetchPort` controlled stub.
- CDN cache hit rate and the full 100k/1m production sizes require a separately approved dedicated environment; M5 remains separate.
