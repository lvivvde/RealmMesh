#!/usr/bin/env bash
# 隔离测试 CA 与服务身份。所有私钥仅在构建目录生成。
set -euo pipefail
out="$1"
openssl_bin="$2"
mkdir -p "$out"
umask 077
"$openssl_bin" req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes \
    -keyout "$out/ca.key" -out "$out/ca.pem" -days 3650 -subj /CN=RealmMesh-Test-Service-CA \
    -addext basicConstraints=critical,CA:TRUE >/dev/null 2>&1
for role in realm scene other; do
    "$openssl_bin" req -new -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes \
        -keyout "$out/$role.key" -out "$out/$role.csr" -subj "/CN=$role-test" >/dev/null 2>&1
    printf '%s\n' 'basicConstraints=critical,CA:FALSE' 'keyUsage=critical,digitalSignature' \
        'extendedKeyUsage=serverAuth,clientAuth' "subjectAltName=DNS:$role-dev-01.$role.service" > "$out/$role.ext"
    "$openssl_bin" x509 -req -in "$out/$role.csr" -CA "$out/ca.pem" -CAkey "$out/ca.key" \
        -CAcreateserial -out "$out/$role.pem" -days 3650 -extfile "$out/$role.ext" >/dev/null 2>&1
done
rm -f "$out/ca.key" "$out/ca.srl" "$out/"*.csr "$out/"*.ext
