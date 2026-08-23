-- 公共日志配置:服务层覆盖 service_name 与 metrics 端口;
-- file_path 由分层加载器按实例身份生成,不再手工配置。
return {
    logging = {
        level = "info",
        environment = "development",
        cluster = "local",
        region = "local",
        normal_queue_capacity = 8192,
        priority_queue_capacity = 2048,
        file_size_bytes = 134217728,
        retained_files = 8,
        console = true,
    },
}
