#!/usr/bin/env bash

# 用途：下载并校验固定版本的 etcd，将其安装到项目本地 .tools/ 目录。
# 用法：./scripts/install-etcd.sh
# 支持 Linux(amd64)与 macOS(arm64/amd64)。版本、下载地址与校验和都固定在
# 下方，与上游 releases 页的 SHA256SUMS 一致。

set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
etcd_version="3.6.14"

host_os="$(uname -s)"
host_arch="$(uname -m)"
case "${host_os}" in
    Linux) etcd_os="linux" ;;
    Darwin) etcd_os="darwin" ;;
    *)
        printf 'install-etcd: unsupported OS: %s\n' "${host_os}" >&2
        exit 1
        ;;
esac
case "${host_arch}" in
    x86_64 | amd64) etcd_arch="amd64" ;;
    arm64 | aarch64) etcd_arch="arm64" ;;
    *)
        printf 'install-etcd: unsupported architecture: %s\n' "${host_arch}" >&2
        exit 1
        ;;
esac

# 上游只发布 linux-amd64 的 tar.gz,其他平台是 zip,校验和因此逐平台固定。
platform="${etcd_os}-${etcd_arch}"
case "${platform}" in
    linux-amd64)
        archive_name="etcd-v${etcd_version}-linux-amd64.tar.gz"
        expected_sha256="ffe840ff9295808e88cce2794a18a5ac87f12a5203c8314d0bf6aa119b41bac5"
        ;;
    darwin-arm64)
        archive_name="etcd-v${etcd_version}-darwin-arm64.zip"
        expected_sha256="fd154573a1f4c098c214d8233507d2766c43a164f7e339c0385aae71d9a1afaf"
        ;;
    darwin-amd64)
        archive_name="etcd-v${etcd_version}-darwin-amd64.zip"
        expected_sha256="17b8639cf303fee6c35958278d557263215a9a8f8c15fd89022b5aa67091b228"
        ;;
    *)
        printf 'install-etcd: no pinned etcd build for %s\n' "${platform}" >&2
        exit 1
        ;;
esac

archive="${project_root}/.tools/${archive_name}"
install_dir="${project_root}/.tools/etcd-v${etcd_version}"
download_url="https://github.com/etcd-io/etcd/releases/download/v${etcd_version}/${archive_name}"

mkdir -p "${project_root}/.tools" "${install_dir}"

if [[ ! -f "${archive}" ]]; then
    curl --fail --location --silent --show-error "${download_url}" --output "${archive}"
fi

# macOS 自带 shasum 而没有 sha256sum,两者取其一。
if command -v sha256sum >/dev/null 2>&1; then
    actual_sha256="$(sha256sum "${archive}" | awk '{print $1}')"
else
    actual_sha256="$(shasum -a 256 "${archive}" | awk '{print $1}')"
fi
if [[ "${actual_sha256}" != "${expected_sha256}" ]]; then
    printf 'install-etcd: checksum mismatch for %s\n  expected %s\n  actual   %s\n' \
        "${archive}" "${expected_sha256}" "${actual_sha256}" >&2
    exit 1
fi

case "${archive_name}" in
    *.tar.gz) tar -xzf "${archive}" --strip-components=1 -C "${install_dir}" ;;
    *.zip) unzip -q -o -j "${archive}" -d "${install_dir}" ;;
    *)
        printf 'install-etcd: unknown archive type: %s\n' "${archive_name}" >&2
        exit 1
        ;;
esac

"${install_dir}/etcd" --version
