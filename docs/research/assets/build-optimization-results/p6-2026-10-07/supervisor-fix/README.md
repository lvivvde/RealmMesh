# Supervisor 第二次 wait 中断的诊断与回归

这是 #129 的独立诊断证据，不能作为性能样本。最小探针从传入源码提取准确的管理函数，
使用本次拥有的最小 worker；`--trace` 只在临时函数加入带 `[DEBUG-p6]` 标签的边界日志，
`--repeat-wait` 仅替换等待逻辑作单变量对照。生产脚本没有诊断输出。

延迟 worker 的原实现与诊断组各失败 13/1000；重复等待对照为 0/1000。
最终真实回归夹具在原实现失败，在修复后两平台通过。四个真实停止边界各 20 次验证日志
同样保存。所有 MongoDB/etcd 均为隔离夹具，不接共享开发库。

`*-precondition*-run.log` 是验证副本初期缺少工具发现或脚本的夹具错误；
`mac-stop-regression-green-run.log` 是最初包装 builtin wait 的夹具失败，
最终可移植注入结果见 `mac-stop-regression-alias-run.log` 与 Linux 的 `stop-regression-*-run.log`。
