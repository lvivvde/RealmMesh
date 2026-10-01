#!/usr/bin/env bash
# 新开发机：通过已配置且指纹已验证的 SSH 别名获取私有连接材料。
# 用法：./scripts/configure-shared-mongodb.sh [ssh-alias]（默认 vps）
set -euo pipefail
umask 077
ssh_host="${1:-vps}"
config_dir="${REALMMESH_MONGODB_CONFIG_DIR:-${XDG_CONFIG_HOME:-${HOME}/.config}/realmmesh/mongodb}"
local_port="${REALMMESH_MONGODB_LOCAL_PORT:-27018}"
[[ "${local_port}" =~ ^[0-9]+$ && "${local_port}" -ge 1024 && "${local_port}" -le 65535 ]] || {
    echo 'Invalid MongoDB tunnel port.' >&2; exit 1;
}
mkdir -p "${config_dir}"
chmod 700 "${config_dir}"
staging="$(mktemp -d "${config_dir}/configure.XXXXXX")"
trap 'rm -rf -- "${staging}"' EXIT
scp -q -o BatchMode=yes -o StrictHostKeyChecking=yes \
    "${ssh_host}:.config/realmmesh/mongodb/client.json" "${staging}/client.json"
scp -q -o BatchMode=yes -o StrictHostKeyChecking=yes \
    "${ssh_host}:.config/realmmesh/mongodb/ca.pem" "${staging}/ca.pem"
python3 - "${staging}" "${config_dir}" "${ssh_host}" "${local_port}" <<'PY'
import json, pathlib, shlex, sys, urllib.parse
staging, directory = map(pathlib.Path, sys.argv[1:3])
client = json.loads((staging / 'client.json').read_text())
q = urllib.parse.quote
uri = (f"mongodb://{q(client['username'], safe='')}:{q(client['password'], safe='')}@127.0.0.1:{sys.argv[4]}/"
       f"?replicaSet=rs0&directConnection=true&authSource={q(client['database'], safe='')}&tls=true"
       f"&tlsCAFile={q(str(directory / 'ca.pem'), safe='')}")
(staging / 'env.sh').write_text('export REALMMESH_MONGODB_URI=' + shlex.quote(uri) + '\n'
    'REALMMESH_MONGODB_SSH_HOST=' + shlex.quote(sys.argv[3]) + '\n'
    'REALMMESH_MONGODB_LOCAL_PORT=' + shlex.quote(sys.argv[4]) + '\n')
PY
for file in ca.pem client.json env.sh; do
    chmod 600 "${staging}/${file}"
    mv -f "${staging}/${file}" "${config_dir}/${file}"
done
printf 'Private MongoDB configuration installed outside the repository.\n'
