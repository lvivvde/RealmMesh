return {
    logging = {
        service_name = "queue",
        metrics_listen_address = "127.0.0.1",
        metrics_port = 9105,
        module_levels = {
            ["framework.network"] = "info",
        },
    },
    service_discovery = {
        instance_id = "queue-dev-01",
        node_id = "development-node",
        zone = "development",
        advertise_address = "127.0.0.1",
    },
    queue = {
        -- listen_port = 0 由内核分配(开发/测试避免端口冲突);部署时显式指定。
        listen_address = "127.0.0.1",
        listen_port = 0,
        queue_number_kid = "queue-number-v2",
        admission_grant_kid = "admission-grant-v1",
        admission_grant_issuer = "realmmesh/queue",
        deployment_id = "development",
        identity_kid = "login-verify-v1",
        identity_issuer = "realmmesh/login-verify",
        release_step = 3000,
        release_interval_seconds = 2,
        budget_interval_seconds = 1,
        -- Admission Grant 窗口:必须与 gateway.lua 的
        -- admission.grant_window_seconds 一致,否则合法 Grant 会在网关侧
        -- 被误判为过期(或反向放宽)。协议硬上限 600s。
        admit_grace_seconds = 300,
        certificate_chain_file_environment = "REALMMESH_TLS_CERTIFICATE_FILE",
        private_key_file_environment = "REALMMESH_TLS_PRIVATE_KEY_FILE",
        alpn = "http/1.1",
        etcd_endpoint = "http://127.0.0.1:2379",
        snapshot_key = "/realmmesh/queue/snapshot",
        issuance_prefix = "/realmmesh/queue/issuance",
        -- 发号映射与 next_number 是同一笔 etcd 事务；权威状态不可读时
        -- 必须拒绝启动，不能从零重发已经确认给客户端的号码。
    },
}
