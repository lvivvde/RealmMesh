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
        kid = "queue-v1",
        identity_kid = "login-verify-v1",
        identity_issuer = "realmmesh/login-verify",
        release_step = 3000,
        release_interval_seconds = 2,
        budget_interval_seconds = 1,
        certificate_chain_file_environment = "REALMMESH_TLS_CERTIFICATE_FILE",
        private_key_file_environment = "REALMMESH_TLS_PRIVATE_KEY_FILE",
        alpn = "http/1.1",
        etcd_endpoint = "http://127.0.0.1:2379",
    },
}
