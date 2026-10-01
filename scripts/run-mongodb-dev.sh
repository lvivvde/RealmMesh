#!/usr/bin/env bash

# 用途：以前台单节点副本集 rs0 启动本地 MongoDB,供 RealmMesh 玩家数据开发使用
# (ADR-0011:事务需要副本集，单机也以 --replSet 启动)。首次启动时自动完成
# 副本集初始化。二进制取自 PATH(macOS 即 Homebrew 安装的 mongodb-community),
# 数据放在仓库内 .runtime/mongodb,不动 Homebrew 服务自己的数据目录。
# 用法：./scripts/run-mongodb-dev.sh(Ctrl-C 停止)
# 可选：REALMMESH_MONGODB_DATA_DIR=/path/to/data REALMMESH_MONGODB_PORT=27017

set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
realmmesh_mongodb_root="${project_root}"
# shellcheck source=lib/mongodb-tools.sh
source "${project_root}/scripts/lib/mongodb-tools.sh"
data_dir="${REALMMESH_MONGODB_DATA_DIR:-${project_root}/.runtime/mongodb}"
port="${REALMMESH_MONGODB_PORT:-27017}"

umask 077

if ! mongod_bin="$(realmmesh_mongod_binary)" ||
    ! mongosh_bin="$(realmmesh_mongosh_binary)"; then
    realmmesh_mongodb_install_hint >&2
    exit 1
fi

mkdir -p "${data_dir}"

"${mongod_bin}" \
    --replSet rs0 \
    --bind_ip 127.0.0.1 \
    --port "${port}" \
    --dbpath "${data_dir}" \
    --logpath "${data_dir}/mongod.log" \
    --logappend &
mongod_pid=$!
trap 'kill -TERM "${mongod_pid}" 2>/dev/null || true; wait "${mongod_pid}" 2>/dev/null || true' INT TERM EXIT

# 副本集成员地址必须与服务 URI 中的主机一致，否则驱动会按 rs0 的成员表去连
# 一个不可达的主机名。
"${project_root}/scripts/mongodb-init-replset.sh" "${mongosh_bin}" "127.0.0.1:${port}"

echo "mongod rs0 primary on 127.0.0.1:${port} (log: ${data_dir}/mongod.log)"
wait "${mongod_pid}"
