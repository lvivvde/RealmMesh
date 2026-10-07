# Linux Login Chain M1–M4 acceptance

- Started: 2026-10-07T11:58:16Z
- Platform: Ubuntu 26.04.1 LTS; kernel 7.0.0-34-generic; aarch64
- CPU: 8 logical; unknown
- Memory: 8103496 KiB
- File-descriptor limit: 1024
- Revision: `7858a925e10a85f6d190e34356cf8bfeb02ff12b` (dirty working tree)
- Build: preset `dev`; directory `/home/edwin.guest/code/.bench/p6-clock-fixed-20261007/final/build/dev-ninja`
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
| M1 directed soak | 3 | 67s | PASS |
| M2 admission and durable issuance | 3 | 34s | PASS |
| M3 ticket and progress load | 1 | 53s | PASS |
| M4 failure and recovery | 1 | 61s | PASS |

## Measured evidence

```text
16: acceptance_runtime login_verify=127.0.0.1:27096 queue=127.0.0.1:27852 gateway=127.0.0.1:26402 realm=127.0.0.1:29815 etcd=http://127.0.0.1:28379 mongodb=127.0.0.1:29106
16: acceptance_runtime_config badf4cc0e32b5da4a7c84da777978704f2a37ff71b021332ae2f16b3e94bcfbb  configs/main.config
16: acceptance_runtime_config d24b983ee143af2a5a65d8172122a1a934e91c4cc9768b7d80e3a593a157cba1  configs/common/discovery.lua
16: acceptance_runtime_config 69b5c275f09cbca512ae487c308a2b07accc71404e48b11c3dd5973212203b63  configs/common/accounts.lua
16: acceptance_runtime_config 7a42dc46926297894fbf1783556d5f7f5443f0c11dbaa6ba36b226124557ea99  configs/common/player_data.lua
16: acceptance_runtime_config 5cc08b229cb9c52472d3657e4761ebb6e7b0982b52de40ce1fdc527687e19465  configs/services/login_verify.lua
16: acceptance_runtime_config af6c57d93ced3b4c64a9e90140a6b73d08a31ae965aab346d0ec8d315ddf1dde  configs/services/queue.lua
16: acceptance_runtime_config 89bd55ddbc89189d0d3ee6741ae9c137f9875e4657ef200c0ceda541d9937ca7  configs/services/gateway.lua
16: acceptance_runtime_config a475f0ccff700f71914101f5bfba0fa0ffc7a81c2cf212f3acf142c1cbf358f5  configs/services/realm.lua
16: loadgen report
16: robots: 1  completed: 1
16: verify: attempts=1 failures=0 p50_ms=160.794542 p99_ms=160.794542 max_ms=160.794542
16: tickets: attempts=1 failures=0 p50_ms=147.474083 p99_ms=147.474083 max_ms=147.474083
16: poll: attempts=23 failures=0 p50_ms=12.891333 p99_ms=52.373625 max_ms=52.373625
16: attach: attempts=1 failures=0 p50_ms=41.240042 p99_ms=41.240042 max_ms=41.240042
16: handoff: attempts=1 failures=0 p50_ms=79.499500 p99_ms=79.499500 max_ms=79.499500
16: realm: attempts=1 failures=0 p50_ms=45.505375 p99_ms=45.505375 max_ms=45.505375
16: acceptance_runtime login_verify=127.0.0.1:25492 queue=127.0.0.1:29518 gateway=127.0.0.1:21017 realm=127.0.0.1:22206 etcd=http://127.0.0.1:27573 mongodb=127.0.0.1:21653
16: acceptance_runtime_config badf4cc0e32b5da4a7c84da777978704f2a37ff71b021332ae2f16b3e94bcfbb  configs/main.config
16: acceptance_runtime_config 350f89d1c447aa65dcc8d5c105b5261d1aa833457ff27a526e2b6c7d8c346977  configs/common/discovery.lua
16: acceptance_runtime_config 69b5c275f09cbca512ae487c308a2b07accc71404e48b11c3dd5973212203b63  configs/common/accounts.lua
16: acceptance_runtime_config 896fd1f31a9065fe5c62d6e2bfeb2110432c6a9bbcb104212443bb67b4671df2  configs/common/player_data.lua
16: acceptance_runtime_config c1d461852b8f4eda56fe9e0c3b98e2c5140508216bc479690d9d7ab3d29b0908  configs/services/login_verify.lua
16: acceptance_runtime_config ebe757caa9282b69a1cbfbd2d0175e20802de6fe08acbc1b83dd167def2536ef  configs/services/queue.lua
16: acceptance_runtime_config 8286a1c43051beb8d69a6bfa69a819e2f34cf8d1a5972a09c54aed0e2dabd54a  configs/services/gateway.lua
16: acceptance_runtime_config 511821452194a0a0a35f2cf19c07c1a303c5f1e91d024c36f40918a4c6cec634  configs/services/realm.lua
16: loadgen report
16: robots: 1  completed: 1
16: verify: attempts=1 failures=0 p50_ms=173.950166 p99_ms=173.950166 max_ms=173.950166
16: tickets: attempts=1 failures=0 p50_ms=143.566500 p99_ms=143.566500 max_ms=143.566500
16: poll: attempts=22 failures=0 p50_ms=42.719583 p99_ms=51.987375 max_ms=51.987375
16: attach: attempts=1 failures=0 p50_ms=41.451125 p99_ms=41.451125 max_ms=41.451125
16: handoff: attempts=1 failures=0 p50_ms=75.356500 p99_ms=75.356500 max_ms=75.356500
16: realm: attempts=1 failures=0 p50_ms=45.288667 p99_ms=45.288667 max_ms=45.288667
16: acceptance_runtime login_verify=127.0.0.1:24017 queue=127.0.0.1:20278 gateway=127.0.0.1:24905 realm=127.0.0.1:28602 etcd=http://127.0.0.1:25757 mongodb=127.0.0.1:28633
16: acceptance_runtime_config badf4cc0e32b5da4a7c84da777978704f2a37ff71b021332ae2f16b3e94bcfbb  configs/main.config
16: acceptance_runtime_config 553d803284923cd5c9d605c9d722de6402737b703429ac7162cd78b5c25e74ae  configs/common/discovery.lua
16: acceptance_runtime_config 69b5c275f09cbca512ae487c308a2b07accc71404e48b11c3dd5973212203b63  configs/common/accounts.lua
16: acceptance_runtime_config b4e86c4469cba6ea656e4c05e4802ea1e4949c421b722127ab011158bdfa62a2  configs/common/player_data.lua
16: acceptance_runtime_config eb0a3fe73f1e71dcc68e05fbc108be330e7fb82c6e506387f80fee8e4c4d07ac  configs/services/login_verify.lua
16: acceptance_runtime_config d128262f9eb6ff75d98ab4f268112b16ceaf7d942e95b35df4f960a1686dc617  configs/services/queue.lua
16: acceptance_runtime_config 88ec860cf2dcc32953540b732b818468be38a20a8263e4059c669da2811a114c  configs/services/gateway.lua
16: acceptance_runtime_config 3c7586ec7dbc66e70d8be04933d72fa9f37fac13cbf9ff4d9970310a71bec64a  configs/services/realm.lua
16: loadgen report
16: robots: 1  completed: 1
16: verify: attempts=1 failures=0 p50_ms=165.151583 p99_ms=165.151583 max_ms=165.151583
16: tickets: attempts=1 failures=0 p50_ms=152.164666 p99_ms=152.164666 max_ms=152.164666
16: poll: attempts=20 failures=0 p50_ms=44.710625 p99_ms=52.154958 max_ms=52.154958
16: attach: attempts=1 failures=0 p50_ms=51.665250 p99_ms=51.665250 max_ms=51.665250
16: handoff: attempts=1 failures=0 p50_ms=90.844250 p99_ms=90.844250 max_ms=90.844250
16: realm: attempts=1 failures=0 p50_ms=44.049625 p99_ms=44.049625 max_ms=44.049625
658: acceptance_m1 gateway_soak completed=100 attach_failures=0 handoff_failures=0 capacity=20000 max_pending=0 max_fetching=63 max_handed_off=100 mixed_water_sample=1 water_samples=590 baseline_attach_p50_ms=124.374 main_attach_p50_ms=90.6726 baseline_attach_p99_ms=164.106 main_attach_p99_ms=127.733 fd_before=78 fd_after=78
658: acceptance_m1 gateway_soak completed=100 attach_failures=0 handoff_failures=0 capacity=20000 max_pending=0 max_fetching=61 max_handed_off=100 mixed_water_sample=1 water_samples=585 baseline_attach_p50_ms=117.754 main_attach_p50_ms=106.922 baseline_attach_p99_ms=130.883 main_attach_p99_ms=119.642 fd_before=78 fd_after=78
658: acceptance_m1 gateway_soak completed=100 attach_failures=0 handoff_failures=0 capacity=20000 max_pending=0 max_fetching=78 max_handed_off=100 mixed_water_sample=1 water_samples=590 baseline_attach_p50_ms=102.825 main_attach_p50_ms=100.559 baseline_attach_p99_ms=117.089 main_attach_p99_ms=148.039 fd_before=78 fd_after=78
660: loadgen report
660: robots: 1  completed: 1
660: verify: attempts=1 failures=0 p50_ms=50.913500 p99_ms=50.913500 max_ms=50.913500
660: tickets: attempts=1 failures=0 p50_ms=50.819667 p99_ms=50.819667 max_ms=50.819667
660: poll: attempts=38 failures=0 p50_ms=1.613084 p99_ms=6.280541 max_ms=6.280541
660: attach: attempts=1 failures=0 p50_ms=41.600541 p99_ms=41.600541 max_ms=41.600541
660: handoff: attempts=1 failures=0 p50_ms=0.054500 p99_ms=0.054500 max_ms=0.054500
660: realm: attempts=1 failures=0 p50_ms=43.069292 p99_ms=43.069292 max_ms=43.069292
660: loadgen report
660: robots: 1  completed: 1
660: verify: attempts=1 failures=0 p50_ms=51.294083 p99_ms=51.294083 max_ms=51.294083
660: tickets: attempts=1 failures=0 p50_ms=53.286291 p99_ms=53.286291 max_ms=53.286291
660: poll: attempts=38 failures=0 p50_ms=0.933792 p99_ms=5.336542 max_ms=5.336542
660: attach: attempts=1 failures=0 p50_ms=41.249666 p99_ms=41.249666 max_ms=41.249666
660: handoff: attempts=1 failures=0 p50_ms=0.082125 p99_ms=0.082125 max_ms=0.082125
660: realm: attempts=1 failures=0 p50_ms=43.068001 p99_ms=43.068001 max_ms=43.068001
660: loadgen report
660: robots: 1  completed: 1
660: verify: attempts=1 failures=0 p50_ms=50.620834 p99_ms=50.620834 max_ms=50.620834
660: tickets: attempts=1 failures=0 p50_ms=52.084333 p99_ms=52.084333 max_ms=52.084333
660: poll: attempts=38 failures=0 p50_ms=1.381417 p99_ms=6.344625 max_ms=6.344625
660: attach: attempts=1 failures=0 p50_ms=41.099917 p99_ms=41.099917 max_ms=41.099917
660: handoff: attempts=1 failures=0 p50_ms=0.074583 p99_ms=0.074583 max_ms=0.074583
660: realm: attempts=1 failures=0 p50_ms=43.904584 p99_ms=43.904584 max_ms=43.904584
595: queue issuance write load: 2322.03 committed numbers/s
595: queue issuance write load: 2329.73 committed numbers/s
595: queue issuance write load: 2286.2 committed numbers/s
662: acceptance_m2 robots=250 completed=250 skipped=0 attach_attempts=250 attach_failures=0 handoff_attempts=250 handoff_failures=0 fetch_count=250 fetch_retries=0 fetch_failure_rate=0
662: acceptance_m2 robots=250 completed=250 skipped=0 attach_attempts=250 attach_failures=0 handoff_attempts=250 handoff_failures=0 fetch_count=250 fetch_retries=0 fetch_failure_rate=0
662: acceptance_m2 robots=250 completed=250 skipped=0 attach_attempts=250 attach_failures=0 handoff_attempts=250 handoff_failures=0 fetch_count=250 fetch_retries=0 fetch_failure_rate=0
663: acceptance_m3 tickets_robots=10000 tickets_completed=10000 tickets_skipped=0 poll_robots=2000 poll_completed=2000 poll_skipped=0 poll_attempts=30642
17: acceptance_fault gateway_budget_removal elapsed_s=5 threshold_s=10 result=PASS
17: loadgen report
17: robots: 1  completed: 0
17: verify: attempts=1 failures=0 p50_ms=229.403708 p99_ms=229.403708 max_ms=229.403708
17: tickets: attempts=1 failures=0 p50_ms=160.161292 p99_ms=160.161292 max_ms=160.161292
17: poll: attempts=100 failures=3 p50_ms=41.763209 p99_ms=52.299458 max_ms=53.072416
17: loadgen report
17: robots: 1  completed: 1
17: verify: attempts=1 failures=0 p50_ms=242.766333 p99_ms=242.766333 max_ms=242.766333
17: tickets: attempts=1 failures=0 p50_ms=161.230833 p99_ms=161.230833 max_ms=161.230833
17: poll: attempts=4 failures=0 p50_ms=5.946291 p99_ms=48.249459 max_ms=48.249459
17: attach: attempts=1 failures=0 p50_ms=42.429667 p99_ms=42.429667 max_ms=42.429667
17: handoff: attempts=1 failures=0 p50_ms=63.454083 p99_ms=63.454083 max_ms=63.454083
17: realm: attempts=1 failures=0 p50_ms=43.652708 p99_ms=43.652708 max_ms=43.652708
17: acceptance_fault gateway_budget_restore elapsed_s=1 threshold_s=10 result=PASS
17: acceptance_fault realm_budget_removal elapsed_s=0 threshold_s=10 result=PASS
17: loadgen report
17: robots: 1  completed: 0
17: verify: attempts=1 failures=0 p50_ms=234.453958 p99_ms=234.453958 max_ms=234.453958
17: tickets: attempts=1 failures=0 p50_ms=157.396333 p99_ms=157.396333 max_ms=157.396333
17: poll: attempts=96 failures=1 p50_ms=43.656125 p99_ms=55.040708 max_ms=55.040708
17: loadgen report
17: robots: 1  completed: 1
17: verify: attempts=1 failures=0 p50_ms=234.716709 p99_ms=234.716709 max_ms=234.716709
17: tickets: attempts=1 failures=0 p50_ms=157.689833 p99_ms=157.689833 max_ms=157.689833
17: poll: attempts=19 failures=0 p50_ms=7.968291 p99_ms=50.984292 max_ms=50.984292
17: attach: attempts=1 failures=0 p50_ms=54.304292 p99_ms=54.304292 max_ms=54.304292
17: handoff: attempts=1 failures=0 p50_ms=96.052209 p99_ms=96.052209 max_ms=96.052209
17: realm: attempts=1 failures=0 p50_ms=44.699583 p99_ms=44.699583 max_ms=44.699583
17: acceptance_fault realm_budget_restore elapsed_s=2 threshold_s=10 result=PASS
17: loadgen report
17: robots: 1  completed: 1
17: verify: attempts=1 failures=0 p50_ms=250.664459 p99_ms=250.664459 max_ms=250.664459
17: tickets: attempts=1 failures=0 p50_ms=145.182917 p99_ms=145.182917 max_ms=145.182917
17: poll: attempts=8 failures=0 p50_ms=0.453917 p99_ms=47.746042 max_ms=47.746042
17: attach: attempts=1 failures=0 p50_ms=41.663875 p99_ms=41.663875 max_ms=41.663875
17: handoff: attempts=1 failures=0 p50_ms=61.435166 p99_ms=61.435166 max_ms=61.435166
17: realm: attempts=1 failures=0 p50_ms=44.068542 p99_ms=44.068542 max_ms=44.068542
17: acceptance_fault queue_released_number before=5 after=5 result=PASS
17: loadgen report
17: robots: 1  completed: 1
17: verify: attempts=1 failures=0 p50_ms=219.878375 p99_ms=219.878375 max_ms=219.878375
17: tickets: attempts=1 failures=0 p50_ms=162.152292 p99_ms=162.152292 max_ms=162.152292
17: poll: attempts=24 failures=0 p50_ms=4.506000 p99_ms=52.701542 max_ms=52.701542
17: attach: attempts=1 failures=0 p50_ms=45.555417 p99_ms=45.555417 max_ms=45.555417
17: handoff: attempts=1 failures=0 p50_ms=91.170334 p99_ms=91.170334 max_ms=91.170334
17: realm: attempts=1 failures=0 p50_ms=44.461875 p99_ms=44.461875 max_ms=44.461875
17: acceptance_fault queue_cold_restart elapsed_s=7 threshold_s=30 result=PASS
17: acceptance_fault etcd_outage_unready elapsed_s=2 threshold_s=10 result=PASS
17: loadgen report
17: robots: 1  completed: 0
17: verify: attempts=1 failures=0 p50_ms=213.254000 p99_ms=213.254000 max_ms=213.254000
17: tickets: attempts=1 failures=1 p50_ms=4248.461168 p99_ms=4248.461168 max_ms=4248.461168
17: acceptance_fault etcd_recovery elapsed_s=0 threshold_s=10 result=PASS
17: loadgen report
17: robots: 1  completed: 1
17: verify: attempts=1 failures=0 p50_ms=251.478542 p99_ms=251.478542 max_ms=251.478542
17: tickets: attempts=1 failures=0 p50_ms=187.391250 p99_ms=187.391250 max_ms=187.391250
17: poll: attempts=25 failures=0 p50_ms=0.494750 p99_ms=48.219417 max_ms=48.219417
17: attach: attempts=1 failures=0 p50_ms=41.119500 p99_ms=41.119500 max_ms=41.119500
17: handoff: attempts=1 failures=0 p50_ms=71.419167 p99_ms=71.419167 max_ms=71.419167
17: realm: attempts=1 failures=0 p50_ms=47.127416 p99_ms=47.127416 max_ms=47.127416
17: acceptance_fault queue_authoritative_recovery elapsed_s=3 threshold_s=30 result=PASS
```

## Limitations

- CI runs on one GitHub-hosted Ubuntu 24.04 machine; it does not prove cross-machine network or etcd TLS/authentication behavior.
- Accounts and characters come from MongoDB Player Data (ADR-0011): each case starts its own single-node `rs0` replica set on the runner, and Login Verifier authenticates, Gateway fetches and Realm rechecks against the same database. The replica set is local and unauthenticated, so this does not prove MongoDB TLS/authentication or multi-member failover. Loadgen capacity fixtures (thousands of robot accounts) use `credential_hash_cost = "minimum"`, so their Login Verify CPU cost is below a production Argon2 profile; single-account dev-services cases keep the default `interactive` cost.
- Results are not comparable with pre-#92 reports, whose Gateway fetch path was the `DelayedAccountFetchPort` controlled stub.
- CDN cache hit rate and the full 100k/1m production sizes require a separately approved dedicated environment; M5 remains separate.
