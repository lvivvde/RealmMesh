#!/usr/bin/env python3
"""冻结 Scene 验收版本与配置,将 CTest 原始结果汇成可复核报告。"""
import datetime
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys
import xml.etree.ElementTree as ET

mode, root_arg, output_arg, value = sys.argv[1:]
root, output = Path(root_arg), Path(output_arg)

def git(*args):
    return subprocess.check_output(["git", "-C", str(root), *args], text=True).strip()

if mode == "freeze":
    files = sorted((root / "configs").rglob("*.lua")) + [root / "configs/main.config"]
    manifest = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
    diff = subprocess.check_output(["git", "-C", str(root), "diff", "HEAD", "--"])
    untracked = git("ls-files", "--others", "--exclude-standard").splitlines()
    tree_hash = hashlib.sha256(diff)
    for name in sorted(untracked):
        path = root / name
        if path.is_file():
            tree_hash.update(name.encode() + b"\0" + path.read_bytes())
    metadata = {
        "started_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "revision": git("rev-parse", "HEAD"), "status": git("status", "--porcelain"),
        "difference_sha256": tree_hash.hexdigest(), "preset": value,
        "platform": platform.platform(), "architecture": platform.machine(),
        "scene_transport": "TLS/TCP, mutual TLS, realmmesh-scene/1",
        "scope": "S1-S3 local multi-process; no Scene QUIC/cross-host/capacity claim",
        "configuration_sha256": manifest,
    }
    (output / "metadata.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n")
else:
    metadata = json.loads((output / "metadata.json").read_text())
    cases = []
    xml = output / "results.xml"
    if xml.exists():
        for case in ET.parse(xml).iter("testcase"):
            failed = case.find("failure") is not None or case.find("error") is not None
            skipped = case.find("skipped") is not None
            cases.append({"name": case.get("name"), "seconds": case.get("time"),
                          "outcome": "failed" if failed else "skipped" if skipped else "passed",
                          "properties": {p.get("name"): p.get("value") for p in case.findall("./properties/property")}})
    raw_properties = {}
    for raw_xml in sorted((output / "gtest").glob("*.xml")):
        for case in ET.parse(raw_xml).iter("testcase"):
            name = f"{case.get('classname')}.{case.get('name')}"
            properties = {p.get("name"): p.get("value") for p in case.findall("./properties/property")}
            if "database_verification" in case.attrib:
                properties["database_verification"] = case.attrib["database_verification"]
            if properties:
                raw_properties[name] = {"raw_gtest": str(raw_xml.relative_to(output)), **properties}
    for case in cases:
        case["properties"].update(raw_properties.get(case["name"], {}))
    expected = ("SceneJourneyTest.", "RealmScenesTest.", "SceneShardTest.", "MutualTlsTest.", "SceneSessionTest.", "SceneTransferTest.", "SceneTransferJourneyTest.")
    complete = all(any(c["name"].startswith(prefix) for c in cases) for prefix in expected)
    passed = value == "0" and complete and all(c["outcome"] == "passed" for c in cases)
    metadata.update({"result": "passed" if passed else "failed", "exit_status": value, "cases": cases})
    (output / "metadata.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n")
    lines = ["# Scene S1–S3 本机多进程验收", "", f"- 结果：{metadata['result']}",
             f"- 版本：`{metadata['revision']}`；工作树差异 SHA256：`{metadata['difference_sha256']}`",
             f"- 环境：{metadata['platform']}；预设：`{metadata['preset']}`",
             "- 传输：TLS/TCP，双向服务身份校验；不代表 Scene QUIC、跨机或玩法容量验收。",
             "- 拓扑：常规/SIGKILL Scene 用例为入口组、Realm、Scene A/B 四个独立 realm_mesh 进程；Realm 激活边界故障使用生产 SceneShard 的 mTLS 替身，隔离 etcd 与 MongoDB 副本集。",
             "- 数据库核对项：双人各 10 次移动后，双方快照、事件水位及数据库 seq=11、落点一致；任何断言失败即验收失败。",
             "- 原始证据：`results.xml`、`ctest.log`、`build.log`、`configure.log`、`services/`、`gtest/`。",
             "- 配置哈希与逐例结果：`metadata.json`；每次运行单独保存，不覆盖旧报告。", "",
             "| 用例 | 秒 | 结果 |", "|---|---:|---|"]
    lines += [f"| {c['name']} | {c['seconds']} | {c['outcome']} |" for c in cases]
    (output / "report.md").write_text("\n".join(lines) + "\n")
    if value == "0" and not passed:
        raise SystemExit("Incomplete or skipped Scene acceptance groups")
