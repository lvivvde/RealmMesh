# Scene 阶段4最终验收与合并记录

- 2026-10-23：S5 [PR #174](https://github.com/lvivvde/RealmMesh/pull/174)已合并，合并提交`196d678dcf463ad483769ebac2ff40c806b45fe0`；#165自动关闭，父任务#160完成。
- 实际冻结验收源码为`d5497bfad1272e40f4030ecff2dcac154ca2f867`，工作树干净；不是本次后续文档变更的测试记录。

| 门禁 | 结果 | CTest秒数 |
|---|---|---|
| macOS本机全量 | 771/771 | 703.20 |
| Linux本机全量 | 772/772 | 625.56 |
| macOS三次完整Scene | 每次87/87 | 358.07 / 359.05 / 359.31 |
| Linux三次完整Scene | 每次87/87 | 342.72 / 342.82 / 345.94 |
| Linux本机QUIC/TLS、M1–M4 | 全部通过 | 原始报告记录 |
| CI macOS / Linux | 770/770 / 772/772；Linux QUIC/M1–M4通过 | 原始日志记录 |
| 双轴复审、报告契约与无操作构建 | Standards/Spec各0项未解决；10项契约通过；双平台no work to do | 不作性能比较 |

[CI39821536024](https://github.com/lvivvde/RealmMesh/actions/runs/39821536024)关联head为d5497bf，
checkout的PR合并提交为`a4fe07252562fec97fe9843057a6528973020aa6`。macOS本机装有MsQuic，
CI macOS不编QUIC；本机Linux为Lima Ubuntu26.04.1/aarch64，CI为Ubuntu24.04.5/x86_64。
Scene均只验收本机多进程TLS/TCP，不代表跨机器、玩法容量、Chat或M5；耗时不用于性能对比。

六轮报告均核对14个故障边界、21个必需指标、进入/移动/转场非零结果和延迟样本，以及真实
数据库与回收属性。服务端背压、客户端通知缓存、私有MongoDB暂停及响应丢失边界分别保留证据，
不互相替代。早期失败、TLS bad write retry修复和CI健康会话空闲超时修正也保留原始记录。

原工作区的持久证据：

- 汇总与1518个文件的SHA256清单：`.local/acceptance/scene-s5/d5497bf/report.md`及`manifest.json`。
- 完整归档：`.local/acceptance/scene-s5/scene-s5-d5497bf-complete-evidence.tar.gz`，2,859,212字节。
- 归档SHA256：`9057ce6ab6ecd37195bda8d5fa654a16f5a40255c5df17344f6b48bd9bd7f479`。
- 归档保存在原工作区`/Users/edwin/Projects/RealmMesh`内，上述路径均相对该目录；已逐项校验并保留Linux/CI原始日志及失败目录。归档不包含共享开发库凭据。

复现入口：`./scripts/build.sh --preset dev --jobs N`；每平台三次
`./scripts/run-scene-acceptance.sh --preset dev --jobs N`；Linux另运行
`REALMMESH_LINUX_ACCEPTANCE_SKIP_BUILD=1 ./scripts/run-linux-login-acceptance.sh --preset dev --jobs N`。
配置、环境、排除项与原始来源以归档中的冻结报告为准，不以更新文档重写历史验收结论。
