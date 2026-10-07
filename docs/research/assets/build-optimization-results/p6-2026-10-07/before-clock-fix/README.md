# 时钟修正前的 Linux 冻结组（#129）

产品已包含 Supervisor 修复（`7858a925`），工具为 `ee44ff80`。主性能 R0/最终各 42 行与 cache 7 行已经完成，但稳定性首轮发生 0 编译 / 55 链接，sodium 库恢复后的无操作再次为 0 / 33。原检查驱动两次退出 1，全部失败和输入快照保留，未修改产物时间戳。

Lima guestagent 每十秒把系统时间校回主机约 428ms；专用目录 125 次自然写入捕获 mtime 倒退 325ms、单调时钟前进 103ms。原 chrony 同时运行，tracking 记录总频率校正约 42815 ppm；实际内核细粒度频率约 15.54 ppm，粗粒度 tick 为 10428µs。关闭 chrony 后先清除细粒度频率，再复原 tick 为 10000µs；Lima 继续独立同步。第一次校正后的检查捕获残余约 218ms 的收敛步进，也原样保留；随后新目录独立 45 秒 / 434 次自然写入没有倒退或超过 50ms 的跳变。

用户明确选择修正 VM 并重新运行 Linux **全部**配对组。此处全部样本因此排除出最终统计，续跑中的完整 CTest 被精确中断；后续 Make 回退、服务验收及独立冷入口未执行，不将本目录记成完整验证。原始目录 `/home/edwin.guest/code/.bench/p6-fixed-20261007` 保存所有编译事件、内存/时间线与 Ninja 输入前后快照。

`clock-invalid-audit.json` 只验证保存证据的完整性，`formal_run_complete=false`、`stability_gate_passed=false`；逐文件原始 SHA 保存在 `lima-clock-invalid-samples.json`。`shipment-manifest.json` 校验本目录交付文件，最终总目录 manifest 另覆盖本 README。新的完整组位于 `/home/edwin.guest/code/.bench/p6-clock-fixed-20261007`，监测时钟后重新取全部样本，不从本组挑选成功行。
