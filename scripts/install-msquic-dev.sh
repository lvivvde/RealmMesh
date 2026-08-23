#!/usr/bin/env bash

# 用途：为 Linux x86_64 开发环境下载并校验固定版本的 MsQuic 库与头文件。
# 用法：./scripts/install-msquic-dev.sh
# 输出：.tools/msquic/；系统运行依赖仍需按脚本末尾提示安装。

set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
install_root="${project_root}/.tools/msquic"
temporary_root="$(mktemp -d)"
trap 'rm -rf -- "${temporary_root}"' EXIT

if [[ "$(uname -s)" != "Linux" || "$(uname -m)" != "x86_64" ]]; then
    echo "This development installer currently supports Linux x86_64 only." >&2
    exit 1
fi

package_url="https://packages.microsoft.com/ubuntu/24.04/prod/pool/main/libm/libmsquic/libmsquic_2.5.10_amd64.deb"
package_sha256="3b98ced3af4a681d567c8710692fc43d208c427e227684f2224f1f6d5ef23089"
header_base="https://raw.githubusercontent.com/microsoft/msquic/v2.5.10/src/inc"

curl --fail --location --retry 3 --output "${temporary_root}/libmsquic.deb" "${package_url}"
echo "${package_sha256}  ${temporary_root}/libmsquic.deb" | sha256sum --check --status
dpkg-deb --extract "${temporary_root}/libmsquic.deb" "${install_root}"

mkdir -p "${install_root}/usr/include"
curl --fail --location --retry 3 --output "${install_root}/usr/include/msquic.h" "${header_base}/msquic.h"
curl --fail --location --retry 3 --output "${install_root}/usr/include/msquic_posix.h" "${header_base}/msquic_posix.h"
curl --fail --location --retry 3 --output "${install_root}/usr/include/quic_sal_stub.h" "${header_base}/quic_sal_stub.h"

echo "c9abfdd02c45910649dd335d6bd82718e4ddd2fdb35fe550567c78f032551e0c  ${install_root}/usr/include/msquic.h" | sha256sum --check --status
echo "b285fa66b9c9bdc886c30ef92910da472692b25f5c6192416fb40f08f64e22ec  ${install_root}/usr/include/msquic_posix.h" | sha256sum --check --status
echo "9b13328d9aec8807a754b2bc391b31b5d09b1c5f6cec064012051683ed169055  ${install_root}/usr/include/quic_sal_stub.h" | sha256sum --check --status

echo "MsQuic 2.5.10 installed under ${install_root}."
echo "Install its runtime dependencies and OpenSSL headers if needed:"
echo "  sudo apt install libssl-dev libxdp1 libnl-3-200 libnl-route-3-200 libnuma1"
