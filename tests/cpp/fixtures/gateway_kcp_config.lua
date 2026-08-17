return {
    transports = {
        {
            name = "client_kcp",
            protocol = "kcp",
            enabled = true,
            listen_address = "127.0.0.1",
            listen_port = 0,
            max_sessions = 16,
            max_payload_size = 1024,
            idle_timeout_ms = 15000,
            ticket_key_environment = "REALMMESH_TEST_KCP_KEY",
        },
    },
}
