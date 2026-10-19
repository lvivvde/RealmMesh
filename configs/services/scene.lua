-- 在自己的 main.config 加入 scene 后独立运行 --service scene。服务 CA 与公网接入证书分离。
return {
    scene = { shard_id = 1 },
    logging = { service_name = "scene", metrics_listen_address = "127.0.0.1", metrics_port = 9106 },
    service_discovery = {
        instance_id = "scene-dev-01", node_id = "development-node", zone = "development",
        advertise_address = "127.0.0.1",
    },
    tick_rate = 100,
    runtime = { inbound_capacity = 256, outbound_capacity = 1024,
        max_commands_per_cycle = 256, io_poll_interval_ms = 2 },
    transports = {{
        name = "scene_internal", protocol = "tls_tcp", enabled = true,
        listen_address = "127.0.0.1", listen_port = 7200,
        max_sessions = 64, max_payload_size = 65536, max_pending_output_bytes = 262144,
        handshake_timeout_ms = 3000, idle_timeout_ms = 30000,
        alpn = "realmmesh-scene/1",
        certificate_chain_file_environment = "REALMMESH_SCENE_CERTIFICATE_FILE",
        private_key_file_environment = "REALMMESH_SCENE_PRIVATE_KEY_FILE",
        client_ca_file_environment = "REALMMESH_SERVICE_CA_FILE",
        allowed_client_identities = { "realm-dev-01.realm.service" },
    }},
}
