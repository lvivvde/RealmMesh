#!/usr/bin/env bash

# 用途：以前台单节点模式启动本地 etcd，供 RealmMesh 服务发现开发与测试使用。
# 用法：./scripts/run-etcd-dev.sh（Ctrl-C 停止）
# 可选：REALMMESH_ETCD_DATA_DIR=/path/to/data ./scripts/run-etcd-dev.sh

set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
etcd_version="3.6.14"
etcd_bin="${project_root}/.tools/etcd-v${etcd_version}/etcd"
data_dir="${REALMMESH_ETCD_DATA_DIR:-${project_root}/.runtime/etcd}"

umask 077

if [[ ! -x "${etcd_bin}" ]]; then
    echo "etcd is not installed; run ./scripts/install-etcd.sh first" >&2
    exit 1
fi

mkdir -p "${data_dir}"

exec "${etcd_bin}" \
    --name realmmesh-dev \
    --data-dir "${data_dir}" \
    --listen-client-urls http://127.0.0.1:2379 \
    --advertise-client-urls http://127.0.0.1:2379 \
    --listen-peer-urls http://127.0.0.1:2380 \
    --initial-advertise-peer-urls http://127.0.0.1:2380 \
    --initial-cluster realmmesh-dev=http://127.0.0.1:2380 \
    --log-level warn
