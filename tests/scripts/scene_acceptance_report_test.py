#!/usr/bin/env python3
"""只测试报告的拒绝边界；合成 XML/指标不作为业务或外部后端证据。"""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

sys.dont_write_bytecode = True
ROOT = Path(sys.argv.pop(1))
spec = importlib.util.spec_from_file_location("scene_report", ROOT / "scripts/lib/scene-acceptance-report.py")
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
        self.props = {name: {key: "synthetic report-contract fixture" for key in report.PROPERTIES}
                      for name in self.names}
        self.skipped = set()
        (self.output / "gtest").mkdir()
        (self.output / "services").mkdir()
        self.metric_text = "\n".join(name + " 0" for name in sorted(report.METRICS)) + "\n"

    def run_report(self, expected=None):
        expected = self.names if expected is None else expected
        (self.output / "expected-cases.json").write_text(json.dumps({"tests": [{"name": n} for n in expected]}))
        junit = ET.Element("testsuite")
        raw = ET.Element("testsuite")
        for name in sorted(self.names):
            case = ET.SubElement(junit, "testcase", name=name, time="0")
            if name in self.skipped:
                ET.SubElement(case, "skipped")
            suite, _, test = name.partition(".")
            case = ET.SubElement(raw, "testcase", classname=suite, name=test)
            props = ET.SubElement(case, "properties")
            for key, value in self.props[name].items():
                ET.SubElement(props, "property", name=key, value=value)
        ET.ElementTree(junit).write(self.output / "results.xml")
        ET.ElementTree(raw).write(self.output / "gtest/contract.xml")
        (self.output / "services/metrics-contract.prom").write_text(self.metric_text)
        result = report.finish(ROOT, self.output, "0")
        self.errors = json.loads((self.output / "metadata.json").read_text())["evidence_errors"]
        return result

    def test_complete_evidence_passes(self):
        self.assertEqual(self.run_report(), 0)
        self.assertEqual(self.errors, [])

    def test_missing_case_fails_even_when_its_group_exists(self):
        expected = set(self.names)
        self.names.remove(report.MATRIX["准备响应丢失"][0])
        self.assertEqual(self.run_report(expected), 1)
        self.assertTrue(any("发现清单" in error for error in self.errors))
        self.assertTrue(any("准备响应丢失" in error for error in self.errors))

    def test_missing_metrics_fails(self):
        self.metric_text = ""
        self.assertEqual(self.run_report(), 1)
        self.assertTrue(any("HTTP 指标" in error for error in self.errors))

    def test_invalid_metrics_fail_with_report(self):
        self.metric_text += "scene_drain_failed invalid\n"
        self.assertEqual(self.run_report(), 1)
        self.assertTrue(any("无效指标" in error for error in self.errors))

    def test_missing_cleanup_fails(self):
        del self.props["SceneFaultJourneyTest.Contract"]["cleanup_verification"]
        self.assertEqual(self.run_report(), 1)
        self.assertTrue(any("回收证明" in error for error in self.errors))

    def test_missing_database_fails(self):
        del self.props["SceneFaultJourneyTest.Contract"]["database_verification"]
        self.assertEqual(self.run_report(), 1)
        self.assertTrue(any("数据库核对" in error for error in self.errors))

    def test_skipped_case_fails(self):
        self.skipped.add("SceneFaultJourneyTest.Contract")
        self.assertEqual(self.run_report(), 1)
        self.assertTrue(any("失败或跳过" in error for error in self.errors))

    def test_source_change_fails(self):
        p = self.output / "metadata.json"
        data = json.loads(p.read_text())
        data["revision"] = "stale"
        p.write_text(json.dumps(data))
        self.assertEqual(self.run_report(), 1)
        self.assertTrue(any("源码或工作树变化" in error for error in self.errors))


if __name__ == "__main__":
    unittest.main()
