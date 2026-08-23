#!/usr/bin/env bash

# 用途：生成仅供本地开发使用的可观测性 CA、Vector Agent 和日志网关 mTLS 证书。
# 用法：./scripts/generate-observability-dev-certs.sh
# 输出：.runtime/observability/certs/（再次执行会更新同名证书）

set -euo pipefail

realmmesh_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
realmmesh_cert_dir="${realmmesh_root}/.runtime/observability/certs"
realmmesh_temp_dir="$(mktemp -d)"
trap 'rm -rf -- "${realmmesh_temp_dir}"' EXIT

mkdir -p "${realmmesh_cert_dir}"

openssl req -x509 -newkey rsa:3072 -nodes -sha256 -days 30 \
  -subj "/CN=RealmMesh Development Observability CA" \
  -keyout "${realmmesh_temp_dir}/ca.key" \
  -out "${realmmesh_cert_dir}/ca.crt"

openssl req -new -newkey rsa:2048 -nodes -sha256 \
  -subj "/CN=realmmesh-vector-agent" \
  -keyout "${realmmesh_cert_dir}/agent.key" \
  -out "${realmmesh_temp_dir}/agent.csr"
printf '%s\n' \
  'basicConstraints=CA:FALSE' \
  'keyUsage=digitalSignature,keyEncipherment' \
  'extendedKeyUsage=clientAuth' \
  'subjectAltName=DNS:vector-agent' \
  > "${realmmesh_temp_dir}/agent.ext"
openssl x509 -req -sha256 -days 30 \
  -in "${realmmesh_temp_dir}/agent.csr" \
  -CA "${realmmesh_cert_dir}/ca.crt" \
  -CAkey "${realmmesh_temp_dir}/ca.key" \
  -CAcreateserial \
  -extfile "${realmmesh_temp_dir}/agent.ext" \
  -out "${realmmesh_cert_dir}/agent.crt"

openssl req -new -newkey rsa:2048 -nodes -sha256 \
  -subj "/CN=log-gateway-a" \
  -keyout "${realmmesh_cert_dir}/gateway.key" \
  -out "${realmmesh_temp_dir}/gateway.csr"
printf '%s\n' \
  'basicConstraints=CA:FALSE' \
  'keyUsage=digitalSignature,keyEncipherment' \
  'extendedKeyUsage=serverAuth' \
  'subjectAltName=DNS:log-gateway-a,DNS:log-gateway-b' \
  > "${realmmesh_temp_dir}/gateway.ext"
openssl x509 -req -sha256 -days 30 \
  -in "${realmmesh_temp_dir}/gateway.csr" \
  -CA "${realmmesh_cert_dir}/ca.crt" \
  -CAkey "${realmmesh_temp_dir}/ca.key" \
  -CAcreateserial \
  -extfile "${realmmesh_temp_dir}/gateway.ext" \
  -out "${realmmesh_cert_dir}/gateway.crt"

chmod 600 "${realmmesh_cert_dir}/agent.key" \
  "${realmmesh_cert_dir}/gateway.key"
chmod 644 "${realmmesh_cert_dir}/ca.crt" \
  "${realmmesh_cert_dir}/agent.crt" \
  "${realmmesh_cert_dir}/gateway.crt"

printf 'Development observability certificates written to %s\n' \
  "${realmmesh_cert_dir}"
