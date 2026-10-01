return {
    player_data = {
        -- 三个服务共享同一个权威 MongoDB 副本集与库(ADR-0011)。开发拓扑
        -- 只监听本机且不开认证；跨机器暴露前须交付 TLS 与认证。
        uri = "mongodb://127.0.0.1:27017/?replicaSet=rs0",
        database = "realmmesh",
        -- 找不到可写 primary、或单次读写超过这两个上限，即按数据源不可用失败。
        server_selection_timeout_ms = 2000,
        socket_timeout_ms = 2000,
        -- 仅空库首次启动时导入开发账号；之后 MongoDB 永远是事实来源。
        bootstrap_accounts_file = "common/accounts.lua",
        -- 新写入口令的 Argon2 成本："interactive"（生产默认）或仅供批量
        -- 机器人夹具使用的 "minimum"。已存哈希按自身参数校验。
        credential_hash_cost = "interactive",
    },
}
