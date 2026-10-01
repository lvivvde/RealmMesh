# Linux Login Chain M1–M4 验收规格

状态：已实现 CI 缩减基线；专用机器容量执行需单独批准。

## 目标与边界

本规格把 #91 的 Linux 行为与 M1–M4 验收收敛为一个可重复命令：

```bash
./scripts/run-linux-login-acceptance.sh
```

它验证生产 Login Chain、`tools/loadgen`、真实 MsQuic、TLS/TCP、真实证书、
独立服务进程、服务发现、临时真实 etcd、故障关闭与恢复。M5 云端百万端到端、
真实账号/角色数据和玩法不在本规格内。

## 环境决定

自动门禁固定在 GitHub-hosted Ubuntu 24.04：gcc、epoll、官方固定版本
libmsquic、四个 `realm_mesh --service` 进程和单节点临时 etcd 位于同一主机。
报告记录内核、CPU、内存、fd 上限、revision、配置哈希、命令、重复次数与原始输出。
服务发现 lease TTL 固定为 5 秒，使 SIGKILL 后的 budget key 能在 10 秒门槛内
自然过期；publisher 仍按 `ttl/3` 续租。

此拓扑刻意只绑定 loopback。仓库当前没有 etcd TLS/客户端认证配置，因而不得把
未加固的 etcd 暴露到跨机器网络；跨机器执行必须先另行交付 etcd TLS/认证和证书
分发，再用相同报告格式验收。该限制必须出现在每份 CI 报告里。

专用容量机沿用 #32 的输入假设：64 GiB、32 核、fd 1048576。执行 10 万/百万
规模会产生显著资源成本，只能在明确批准的环境运行；共享 CI 只做确定性缩减回归，
不得把缩减结果写成生产容量结论。

## 公共测试缝

- 传输：真实 MsQuic 客户端/服务端交换一个已验签流；生产 Login Chain 同时验证
  QUIC 不支持时的 TLS/TCP 降级以及证书失败不得降级。
- 拓扑：真实 Login Verifier、Queue、Gateway、Realm 独立进程和真实 etcd。
- 负载：所有 M1–M3 机器人仍执行同一个 `framework/client::LoginChain`；不创建
  第二条压测专用状态机。
- 恢复：从外部停止服务进程或暂停 etcd，通过 readiness、再次 Full 登录、持久化
  Queue Number 与 Admission Grant 单次消费观察结果。

## 门槛

| 级别 | CI 缩减输入与硬门槛 | 专用环境目标（#32） | 重复 |
|---|---|---|---:|
| Transport | 真实 QUIC 帧交换；TLS/TCP 竞速降级；真实 CA；错误证书不降级 | Gateway 与 Realm 两段候选路径 | 3 |
| M1 | 100 Gateway Session 保持 5 秒；至少两个管线阶段同时非零；attach P99 不超过 `max(同负载基线 P99, 1ms) × 5`；fd 不增长 | 10 万混合水位 30 分钟；内存 ≤3 GiB；P99 不漂移；零 fd/OOM | 3 |
| M2 | 250 robots、64 并发、25 秒；至少 248 handed-off；拉取失败率 <1% | 10 万 robots 在 60 秒内 handed-off | 3 |
| M3 | 10000 次耐久取号 + 2000 个 progress 客户端；成功率 ≥99.9% | 百万取号；源站 ≤5000 QPS；缓存命中 ≥99% | 1 |
| M4 | SIGKILL Gateway 后 budget key 移除与重建 ≤10 秒；SIGKILL Queue 后冷恢复 ≤30 秒且真实 etcd `released_number` 不回退；Grant 不重复消费；etcd 故障 readiness 收敛 ≤10 秒 | 同门槛在 staged 拓扑执行 | 1 |

Queue 发号测试另输出 1700 次线性一致提交的实测速率。该数字是环境观测值，
不是跨机器稳定的单测下限；若未达到约 1700/s，报告必须保留差距，不能放宽一致性。

## 报告与 CI

Linux CI 在全量 `ctest` 通过后运行验收脚本，并始终尝试上传：

- `build/dev/acceptance/linux-login-chain-m1-m4.md`
- `build/dev/acceptance/linux-login-chain-m1-m4.log`

任一分组失败、报告缺少度量标记或 Linux 专属 QUIC 测试不存在，脚本返回非零。
报告中的 PASS 只表示上述 CI 缩减门槛；专用容量目标必须由单独报告声明输入规模、
硬件、拓扑、配置和原始输出后才能接受。
