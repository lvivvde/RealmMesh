# RealmMesh

C++ game server / client framework (CMake, Lua scripting, protobuf protocols).

## Agent skills

### Issue tracker

Issues are tracked in GitHub Issues (github.com/lvivvde/RealmMesh) via the `gh` CLI. See `docs/agents/issue-tracker.md`.

### Triage labels

Default triage label vocabulary: `needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, `wontfix`. See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: one `CONTEXT.md` + `docs/adr/` at the repo root. See `docs/agents/domain.md`.

### Testing

Tests live under `tests/`: C++ GTest in `tests/cpp` (mirrors the source tree), Lua suites in `tests/lua`, script-driven integration in `tests/scripts`. Read `tests/README.md` before adding tests — every test must carry a ctest label (`unit`/`integration`); fast subset is `ctest -L unit`, full run is `./scripts/build.sh`.
