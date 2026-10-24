# 独立Chat服务（C2）

本页只描述Chat内部服务。客户端仍不能直接聊天，Realm/客户端接线在C3；双Scene/双Chat和
组合故障验收在C4/C5。Chat不访问MongoDB，不保存历史或离线消息。

## 启动与配置

在专用配置根的`main.config`显式加入`{name="chat"}`，沿用`configs/services/chat.lua`。
默认四服务登录配置不会启动Chat，也不要求它在线。用所选preset构建后的`bin/realm_mesh`
执行`--config <专用配置根> --service chat`，不要另起一套业务宿主。

证书和私钥由环境变量`REALMMESH_CHAT_CERTIFICATE_FILE`、`REALMMESH_CHAT_PRIVATE_KEY_FILE`
指向仓库外文件，`REALMMESH_SERVICE_CA_FILE`指向专用服务CA；不使用公网接入票据或公网CA。
Realm客户端须用服务CA证书并校验Chat精确DNS SAN。测试身份仅在构建目录生成，不能用于部署。

默认TLS/TCP监听7300、HTTP指标端口9107。`chat.realm_id`、`realm_instance_id`与
`realm_identity`共同固定允许的唯一Realm；transport的allowed_client_identities必须恰好等于
该realm_identity，ALPN固定为realmmesh-chat/1。`channels`是本实例唯一承载的稠密数组，
0表示Realm Channel，1/2表示Scene Channel；不得重复。C4两个承载者需配置互不重叠子集，
不能靠etcd出现多个端点自动换主。开启通用service_discovery后由宿主注册chat实例；etcd只发现地址。

可调资源只允许在C1硬上限内缩小：pending_requests=64、queued_per_link=8、
pending_pushes=1024、pending_push_bytes=4194304、operation_timeout_ms=1000。
传输最多16连接（含握手）、Envelope64KiB、每链路TLS出站1MiB；Chat内部启用每连接每poll
64KiB解密读取预算，SSL缓冲在后续poll继续读取。不是容量测量或跨机器性能承诺。

## 受理与恢复

认证后先HELLO安装新链路，再JOIN取得Epoch/W。活动JOIN重试不刷新W或清空最后回执；
SEND同号同正文回原回执，冲突/过旧/跳号拒绝。每资格一个SEND在途、每轮至多一个新SEND。
请求和未取走回复占同一预算；排队过期不执行，满额明确BUSY。Accepted只表示内存在线受理。

单帧TLS输出预算拒绝会关闭该Realm链路并回收资格；已受理回执与频道水位不回滚。
重新认证、重新JOIN从新W观察后续，不补投缺口。SIGKILL重启创建新Epoch；旧Epoch/链路拒绝，
丢失回执在调用方保持Unknown。发现开启时，同名重启须等旧租约注销/过期，不能抢占旧注册。
正常停止与异常退出都不执行无限网络清理或数据库工作。

## 验证

状态机：`./scripts/test-fast.sh --target chat_channels_test --test-regex '^ChatChannelsTest\.'`。
真实生产进程：先构建chat_service_test，再用`ctest --preset dev -j 1 -R '^ChatServiceTest\.'`。
它使用独立测试CA和真实临时etcd，覆盖证书/ALPN/Realm拒绝、排序/回执、旧链路隔离、单帧
TLS输出拒绝、SIGKILL/租约过期/同名重启、未HELLO截止和正常回收；不连接共享MongoDB。
TLS缓冲预算回归由BoundedMutualTlsTest验证。网络/公共头改动仍须完整`./scripts/build.sh`。

`/metrics`发布Chat请求/发送计数与延迟、成员及待处理/待推送数量、字节占用、回执重放、限流/
过期/旧资格/资源拒绝。零拒绝系列显式发布；标签限有限操作/状态/结果，不含角色、具体频道ID、
Epoch或聊天正文。C3添加Realm侧Unknown/客户端缺口等指标，不能把服务端零Unknown理解为端到端确认。
