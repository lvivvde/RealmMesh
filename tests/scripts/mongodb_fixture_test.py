#!/usr/bin/env python3
"""Exercise fixture initialization through real, isolated MongoDB processes."""
import argparse
import contextlib
import os
import signal
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest

parser = argparse.ArgumentParser()
parser.add_argument('--source', required=True, type=Path)
parser.add_argument('--initializer', required=True, type=Path)
parser.add_argument('--probe', type=Path)
ARGS, unittest_args = parser.parse_known_args()


def tool(name):
    return subprocess.check_output([
        'bash', '-c',
        'realmmesh_mongodb_root="$1"; source "$1/scripts/lib/mongodb-tools.sh"; '
        '"realmmesh_${2}_binary"',
        'bash', str(ARGS.source), name,
    ], text=True).strip()


@contextlib.contextmanager
def mongod(replica_set='rs0'):
    with tempfile.TemporaryDirectory(prefix='mongodb-fixture-contract-') as directory:
        with socket.socket() as port_socket:
            port_socket.bind(('127.0.0.1', 0))
            port = port_socket.getsockname()[1]
        process = subprocess.Popen([
            tool('mongod'), '--replSet', replica_set, '--bind_ip', '127.0.0.1',
            '--port', str(port), '--dbpath', directory,
            '--logpath', str(Path(directory) / 'mongod.log'),
            '--nounixsocket', '--wiredTigerCacheSizeGB', '0.25',
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError('real mongod exited during startup')
                try:
                    with socket.create_connection(('127.0.0.1', port), timeout=0.1):
                        break
                except OSError:
                    time.sleep(0.02)
            else:
                raise RuntimeError('real mongod did not open its port')
            yield process, f'127.0.0.1:{port}'
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGCONT)
                process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


class MongoFixtureContract(unittest.TestCase):
    def test_initializes_replica_set_and_waits_for_primary(self):
        with mongod() as (_, member):
            result = subprocess.run([
                str(ARGS.initializer), member, '30000',
            ], capture_output=True, text=True, timeout=35)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            observed = subprocess.check_output([
                tool('mongosh'), f'mongodb://{member}/?directConnection=true',
                '--quiet', '--norc', '--eval',
                "const hello = db.hello(); print(hello.setName + ':' + hello.isWritablePrimary)",
            ], text=True, timeout=10).strip()
            self.assertEqual(observed, 'rs0:true')
            repeated = subprocess.run([
                str(ARGS.initializer), member, '30000',
            ], capture_output=True, text=True, timeout=35)
            self.assertEqual(repeated.returncode, 0, repeated.stdout + repeated.stderr)

    def test_initialization_failure_does_not_report_ready(self):
        with mongod(replica_set='different-set') as (_, member):
            result = subprocess.run([
                str(ARGS.initializer), member, '30000',
            ], capture_output=True, text=True, timeout=35)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('replSetInitiate failed', result.stderr)
            self.assertNotIn('primary ready', result.stdout)

    def test_deadline_reaps_initializer_and_allows_recovery(self):
        with mongod() as (server, member):
            initialized = subprocess.run([
                str(ARGS.initializer), member, '30000',
            ], capture_output=True, text=True, timeout=35)
            self.assertEqual(initialized.returncode, 0, initialized.stderr)
            server.send_signal(signal.SIGSTOP)
            try:
                child = subprocess.Popen([
                    str(ARGS.initializer), member, '200',
                ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                try:
                    _, error = child.communicate(timeout=2)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.communicate()
                    self.fail('initializer did not respect its deadline')
                self.assertEqual(child.returncode, 124, error)
                with self.assertRaises(ProcessLookupError):
                    os.kill(child.pid, 0)
            finally:
                server.send_signal(signal.SIGCONT)
            recovered = subprocess.run([
                str(ARGS.initializer), member, '30000',
            ], capture_output=True, text=True, timeout=35)
            self.assertEqual(recovered.returncode, 0, recovered.stderr)

    def test_constructor_failure_reaps_mongod_and_removes_its_data(self):
        self.assertIsNotNone(ARGS.probe, 'fixture constructor probe is required')
        for failure in ('missing', 'stopped'):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory(
                    prefix='mongodb-constructor-contract-') as directory:
                root = Path(directory)
                wrapper = root / 'mongod-wrapper.py'
                wrapper.write_text(
                    '#!/usr/bin/env python3\n'
                    'import os,sys\n'
                    'from pathlib import Path\n'
                    'Path(os.environ["REALMMESH_FIXTURE_PID_FILE"]).write_text(str(os.getpid()))\n'
                    'binary=os.environ["REALMMESH_FIXTURE_REAL_MONGOD"]\n'
                    'os.execv(binary,[binary,*sys.argv[1:]])\n')
                wrapper.chmod(0o755)
                initializer = root / 'initializer'
                if failure == 'stopped':
                    initializer.write_text(
                        '#!/usr/bin/env python3\n'
                        'import os,signal,sys\n'
                        'from pathlib import Path\n'
                        'os.kill(int(Path(os.environ["REALMMESH_FIXTURE_PID_FILE"]).read_text()),signal.SIGSTOP)\n'
                        'binary=os.environ["REALMMESH_FIXTURE_REAL_INITIALIZER"]\n'
                        'os.execv(binary,[binary,sys.argv[1],"200"])\n')
                    initializer.chmod(0o755)
                env = os.environ.copy()
                env.update(
                    TMPDIR=directory, REALMMESH_MONGOD_BINARY=str(wrapper),
                    REALMMESH_FIXTURE_REAL_MONGOD=tool('mongod'),
                    REALMMESH_FIXTURE_REAL_INITIALIZER=str(ARGS.initializer),
                    REALMMESH_FIXTURE_PID_FILE=str(root / 'pid'),
                    REALMMESH_TEST_MONGODB_INITIALIZER=str(initializer),
                )
                reclaimed = False
                try:
                    result = subprocess.run([
                        str(ARGS.probe),
                    ], env=env, capture_output=True, text=True, timeout=10)
                    self.assertNotEqual(result.returncode, 0, result.stdout)
                    pid = int((root / 'pid').read_text())
                    with self.assertRaises(ProcessLookupError):
                        os.kill(pid, 0)
                    self.assertEqual(list(root.glob('realmmesh-mongod-*')), [])
                    reclaimed = True
                finally:
                    # Only a failed probe needs emergency cleanup of its own child.
                    if not reclaimed and (root / 'pid').exists():
                        pid = int((root / 'pid').read_text())
                        try:
                            os.kill(pid, signal.SIGCONT)
                            os.kill(pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass


if __name__ == '__main__':
    unittest.main(argv=[__file__, *unittest_args], verbosity=2)
