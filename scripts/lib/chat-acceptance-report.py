#!/usr/bin/env python3
"""冻结 Chat C1–C5 阶段验收；缺证据、跳过或源码漂移均失败。"""

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
    "ChatProtocolTest",
    "ChatChannelsTest",
    "ChatServiceTest",
    "RealmChatTest",
    "ChatSessionTest",
    "ChatJourneyTest",
    "DualSceneDualChatJourneyTest",
    "ChatAcceptanceReportTest",
)

MATRIX = {
    "Chat SIGKILL、同名重启与新 Epoch": [
        "ChatJourneyTest.ChatCrashAndSameNameRestartDoNotBlockOtherRealmOperationsOrReplayOldUnknown",
        "ChatServiceTest.RealDiscoveryRegistrationCrashLeaseExpiryAndSameNameRestartGetFreshEpoch",
    ],
    "Realm SIGKILL 与同名重启": [
        "ChatJourneyTest.RealmSigkillAndSameNameRestartInstallFreshLinkWithoutLosingSceneFacts"
    ],
    "链路断开与恢复": [
        "RealmChatTest.DispatchedDeadlineRemainsUnknownAndNewLinkUsesFreshQualification",
        "RealmChatTest.AcceptedReplySurvivesLinkClosingBeforeFrameCollectsIt",
    ],
    "JOIN/LEAVE/已受理 SEND 回执丢失": [
        "RealmChatTest.LostJoinReplyRetainsOriginalWatermarkAndBuffersInterveningMessage",
        "RealmChatTest.LostLeaveReplyFencesOldQualificationBeforeReplacementJoins",
        "RealmChatTest.LostSendReplyRetriesOriginalRequestWithoutRepeatingBroadcast",
    ],
    "Unknown 同号重试": [
        "RealmChatTest.DispatchedDeadlineRemainsUnknownAndNewLinkUsesFreshQualification",
        "ChatSessionTest.UnknownRetryDoesNotBecomeRejectedWhenQualificationHasEnded",
    ],
    "旧资格、旧 Epoch 与迟到清理": [
        "ChatChannelsTest.NewLinkFencesOldRequestsAndLateCloseWithoutResettingChannelWatermark",
        "ChatChannelsTest.FreshEpochDoesNotReplayOldReceiptAndIdleChannelRetainsWatermark",
        "DualSceneDualChatJourneyTest.SameShardRebuildKeepsLogicalChannelAndInstallsNewBindingQualification",
    ],
    "单 Chat 故障隔离": [
        "DualSceneDualChatJourneyTest.StaticOwnersIsolateTransfersAndOneChatFailureWhileRealmChannelContinues"
    ],
    "内部队列与截止": [
        "ChatChannelsTest.PendingRepliesStillOccupyBudgetAndQueueDeadlineDoesNotExecute",
        "ChatChannelsTest.GlobalAndPerPeerQueueAndConnectionBoundsReleaseAllOccupancy",
    ],
    "服务端真实 TLS 出站满额": [
        "ChatServiceTest.ProductionOneMiBSlowTlsBacklogClosesOnlyLinkAndKeepsChannelEpoch"
    ],
    "参考客户端通知缓存溢出": [
        "ChatSessionTest.GapIsExplicitAndOverflowClosesOnlySlowSessionWhileHealthySessionContinues"
    ],
    "mTLS 身份、ALPN 与越权拒绝": [
        "ChatServiceTest.WrongCertificateCaServerIdentityAndAlpnNeverReachHello",
        "ChatServiceTest.AuthenticatedWrongRealmInstanceAndClientWireAreRejected",
    ],
    "双 Scene/双 Chat 转场与排空": [
        "DualSceneDualChatJourneyTest.SceneDrainMovesMembershipToTargetChatWithoutResettingRealmChannel"
    ],
}

METRICS = {
    "chat_ingress_rejected_total",
    "chat_messages_accepted_total",
    "chat_receipt_replays_total",
    "chat_queue_rejected_total",
    "chat_deadline_expired_total",
    "chat_rate_rejected_total",
    "chat_stale_rejected_total",
    "chat_resource_rejected_total",
    "chat_members",
    "chat_pending_requests",
    "chat_pending_pushes",
    "chat_pending_push_bytes",
    "realm_chat_gap_total",
    "realm_chat_reconnect_total",
    "realm_chat_queue_rejections_total",
    "realm_chat_rate_rejections_total",
    "realm_chat_active_memberships",
    "realm_chat_pending_requests",
}
OUTCOME_METRICS = {"chat_send_total", "realm_chat_send_total"}
OUTCOMES = {"accepted", "rejected", "unknown"}
PROPERTIES = (
    "database_verification",
    "cleanup_verification",
    "fault_matrix_verification",
    "metrics_verification",
)
JOURNEY_GROUPS = {"ChatJourneyTest", "DualSceneDualChatJourneyTest"}


def git(root, *args):
    # 本机开发仓库把真实 git 目录放在 .realmmesh-git（根 .git 是空仓库），
    # CI 与常规克隆只有 .git。两种布局都要能用，不能写死其中一种。
    command = ["git"]
    local_git_dir = root / ".realmmesh-git"
    if local_git_dir.is_dir():
        command += ["--git-dir", str(local_git_dir), "--work-tree", str(root)]
    else:
        command += ["-C", str(root)]
    command += list(args)
    return subprocess.check_output(command, text=True).strip()


def workspace_sha256(root):
    digest = hashlib.sha256()
    names = set(git(root, "ls-files").splitlines())
    names.update(git(root, "ls-files", "--others", "--exclude-standard").splitlines())
    for name in sorted(names):
        path = root / name
        if not path.is_file():
            continue
        digest.update(name.encode() + b"\0")
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    return digest.hexdigest()


def source_state(root):
    return {
        "revision": git(root, "rev-parse", "HEAD"),
        "status": git(root, "status", "--porcelain"),
        "workspace_sha256": workspace_sha256(root),
    }


def freeze(root, output, preset):
    output.mkdir(parents=True, exist_ok=True)
    configs = sorted((root / "configs").rglob("*.lua")) + [
        root / "configs" / "main.config"
    ]
    metadata = {
        **source_state(root),
        "started_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "preset": preset,
        "platform": platform.platform(),
        "architecture": platform.machine(),
        "python": platform.python_version(),
        "transports": {
            "client_realm": "TLS/TCP, realmmesh-edge/1",
            "realm_chat": "TLS/TCP, mutual TLS, realmmesh-chat/1",
            "realm_scene": "TLS/TCP, mutual TLS, realmmesh-scene/1",
        },
        "scope": "C1-C5 local multi-process; no Chat history/offline/cross-host/capacity claim",
        "configuration_sha256": {
            str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in configs
        },
    }
    (output / "metadata.json").write_text(
        json.dumps(metadata, ensure_ascii=False, indent=2) + "\n"
    )


def read_cases(output):
    cases = []
    junit = output / "results.xml"
    if junit.exists():
        for case in ET.parse(junit).iter("testcase"):
            failed = case.find("failure") is not None or case.find("error") is not None
            skipped = case.find("skipped") is not None
            cases.append(
                {
                    "name": case.get("name"),
                    "seconds": case.get("time"),
                    "outcome": "failed" if failed else "skipped" if skipped else "passed",
                    "properties": {
                        item.get("name"): item.get("value")
                        for item in case.findall("./properties/property")
                    },
                }
            )
    raw_properties = {}
    for raw_xml in sorted((output / "gtest").glob("*.xml")):
        for case in ET.parse(raw_xml).iter("testcase"):
            name = f"{case.get('classname')}.{case.get('name')}"
            properties = {
                item.get("name"): item.get("value")
                for item in case.findall("./properties/property")
            }
            properties.update(
                {key: case.attrib[key] for key in PROPERTIES if key in case.attrib}
            )
            if properties:
                raw_properties[name] = {
                    "raw_gtest": str(raw_xml.relative_to(output)),
                    **properties,
                }
    for case in cases:
        case["properties"].update(raw_properties.get(case["name"], {}))
    return cases


def metric_evidence(output, errors):
    observed = set()
    outcomes = {name: set() for name in OUTCOME_METRICS}
    samples = []
    metric_pattern = re.compile(
        r"([a-zA-Z_:][a-zA-Z0-9_:]*)(?:\{[^}]*\})?\s+([^\s]+)$"
    )
    for path in sorted((output / "services").rglob("*.prom")):
        samples.append(str(path.relative_to(output)))
        for line in path.read_text().splitlines():
            if not line or line.startswith("#"):
                continue
            match = metric_pattern.match(line)
            try:
                valid = match is not None and math.isfinite(float(match[2]))
            except ValueError:
                valid = False
            if not valid:
                errors.append("无效指标样本：" + str(path.relative_to(output)))
                continue
            name = match[1]
            observed.add(name)
            if name in outcomes:
                outcome = re.search(r'\boutcome="([^"]+)"', line)
                if outcome:
                    outcomes[name].add(outcome[1])
    missing = METRICS - observed
    if missing:
        errors.append("缺少实际 HTTP 指标（含零值）：" + ", ".join(sorted(missing)))
    for name, values in outcomes.items():
        if OUTCOMES - values:
            errors.append(
                "缺少实际结果指标："
                + name
                + ": "
                + ", ".join(sorted(OUTCOMES - values))
            )
    return {
        "samples": samples,
        "observed_names": sorted(observed),
        "outcomes": {name: sorted(values) for name, values in outcomes.items()},
        "required_names": sorted(METRICS | OUTCOME_METRICS),
    }


def evidence_errors(output, cases):
    errors = []
    names = {case["name"] for case in cases}
    try:
        expected = {
            item["name"]
            for item in json.loads((output / "expected-cases.json").read_text())[
                "tests"
            ]
        }
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
        name = case["name"]
        group = name.split(".")[0]
        properties = case["properties"]
        if case["outcome"] != "passed":
            errors.append("失败或跳过：" + name)
        if group in JOURNEY_GROUPS:
            if not properties.get("cleanup_verification"):
                errors.append("缺少进程/监听回收证明：" + name)
            if not properties.get("database_verification"):
                errors.append("缺少数据库核对：" + name)
    for axis in ("服务端真实 TLS 出站满额", "参考客户端通知缓存溢出"):
        for name in MATRIX[axis]:
            case = next((item for item in cases if item["name"] == name), None)
            if case and not case["properties"].get("fault_matrix_verification"):
                errors.append("缺少独立饱和边界证明：" + name)
    if not {"MongoCtestFixturePrepare", "MongoCtestFixtureCleanup"} <= names:
        errors.append("缺少共享 MongoDB 准备/回收结果")
    raw_logs = sorted((output / "services").rglob("*.log"))
    if not raw_logs:
        errors.append("缺少原始服务日志")
    metrics = metric_evidence(output, errors)
    metrics["raw_logs"] = [str(path.relative_to(output)) for path in raw_logs]
    return errors, metrics


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
    if any(
        final[key] != metadata[key]
        for key in ("revision", "status", "workspace_sha256")
    ):
        errors.append("验收期间源码或工作树变化")
    if value != "0":
        errors.append("构建/测试退出非零：" + value)
    passed = not errors
    metadata.update(
        {
            "result": "passed" if passed else "failed",
            "exit_status": value,
            "finished_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "final_source": final,
            "cases": cases,
            "evidence_errors": errors,
            "fault_matrix": MATRIX,
            "metrics_evidence": metrics,
        }
    )
    (output / "metadata.json").write_text(
        json.dumps(metadata, ensure_ascii=False, indent=2) + "\n"
    )
    lines = [
        "# Chat C1–C5 本机多进程阶段总验收",
        "",
        f"- 结果：{metadata['result']}",
        f"- 版本：`{metadata['revision']}`；冻结工作区 SHA256：`{metadata['workspace_sha256']}`",
        f"- 环境：{metadata['platform']}；预设：`{metadata['preset']}`",
        "- 传输：客户端到 Realm 为 TLS/TCP；Realm–Chat/Scene 为双向 TLS/TCP；不代表 Chat QUIC、跨机、历史、离线或容量验收。",
        "- 数据：Chat Accepted 仅为内存在线受理；数据库只核对角色、Scene Binding 与 Confirmed Location 等既有事实。",
        "- 原始证据：`results.xml`、`expected-cases.json`、`ctest.log`、`build.log`、`configure.log`、`gtest/`、`services/`。",
        "- 归档：运行入口在本目录旁生成同名 `.tar.gz` 与 `.tar.gz.sha256`；失败目录同样保留；CI 以 `chat-c1-c5-<OS>-<run>-<attempt>` artifact 持久上传。",
        "- 缺测试/指标/核对属性/日志、跳过、失败或运行中改源码均判失败。",
        "",
    ]
    if errors:
        lines += ["## 证据缺口", ""] + ["- " + error for error in errors] + [""]
    lines += ["## 故障矩阵", "", "| 边界 | 必须通过的用例 |", "|---|---|"]
    lines += [
        f"| {axis} | {'、'.join(required)} |" for axis, required in MATRIX.items()
    ]
    lines += ["", "## 逐例结果", "", "| 用例 | 秒 | 结果 |", "|---|---:|---|"]
    lines += [
        f"| {case['name']} | {case['seconds']} | {case['outcome']} |"
        for case in cases
    ]
    (output / "report.md").write_text("\n".join(lines) + "\n")
    return 0 if passed else 1


if __name__ == "__main__":
    mode, root_arg, output_arg, value = sys.argv[1:]
    root, output = Path(root_arg), Path(output_arg)
    if mode == "freeze":
        freeze(root, output, value)
    else:
        raise SystemExit(finish(root, output, value))
