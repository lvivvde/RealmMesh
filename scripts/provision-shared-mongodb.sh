#!/usr/bin/env bash
# 在 Ubuntu 24.04 x86_64 上部署共享临时开发库。以 sudo 运行，旁边需有
# install-mongodb.sh；实际地址和随机凭据只保存在服务器与客户端私有目录。
set -euo pipefail
umask 077

[[ "$(id -u)" == 0 ]] || { echo 'Run with sudo.' >&2; exit 1; }
client_user="${REALMMESH_MONGODB_CLIENT_USER:-${SUDO_USER:-}}"
[[ -n "${client_user}" && "${client_user}" != root ]] || {
    echo 'Set REALMMESH_MONGODB_CLIENT_USER to the development user.' >&2; exit 1;
}
client_home="$(getent passwd "${client_user}" | cut -d: -f6)"
[[ -n "${client_home}" ]] || exit 1
[[ "${client_user}" =~ ^[[:alnum:]_-]+$ ]] || exit 1
install_root=/opt/realmmesh-mongodb
secret_root=/etc/realmmesh-mongodb
client_dir="${client_home}/.config/realmmesh/mongodb"

# 仅为开发用户开放到数据库回环端口的本地转发，保留全局 SSH 策略。
cat > /etc/ssh/sshd_config.d/60-realmmesh-mongodb.conf <<SSH
Match User ${client_user}
    AllowTcpForwarding local
    PermitOpen 127.0.0.1:27017
Match all
SSH
chmod 644 /etc/ssh/sshd_config.d/60-realmmesh-mongodb.conf
/usr/sbin/sshd -t
systemctl reload ssh

if systemctl is-active --quiet realmmesh-mongodb; then
    echo 'Shared MongoDB is already running; existing data and credentials retained.'
    exit 0
fi
if ! id realm-mongodb >/dev/null 2>&1; then
    useradd --system --home-dir /var/lib/realmmesh-mongodb --shell /usr/sbin/nologin realm-mongodb
fi
install -d -m 755 "${install_root}/scripts"
install -m 755 "$(dirname "$0")/install-mongodb.sh" "${install_root}/scripts/install-mongodb.sh"
if [[ ! -x "${install_root}/.tools/mongodb-8.0.32/bin/mongod" ||
      ! -x "${install_root}/.tools/mongosh-2.12.0/bin/mongosh" ]]; then
    "${install_root}/scripts/install-mongodb.sh"
fi
# 解压后的可执行文件保留；已校验的压缩包不再占用小磁盘。
rm -f "${install_root}/.tools/mongodb-linux-x86_64-ubuntu2404-8.0.32.tgz" \
    "${install_root}/.tools/mongosh-2.12.0-linux-x64.tgz"
chmod -R a+rX "${install_root}/.tools"
install -d -m 750 -o root -g realm-mongodb "${secret_root}"
install -d -m 750 -o realm-mongodb -g realm-mongodb \
    /var/lib/realmmesh-mongodb /var/log/realmmesh-mongodb

if [[ ! -f "${secret_root}/server.pem" ]]; then
    openssl req -x509 -newkey rsa:3072 -nodes -sha256 -days 3650 \
        -subj '/CN=RealmMesh Development MongoDB CA' \
        -addext 'basicConstraints=critical,CA:TRUE' \
        -addext 'keyUsage=critical,keyCertSign,cRLSign' \
        -keyout "${secret_root}/ca-key.pem" -out "${secret_root}/ca.pem" 2>/dev/null
    openssl req -new -newkey rsa:3072 -nodes \
        -subj '/CN=localhost' -keyout "${secret_root}/server-key.pem" \
        -out "${secret_root}/server.csr" 2>/dev/null
    cat > "${secret_root}/server.ext" <<'TLS'
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth,clientAuth
subjectAltName=DNS:localhost,IP:127.0.0.1
TLS
    openssl x509 -req -in "${secret_root}/server.csr" \
        -CA "${secret_root}/ca.pem" -CAkey "${secret_root}/ca-key.pem" \
        -CAcreateserial -days 825 -sha256 -extfile "${secret_root}/server.ext" \
        -out "${secret_root}/server-cert.pem" 2>/dev/null
    cat "${secret_root}/server-cert.pem" "${secret_root}/server-key.pem" \
        > "${secret_root}/server.pem"
    rm -f "${secret_root}/server-key.pem" "${secret_root}/server.csr"
fi
if [[ ! -f "${secret_root}/keyfile" ]]; then
    openssl rand -base64 756 > "${secret_root}/keyfile"
fi
chown realm-mongodb:realm-mongodb "${secret_root}/server.pem" "${secret_root}/keyfile"
chmod 400 "${secret_root}/server.pem" "${secret_root}/keyfile"
chmod 644 "${secret_root}/ca.pem"
if [[ ! -f "${secret_root}/credentials.json" ]]; then
    python3 - "${secret_root}/credentials.json" <<'PY'
import json, secrets, sys
with open(sys.argv[1], 'x') as f:
    json.dump({'admin_password': secrets.token_hex(32), 'username': 'realmmesh_dev',
               'password': secrets.token_hex(32), 'database': 'realmmesh'}, f)
PY
fi
cat > "${secret_root}/mongod.conf" <<'CONFIG'
storage:
  dbPath: /var/lib/realmmesh-mongodb
  wiredTiger:
    engineConfig:
      cacheSizeGB: 0.256
systemLog:
  destination: file
  path: /var/log/realmmesh-mongodb/mongod.log
  logAppend: true
  logRotate: reopen
net:
  bindIp: 127.0.0.1
  port: 27017
  maxIncomingConnections: 64
  tls:
    mode: requireTLS
    certificateKeyFile: /etc/realmmesh-mongodb/server.pem
    CAFile: /etc/realmmesh-mongodb/ca.pem
    allowConnectionsWithoutCertificates: true
security:
  authorization: enabled
  keyFile: /etc/realmmesh-mongodb/keyfile
replication:
  replSetName: rs0
  oplogSizeMB: 128
CONFIG
chown root:realm-mongodb "${secret_root}/mongod.conf"
chmod 640 "${secret_root}/mongod.conf"
cat > /etc/systemd/system/realmmesh-mongodb.service <<'UNIT'
[Unit]
Description=RealmMesh shared temporary development MongoDB
After=network.target
StartLimitIntervalSec=60
StartLimitBurst=3
[Service]
Type=simple
User=realm-mongodb
Group=realm-mongodb
ExecStart=/opt/realmmesh-mongodb/.tools/mongodb-8.0.32/bin/mongod --config /etc/realmmesh-mongodb/mongod.conf
Restart=on-failure
RestartSec=5
TimeoutStopSec=60
LimitNOFILE=64000
UMask=0077
MemoryHigh=480M
MemoryMax=600M
MemorySwapMax=256M
NoNewPrivileges=true
PrivateTmp=true
ProtectHome=true
ProtectSystem=strict
ReadWritePaths=/var/lib/realmmesh-mongodb /var/log/realmmesh-mongodb
[Install]
WantedBy=multi-user.target
UNIT
cat > /etc/logrotate.d/realmmesh-mongodb <<'ROTATE'
/var/log/realmmesh-mongodb/mongod.log {
    daily
    maxsize 10M
    rotate 3
    compress
    delaycompress
    missingok
    notifempty
    create 0600 realm-mongodb realm-mongodb
    postrotate
        /bin/systemctl kill --kill-who=main --signal=SIGUSR1 realmmesh-mongodb.service >/dev/null 2>&1 || true
    endscript
}
ROTATE
systemctl daemon-reload
systemctl enable --now realmmesh-mongodb
cat > "${secret_root}/bootstrap.js" <<'JS'
const fs = require('fs');
const secrets = JSON.parse(fs.readFileSync('/etc/realmmesh-mongodb/credentials.json', 'utf8'));
const uri = 'mongodb://127.0.0.1:27017/?directConnection=true&tls=true&tlsCAFile=%2Fetc%2Frealmmesh-mongodb%2Fca.pem';
let conn;
for (let i = 0; i < 60; ++i) {
    try { conn = new Mongo(uri); break; } catch (_) { sleep(500); }
}
if (!conn) throw new Error('MongoDB did not start');
const admin = conn.getDB('admin');
let authenticated = false;
try { authenticated = !!admin.auth('realmmesh_admin', secrets.admin_password); } catch (_) {}
if (!authenticated) {
    const initiated = admin.runCommand({replSetInitiate: {_id: 'rs0', members: [{_id: 0, host: 'localhost:27017'}]}});
    if (!initiated.ok && initiated.codeName !== 'AlreadyInitialized') throw new Error('Replica set initiation failed');
    for (let i = 0; i < 60 && !admin.hello().isWritablePrimary; ++i) sleep(500);
    if (!admin.hello().isWritablePrimary) throw new Error('No writable primary');
    admin.createUser({user: 'realmmesh_admin', pwd: secrets.admin_password,
                      roles: [{role: 'root', db: 'admin'}]});
    if (!admin.auth('realmmesh_admin', secrets.admin_password)) throw new Error('Admin authentication failed');
}
const app = conn.getDB(secrets.database);
if (!app.getUser(secrets.username)) {
    app.createUser({user: secrets.username, pwd: secrets.password,
                   roles: [{role: 'readWrite', db: secrets.database}]});
}
print('rs0 primary ready; database-scoped development user configured');
JS
"${install_root}/.tools/mongosh-2.12.0/bin/mongosh" --nodb --quiet --file "${secret_root}/bootstrap.js"
install -d -m 700 -o "${client_user}" -g "$(id -gn "${client_user}")" "${client_dir}"
install -m 600 -o "${client_user}" -g "$(id -gn "${client_user}")" "${secret_root}/ca.pem" "${client_dir}/ca.pem"
python3 - "${secret_root}/credentials.json" "${client_dir}" <<'PY'
import json, pathlib, shlex, sys, urllib.parse
data = json.loads(pathlib.Path(sys.argv[1]).read_text())
directory = pathlib.Path(sys.argv[2])
client = {k: data[k] for k in ('username', 'password', 'database')}
(directory / 'client.json').write_text(json.dumps(client))
q = urllib.parse.quote
uri = (f"mongodb://{q(client['username'], safe='')}:{q(client['password'], safe='')}@127.0.0.1:27017/"
       f"?replicaSet=rs0&directConnection=true&authSource={q(client['database'], safe='')}&tls=true"
       f"&tlsCAFile={q(str(directory / 'ca.pem'), safe='')}")
(directory / 'env.sh').write_text('export REALMMESH_MONGODB_URI=' + shlex.quote(uri) + '\n'
    "REALMMESH_MONGODB_SSH_HOST=''\nREALMMESH_MONGODB_LOCAL_PORT=27017\n")
PY
chown "${client_user}:$(id -gn "${client_user}")" "${client_dir}/client.json" "${client_dir}/env.sh"
chmod 600 "${client_dir}/client.json" "${client_dir}/env.sh"
systemctl show realmmesh-mongodb --property=ActiveState,MemoryCurrent,MemoryHigh,MemoryMax
echo 'Shared MongoDB deployed; credentials were saved privately, not printed.'
