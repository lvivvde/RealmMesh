#!/usr/bin/env bash

# 用途：在 Linux 上下载并校验固定版本的 MongoDB 服务器(mongod)与 mongosh,
# 安装到项目本地 .tools/ 目录(ADR-0011),供 CI 与 Linux 开发机使用。
# 用法：./scripts/install-mongodb.sh
# 仅支持 Linux x86_64(Ubuntu 24.04 构建)。macOS 直接用 Homebrew:
#   brew tap mongodb/brew && brew install mongodb-community mongosh
# 脚本与测试夹具都先查 PATH,再退回这里装到 .tools/ 的版本。版本、下载地址与
# 校验和都固定在下方:mongod 取自 fastdl.mongodb.org,mongosh 取自 GitHub releases
# 公布的 SHA256。

set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mongodb_version="8.0.32"
mongosh_version="2.12.0"

host_os="$(uname -s)"
host_arch="$(uname -m)"
case "${host_arch}" in
    x86_64 | amd64) arch="x86_64" ;;
    arm64 | aarch64) arch="arm64" ;;
    *)
        printf 'install-mongodb: unsupported architecture: %s\n' "${host_arch}" >&2
        exit 1
        ;;
esac

case "${host_os}-${arch}" in
    Linux-x86_64)
        mongodb_archive="mongodb-linux-x86_64-ubuntu2404-${mongodb_version}.tgz"
        mongodb_url="https://fastdl.mongodb.org/linux/${mongodb_archive}"
        mongodb_sha256="b411be17c31ef249767ed91974d876e007c91afd5f45e1534057d247eada9f0d"
        mongosh_archive="mongosh-${mongosh_version}-linux-x64.tgz"
        mongosh_sha256="aa42cb826b7b8e655c5481293f5365ddaf1a23e43af07520326e5bbc957838ad"
        ;;
    *)
        printf 'install-mongodb: no pinned MongoDB build for %s-%s\n' \
            "${host_os}" "${arch}" >&2
        if [[ "${host_os}" == Darwin ]]; then
            printf 'on macOS use: brew tap mongodb/brew && brew install mongodb-community mongosh\n' >&2
        fi
        exit 1
        ;;
esac
mongosh_url="https://github.com/mongodb-js/mongosh/releases/download/v${mongosh_version}/${mongosh_archive}"

sha256_of() {
    sha256sum "$1" | awk '{print $1}'
}

# 下载、校验并解压到 install_dir;两个上游包都带一层顶级目录，统一剥掉。
install_archive() {
    local url="$1" archive_name="$2" expected="$3" install_dir="$4"
    local archive="${project_root}/.tools/${archive_name}"
    if [[ ! -f "${archive}" ]]; then
        curl --fail --location --silent --show-error "${url}" --output "${archive}"
    fi
    local actual
    actual="$(sha256_of "${archive}")"
    if [[ "${actual}" != "${expected}" ]]; then
        printf 'install-mongodb: checksum mismatch for %s\n  expected %s\n  actual   %s\n' \
            "${archive}" "${expected}" "${actual}" >&2
        exit 1
    fi
    rm -rf "${install_dir}"
    mkdir -p "${install_dir}"
    case "${archive_name}" in
        *.tgz) tar -xzf "${archive}" --strip-components=1 -C "${install_dir}" ;;
        *.zip)
            local staging="${install_dir}.unzip"
            rm -rf "${staging}"
            unzip -q -o "${archive}" -d "${staging}"
            mv "${staging}"/*/* "${install_dir}/"
            rm -rf "${staging}"
            ;;
        *)
            printf 'install-mongodb: unknown archive type: %s\n' "${archive_name}" >&2
            exit 1
            ;;
    esac
}

mkdir -p "${project_root}/.tools"
install_archive "${mongodb_url}" "${mongodb_archive}" "${mongodb_sha256}" \
    "${project_root}/.tools/mongodb-${mongodb_version}"
install_archive "${mongosh_url}" "${mongosh_archive}" "${mongosh_sha256}" \
    "${project_root}/.tools/mongosh-${mongosh_version}"

"${project_root}/.tools/mongodb-${mongodb_version}/bin/mongod" --version | head -n 1
"${project_root}/.tools/mongosh-${mongosh_version}/bin/mongosh" --version
