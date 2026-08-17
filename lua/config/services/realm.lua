return {
    tick_rate = 20,
    max_events_per_frame = 4096,
    downstream_address = "127.0.0.1",
    downstream_port = 8000,
    runtime = {
        inbound_capacity = 16384,
        outbound_capacity = 16384,
        max_commands_per_cycle = 4096,
        io_poll_interval_ms = 2,
    },
    transports = {
        {
            name = "realm_tcp",
            protocol = "tcp",
            enabled = true,
            listen_address = "0.0.0.0",
            listen_port = 7100,
            max_sessions = 10000,
            max_payload_size = 4096,
            max_pending_output_bytes = 1048576,
        },
    },
}
