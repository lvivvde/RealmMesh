#!/usr/bin/env bash

# 用途：等待指定 mongod 可连接，若尚未初始化则以单成员 rs0 初始化副本集，
# 并等到它成为 primary。可重复执行。
# 用法：./scripts/mongodb-init-replset.sh <mongosh 路径> <host:port>

set -euo pipefail

mongosh_bin="$1"
member="$2"

for _ in $(seq 1 100); do
    if "${mongosh_bin}" --quiet --norc "mongodb://${member}/?directConnection=true" \
        --eval 'db.adminCommand({ping: 1}).ok' >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
done

"${mongosh_bin}" --quiet --norc "mongodb://${member}/?directConnection=true" --eval "
    try {
        rs.status();
    } catch (error) {
        if (error.codeName !== 'NotYetInitialized') throw error;
        rs.initiate({_id: 'rs0', members: [{_id: 0, host: '${member}'}]});
    }
    for (let i = 0; i < 200 && !db.hello().isWritablePrimary; ++i) sleep(100);
    if (!db.hello().isWritablePrimary) throw new Error('rs0 did not elect a primary');
" >/dev/null
