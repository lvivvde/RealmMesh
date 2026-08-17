#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
etcd_version="3.6.14"
archive="${project_root}/.tools/etcd-v${etcd_version}-linux-amd64.tar.gz"
install_dir="${project_root}/.tools/etcd-v${etcd_version}"
download_url="https://github.com/etcd-io/etcd/releases/download/v${etcd_version}/etcd-v${etcd_version}-linux-amd64.tar.gz"
expected_sha256="ffe840ff9295808e88cce2794a18a5ac87f12a5203c8314d0bf6aa119b41bac5"

mkdir -p "${project_root}/.tools" "${install_dir}"

if [[ ! -f "${archive}" ]]; then
    curl --fail --location --silent --show-error "${download_url}" --output "${archive}"
fi

printf '%s  %s\n' "${expected_sha256}" "${archive}" | sha256sum --check --status
tar -xzf "${archive}" --strip-components=1 -C "${install_dir}"

"${install_dir}/etcd" --version
