# RealmMesh observability development stack

This stack runs the independent logging plane described in
[`docs/logging-architecture.md`](../../docs/logging-architecture.md): a local
Vector agent tails RealmMesh JSONL files, load-balances acknowledged gRPC over
mTLS to two stateless Vector gateways, and the gateways write to Loki. Grafana
provides log search and Prometheus scrapes both application and pipeline health.

## Start

From the repository root:

```bash
./scripts/generate-observability-dev-certs.sh
docker compose -f deploy/observability/compose.yaml config
docker compose -f deploy/observability/compose.yaml up -d
```

Start the RealmMesh services normally. The agent watches `.runtime/logs` and
persists its checkpoints and two disk buffers (8 GiB normal plus 2 GiB reserved
for `WARN`/`ERROR`/`FATAL`) in the `vector-agent-data` volume.

- Grafana: <http://127.0.0.1:3000> (`admin` / `realmmesh-dev` by default)
- Prometheus: <http://127.0.0.1:9090>
- Loki readiness: <http://127.0.0.1:3100/ready>
- Agent metrics: <http://127.0.0.1:9597/metrics>
- Gateway metrics: ports `9598` and `9599`

Example LogQL queries:

```logql
{service_name="gateway", severity="ERROR"}
{environment="development"} | json | correlation_id="<32-lowercase-hex>"
```

The Compose stack is intentionally development-only: Loki is single-binary,
uses local filesystem storage, and has authentication disabled. Production must
use Loki's scalable/distributed deployment with an external maintained object
store, network policy, TLS/authentication at the query boundary, and separate
failure domains. Do not expose any of the development ports publicly.

Set `GRAFANA_ADMIN_PASSWORD` before starting to override the development
password. Rotate the generated CA and leaf certificates whenever this stack is
shared beyond one developer machine.
