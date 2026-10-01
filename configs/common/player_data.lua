return {
    player_data = {
        -- 当前单主机部署由三个服务共享同一个权威 SQLite 文件。
        database_file = "data/player-data.sqlite",
        -- 仅空库首次启动时导入开发账号；之后 SQLite 永远是事实来源。
        bootstrap_accounts_file = "common/accounts.lua",
        -- 新写入口令的 Argon2 成本："interactive"（生产默认）或仅供批量
        -- 机器人夹具使用的 "minimum"。已存哈希按自身参数校验。
        credential_hash_cost = "interactive",
        busy_timeout_ms = 500,
    },
}
