-- 只有在专用main.config显式加入chat时启动；默认登录/Scene拓扑不依赖Chat。
return {
    chat = {
        realm_id = 1, realm_instance_id = "realm-dev-01", realm_identity = "realm-dev-01.realm.service",
        channels = { 0, 1, 2 }, -- 0为Realm Channel，1/2为Scene Channel；C4按承载者拆分。
        pending_requests = 64, queued_per_link = 8,
        pending_pushes = 1024, pending_push_bytes = 4194304, operation_timeout_ms = 1000,
    },
    logging = { service_name = "chat", metrics_listen_address = "127.0.0.1", metrics_port = 9107 },
    service_discovery = {
        instance_id = "chat-dev-01", node_id = "development-node", zone = "development",
        advertise_address = "127.0.0.1",
    },
    tick_rate = 100,
    transports = {{
        name = "chat_internal", protocol = "tls_tcp", enabled = true,
        listen_address = "127.0.0.1", listen_port = 7300,
        max_sessions = 16, max_payload_size = 65536, max_pending_output_bytes = 1048576,
        handshake_timeout_ms = 1000, idle_timeout_ms = 30000,
        alpn = "realmmesh-chat/1",
        certificate_chain_file_environment = "REALMMESH_CHAT_CERTIFICATE_FILE",
        private_key_file_environment = "REALMMESH_CHAT_PRIVATE_KEY_FILE",
        client_ca_file_environment = "REALMMESH_SERVICE_CA_FILE",
        allowed_client_identities = { "realm-dev-01.realm.service" },
    }},
}
