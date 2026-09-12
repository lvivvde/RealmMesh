return {
    logging = {
        service_name = "login_verify",
        metrics_listen_address = "127.0.0.1",
        metrics_port = 9104,
        module_levels = {
            ["framework.network"] = "info",
        },
    },
    service_discovery = {
        instance_id = "login-verify-dev-01",
        node_id = "development-node",
        zone = "development",
        advertise_address = "127.0.0.1",
    },
    login_verify = {
        -- listen_port = 0 由内核分配(开发/测试避免端口冲突);部署时显式指定。
        listen_address = "127.0.0.1",
        listen_port = 0,
        kid = "login-verify-v1",
        accounts_file = "common/accounts.lua",
        certificate_chain_file_environment = "REALMMESH_TLS_CERTIFICATE_FILE",
        private_key_file_environment = "REALMMESH_TLS_PRIVATE_KEY_FILE",
        alpn = "http/1.1",
    },
}
