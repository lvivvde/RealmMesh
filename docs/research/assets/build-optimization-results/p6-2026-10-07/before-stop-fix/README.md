# 停止修复前的独立 P6 组

最终产品为 `51ad054bcb172f8e66995d9a450a1a03c6ea3085`；工具固定为 `ee44ff80c78a0cc6e362e15f52c67e722ac6c849`，R0 补丁与逐文件 SHA 见本目录资产。该组主场景与缓存配对均完整，Mac 正确性检查通过；Linux 最后 666 项完整检查有一次 StopDuringProcessChecks 失败。失败原始日志和根因证据另见 `../supervisor-fix/`。

用户确认在 #129 修复、重新冻结并完整复测后，本组不再用于新结果中位数。Linux 额外完整冷入口仅开始第一轮 R0，随后安全中断；未产生完整冷样本，不拼接此前 build 和后来测试。原始目录分别为 `/private/tmp/realmmesh-p6`、`/home/edwin.guest/code/.bench/p6-20261006`，每个 JSON 的 raw_manifest 保留全部文件 SHA、失败及未完成阶段。

原始 Mac 打包带入的 AppleDouble 污染与清理证据、工具夹具层级修正，以及 Linux 长暂停后的 0/56、0/4→0/0 链接过渡均独立保留。后者输入根因尚未确证，不宣称任意长暂停后均稳定。

最终导出器为 `collect.py`，初期导出器另存 `collect-initial.py`；导出器与 JSON SHA 见 `export-provenance.json`。计时驱动和检查驱动保持 seed 的原 SHA。
