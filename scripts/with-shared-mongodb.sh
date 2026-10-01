#!/usr/bin/env bash
# 加载仓库外的私有连接配置，按需建立仅监听回环地址的 SSH 隧道。
# 用法：./scripts/with-shared-mongodb.sh --check
#       ./scripts/with-shared-mongodb.sh <command> [args...]
set -euo pipefail
umask 077
realmmesh_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
config_dir="${REALMMESH_MONGODB_CONFIG_DIR:-${XDG_CONFIG_HOME:-${HOME}/.config}/realmmesh/mongodb}"
[[ -r "${config_dir}/env.sh" ]] || {
    printf 'Private MongoDB configuration missing; see docs/operations/shared-mongodb.md\n' >&2
    exit 1
}
# shellcheck disable=SC1091
source "${config_dir}/env.sh"
: "${REALMMESH_MONGODB_URI:?Private MongoDB URI is missing}"
export REALMMESH_MONGODB_URI
if [[ -n "${REALMMESH_MONGODB_SSH_HOST:-}" ]]; then
    local_port="${REALMMESH_MONGODB_LOCAL_PORT:-27018}"
    [[ "${local_port}" =~ ^[0-9]+$ && "${local_port}" -ge 1024 && "${local_port}" -le 65535 ]] || {
        echo 'Invalid MongoDB tunnel port.' >&2; exit 1;
    }
    control_socket="${config_dir}/ssh-${local_port}.sock"
    if ! ssh -S "${control_socket}" -O check "${REALMMESH_MONGODB_SSH_HOST}" >/dev/null 2>&1; then
        ssh -M -S "${control_socket}" -fNT \
            -o BatchMode=yes -o StrictHostKeyChecking=yes -o ConnectTimeout=10 \
            -o ExitOnForwardFailure=yes -o ServerAliveInterval=30 -o ServerAliveCountMax=3 \
            -o ControlPersist=600 \
            -L "127.0.0.1:${local_port}:127.0.0.1:27017" "${REALMMESH_MONGODB_SSH_HOST}"
    fi
fi
[[ $# -gt 0 ]] || { echo 'Usage: with-shared-mongodb.sh --check | <command> [args...]' >&2; exit 2; }
if [[ "$1" == --check ]]; then
    mongosh_bin="${REALMMESH_MONGOSH_BINARY:-}"
    if [[ -z "${mongosh_bin}" ]]; then
        mongosh_bin="$(command -v mongosh || true)"
    fi
    if [[ -z "${mongosh_bin}" && -x /opt/realmmesh-mongodb/.tools/mongosh-2.12.0/bin/mongosh ]]; then
        mongosh_bin=/opt/realmmesh-mongodb/.tools/mongosh-2.12.0/bin/mongosh
    fi
    [[ -n "${mongosh_bin}" ]] || { echo 'mongosh is required for --check.' >&2; exit 1; }
    exec "${mongosh_bin}" --nodb --quiet --file "${realmmesh_root}/scripts/check-shared-mongodb.js"
fi
exec "$@"
