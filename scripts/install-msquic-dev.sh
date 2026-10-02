#!/usr/bin/env bash

# 用途：为 Linux x86_64/ARM64 开发环境下载并校验固定版本的 MsQuic 库与头文件。
# 用法：./scripts/install-msquic-dev.sh
# 输出：.tools/msquic/；系统运行依赖仍需按脚本末尾提示安装。

set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
install_root="${project_root}/.tools/msquic"
temporary_root="$(mktemp -d)"
trap 'rm -rf -- "${temporary_root}"' EXIT

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "This development installer supports Linux only." >&2
    exit 1
fi

case "$(uname -m)" in
    x86_64 | amd64)
        package_arch="amd64"
        package_sha256="ff99192ca34d6c5b18fc4b45a94988c3f66c78fbb3bfb7076c3f403eabf531e5"
        ;;
    aarch64 | arm64)
        package_arch="arm64"
        package_sha256="cb2d4d71901cc8bc6b9e1ab35b50b5c381ca70552b6ae800fae22b5b9ac9524e"
        ;;
    *)
        echo "This development installer supports Linux x86_64 and ARM64 only." >&2
        exit 1
        ;;
esac

package_url="https://packages.microsoft.com/ubuntu/24.04/prod/pool/main/libm/libmsquic/libmsquic_2.6.1_${package_arch}.deb"
header_base="https://raw.githubusercontent.com/microsoft/msquic/v2.6.1/src/inc"

curl --fail --location --retry 3 --output "${temporary_root}/libmsquic.deb" "${package_url}"
echo "${package_sha256}  ${temporary_root}/libmsquic.deb" | sha256sum --check --status
# dpkg-deb 只会创建目标的最后一级目录,父目录须先就位。
mkdir -p "${install_root}"
dpkg-deb --extract "${temporary_root}/libmsquic.deb" "${install_root}"

mkdir -p "${install_root}/usr/include"
curl --fail --location --retry 3 --output "${install_root}/usr/include/msquic.h" "${header_base}/msquic.h"
curl --fail --location --retry 3 --output "${install_root}/usr/include/msquic_posix.h" "${header_base}/msquic_posix.h"
curl --fail --location --retry 3 --output "${install_root}/usr/include/quic_sal_stub.h" "${header_base}/quic_sal_stub.h"

echo "3ebde22085df627140fd6208c638a9a3cd7dd3da9bd270f64f022b1c46b1bc4f  ${install_root}/usr/include/msquic.h" | sha256sum --check --status
echo "a0e11c5eb1a4bbd5e18e4f7a381dd024064df2ee76ed555b49c0b8a66c15f526  ${install_root}/usr/include/msquic_posix.h" | sha256sum --check --status
echo "8849135a95fee2a49373e168c12a89380de46e23c2136e6b0597394d09a9614c  ${install_root}/usr/include/quic_sal_stub.h" | sha256sum --check --status

echo "MsQuic 2.6.1 installed under ${install_root}."
echo "Install its runtime dependencies and OpenSSL headers if needed:"
echo "  sudo apt install libssl-dev libnuma1"
