# RealmMesh

C++ game server / client framework (CMake, Lua scripting, protobuf protocols).

## Agent skills

### Issue tracker

Issues are tracked in GitHub Issues (github.com/lvivvde/RealmMesh) via the `gh` CLI. See `docs/agents/issue-tracker.md`.

### Triage labels

Default triage label vocabulary: `needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, `wontfix`. See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: one `CONTEXT.md` + `docs/adr/` at the repo root. See `docs/agents/domain.md`.

### Architecture

The login-chain topology (verify → queue → gateway pipeline → direct Realm connect) is specified in `docs/specs/2026-09-12-login-chain-surge.md`, with decisions in `docs/adr/0004`–`0008` (0004–0007 cover the server-side chain, 0008 the client chain's shared wire and redeemer seam); `docs/architecture.md` holds the implemented structure. Services live in `game/<service>/`: `login_verify` and `queue` are HTTPS/JSON services, while `gateway` and `realm` share `game::gateway::GatewayRuntime`. The old `login` service is retired and its wire name is never reused. `apps/mesh_host` is the single `realm_mesh` entry point (`--service <name>` narrows to one service).

### Code discovery

Prefer `codebase-memory-mcp` tools (`search_graph`, `trace_path`, `get_code_snippet`) over Grep/Glob for code lookup; use Grep for string literals and `third_party/`. After `git pull` with changes, run `index_repository` (incremental).

### Testing

Tests live under `tests/`: C++ GTest in `tests/cpp` (mirrors the source tree), Lua suites in `tests/lua`, script-driven integration in `tests/scripts`. Read `tests/README.md` before adding tests — every test must carry a ctest label (`unit`/`integration`; Lua suites additionally carry the `lua` filter label); fast subset is `ctest -L unit`, full run is `./scripts/build.sh`.
