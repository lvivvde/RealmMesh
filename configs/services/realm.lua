return {
    logging = {
        service_name = "realm",
        metrics_listen_address = "127.0.0.1",
        metrics_port = 9102,
    },
    service_discovery = {
        instance_id = "realm-dev-01",
        node_id = "development-node",
        zone = "development",
        advertise_address = "127.0.0.1",
    },
    realm = {
        -- 训练规则(#93):启动时加载,失败即启动失败;不热更。
        training_rule_file = "services/realm/training.lua",
        -- 可选 Scene 玩法:提供独立服务 CA/身份后启用;缺少 Scene 不影响训练。
        scene = {
            enabled = false,
            host = "127.0.0.1", port = 7200,
            instance_id = "scene-dev-01", server_identity = "scene-dev-01.scene.service",
            ca_file_environment = "REALMMESH_SERVICE_CA_FILE",
            certificate_file_environment = "REALMMESH_REALM_CERTIFICATE_FILE",
            private_key_file_environment = "REALMMESH_REALM_PRIVATE_KEY_FILE",
        },
        data_workers = 4,
        data_capacity = 256,
        max_pending_per_session = 16,
        retry_after_seconds = 1,
    },
    max_events_per_frame = 4096,
    runtime = {
        inbound_capacity = 16384,
        outbound_capacity = 16384,
        max_commands_per_cycle = 4096,
        io_poll_interval_ms = 2,
    },
    transports = {
        {
            name = "realm_tls_tcp",
            protocol = "tls_tcp",
            enabled = true,
            listen_address = "0.0.0.0",
            listen_port = 7100,
            max_sessions = 10000,
            max_payload_size = 4096,
            max_pending_output_bytes = 1048576,
            handshake_timeout_ms = 3000,
            idle_timeout_ms = 30000,
            alpn = "realmmesh-edge/1",
            certificate_chain_file_environment = "REALMMESH_TLS_CERTIFICATE_FILE",
            private_key_file_environment = "REALMMESH_TLS_PRIVATE_KEY_FILE",
        },
    },
}
