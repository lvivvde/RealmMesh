"""CI 缓存验收的公开 JSON/CLI 契约。"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class CICacheSummaryTest(unittest.TestCase):
    def samples(self, off=100, on=50):
        rows = []
        for pair in range(1, 4):
            for mode, total in [('OFF', off), ('ON', on)]:
                rows.append({
                    'pair': pair, 'mode': mode, 'net_wait_s': total,
                    'wall_s': 40 if mode == 'ON' else off, 'exit': 0,
                    'bucket': 'same-compatible-bucket', 'compile_count': 2,
                    'compiled_sources': ['a.cpp', 'b.c'], 'linked_outputs': ['app'],
                    'sodium_compiled_sources': ['sodium.lo'],
                    'memory': {'min_available_mib': 2048, 'swap_growth_mib': 0,
                               'oom_kills': 0, 'max_pressure_level': 1},
                    'cache_delta': {'direct_cache_hit': 2, 'cache_miss': 0} if mode == 'ON' else {},
                    'actions': {'restore_outcome': 'success', 'restore_hit': 'true',
                                'save_outcome': 'success', 'saved_lookup_hit': 'true',
                                'saved_lookup_outcome': 'success'} if mode == 'ON' else {},
                })
        return rows

    def summarize(self, rows, missing_tests=False):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'ci-samples.json').write_text(json.dumps(rows))
            (root / 'ci-state.json').write_text(json.dumps({
                'platform': 'Darwin', 'bucket': 'same-compatible-bucket',
                'seed_verified': True, 'seed_remote_hit': True,
                'unavailable_restore_outcome': 'failure', 'seed_test_count': 2,
                'seed_memory': {'min_available_mib': 2048, 'swap_growth_mib': 0,
                                'oom_kills': None, 'max_pressure_level': 1},
            }))
            for mode in ('seed', 'off', 'on'):
                if missing_tests and mode == 'on':
                    continue
                (root / f'{mode}-tests.log').write_text('100% tests passed, 0 tests failed out of 2\n')
            script = Path(__file__).resolve().parents[2] / 'tools/build-bench/ci_cache.py'
            env = dict(os.environ)
            # 合成判定样本只写隔离目录，不混入真实 job 的验收摘要。
            env.pop('GITHUB_STEP_SUMMARY', None)
            result = subprocess.run([sys.executable, str(script),
                                     'summarize', '--out', str(root)], capture_output=True, text=True, env=env)
            summary = json.loads((root / 'ci-summary.json').read_text())
            return result, summary

    def test_transfer_cost_can_reverse_a_build_gain(self):
        result, summary = self.summarize(self.samples(on=110))
        self.assertEqual(result.returncode, 1)
        self.assertAlmostEqual(summary['reduction'], -0.1)
        self.assertFalse(summary['accepted'])

    def test_missing_restored_snapshot_is_not_a_hot_cache_result(self):
        rows = self.samples()
        rows[1]['actions']['restore_hit'] = ''
        result, summary = self.summarize(rows)
        self.assertEqual(result.returncode, 1)
        self.assertFalse(summary['accepted'])
        self.assertTrue(any('restore' in error for error in summary['errors']))

    def test_one_pair_cannot_meet_the_three_pair_gate(self):
        result, summary = self.summarize(self.samples()[:2])
        self.assertEqual(result.returncode, 1)
        self.assertFalse(summary['accepted'])

    def test_retaining_uncached_external_products_invalidates_the_comparison(self):
        rows = self.samples()
        rows[1]['sodium_compiled_sources'] = []
        result, summary = self.summarize(rows)
        self.assertEqual(result.returncode, 1)
        self.assertTrue(any('workload' in error for error in summary['errors']))

    def test_successful_save_without_a_remote_entry_is_not_accepted(self):
        rows = self.samples()
        rows[1]['actions']['saved_lookup_hit'] = ''
        result, summary = self.summarize(rows)
        self.assertEqual(result.returncode, 1)
        self.assertTrue(any('save' in error for error in summary['errors']))

    def test_resource_floor_is_a_hard_gate_even_when_time_improves(self):
        rows = self.samples()
        rows[1]['memory']['min_available_mib'] = 1000
        result, summary = self.summarize(rows)
        self.assertEqual(result.returncode, 1)
        self.assertTrue(any('memory' in error for error in summary['errors']))

    def test_cache_misses_cannot_be_reported_as_a_hot_rebuild(self):
        rows = self.samples()
        rows[1]['cache_delta'] = {'direct_cache_hit': 1, 'cache_miss': 1}
        result, summary = self.summarize(rows)
        self.assertEqual(result.returncode, 1)
        self.assertTrue(any('hot' in error for error in summary['errors']))

    def test_a_fast_outlier_cannot_hide_two_regressing_pairs(self):
        rows = self.samples()
        for row, seconds in zip(rows, [100, 110, 200, 210, 1000, 1]):
            row['net_wait_s'] = seconds
        result, summary = self.summarize(rows)
        self.assertEqual(result.returncode, 1)
        self.assertTrue(any('majority' in error for error in summary['errors']))

    def test_failed_setup_still_produces_an_explicit_incomplete_report(self):
        result, summary = self.summarize([])
        self.assertEqual(result.returncode, 1)
        self.assertFalse(summary['accepted'])
        self.assertTrue(any('complete' in error for error in summary['errors']))

    def test_resource_swap_growth_invalidates_otherwise_compatible_pairs(self):
        rows = self.samples()
        rows[1]['memory']['swap_growth_mib'] = 512
        result, summary = self.summarize(rows)
        self.assertEqual(result.returncode, 1)
        self.assertTrue(any('swap' in error for error in summary['errors']))

    def test_three_complete_compatible_pairs_meet_the_gate(self):
        result, summary = self.summarize(self.samples())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(summary['accepted'])

    def test_toolchain_bucket_changes_invalidate_the_frozen_group(self):
        rows = self.samples()
        rows[3]['bucket'] = 'different-compiler'
        result, summary = self.summarize(rows)
        self.assertEqual(result.returncode, 1)
        self.assertTrue(any('bucket' in error for error in summary['errors']))

    def test_hot_binaries_require_the_same_complete_correctness_suite(self):
        result, summary = self.summarize(self.samples(), missing_tests=True)
        self.assertEqual(result.returncode, 1)
        self.assertTrue(any('correctness' in error for error in summary['errors']))


if __name__ == '__main__':
    unittest.main()
