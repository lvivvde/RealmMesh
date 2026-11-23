#!/usr/bin/env python3
"""冻结 Scene 阶段总验收；缺分组、指标、数据库或回收证据一律失败。"""
import datetime
import hashlib
import json
import math
from pathlib import Path
import platform
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

GROUPS = (
    "SceneJourneyTest", "RealmScenesTest", "SceneShardTest", "MutualTlsTest",
    "SceneSessionTest", "SceneTransferTest", "SceneTransferJourneyTest",
    "SceneShardStoreTest", "SceneDynamicJourneyTest", "SceneDynamicJoinJourneyTest",
    "SceneStoreTest", "SceneProtocolTest", "SceneCommitRecoveryTest",
    "ObservabilityArtifactsTest", "SceneFaultJourneyTest", "SceneOutboundJourneyTest", "SceneAcceptanceReportTest",
)
MATRIX = {
    "Scene/Realm SIGKILL": ["SceneTransferJourneyTest.SceneSigkillKeepsOtherShardMovingAndNewIncarnationRestoresLocation", "SceneTransferJourneyTest.RealmSigkillAfterTargetCommitRecoversWithoutRollingBackSource"],
    "同名实例重启": ["SceneTransferTest.SameInstanceRestartNeedsNewEpochAndPreservesLastLocation"],
    "链路断开/恢复": ["SceneJourneyTest.MissingScenePreservesHeartbeatTrainingAndDurableLocation"],
    "准备响应丢失": ["SceneTransferTest.PreparationReplyLostKeepsSourceAndCandidateInvisible"],
    "切换响应丢失": ["SceneTransferTest.LostCasResponseReadsTargetReceiptBeforeActivation"],
    "激活/移除响应丢失": ["SceneTransferTest.LostActivationNeverRollsBackAndSameSequenceRestoresTarget", "SceneTransferTest.LostSourceRemovalCannotBlockTargetMovementOrRollback"],
    "提交不确定": ["SceneTransferTest.UnknownUncommittedCasRetriesExactWriteWithoutSkipping", "SceneCommitRecoveryTest.FailedDurabilityAcknowledgementRequiresReadback"],
    "MongoDB 暂不可用+承载故障": ["SceneFaultJourneyTest.MongoWriteOutageThenCarrierLossPreservesConfirmedLocation"],
    "排空失败+同名恢复+重试": ["SceneFaultJourneyTest.TargetCrashDrainFailureAndSameNameRestartRetryRemainFenced"],
    "慢消费者+重新入场": ["SceneFaultJourneyTest.SlowConsumerOverflowRejoinsFromDurableStateWithoutBlockingOtherShard", "SceneSessionTest.NotificationOverflowClosesOnlySlowSessionWithoutDereferencingReleasedStream"],
    "服务端出站满额": ["SceneOutboundJourneyTest.ServerOutputSaturationClosesOnlySlowSessionAndRestoresBinding"],
    "队列满/截止": ["RealmScenesTest.InstanceCapacityIncludesQueuedRunningAndUndrainedResults", "RealmScenesTest.QueueLimitAndWaitingTimeAreIncludedInCommandDeadline"],
    "运行中加入/容量排空": ["SceneDynamicJoinJourneyTest.RuntimeJoinAllocatesNewShardWithoutRebalancing", "SceneDynamicJourneyTest.FullTargetKeepsDrainingAndBothShardsMovableUntilExplicitFailure"],
    "停机故障回收": ["RealmScenesTest.ShutdownFinishesWhenDatabaseRevocationKeepsFailing", "RealmScenesTest.ShutdownFinishesWhenRemovalKeepsReturningBusy"],
}
CORE_METRICS = {
    "scene_commands_total", "scene_command_duration_seconds_bucket",
    "scene_command_duration_seconds_count", "scene_command_duration_seconds_sum",
    "scene_active_bindings", "scene_queue_rejections_total",
    "scene_stale_rejections_total", "scene_pending_readbacks",
    "scene_snapshot_resync_total", "scene_write_uncertain_total",
}
OPERATIONS = {"enter", "move", "transfer"}
METRICS = CORE_METRICS | {
    "scene_owned_shards", "scene_ready_instances", "scene_drain_remaining_members",
    "scene_drain_pending_transfers", "scene_drain_failed", "scene_drain_attempts_total",
    "scene_drain_transfers_total", "scene_drain_retries_total", "scene_drain_failures_total",
    "scene_shard_assignments_total", "scene_shard_reconcile_errors_total",
}
PROPERTIES = ("database_verification", "cleanup_verification", "backend_cleanup_verification",
              "fault_matrix_verification", "metrics_verification")


def git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args], text=True).strip()


def source_state(root):
    diff = subprocess.check_output(["git", "-C", str(root), "diff", "HEAD", "--"])
    tree_hash = hashlib.sha256(diff)
    for name in sorted(git(root, "ls-files", "--others", "--exclude-standard").splitlines()):
        path = root / name
        if path.is_file():
            tree_hash.update(name.encode() + b"\0" + path.read_bytes())
    return {"revision": git(root, "rev-parse", "HEAD"),
            "status": git(root, "status", "--porcelain"),
            "difference_sha256": tree_hash.hexdigest()}


def freeze(root, output, preset):
    files = sorted((root / "configs").rglob("*.lua")) + [root / "configs/main.config"]
    manifest = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
    metadata = {
        **source_state(root),
        "started_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "preset": preset, "platform": platform.platform(), "architecture": platform.machine(),
        "scene_transport": "TLS/TCP, mutual TLS, realmmesh-scene/1",
        "scope": "S1-S5 local multi-process; no Scene QUIC/cross-host/capacity claim",
        "configuration_sha256": manifest,
    }
    (output / "metadata.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n")


def read_cases(output):
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
            properties.update({p: case.attrib[p] for p in PROPERTIES if p in case.attrib})
            if properties:
                raw_properties[name] = {"raw_gtest": str(raw_xml.relative_to(output)), **properties}
    for case in cases:
        case["properties"].update(raw_properties.get(case["name"], {}))
    return cases


def evidence_errors(output, cases):
    errors = []
    names = {c["name"] for c in cases}
    expected_file = output / "expected-cases.json"
    try:
        expected = {t["name"] for t in json.loads(expected_file.read_text())["tests"]}
    except (OSError, ValueError, KeyError, TypeError):
        expected = set()
    if not expected or names != expected or len(names) != len(cases):
        errors.append("CTest 发现清单与实际结果不一致、缺失或重复")
    for group in GROUPS:
        if not any(name.startswith(group + ".") for name in names):
            errors.append("缺少测试分组：" + group)
    for axis, required in MATRIX.items():
        if not set(required) <= names:
            errors.append("缺少故障边界：" + axis)
    for case in cases:
        name, props = case["name"], case["properties"]
        if case["outcome"] != "passed":
            errors.append("失败或跳过：" + name)
        if name.split(".")[0].endswith("JourneyTest"):
            if not props.get("cleanup_verification"):
                errors.append("缺少进程/监听回收证明：" + name)
            if not name.endswith("ControlRequiresExactServerAndAuthorizedClientIdentity") and not props.get("database_verification"):
                errors.append("缺少数据库核对：" + name)
        if name.startswith(("SceneFaultJourneyTest.", "SceneOutboundJourneyTest.")):
            for key in ("backend_cleanup_verification", "fault_matrix_verification", "metrics_verification"):
                if not props.get(key):
                    errors.append("缺少 " + key + "：" + name)
    if not {"MongoCtestFixturePrepare", "MongoCtestFixtureCleanup"} <= names:
        errors.append("缺少共享 MongoDB 准备/回收结果")
    seen = set()
    operations = {name: set() for name in ("scene_commands_total", "scene_command_duration_seconds_count")}
    samples = []
    for path in sorted((output / "services").rglob("metrics-*.prom")):
        samples.append(str(path.relative_to(output)))
        for line in path.read_text().splitlines():
            if not line or line.startswith("#"):
                continue
            match = re.match(r"([a-zA-Z_:][a-zA-Z0-9_:]*)(?:\{[^}]*\})?\s+([^\s]+)$", line)
            try:
                valid = match is not None and math.isfinite(float(match[2]))
            except ValueError:
                valid = False
            if not valid:
                errors.append("无效指标样本：" + str(path.relative_to(output)))
                continue
            seen.add(match[1])
            if match[1] in operations:
                operation = re.search(r'\boperation="([^"]+)"', line)
                if operation and float(match[2]) > 0:
                    operations[match[1]].add(operation[1])
    if METRICS - seen:
        errors.append("缺少实际 HTTP 指标：" + ", ".join(sorted(METRICS - seen)))
    for name, observed in operations.items():
        if OPERATIONS - observed:
            errors.append("缺少实际操作指标：" + name + ": " + ", ".join(sorted(OPERATIONS - observed)))
    return errors, {"operations": {k: sorted(v) for k, v in operations.items()}, "samples": samples, "observed_names": sorted(seen), "required_names": sorted(METRICS)}


def finish(root, output, value):
    metadata = json.loads((output / "metadata.json").read_text())
    parse_error = None
    try:
        cases = read_cases(output)
    except (OSError, ET.ParseError) as error:
        cases = []
        parse_error = "原始测试证据不可读取：" + str(error)
    errors, metrics = evidence_errors(output, cases)
    if parse_error:
        errors.append(parse_error)
    final = source_state(root)
    if any(final[k] != metadata[k] for k in ("revision", "status", "difference_sha256")):
        errors.append("验收期间源码或工作树变化")
    if value != "0":
        errors.append("构建/测试退出非零：" + value)
    passed = not errors
    metadata.update({"result": "passed" if passed else "failed", "exit_status": value,
                     "finished_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                     "final_source": final, "cases": cases, "evidence_errors": errors,
                     "fault_matrix": MATRIX, "metrics_evidence": metrics})
    (output / "metadata.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n")
    lines = ["# Scene S1–S5 本机多进程阶段总验收", "", f"- 结果：{metadata['result']}",
             f"- 版本：`{metadata['revision']}`；工作树差异 SHA256：`{metadata['difference_sha256']}`",
             f"- 环境：{metadata['platform']}；预设：`{metadata['preset']}`",
             "- 传输：TLS/TCP、双向服务身份校验；不代表 Scene QUIC、跨机或玩法容量验收。",
             "- 拓扑：真实入口组、Realm、Scene A/B；响应丢失边界另用生产 SceneShard 的 mTLS 替身，角色写入与存储契约使用真实隔离 MongoDB。MongoDB 暂停故障只注入独占副本集，不影响共享夹具。",
             "- 排除项：Chat、M5、AOI、战斗、跨机器及性能容量实验。未注入真实网络设备丢包；以 mTLS 操作响应丢弃/进程故障验证相应恢复边界。",
             "- 原始证据：`results.xml`、`expected-cases.json`、`ctest.log`、`build.log`、`configure.log`、`services/` 与 `gtest/`。数据库、组合故障、回收核对属性与 HTTP 指标文件列于 `metadata.json`。",
             "- 缺测试/指标/核对属性、跳过、失败或运行中改源码均判失败；每次独立目录保留。", ""]
    if errors:
        lines += ["## 证据缺口", ""] + ["- " + error for error in errors] + [""]
    lines += ["## 故障矩阵", "", "| 边界 | 必须通过的用例 |", "|---|---|"]
    lines += [f"| {axis} | {'、'.join(required)} |" for axis, required in MATRIX.items()]
    lines += ["", "## 逐例结果", "", "| 用例 | 秒 | 结果 |", "|---|---:|---|"]
    lines += [f"| {c['name']} | {c['seconds']} | {c['outcome']} |" for c in cases]
    (output / "report.md").write_text("\n".join(lines) + "\n")
    return 0 if passed else 1


if __name__ == "__main__":
    mode, root_arg, output_arg, value = sys.argv[1:]
    root, output = Path(root_arg), Path(output_arg)
    if mode == "freeze":
        freeze(root, output, value)
    else:
        raise SystemExit(finish(root, output, value))
