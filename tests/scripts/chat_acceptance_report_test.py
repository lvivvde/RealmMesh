#!/usr/bin/env python3
"""只验证 Chat C5 报告的 fail-closed 契约，不冒充业务证据。"""

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET


sys.dont_write_bytecode = True
ROOT = Path(sys.argv.pop(1))
spec = importlib.util.spec_from_file_location(
    "chat_report", ROOT / "scripts/lib/chat-acceptance-report.py"
)
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)


class ReportContract(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.output = Path(self.temp.name)
        report.freeze(ROOT, self.output, "contract")
        self.names = {name for required in report.MATRIX.values() for name in required}
        self.names.update(group + ".Contract" for group in report.GROUPS)
        self.names.update(("MongoCtestFixturePrepare", "MongoCtestFixtureCleanup"))
        self.props = {
            name: {key: "synthetic report-contract fixture" for key in report.PROPERTIES}
            for name in self.names
        }
        self.skipped = set()
        (self.output / "gtest").mkdir()
        (self.output / "services" / "contract").mkdir(parents=True)
        (self.output / "services" / "contract" / "service.log").write_text(
            "synthetic raw log for report contract\n"
        )
        self.metric_text = "\n".join(
            name + " 0" for name in sorted(report.METRICS)
        ) + "\n"
        for metric in report.OUTCOME_METRICS:
            for outcome in report.OUTCOMES:
                self.metric_text += f'{metric}{{outcome="{outcome}"}} 0\n'

    def run_report(self, expected=None):
        expected = self.names if expected is None else expected
        (self.output / "expected-cases.json").write_text(
            json.dumps({"tests": [{"name": name} for name in expected]})
        )
        junit = ET.Element("testsuite")
        raw = ET.Element("testsuite")
        for name in sorted(self.names):
            case = ET.SubElement(junit, "testcase", name=name, time="0")
            if name in self.skipped:
                ET.SubElement(case, "skipped")
            suite, _, test = name.partition(".")
            case = ET.SubElement(raw, "testcase", classname=suite, name=test)
            properties = ET.SubElement(case, "properties")
            for key, value in self.props[name].items():
                ET.SubElement(properties, "property", name=key, value=value)
        ET.ElementTree(junit).write(self.output / "results.xml")
        ET.ElementTree(raw).write(self.output / "gtest" / "contract.xml")
        (self.output / "services" / "metrics-contract.prom").write_text(
            self.metric_text
        )
        result = report.finish(ROOT, self.output, "0")
        self.errors = json.loads(
            (self.output / "metadata.json").read_text()
        )["evidence_errors"]
        return result

    def test_complete_evidence_passes(self):
        self.assertEqual(self.run_report(), 0)
        self.assertEqual(self.errors, [])

    def test_missing_matrix_case_fails(self):
        expected = set(self.names)
        missing = report.MATRIX["服务端真实 TLS 出站满额"][0]
        self.names.remove(missing)
        self.assertEqual(self.run_report(expected), 1)
        self.assertTrue(any("发现清单" in error for error in self.errors))
        self.assertTrue(any("服务端真实 TLS 出站满额" in error for error in self.errors))

    def test_missing_zero_value_metric_fails(self):
        self.metric_text = "\n".join(
            line
            for line in self.metric_text.splitlines()
            if not line.startswith("chat_queue_rejected_total ")
        )
        self.assertEqual(self.run_report(), 1)
        self.assertTrue(any("chat_queue_rejected_total" in error for error in self.errors))

    def test_missing_outcome_series_fails(self):
        self.metric_text = "\n".join(
            line
            for line in self.metric_text.splitlines()
            if 'realm_chat_send_total{outcome="unknown"}' not in line
        )
        self.assertEqual(self.run_report(), 1)
        self.assertTrue(any("unknown" in error for error in self.errors))

    def test_missing_raw_service_log_fails(self):
        (self.output / "services" / "contract" / "service.log").unlink()
        self.assertEqual(self.run_report(), 1)
        self.assertTrue(any("原始服务日志" in error for error in self.errors))

    def test_missing_cleanup_property_fails(self):
        name = report.MATRIX["Realm SIGKILL 与同名重启"][0]
        del self.props[name]["cleanup_verification"]
        self.assertEqual(self.run_report(), 1)
        self.assertTrue(any("回收证明" in error for error in self.errors))

    def test_skipped_case_fails(self):
        self.skipped.add(report.MATRIX["参考客户端通知缓存溢出"][0])
        self.assertEqual(self.run_report(), 1)
        self.assertTrue(any("失败或跳过" in error for error in self.errors))

    def test_source_change_fails(self):
        metadata = self.output / "metadata.json"
        value = json.loads(metadata.read_text())
        value["revision"] = "stale"
        metadata.write_text(json.dumps(value))
        self.assertEqual(self.run_report(), 1)
        self.assertTrue(any("源码或工作树变化" in error for error in self.errors))

    def test_configure_failure_is_archived(self):
        fallback = self.output / "configure-failure"
        environment = os.environ.copy()
        environment["REALMMESH_CHAT_ACCEPTANCE_FALLBACK_DIRECTORY"] = str(
            fallback
        )
        result = subprocess.run(
            [
                str(ROOT / "scripts/run-chat-acceptance.sh"),
                "--preset",
                "chat-contract-missing-preset",
                "--jobs",
                "1",
            ],
            cwd=ROOT,
            env=environment,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertNotEqual(result.returncode, 0)
        archives = list(fallback.glob("*.tar.gz"))
        self.assertEqual(len(archives), 1)
        self.assertTrue(Path(str(archives[0]) + ".sha256").is_file())
        reports = list(fallback.glob("*/report.md"))
        self.assertEqual(len(reports), 1)
        self.assertIn("configure-failed", reports[0].read_text())


if __name__ == "__main__":
    unittest.main()
