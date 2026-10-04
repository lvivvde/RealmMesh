# RealmMesh

C++20 distributed game server framework (CMake, Lua config, protobuf wire). Project docs (CONTEXT, ADRs, specs, architecture) are written in Chinese; write additions to them in Chinese.

## Orientation

- **Glossary first**: read `CONTEXT.md` before naming a domain concept in code, tests, issues, or docs, and use its terms (each entry's `_Avoid_` list names the rejected synonyms). Read the `docs/adr/` entries touching your area; when your change contradicts one, say so explicitly. Details: `docs/agents/domain.md`.
- **Login chain** (verify → queue → gateway pipeline → direct Realm connect): design in `docs/specs/`, implemented structure in `docs/architecture.md`, wire format in `docs/protocol.md`.
- **Services**: `game/login_verify` and `game/queue` are HTTPS/JSON services; `gateway` and `realm` both run on `game::gateway::GatewayRuntime` from `game/gateway/`; Realm business logic (Realm Session phases, characters, Lua training rule) lives in `game/realm`. Shared code lives in `game/common`. `apps/mesh_host` builds the single `realm_mesh` binary (`--service <name>` runs one service). The retired `login` wire name stays unused.
- **Lean tree** (ADR-0003): a directory appears in the same change as the code that fills it.

## Platforms

Linux is the production baseline and always builds QUIC; macOS is the dev baseline and builds QUIC only when Homebrew `libmsquic` is installed, otherwise TLS/TCP only (ADR-0002, ADR-0012). Check the configure line `realm_network: QUIC transport enabled` before trusting a macOS run on QUIC paths. CI's `macos` job never builds QUIC, so CI's `linux` job is their gate.

### Linux SSH 开发环境

- 本机 Lima 的 `ubuntu` 实例已配置 Git 环境，项目已完成构建；需要在 Linux 中开发或验证时，通过 `ssh lima-ubuntu` 进入。
- Linux 仓库位置：`/home/edwin.guest/code/RealmMesh`；连接后执行 `cd /home/edwin.guest/code/RealmMesh`。
- SSH 连接沿用本机 `~/.ssh/config` 与 Lima 生成的配置；认证材料及 Git 凭据保存在仓库外。此处只记录环境指引，密钥、令牌、口令及凭据内容不得写入或提交到仓库。

## Player data

Manual dev runs on macOS and Linux use the remote shared MongoDB (TLS + auth over an SSH tunnel): read `docs/operations/shared-mongodb.md` before starting services against it, debugging DB connections, or setting up a new machine, and launch through `scripts/with-shared-mongodb.sh`. Its address, credentials and CA live outside the repo; never print or commit them. Automated tests never touch it: fixtures start an isolated local `mongod`.

## Testing

Read `tests/README.md` before adding a test. Every target carries a ctest label, and any target that binds ports or spawns `realm_mesh` needs `LABELS integration`.

- Fast loop: `./scripts/test-fast.sh` (configures, builds only the Unit aggregate, runs `unit` tests with 4 jobs; no processes, no etcd). `--target T` / `--test-regex R` narrow it; it is not full verification. Hand-written CTest label filters need anchors: `-L '^unit$'`.
- CI's macOS runner is several times slower than a dev Mac: size load (connection counts, iterations, payloads) down under `__APPLE__` and keep full size on Linux, as the M3 smoke does (#104). A macOS CI `Timeout` can also be a crashed test whose orphaned child processes hold ctest's output pipe open; read the log before shrinking data.
- Full run: `./scripts/build.sh` (configure + build ALL + all tests, `ctest -j 1`). Every script entry takes `--preset NAME` and `--jobs N`; compile jobs default to the CPU/memory budget in `scripts/lib/build-jobs.sh`. Integration tests start a real etcd, installed once by `./scripts/install-etcd.sh`, and a real single-node MongoDB replica set (Homebrew `mongodb-community` + `mongosh` on macOS, `./scripts/install-mongodb.sh` on Linux).

## Docs travel with code

A change that alters implemented structure, wire format, or domain terms updates `docs/architecture.md`, `docs/protocol.md`, `CONTEXT.md`, and the README 实施状态 table in the same PR.

## Code discovery

When `codebase-memory-mcp` is connected, use `search_graph` / `trace_path` / `get_code_snippet` for code lookup, and Grep for string literals and `third_party/` (excluded from the index by `.cbmignore`). After pulling changes, run `index_repository` (incremental).

## Issues and commits

- GitHub Issues via `gh`: operations in `docs/agents/issue-tracker.md`, triage label mapping in `docs/agents/triage-labels.md`.
- Commit subjects follow Conventional Commits with a scope and the issue number: `fix(queue): persist issued positions across restart (#89)`.
