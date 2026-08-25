-- 公共服务发现配置:实例身份(instance_id/node_id/zone/advertise_address)
-- 由各服务层提供。本地/模式 1 默认关闭;分布式部署(模式 2)在各环境配置中
-- 开启并确保 etcd 可达——开启时 ready 需注册成功,etcd 缺失会启动失败
-- (required=true 注册失败直接抛错;required=false 告警降级且不 ready,
-- 续约成功后才补齐)。
return {
    service_discovery = {
        enabled = false,
        required = false,
        endpoint = "http://127.0.0.1:2379",
        key_prefix = "/realmmesh/services",
        lease_ttl_seconds = 15,
        request_timeout_ms = 500,
        watch_interval_ms = 500,
        startup_timeout_ms = 5000,
    },
}
