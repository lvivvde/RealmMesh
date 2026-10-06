"""Black-box CMake/compiler-cache contracts; registered as integration tests."""
import argparse
import os
from pathlib import Path
import shutil
import re
import subprocess
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument('--source', required=True)
parser.add_argument('--work', required=True)
parser.add_argument('--cmake', default='cmake')
parser.add_argument('--native', action='store_true')
parser.add_argument('--ccache', help='actual configured native cache executable')
options, remaining = parser.parse_known_args()
root = Path(options.source).resolve()
work = Path(options.work).resolve()
work.mkdir(parents=True, exist_ok=True)


class CacheFixture(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=work)
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)
        self.src = self.path / 'src'
        self.src.mkdir()
        self.build = self.path / 'build'
        self.cache = self.path / 'cache'
        self.env = {key: value for key, value in os.environ.items()
                    if not key.startswith(('CCACHE_', 'CMAKE_C_COMPILER_LAUNCHER', 'CMAKE_CXX_COMPILER_LAUNCHER'))}
        self.write_project()

    def write_project(self, languages='NONE'):
        (self.src / 'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.20)\n'
            f'project(CacheContract LANGUAGES {languages})\n'
            f'include("{root}/cmake/RealmMeshCCache.cmake")\n'
            'realmmesh_configure_ccache()\n'
            'file(WRITE "${CMAKE_BINARY_DIR}/launcher.txt" "${CMAKE_C_COMPILER_LAUNCHER}|${CMAKE_CXX_COMPILER_LAUNCHER}")\n')

    def configure(self, *args, ok=True):
        cmd = [options.cmake, '-S', str(self.src), '-B', str(self.build), '-G', 'Ninja',
               f'-DCMAKE_MAKE_PROGRAM={shutil.which("ninja") or shutil.which("ninja-build")}',
               f'-DREALMMESH_CCACHE_DIR={self.cache}', '-DREALMMESH_CCACHE_MAX_SIZE=5GiB'] + list(args)
        result = subprocess.run(cmd, env=self.env, capture_output=True, text=True)
        if ok:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout + result.stderr

    def fake_tool(self, version, status=0):
        tool = self.path / 'fake-ccache'
        tool.write_text(f'#!/bin/sh\nprintf "%s\\n" "{version}"\nexit {status}\n')
        tool.chmod(0o700)
        return f'-DREALMMESH_CCACHE_EXECUTABLE={tool}'



class CacheContract(CacheFixture):
    def test_malformed_explicit_version_is_rejected(self):
        output = self.configure('-DREALMMESH_CCACHE=ON', self.fake_tool('ccache version 4.8garbage'), ok=False)
        self.assertIn('Invalid ccache executable', output)

    def test_modes_absence_and_user_launcher(self):
        # Ignore all possible ccache search directories. Generator is explicit;
        # LANGUAGES NONE keeps the absence check independent of compiler lookup.
        ignored = ';'.join(set(self.env.get('PATH', '').split(os.pathsep) +
                               ['/usr/bin', '/bin', '/usr/local/bin', '/opt/homebrew/bin']))
        missing = ['-DREALMMESH_CCACHE_EXECUTABLE=REALMMESH_CCACHE_EXECUTABLE-NOTFOUND',
                   f'-DCMAKE_IGNORE_PATH={ignored}']
        output = self.configure('-DREALMMESH_CCACHE=AUTO', *missing)
        self.assertIn('disabled', output)
        self.assertEqual((self.build / 'launcher.txt').read_text(), '|')
        output = self.configure('-DREALMMESH_CCACHE=ON', *missing, ok=False)
        self.assertIn('requires ccache', output)
        custom = '-DCMAKE_C_COMPILER_LAUNCHER=custom-observer'
        self.configure('-DREALMMESH_CCACHE=OFF', custom)
        self.assertEqual((self.build / 'launcher.txt').read_text(), 'custom-observer|')
        output = self.configure('-DREALMMESH_CCACHE=ON', self.fake_tool('ccache version 4.8'), ok=False)
        self.assertIn('conflicts', output)

    def test_auto_rediscovers_after_a_discovered_tool_is_removed(self):
        bindir = self.path / 'discovered'
        bindir.mkdir()
        tool = bindir / 'ccache'
        tool.write_text('#!/bin/sh\necho "ccache version 4.8.2"\n')
        tool.chmod(0o700)
        ignored = ';'.join(set(self.env.get('PATH', '').split(os.pathsep) +
                               ['/usr/bin', '/bin', '/usr/local/bin', '/opt/homebrew/bin']))
        args = ['-DREALMMESH_CCACHE=AUTO', f'-DCMAKE_PROGRAM_PATH={bindir}',
                f'-DCMAKE_IGNORE_PATH={ignored}']
        self.configure(*args)
        self.assertIn(str(tool), (self.build / 'launcher.txt').read_text())
        tool.unlink()
        output = self.configure(*args)
        self.assertIn('disabled', output)
        self.assertEqual((self.build / 'launcher.txt').read_text(), '|')

    def test_explicit_tool_has_same_native_registration_on_and_off(self):
        with (self.src / 'CMakeLists.txt').open('a') as stream:
            stream.write('enable_testing()\n'
                         f'include("{root}/tests/cmake/compiler-cache-contract.cmake")\n')
        ignored = ';'.join(set(self.env.get('PATH', '').split(os.pathsep) +
                               ['/usr/bin', '/bin', '/usr/local/bin', '/opt/homebrew/bin']))
        import sys
        import json
        tool = self.fake_tool('ccache version 4.8.2')
        for mode in ('ON', 'OFF'):
            self.configure(f'-DREALMMESH_CCACHE={mode}', tool, f'-DCMAKE_IGNORE_PATH={ignored}',
                           f'-DREALMMESH_TEST_PYTHON={sys.executable}')
            output = subprocess.check_output([str(Path(options.cmake).with_name('ctest'))
                                              if Path(options.cmake).is_absolute() else 'ctest',
                                              '--test-dir', str(self.build), '--show-only=json-v1'],
                                             env=self.env, text=True)
            names = {test['name'] for test in json.loads(output)['tests']}
            self.assertEqual(names, {'CompilerCacheConfigTest', 'CompilerCacheNativeTest',
                                     'CompilerCacheCISummaryTest'})

    def test_on_to_off_clears_only_managed_launchers(self):
        tool = self.fake_tool('ccache version 4.8.2')
        self.configure('-DREALMMESH_CCACHE=ON', tool)
        self.assertIn('fake-ccache', (self.build / 'launcher.txt').read_text())
        self.cache.mkdir()
        (self.cache / 'sentinel').write_text('keep cache')
        self.configure('-DREALMMESH_CCACHE=OFF')
        self.assertEqual((self.build / 'launcher.txt').read_text(), '|')
        self.configure('-DREALMMESH_CCACHE=AUTO', tool)
        self.configure('-DREALMMESH_CCACHE=OFF', '-DCMAKE_CXX_COMPILER_LAUNCHER=custom')
        self.assertEqual((self.build / 'launcher.txt').read_text(), '|custom')
        self.assertEqual((self.cache / 'sentinel').read_text(), 'keep cache')

    def test_invalid_configuration_and_tool(self):
        for value in ('invalid', 'on', ''):
            self.assertIn('must be AUTO, ON or OFF', self.configure(f'-DREALMMESH_CCACHE={value}', ok=False))
        for version in ('ccache version 3.7', 'ccache version 4.7.5', 'ccache version 5.0'):
            self.configure('-DREALMMESH_CCACHE=ON', self.fake_tool(version), ok=False)
        self.configure('-DREALMMESH_CCACHE=AUTO', self.fake_tool('ccache version 4.8', status=7), ok=False)
        self.configure('-DREALMMESH_CCACHE=AUTO', '-DREALMMESH_CCACHE_EXECUTABLE=/nonexistent/ccache', ok=False)
        tool = self.fake_tool('ccache version 4.8')
        for size in ('0', '-1GiB', 'unlimited', 'bad', '0GiB'):
            self.assertIn('positive size', self.configure('-DREALMMESH_CCACHE=ON', tool,
                          f'-DREALMMESH_CCACHE_MAX_SIZE={size}', ok=False))
        self.assertIn('outside the build', self.configure('-DREALMMESH_CCACHE=ON', tool,
                      f'-DREALMMESH_CCACHE_DIR={self.build}/cache', ok=False))
        self.assertIn('absolute path', self.configure('-DREALMMESH_CCACHE=ON', tool,
                      '-DREALMMESH_CCACHE_DIR=relative', ok=False))

    def test_ci_bucket_is_stable_and_separates_fixed_metadata(self):
        key_root = self.path / 'key-root'
        (key_root / 'scripts').mkdir(parents=True)
        shutil.copy2(root / 'scripts/ci-ccache-key.py', key_root / 'scripts/ci-ccache-key.py')
        (key_root / 'CMakePresets.json').write_text('{"version":2}')
        cmake = key_root / 'CMakeLists.txt'
        cmake.write_text('set(CMAKE_BUILD_TYPE Debug)\n')
        fakebin = self.path / 'tools'
        fakebin.mkdir()
        fake = fakebin / 'ccache'
        fake.write_text('#!/bin/sh\necho "ccache version 4.8.2"\n')
        fake.chmod(0o700)
        env = dict(self.env, PATH=str(fakebin) + os.pathsep + self.env['PATH'])
        env.pop('GITHUB_OUTPUT', None)
        import sys

        def bucket():
            text = subprocess.check_output([sys.executable, str(key_root / 'scripts/ci-ccache-key.py')], env=env, text=True)
            return dict(line.split('=', 1) for line in text.splitlines())

        first = bucket()
        self.assertEqual(bucket(), first)
        self.assertTrue(first['key'].startswith(first['bucket'] + '-'))
        cmake.write_text('set(CMAKE_BUILD_TYPE Release)\n')
        second = bucket()
        self.assertNotEqual(first['bucket'], second['bucket'])
        dependency = key_root / 'third_party/example'
        dependency.mkdir(parents=True)
        (dependency / 'CMakeLists.txt').write_text('set(DEPENDENCY_SHA256 changed)\n')
        third = bucket()
        self.assertNotEqual(second['bucket'], third['bucket'])
        fake.write_text('#!/bin/sh\necho "ccache version 4.12.3"\n')
        self.assertNotEqual(third['bucket'], bucket()['bucket'])


class NativeCacheContract(CacheFixture):
    def setUp(self):
        super().setUp()
        self.tool = options.ccache or shutil.which('ccache')
        self.assertIsNotNone(self.tool, 'native cache contract requires ccache >= 4.8')
        self.write_project('C CXX')
        with (self.src / 'CMakeLists.txt').open('a') as stream:
            stream.write('add_executable(probe main.cpp value.c)\n')
        (self.src / 'value.h').write_text('#define HEADER_VALUE 10\n')
        (self.src / 'value.c').write_text('#include "value.h"\n#ifndef VALUE\n#define VALUE 2\n#endif\nint value(void) { return HEADER_VALUE + VALUE; }\n')
        (self.src / 'main.cpp').write_text('#include <cstdio>\nextern "C" int value(void);\nint main() { std::printf("%d\\n", value()); }\n')

    def build_probe(self, expected, ok=True):
        result = subprocess.run([options.cmake, '--build', str(self.build), '--parallel', '2'],
                                env=self.env, capture_output=True, text=True)
        if not ok:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            return result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(subprocess.check_output([str(self.build / 'probe')], env=self.env, text=True).strip(), expected)
        return result.stdout + result.stderr

    def clean(self):
        subprocess.run([options.cmake, '--build', str(self.build), '--target', 'clean'],
                       env=self.env, capture_output=True, check=True)

    def stats(self):
        output = subprocess.check_output([self.tool, '--config-path', str(self.build / 'realmmesh-ccache.conf'),
                                          '-d', str(self.cache), '--print-stats'], env=self.env, text=True)
        return {key: int(value) for key, value in (line.split() for line in output.splitlines())}

    def enable(self, *args):
        self.configure('-DREALMMESH_CCACHE=ON', f'-DREALMMESH_CCACHE_EXECUTABLE={self.tool}', *args)
        # Header timestamp safety is intentional; do not relax sloppiness.
        import time
        time.sleep(1.05)

    def test_cold_hot_and_off_run_same_program(self):
        self.enable()
        self.build_probe('12')
        before = self.stats()
        self.assertGreaterEqual(before['cache_miss'], 2)
        self.clean()
        self.build_probe('12')
        after = self.stats()
        self.assertGreater(after['direct_cache_hit'] + after['preprocessed_cache_hit'],
                           before['direct_cache_hit'] + before['preprocessed_cache_hit'])
        self.configure('-DREALMMESH_CCACHE=OFF')
        self.clean()
        self.build_probe('12')
        self.assertEqual(self.stats()['cache_miss'], after['cache_miss'])

    def test_source_headers_defines_and_flags_invalidate(self):
        self.enable()
        self.build_probe('12')
        misses = self.stats()['cache_miss']
        (self.src / 'value.h').write_text('#define HEADER_VALUE 20\n')
        self.build_probe('22')
        self.assertGreater(self.stats()['cache_miss'], misses)
        misses = self.stats()['cache_miss']
        (self.src / 'value.c').write_text('#include "value.h"\nint value(void) { return HEADER_VALUE + 5; }\n')
        self.build_probe('25')
        self.assertGreater(self.stats()['cache_miss'], misses)
        (self.src / 'value.c').write_text('#include "value.h"\n#ifndef VALUE\n#define VALUE 2\n#endif\nint value(void) { return HEADER_VALUE + VALUE; }\n')
        self.enable('-DCMAKE_C_FLAGS=-DVALUE=7')
        self.build_probe('27')
        misses = self.stats()['cache_miss']
        self.enable('-DCMAKE_C_FLAGS=-DVALUE=7 -O2')
        self.build_probe('27')
        self.assertGreater(self.stats()['cache_miss'], misses)

    def test_corruption_eviction_and_compile_error(self):
        self.enable()
        self.build_probe('12')
        # Cache filenames differ between ccache versions. Identify entries via
        # the public diagnostic command instead of depending on their encoding.
        results = [p for p in self.cache.rglob('*') if p.is_file() and
                   subprocess.run([self.tool, '--inspect', str(p)], env=self.env,
                                  capture_output=True).returncode == 0]
        self.assertTrue(results, 'no ccache result objects found')
        for path in results:
            path.write_bytes(b'corrupted-cache-object')
        self.env['CCACHE_LOGFILE'] = str(self.path / 'cache.log')
        self.clean()
        self.build_probe('12')
        self.assertRegex((self.path / 'cache.log').read_text().lower(), 'error|corrupt|bad|checksum')
        self.enable('-DREALMMESH_CCACHE_MAX_SIZE=1KiB')
        (self.src / 'value.h').write_text('#define HEADER_VALUE 30\n')
        self.clean()
        self.build_probe('32')
        subprocess.run([self.tool, '-d', str(self.cache), '-M', '1KiB', '--cleanup'],
                       env=self.env, capture_output=True, check=True)
        self.assertGreater(self.stats()['cleanups_performed'], 0)
        self.clean()
        self.build_probe('32')
        (self.src / 'value.c').write_text('this is a compiler error\n')
        output = self.build_probe(None, ok=False)
        self.assertIn('error', output.lower())

    def test_compiler_content_and_generated_header_invalidate(self):
        tool_dir = self.path / 'compiler'
        tool_dir.mkdir()
        wrapper = tool_dir / 'cc'
        real_cc = shutil.which('cc')
        wrapper.write_text(f'#!/bin/sh\nexec "{real_cc}" "$@"\n')
        wrapper.chmod(0o700)
        self.enable(f'-DCMAKE_C_COMPILER={wrapper}')
        self.build_probe('12')
        misses = self.stats()['cache_miss']
        wrapper.write_text(f'#!/bin/sh\n# distinct compiler content\nexec "{real_cc}" "$@"\n')
        self.clean()
        self.build_probe('12')
        self.assertGreater(self.stats()['cache_miss'], misses)
        with (self.src / 'CMakeLists.txt').open('a') as stream:
            stream.write('configure_file(input.h.in generated.h COPYONLY)\n'
                         'target_include_directories(probe PRIVATE "${CMAKE_BINARY_DIR}")\n')
        (self.src / 'input.h.in').write_text('#define GENERATED_VALUE 3\n')
        (self.src / 'value.c').write_text('#include "generated.h"\nint value(void) { return GENERATED_VALUE; }\n')
        self.enable()
        self.build_probe('3')
        (self.src / 'input.h.in').write_text('#define GENERATED_VALUE 9\n')
        self.enable()
        self.build_probe('9')
        (self.build / 'generated.h').unlink()
        self.enable()
        self.clean()
        self.build_probe('9')

    def test_measurement_stats_use_configured_directory_and_tool(self):
        # Exercise the public measurement CLI with an unrelated personal cache
        # and a bogus PATH ccache. Explicit tool/cache overrides must still work.
        project = self.src / 'CMakeLists.txt'
        project.write_text(project.read_text().replace('project(CacheContract', 'project(RealmMesh'))
        sodium = self.src / 'third_party/sodium'
        sodium.mkdir(parents=True)
        shutil.copy2(root / 'third_party/sodium/CMakeLists.txt', sodium / 'CMakeLists.txt')
        (self.src / 'CMakePresets.json').write_text(
            '{"version":2,"configurePresets":[{"name":"dev","generator":"Ninja",'
            '"binaryDir":"${sourceDir}/build"}],"buildPresets":[{"name":"dev","configurePreset":"dev"}]}')
        deps = self.path / 'deps'
        (deps / 'dummy-src').mkdir(parents=True)
        (deps / 'sodium-download').mkdir()
        (deps / 'sodium-download/libsodium-1.0.22.tar.gz').write_bytes(b'unused fixture package')
        fakebin = self.path / 'bin'
        fakebin.mkdir()
        (fakebin / 'ccache').write_text('#!/bin/sh\nexit 99\n')
        (fakebin / 'ccache').chmod(0o700)
        observer = self.path / 'observer'
        observer.write_text('#!/bin/sh\nexec "$@"\n')
        observer.chmod(0o700)
        out = self.path / 'measure'
        env = dict(self.env, CCACHE_DIR=str(self.path / 'unrelated'),
                   PATH=str(fakebin) + os.pathsep + self.env['PATH'])
        import sys
        import json
        result = subprocess.run([sys.executable, str(root / 'tools/build-bench/measure.py'), 'run',
                                 '--source', str(self.src), '--out', str(out), '--launcher', str(observer),
                                 '--deps-dir', str(deps), '--scenario', 'cold-build', '--long-samples', '1',
                                 '--cache-mode', 'ON', '--commit', 'fixture', '--jobs', '2',
                                 f'--cmake-arg=-DREALMMESH_CCACHE_DIR={self.cache}',
                                 f'--cmake-arg=-DREALMMESH_CCACHE_EXECUTABLE={self.tool}'],
                                env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        row = json.loads((out / 'stages.json').read_text())[0]
        self.assertNotIn('error', row['ccache_after'])
        self.assertGreaterEqual(row['ccache_after']['cache_miss'], 2)

    def test_unsafe_environment_does_not_relax_launcher(self):
        self.env.update(CCACHE_COMPILERCHECK='none', CCACHE_SLOPPINESS='time_macros,include_file_mtime',
                        CCACHE_NOHASHDIR='1', CCACHE_HARDLINK='1', CCACHE_DISABLE='1')
        self.enable()
        self.build_probe('12')
        self.assertGreaterEqual(self.stats()['cache_miss'], 2)
        (self.src / 'value.h').write_text('#define HEADER_VALUE 40\n')
        self.build_probe('42')


if __name__ == '__main__':
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(NativeCacheContract if options.native else CacheContract)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if result.wasSuccessful() else 1)
