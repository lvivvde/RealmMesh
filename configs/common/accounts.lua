-- 开发账号集:有效/封禁/白名单外三类样例(登录链路规格 §3、实施票 #40)。
-- credential 是登录请求的口令字段;account_id 缺省按账号名 FNV-1a 派生非零值。
return {
    accounts = {
        { account = "player",   credential = "dev", whitelisted = true },
        { account = "pinned",   credential = "dev", whitelisted = true, account_id = 4242 },
        { account = "banned",   credential = "dev", whitelisted = true, banned = true },
        { account = "outsider", credential = "dev" },
    },
}
