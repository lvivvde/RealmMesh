-- 公共服务发现配置:实例身份(instance_id/node_id/zone/advertise_address)
-- 由各服务层提供;enabled=true 且 required=false 时 etcd 不可用仅告警降级。
return {
    service_discovery = {
        enabled = true,
        required = false,
        endpoint = "http://127.0.0.1:2379",
        key_prefix = "/realmmesh/services",
        lease_ttl_seconds = 15,
        request_timeout_ms = 500,
        watch_interval_ms = 500,
    },
}
