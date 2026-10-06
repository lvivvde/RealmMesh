# Linux Login Chain M1–M4 acceptance

- Started: 2026-10-06T11:12:47Z
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
| Transport and topology | 3 | 14s | PASS |
| M1 directed soak | 3 | 66s | PASS |
| M2 admission and durable issuance | 3 | 34s | PASS |
| M3 ticket and progress load | 1 | 52s | PASS |
| M4 failure and recovery | 1 | 64s | PASS |

## Measured evidence

```text
13: acceptance_runtime login_verify=127.0.0.1:25601 queue=127.0.0.1:26807 gateway=127.0.0.1:26034 realm=127.0.0.1:22114 etcd=http://127.0.0.1:29171 mongodb=127.0.0.1:27176
13: acceptance_runtime_config badf4cc0e32b5da4a7c84da777978704f2a37ff71b021332ae2f16b3e94bcfbb  configs/main.config
13: acceptance_runtime_config 426e02bfbb6c3b503664db1e76de4ecf679b61e2caf98bd2b35bce8551cb2012  configs/common/discovery.lua
13: acceptance_runtime_config 69b5c275f09cbca512ae487c308a2b07accc71404e48b11c3dd5973212203b63  configs/common/accounts.lua
13: acceptance_runtime_config ac933031d61cc6bfe84f71ee6ac9233ca3a302cc34a04131b10762779c1e8921  configs/common/player_data.lua
13: acceptance_runtime_config 7ab27e66533cb6e40e1e5a5239f6927c81465fe6928c7d186519b42c93390047  configs/services/login_verify.lua
13: acceptance_runtime_config 046632f4cddd54b00980496b0ae5faf1b8fcbbf35f88eeca0568f5d13ecba9d3  configs/services/queue.lua
13: acceptance_runtime_config 3ef32ecae5c16987ae91e4f4f74c171fffb246a5f155bfb57108308dc2f881e5  configs/services/gateway.lua
13: acceptance_runtime_config e43d2958ac7327ed752a3ba0de28c28be85962bc122ff59c63bae7217e6e158d  configs/services/realm.lua
13: loadgen report
13: robots: 1  completed: 1
13: verify: attempts=1 failures=0 p50_ms=168.454244 p99_ms=168.454244 max_ms=168.454244
13: tickets: attempts=1 failures=0 p50_ms=183.347151 p99_ms=183.347151 max_ms=183.347151
13: poll: attempts=31 failures=0 p50_ms=0.105337 p99_ms=49.656283 max_ms=49.656283
13: attach: attempts=1 failures=0 p50_ms=41.589203 p99_ms=41.589203 max_ms=41.589203
13: handoff: attempts=1 failures=0 p50_ms=84.364362 p99_ms=84.364362 max_ms=84.364362
13: realm: attempts=1 failures=0 p50_ms=41.576795 p99_ms=41.576795 max_ms=41.576795
13: acceptance_runtime login_verify=127.0.0.1:22273 queue=127.0.0.1:23959 gateway=127.0.0.1:22722 realm=127.0.0.1:29140 etcd=http://127.0.0.1:23860 mongodb=127.0.0.1:21928
13: acceptance_runtime_config badf4cc0e32b5da4a7c84da777978704f2a37ff71b021332ae2f16b3e94bcfbb  configs/main.config
13: acceptance_runtime_config 52501342afc1060c39d122ff77fc9ee799521c23cbc1f3fcc1d88966290ae267  configs/common/discovery.lua
13: acceptance_runtime_config 69b5c275f09cbca512ae487c308a2b07accc71404e48b11c3dd5973212203b63  configs/common/accounts.lua
13: acceptance_runtime_config d0d7de5730b9d9b6b5942297fb2d3101d6d3567249bbccaa96f17228b4f627ce  configs/common/player_data.lua
13: acceptance_runtime_config 96d735ce96f07a0a12027a9ad185aeaa8b3a892a29889745211fe34ccdaf2b78  configs/services/login_verify.lua
13: acceptance_runtime_config e21f5676ab53a59bbf579177d66642ddc26db268004c638532edf87ba48fbdbe  configs/services/queue.lua
13: acceptance_runtime_config 11442f38b6f35d53b40445cb0fb1b2a6b5d65600f81d0bb085b678495a2996f9  configs/services/gateway.lua
13: acceptance_runtime_config 17eb6a872c4487f03f7c7081010194067a42c8075e5e0d100d6dfaeb79b2d58d  configs/services/realm.lua
13: loadgen report
13: robots: 1  completed: 1
13: verify: attempts=1 failures=0 p50_ms=174.775540 p99_ms=174.775540 max_ms=174.775540
13: tickets: attempts=1 failures=0 p50_ms=181.523336 p99_ms=181.523336 max_ms=181.523336
13: poll: attempts=31 failures=0 p50_ms=0.115883 p99_ms=49.496039 max_ms=49.496039
13: attach: attempts=1 failures=0 p50_ms=41.651733 p99_ms=41.651733 max_ms=41.651733
13: handoff: attempts=1 failures=0 p50_ms=77.862747 p99_ms=77.862747 max_ms=77.862747
13: realm: attempts=1 failures=0 p50_ms=42.080130 p99_ms=42.080130 max_ms=42.080130
13: acceptance_runtime login_verify=127.0.0.1:27233 queue=127.0.0.1:27073 gateway=127.0.0.1:22511 realm=127.0.0.1:21505 etcd=http://127.0.0.1:28916 mongodb=127.0.0.1:21755
13: acceptance_runtime_config badf4cc0e32b5da4a7c84da777978704f2a37ff71b021332ae2f16b3e94bcfbb  configs/main.config
13: acceptance_runtime_config 174b44d8747ba5025a13a827ccf41fbd581ad8a0e7675f73f3dedd142c09fbed  configs/common/discovery.lua
13: acceptance_runtime_config 69b5c275f09cbca512ae487c308a2b07accc71404e48b11c3dd5973212203b63  configs/common/accounts.lua
13: acceptance_runtime_config 63de8c3fb8c0c05a28392c40128e2930fbb38814f3e84806ab83abc7c81d0d31  configs/common/player_data.lua
13: acceptance_runtime_config e1b1817004500af5398293f12d0668888f791e313070f8c5466762d0cb84e2b8  configs/services/login_verify.lua
13: acceptance_runtime_config 2aa8531b780ad0958a1bd70bb34cf21e8ebe2ae2d46b8d789724df1fa218d126  configs/services/queue.lua
13: acceptance_runtime_config 8ffe4abf7601f421d5a6f9b79850a45f5c7ef0ce10757da17e9f774933249b06  configs/services/gateway.lua
13: acceptance_runtime_config 1fb4930524b3ede147d0b605cc4f3f8ed9abd2a1eb876359cb8e6a5e5f9a918c  configs/services/realm.lua
13: loadgen report
13: robots: 1  completed: 1
13: verify: attempts=1 failures=0 p50_ms=176.780255 p99_ms=176.780255 max_ms=176.780255
13: tickets: attempts=1 failures=0 p50_ms=180.032475 p99_ms=180.032475 max_ms=180.032475
13: poll: attempts=31 failures=0 p50_ms=0.102362 p99_ms=49.781955 max_ms=49.781955
13: attach: attempts=1 failures=0 p50_ms=47.568640 p99_ms=47.568640 max_ms=47.568640
13: handoff: attempts=1 failures=0 p50_ms=99.537732 p99_ms=99.537732 max_ms=99.537732
13: realm: attempts=1 failures=0 p50_ms=41.682488 p99_ms=41.682488 max_ms=41.682488
655: acceptance_m1 gateway_soak completed=100 attach_failures=0 handoff_failures=0 capacity=20000 max_pending=0 max_fetching=50 max_handed_off=100 mixed_water_sample=1 water_samples=814 baseline_attach_p50_ms=195.867 main_attach_p50_ms=131.702 baseline_attach_p99_ms=222.775 main_attach_p99_ms=182.954 fd_before=48 fd_after=48
655: acceptance_m1 gateway_soak completed=100 attach_failures=0 handoff_failures=0 capacity=20000 max_pending=1 max_fetching=63 max_handed_off=100 mixed_water_sample=1 water_samples=721 baseline_attach_p50_ms=157.766 main_attach_p50_ms=208.486 baseline_attach_p99_ms=263.078 main_attach_p99_ms=230.45 fd_before=48 fd_after=48
655: acceptance_m1 gateway_soak completed=100 attach_failures=0 handoff_failures=0 capacity=20000 max_pending=0 max_fetching=79 max_handed_off=100 mixed_water_sample=1 water_samples=723 baseline_attach_p50_ms=160.695 main_attach_p50_ms=229.201 baseline_attach_p99_ms=220.91 main_attach_p99_ms=277.004 fd_before=48 fd_after=48
657: loadgen report
657: robots: 1  completed: 1
657: verify: attempts=1 failures=0 p50_ms=49.955239 p99_ms=49.955239 max_ms=49.955239
657: tickets: attempts=1 failures=0 p50_ms=52.989838 p99_ms=52.989838 max_ms=52.989838
657: poll: attempts=37 failures=0 p50_ms=3.781409 p99_ms=5.421726 max_ms=5.421726
657: attach: attempts=1 failures=0 p50_ms=41.166333 p99_ms=41.166333 max_ms=41.166333
657: handoff: attempts=1 failures=0 p50_ms=0.027370 p99_ms=0.027370 max_ms=0.027370
657: realm: attempts=1 failures=0 p50_ms=41.926706 p99_ms=41.926706 max_ms=41.926706
657: loadgen report
657: robots: 1  completed: 1
657: verify: attempts=1 failures=0 p50_ms=50.008513 p99_ms=50.008513 max_ms=50.008513
657: tickets: attempts=1 failures=0 p50_ms=51.990995 p99_ms=51.990995 max_ms=51.990995
657: poll: attempts=37 failures=0 p50_ms=3.791625 p99_ms=5.434811 max_ms=5.434811
657: attach: attempts=1 failures=0 p50_ms=41.537877 p99_ms=41.537877 max_ms=41.537877
657: handoff: attempts=1 failures=0 p50_ms=0.028492 p99_ms=0.028492 max_ms=0.028492
657: realm: attempts=1 failures=0 p50_ms=41.927922 p99_ms=41.927922 max_ms=41.927922
657: loadgen report
657: robots: 1  completed: 1
657: verify: attempts=1 failures=0 p50_ms=49.664461 p99_ms=49.664461 max_ms=49.664461
657: tickets: attempts=1 failures=0 p50_ms=52.991815 p99_ms=52.991815 max_ms=52.991815
657: poll: attempts=38 failures=0 p50_ms=3.779517 p99_ms=4.374623 max_ms=4.374623
657: attach: attempts=1 failures=0 p50_ms=41.092402 p99_ms=41.092402 max_ms=41.092402
657: handoff: attempts=1 failures=0 p50_ms=0.029594 p99_ms=0.029594 max_ms=0.029594
657: realm: attempts=1 failures=0 p50_ms=41.920816 p99_ms=41.920816 max_ms=41.920816
592: queue issuance write load: 1399.21 committed numbers/s
592: queue issuance write load: 1389.15 committed numbers/s
592: queue issuance write load: 1348.37 committed numbers/s
659: acceptance_m2 robots=250 completed=250 skipped=0 attach_attempts=250 attach_failures=0 handoff_attempts=250 handoff_failures=0 fetch_count=250 fetch_retries=0 fetch_failure_rate=0
659: acceptance_m2 robots=250 completed=250 skipped=0 attach_attempts=250 attach_failures=0 handoff_attempts=250 handoff_failures=0 fetch_count=250 fetch_retries=0 fetch_failure_rate=0
659: acceptance_m2 robots=250 completed=250 skipped=0 attach_attempts=250 attach_failures=0 handoff_attempts=250 handoff_failures=0 fetch_count=250 fetch_retries=0 fetch_failure_rate=0
660: acceptance_m3 tickets_robots=10000 tickets_completed=10000 tickets_skipped=0 poll_robots=2000 poll_completed=2000 poll_skipped=0 poll_attempts=30189
14: acceptance_fault gateway_budget_removal elapsed_s=5 threshold_s=10 result=PASS
14: loadgen report
14: robots: 1  completed: 0
14: verify: attempts=1 failures=0 p50_ms=193.134420 p99_ms=193.134420 max_ms=193.134420
14: tickets: attempts=1 failures=0 p50_ms=181.358767 p99_ms=181.358767 max_ms=181.358767
14: poll: attempts=1 failures=0 p50_ms=9.167398 p99_ms=9.167398 max_ms=9.167398
14: attach: attempts=42 failures=42 p50_ms=0.000000 p99_ms=0.000000 max_ms=0.000000
14: loadgen report
14: robots: 1  completed: 1
14: verify: attempts=1 failures=0 p50_ms=168.136825 p99_ms=168.136825 max_ms=168.136825
14: tickets: attempts=1 failures=0 p50_ms=181.358265 p99_ms=181.358265 max_ms=181.358265
14: poll: attempts=40 failures=0 p50_ms=0.116022 p99_ms=49.825679 max_ms=49.825679
14: attach: attempts=1 failures=0 p50_ms=53.878724 p99_ms=53.878724 max_ms=53.878724
14: handoff: attempts=1 failures=0 p50_ms=97.374097 p99_ms=97.374097 max_ms=97.374097
14: realm: attempts=1 failures=0 p50_ms=41.272273 p99_ms=41.272273 max_ms=41.272273
14: acceptance_fault gateway_budget_restore elapsed_s=3 threshold_s=10 result=PASS
14: acceptance_fault realm_budget_removal elapsed_s=0 threshold_s=10 result=PASS
14: loadgen report
14: robots: 1  completed: 0
14: verify: attempts=1 failures=0 p50_ms=186.939722 p99_ms=186.939722 max_ms=186.939722
14: tickets: attempts=1 failures=0 p50_ms=181.331185 p99_ms=181.331185 max_ms=181.331185
14: poll: attempts=155 failures=13 p50_ms=0.105897 p99_ms=48.069494 max_ms=48.072729
14: loadgen report
14: robots: 1  completed: 1
14: verify: attempts=1 failures=0 p50_ms=157.243030 p99_ms=157.243030 max_ms=157.243030
14: tickets: attempts=1 failures=0 p50_ms=181.356301 p99_ms=181.356301 max_ms=181.356301
14: poll: attempts=17 failures=0 p50_ms=0.115312 p99_ms=49.286983 max_ms=49.286983
14: attach: attempts=1 failures=0 p50_ms=53.882045 p99_ms=53.882045 max_ms=53.882045
14: handoff: attempts=1 failures=0 p50_ms=97.467249 p99_ms=97.467249 max_ms=97.467249
14: realm: attempts=1 failures=0 p50_ms=41.315775 p99_ms=41.315775 max_ms=41.315775
14: acceptance_fault realm_budget_restore elapsed_s=2 threshold_s=10 result=PASS
14: loadgen report
14: robots: 1  completed: 1
14: verify: attempts=1 failures=0 p50_ms=152.235725 p99_ms=152.235725 max_ms=152.235725
14: tickets: attempts=1 failures=0 p50_ms=187.668285 p99_ms=187.668285 max_ms=187.668285
14: poll: attempts=15 failures=0 p50_ms=0.112116 p99_ms=50.133164 max_ms=50.133164
14: attach: attempts=1 failures=0 p50_ms=53.853664 p99_ms=53.853664 max_ms=53.853664
14: handoff: attempts=1 failures=0 p50_ms=97.428622 p99_ms=97.428622 max_ms=97.428622
14: realm: attempts=1 failures=0 p50_ms=41.292206 p99_ms=41.292206 max_ms=41.292206
14: acceptance_fault queue_released_number before=5 after=5 result=PASS
14: loadgen report
14: robots: 1  completed: 1
14: verify: attempts=1 failures=0 p50_ms=176.630560 p99_ms=176.630560 max_ms=176.630560
14: tickets: attempts=1 failures=0 p50_ms=158.680874 p99_ms=158.680874 max_ms=158.680874
14: poll: attempts=32 failures=0 p50_ms=0.109694 p99_ms=49.309226 max_ms=49.309226
14: attach: attempts=1 failures=0 p50_ms=41.353112 p99_ms=41.353112 max_ms=41.353112
14: handoff: attempts=1 failures=0 p50_ms=88.766432 p99_ms=88.766432 max_ms=88.766432
14: realm: attempts=1 failures=0 p50_ms=41.176198 p99_ms=41.176198 max_ms=41.176198
14: acceptance_fault queue_cold_restart elapsed_s=7 threshold_s=30 result=PASS
14: acceptance_fault etcd_outage_unready elapsed_s=2 threshold_s=10 result=PASS
14: loadgen report
14: robots: 1  completed: 0
14: verify: attempts=1 failures=0 p50_ms=182.917932 p99_ms=182.917932 max_ms=182.917932
14: tickets: attempts=1 failures=1 p50_ms=4076.047994 p99_ms=4076.047994 max_ms=4076.047994
14: acceptance_fault etcd_recovery elapsed_s=0 threshold_s=10 result=PASS
14: loadgen report
14: robots: 1  completed: 1
14: verify: attempts=1 failures=0 p50_ms=188.918777 p99_ms=188.918777 max_ms=188.918777
14: tickets: attempts=1 failures=0 p50_ms=150.682605 p99_ms=150.682605 max_ms=150.682605
14: poll: attempts=32 failures=0 p50_ms=0.106919 p99_ms=49.505010 max_ms=49.505010
14: attach: attempts=1 failures=0 p50_ms=40.993736 p99_ms=40.993736 max_ms=40.993736
14: handoff: attempts=1 failures=0 p50_ms=95.542585 p99_ms=95.542585 max_ms=95.542585
14: realm: attempts=1 failures=0 p50_ms=41.376714 p99_ms=41.376714 max_ms=41.376714
14: acceptance_fault queue_authoritative_recovery elapsed_s=3 threshold_s=30 result=PASS
```

## Limitations

- CI runs on one GitHub-hosted Ubuntu 24.04 machine; it does not prove cross-machine network or etcd TLS/authentication behavior.
- Accounts and characters come from MongoDB Player Data (ADR-0011): each case starts its own single-node `rs0` replica set on the runner, and Login Verifier authenticates, Gateway fetches and Realm rechecks against the same database. The replica set is local and unauthenticated, so this does not prove MongoDB TLS/authentication or multi-member failover. Loadgen capacity fixtures (thousands of robot accounts) use `credential_hash_cost = "minimum"`, so their Login Verify CPU cost is below a production Argon2 profile; single-account dev-services cases keep the default `interactive` cost.
- Results are not comparable with pre-#92 reports, whose Gateway fetch path was the `DelayedAccountFetchPort` controlled stub.
- CDN cache hit rate and the full 100k/1m production sizes require a separately approved dedicated environment; M5 remains separate.
