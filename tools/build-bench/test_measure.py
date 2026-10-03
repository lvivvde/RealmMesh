"""measure.py 纯函数的单元测试：python3 -m unittest discover tools/build-bench"""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import measure


class ClassifyEventTest(unittest.TestCase):
    root = Path('/w/source')

    def test_project_compile(self):
        event = {'argv': ['c++', '-Iinc', '-c', '/w/source/framework/a.cpp', '-o', 'a.o'], 'cwd': '/w/source/build/dev'}
        got = measure.classify_event(event, self.root)
        self.assertEqual(got['kind'], 'compile')
        self.assertEqual(got['source'], '/w/source/framework/a.cpp')
        self.assertFalse(got['dependency'])

    def test_generated_source_in_build_tree_is_project(self):
        event = {'argv': ['c++', '-c', '/w/source/build/dev/proto/envelope.pb.cc'], 'cwd': '/w/source/build/dev'}
        self.assertFalse(measure.classify_event(event, self.root)['dependency'])

    def test_fetchcontent_source_is_dependency(self):
        event = {'argv': ['cc', '-c', '/w/source/build/dev/_deps/lua_source-src/lapi.c'], 'cwd': '/w/source/build/dev'}
        self.assertTrue(measure.classify_event(event, self.root)['dependency'])

    def test_source_outside_root_is_dependency(self):
        event = {'argv': ['c++', '-c', '/w/deps/absl-src/absl/base/log.cc'], 'cwd': '/w/source/build/dev'}
        self.assertTrue(measure.classify_event(event, self.root)['dependency'])

    def test_relative_source_resolves_against_cwd(self):
        event = {'argv': ['c++', '-c', '../../framework/a.cpp'], 'cwd': '/w/source/build/dev'}
        got = measure.classify_event(event, self.root)
        self.assertEqual(got['source'], '/w/source/framework/a.cpp')
        self.assertFalse(got['dependency'])

    def test_link_records_output(self):
        event = {'argv': ['c++', 'a.o', 'b.o', '-o', '../../bin/realm_mesh', '-lssl'], 'cwd': '/w/source/build/dev/apps/mesh_host'}
        got = measure.classify_event(event, self.root)
        self.assertEqual(got['kind'], 'link')
        self.assertIsNone(got['source'])
        self.assertEqual(got['output'], '/w/source/build/dev/bin/realm_mesh')


class SampleStatsTest(unittest.TestCase):
    def test_odd_samples(self):
        got = measure.sample_stats([30.0, 29.0, 31.0])
        self.assertEqual((got['n'], got['median'], got['min'], got['max']), (3, 30.0, 29.0, 31.0))

    def test_noise_band_floor_is_five_percent(self):
        self.assertAlmostEqual(measure.sample_stats([100.0, 101.0, 102.0])['noise'], 0.05)

    def test_noise_band_uses_range_over_median(self):
        self.assertAlmostEqual(measure.sample_stats([90.0, 100.0, 110.0])['noise'], 0.2)

    def test_single_sample_has_no_noise_band(self):
        got = measure.sample_stats([363.0])
        self.assertEqual(got['median'], 363.0)
        self.assertIsNone(got['noise'])

    def test_zero_median(self):
        self.assertEqual(measure.sample_stats([0, 0, 0])['noise'], 0.05)


class StageGroupTest(unittest.TestCase):
    def test_strips_sample_index(self):
        self.assertEqual(measure.stage_group('probe-lua-hpp-2'), 'probe-lua-hpp')

    def test_keeps_unnumbered_stage(self):
        self.assertEqual(measure.stage_group('test-full'), 'test-full')

    def test_keeps_inner_digits(self):
        self.assertEqual(measure.stage_group('p2b-cold-build-3'), 'p2b-cold-build')


class ParseCtestLogTest(unittest.TestCase):
    def test_all_passed(self):
        text = '100% tests passed, 0 tests failed out of 647\n\nTotal Test time (real) = 362.79 sec\n'
        got = measure.parse_ctest_log(text)
        self.assertEqual((got['tests_total'], got['tests_failed'], got['failed_tests']), (647, 0, []))
        self.assertAlmostEqual(got['ctest_real_s'], 362.79)

    def test_failures_listed(self):
        text = (
            '271/647 Test #271: HttpServerTest.DeferredResponseSurvivesClientHalfClose ....***Failed    0.17 sec\n'
            '300/647 Test #300: Foo.Bar ....   Passed    0.01 sec\n'
            '620/647 Test #620: RealmJourneyTest.Character ....***Timeout 300.01 sec\n'
            '99% tests passed, 2 tests failed out of 647\n'
            'Total Test time (real) = 281.73 sec\n'
        )
        got = measure.parse_ctest_log(text)
        self.assertEqual(got['tests_failed'], 2)
        self.assertEqual(got['failed_tests'], [
            'HttpServerTest.DeferredResponseSurvivesClientHalfClose',
            'RealmJourneyTest.Character',
        ])

    def test_all_passed_without_failed_clause(self):
        # CMake 4.x 的 CTest 全部通过时省略 “N tests failed”。
        got = measure.parse_ctest_log('100% tests passed out of 649\n\nTotal Test time (real) = 364.24 sec\n')
        self.assertEqual((got['tests_total'], got['tests_failed']), (649, 0))

    def test_no_ctest_output(self):
        self.assertEqual(measure.parse_ctest_log('[ 10%] Building CXX object a.o\n'), {})


class DelayToNextSecondTest(unittest.TestCase):
    # GNU Make 3.81（macOS /usr/bin/make）按整秒比较 mtime：改动须落在构建产物之后的整秒里。
    def test_mid_second_waits_past_boundary(self):
        delay = measure.delay_to_next_second(100.4)
        self.assertGreater(100.4 + delay, 101.0)
        self.assertLess(delay, 1.0)

    def test_on_boundary_still_waits_a_full_second(self):
        self.assertGreater(100.0 + measure.delay_to_next_second(100.0), 101.0)

    def test_just_before_boundary(self):
        delay = measure.delay_to_next_second(100.999)
        self.assertGreater(100.999 + delay, 101.0)
        self.assertLess(delay, 0.2)


class VariantBytesTest(unittest.TestCase):
    original = b'namespace a {}\n'

    def test_comment_variant_only_appends_comment(self):
        got = measure.variant_bytes(self.original, 'x/a.hpp', 'lua-hpp', 2, 'comment')
        self.assertTrue(got.startswith(self.original))
        self.assertIn(b'// build-bench lua-hpp content-change sample 2', got)

    def test_cpp_token_variant_declares_inline_variable(self):
        got = measure.variant_bytes(self.original, 'x/a.cpp', 'lua-cpp', 3, 'token')
        self.assertTrue(got.startswith(self.original))
        self.assertIn(b'inline constexpr int realmmesh_build_bench_lua_cpp_variant = 3;', got)

    def test_header_token_variant(self):
        got = measure.variant_bytes(self.original, 'x/a.hpp', 'player-data-hpp', 0, 'token')
        self.assertIn(b'realmmesh_build_bench_player_data_hpp_variant = 0;', got)

    def test_proto_token_variant_adds_message(self):
        got = measure.variant_bytes(b'syntax = "proto3";\n', 'p/e.proto', 'proto', 4, 'token')
        self.assertIn(b'message BuildBenchVariant4 {}', got)

    def test_samples_differ(self):
        self.assertNotEqual(measure.variant_bytes(self.original, 'a.cpp', 'l', 1, 'token'),
                            measure.variant_bytes(self.original, 'a.cpp', 'l', 2, 'token'))

    def test_unknown_suffix_rejected(self):
        with self.assertRaises(ValueError):
            measure.variant_bytes(self.original, 'a.lua', 'l', 1, 'token')


class EditedTest(unittest.TestCase):
    def write(self, path, content):
        path.write_bytes(content)

    def test_variants_derive_from_original_and_restore(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'a.cpp'
            path.write_bytes(b'int a;\n')
            with measure.edited(path, self.write) as original:
                for i in (1, 2, 3):
                    self.write(path, measure.variant_bytes(original, 'a.cpp', 'x', i, 'comment'))
                self.assertEqual(path.read_bytes().count(b'build-bench'), 1)
            self.assertEqual(path.read_bytes(), b'int a;\n')

    def test_restores_on_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'a.cpp'
            path.write_bytes(b'int a;\n')
            with self.assertRaises(RuntimeError):
                with measure.edited(path, self.write):
                    self.write(path, b'broken')
                    raise RuntimeError
            self.assertEqual(path.read_bytes(), b'int a;\n')

    def test_restores_on_sigterm(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'a.cpp'
            path.write_bytes(b'int a;\n')
            child = subprocess.Popen([sys.executable, '-c', (
                'import os, signal, sys, time\n'
                f'sys.path.insert(0, {str(Path(__file__).parent)!r})\n'
                'import measure\n'
                'measure.exit_on_sigterm()\n'
                f'path = measure.Path({str(path)!r})\n'
                'with measure.edited(path, measure.Path.write_bytes):\n'
                "    path.write_bytes(b'broken')\n"
                "    print('edited', flush=True)\n"
                '    time.sleep(30)\n'
            )], stdout=subprocess.PIPE)
            self.assertEqual(child.stdout.readline(), b'edited\n')
            child.terminate()
            child.wait(timeout=10)
            child.stdout.close()
            self.assertEqual(child.returncode, 143)
            self.assertEqual(path.read_bytes(), b'int a;\n')


class TreeRssTest(unittest.TestCase):
    def test_sums_root_and_descendants_only(self):
        rows = [(10, 1, 100), (11, 10, 50), (12, 11, 25), (20, 1, 999), (13, 10, 5)]
        self.assertEqual(measure.tree_rss_kib(rows, 10), 180)

    def test_missing_root(self):
        self.assertEqual(measure.tree_rss_kib([(2, 1, 7)], 10), 0)

    def test_parse_ps(self):
        self.assertEqual(measure.parse_ps('  10     1  100\n 11 10 50\nbad line\n'), [(10, 1, 100), (11, 10, 50)])


class EffectiveAvailableTest(unittest.TestCase):
    def test_no_cgroup_limit(self):
        self.assertEqual(measure.effective_available_mib(4096.0, None, None), 4096.0)

    def test_cgroup_limit_is_tighter(self):
        self.assertEqual(measure.effective_available_mib(4096.0, 2048 * 1048576, 1536 * 1048576), 512.0)

    def test_host_is_tighter(self):
        self.assertEqual(measure.effective_available_mib(100.0, 2048 * 1048576, 0), 100.0)


class ParseExternalUrlTest(unittest.TestCase):
    def test_reads_url_and_hash(self):
        text = (
            'ExternalProject_Add(\n    sodium_external\n'
            '    URL https://example.org/libsodium-1.0.22.tar.gz\n'
            '    URL_HASH SHA256=ABCDEF0123\n)\n'
        )
        self.assertEqual(measure.parse_external_url(text), ('https://example.org/libsodium-1.0.22.tar.gz', 'abcdef0123'))

    def test_missing(self):
        with self.assertRaises(ValueError):
            measure.parse_external_url('add_library(x)')


class SummarizeTest(unittest.TestCase):
    def test_groups_rows_and_flags_failures(self):
        rows = [
            {'name': 'noop-1', 'wall_s': 6.0, 'exit': 0, 'compile_count': 0, 'link_count': 0},
            {'name': 'noop-2', 'wall_s': 6.4, 'exit': 0, 'compile_count': 0, 'link_count': 0},
            {'name': 'noop-3', 'wall_s': 6.2, 'exit': 0, 'compile_count': 0, 'link_count': 0},
            {'name': 'test-full', 'wall_s': 360.0, 'exit': 8, 'compile_count': 0, 'link_count': 0,
             'tests_total': 647, 'tests_failed': 1, 'failed_tests': ['A.B']},
        ]
        got = {g['group']: g for g in measure.summarize(rows)}
        self.assertEqual(got['noop']['wall_s']['median'], 6.2)
        self.assertEqual(got['noop']['wall_s']['values'], [6.0, 6.4, 6.2])
        self.assertEqual(got['noop']['failures'], [])
        self.assertEqual(got['test-full']['failures'], ['test-full: exit 8; failed A.B'])

    def test_warmup_excluded(self):
        rows = [
            {'name': 'unit-entry-warmup', 'warmup': True, 'wall_s': 99.0, 'exit': 0},
            {'name': 'unit-entry-1', 'wall_s': 10.0, 'exit': 0},
            {'name': 'unit-entry-2', 'wall_s': 12.0, 'exit': 0},
        ]
        got = measure.summarize(rows)
        self.assertEqual([g['group'] for g in got], ['unit-entry'])
        self.assertEqual(got[0]['wall_s']['n'], 2)

    def test_step_breakdown_keeps_order_and_total_is_measured(self):
        rows = [
            {'name': 'hot-full-1', 'wall_s': 20.0, 'exit': 0,
             'steps': [{'label': 'configure', 'wall_s': 1.0}, {'label': 'build', 'wall_s': 5.0}, {'label': 'test', 'wall_s': 13.0}]},
            {'name': 'hot-full-2', 'wall_s': 30.0, 'exit': 0,
             'steps': [{'label': 'configure', 'wall_s': 2.0}, {'label': 'build', 'wall_s': 9.0}, {'label': 'test', 'wall_s': 18.0}]},
        ]
        got = measure.summarize(rows)[0]
        self.assertEqual([s['label'] for s in got['steps']], ['configure', 'build', 'test'])
        self.assertEqual(got['steps'][1]['wall_s']['median'], 7.0)
        # 总墙钟取整次实测的中位数，不是各步中位数之和（1.5 + 7 + 15.5 = 24）。
        self.assertEqual(got['wall_s']['median'], 25.0)


if __name__ == '__main__':
    unittest.main()
