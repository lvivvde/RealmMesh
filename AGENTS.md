# RealmMesh

C++20 distributed game server framework (CMake, Lua config, protobuf wire). Project docs (CONTEXT, ADRs, specs, architecture) are written in Chinese; write additions to them in Chinese.

## Orientation

- **Glossary first**: read `CONTEXT.md` before naming a domain concept in code, tests, issues, or docs, and use its terms (each entry's `_Avoid_` list names the rejected synonyms). Read the `docs/adr/` entries touching your area; when your change contradicts one, say so explicitly. Details: `docs/agents/domain.md`.
- **Login chain** (verify → queue → gateway pipeline → direct Realm connect): design in `docs/specs/`, implemented structure in `docs/architecture.md`, wire format in `docs/protocol.md`.
- **Services**: `game/login_verify` and `game/queue` are HTTPS/JSON services; `gateway` and `realm` both run on `game::gateway::GatewayRuntime` inside `game/gateway/` (realm has no directory of its own). Shared code lives in `game/common`. `apps/mesh_host` builds the single `realm_mesh` binary (`--service <name>` runs one service). The retired `login` wire name stays unused.
- **Lean tree** (ADR-0003): a directory appears in the same change as the code that fills it.

## Platforms

macOS is the dev baseline and builds TLS/TCP only; QUIC compiles and runs only on Linux (ADR-0002). A green macOS run leaves QUIC paths unverified: CI's `linux` job is their gate.

## Testing

Read `tests/README.md` before adding a test. Every target carries a ctest label, and any target that binds ports or spawns `realm_mesh` needs `LABELS integration`.

- Fast loop: `ctest --preset dev -L unit` (no processes, no etcd).
- Full run: `./scripts/build.sh` (configure + build + all tests). Integration tests start a real etcd, installed once by `./scripts/install-etcd.sh`.

## Docs travel with code

A change that alters implemented structure, wire format, or domain terms updates `docs/architecture.md`, `docs/protocol.md`, `CONTEXT.md`, and the README 实施状态 table in the same PR.

## Code discovery

When `codebase-memory-mcp` is connected, use `search_graph` / `trace_path` / `get_code_snippet` for code lookup, and Grep for string literals and `third_party/` (excluded from the index by `.cbmignore`). After pulling changes, run `index_repository` (incremental).

## Issues and commits

- GitHub Issues via `gh`: operations in `docs/agents/issue-tracker.md`, triage label mapping in `docs/agents/triage-labels.md`.
- Commit subjects follow Conventional Commits with a scope and the issue number: `fix(queue): persist issued positions across restart (#89)`.
