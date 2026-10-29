#!/usr/bin/env python3
"""Own one real rs0 for a CTest process; reclaim it when that owner exits."""
import argparse
import json
import os
from pathlib import Path
import secrets
import shutil
import signal
import socket
import subprocess
import sys
import time


def identity(pid):
    # Linux exposes the process start time as monotonic clock ticks.  Unlike
    # `ps lstart`, this value does not move when a suspended VM corrects its
    # wall clock, so a live CTest owner is not mistaken for a reused PID.
    stat_path = Path(f'/proc/{pid}/stat')
    try:
        stat = stat_path.read_text()
    except (FileNotFoundError, PermissionError, ProcessLookupError):
        stat = ''
    if stat:
        closing_parenthesis = stat.rfind(')')
        fields = stat[closing_parenthesis + 2:].split()
        if closing_parenthesis > 0 and len(fields) > 19:
            if fields[0] == 'Z':
                return None
            return 'linux-start-ticks:' + fields[19]

    result = subprocess.run(['ps', '-p', str(pid), '-o', 'stat=', '-o', 'lstart='],
                            capture_output=True, text=True)
    value = result.stdout.strip()
    if result.returncode or not value or value.startswith('Z'):
        return None
    return value.split(maxsplit=1)[1]


def location(root, owner):
    return Path(root) / f'ctest-{owner}'


def binary(source):
    return subprocess.check_output([
        'bash', '-c', 'realmmesh_mongodb_root="$1"; '
        'source "$1/scripts/lib/mongodb-tools.sh"; realmmesh_mongod_binary',
        'bash', str(source)], text=True).strip()


def stop(child, timeout=2):
    if child is None or child.poll() is not None:
        return
    child.send_signal(signal.SIGCONT)
    child.terminate()
    try:
        child.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        child.kill()
        child.wait()


def retain_server_log(directory):
    # 数据目录随 owner 回收；只把最近一次运行的 mongod.log 留在 root 下，
    # 偶发失败（写关注超时等）事后仍有服务端现场可查。
    log = directory / 'mongod.log'
    if not log.is_file():
        return
    try:
        for stale in directory.parent.glob('ctest-*.mongod.log'):
            stale.unlink(missing_ok=True)
        shutil.copy2(log, Path(str(directory) + '.mongod.log'))
    except OSError:
        pass


def daemon(args):
    directory = location(args.root, args.owner)
    server = initializer = reply = registry = None
    requested = False
    def request_stop(_signal, _frame):
        nonlocal requested
        requested = True
    signal.signal(signal.SIGTERM, request_stop)
    signal.signal(signal.SIGINT, request_stop)
    data = directory / 'data'
    data.mkdir()
    try:
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            port = reservation.getsockname()[1]
        server = subprocess.Popen([
            binary(args.source), '--replSet', 'rs0', '--bind_ip', '127.0.0.1',
            '--port', str(port), '--dbpath', str(data),
            '--logpath', str(directory / 'mongod.log'), '--nounixsocket',
            '--wiredTigerCacheSizeGB', '0.25'],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if os.environ.get('REALMMESH_TEST_MONGODB_OBSERVER_DIRECTORY'):
            observer = Path(os.environ['REALMMESH_TEST_MONGODB_OBSERVER_DIRECTORY'])
            observer.mkdir(parents=True, exist_ok=True)
            registry = observer / f'guardian-{os.getpid()}.json'
            pending = observer / f'guardian-{os.getpid()}.pending'
            pending.write_text(json.dumps({'guardian_pid': os.getpid(), 'server_pid': server.pid}))
            pending.replace(registry)
        initializer = subprocess.Popen([str(args.initializer), f'127.0.0.1:{port}', '30000'],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        end = time.monotonic() + 30
        while initializer.poll() is None:
            if requested or identity(args.owner) != args.birth or server.poll() is not None:
                raise RuntimeError('CTest owner or real mongod exited during preparation')
            if time.monotonic() >= end:
                raise RuntimeError('MongoDB fixture preparation deadline exceeded')
            time.sleep(0.05)
        error = initializer.communicate()[1].decode()
        if initializer.returncode:
            raise RuntimeError('MongoDB fixture initializer failed: ' + error)
        with socket.socket() as control:
            control.bind(('127.0.0.1', 0))
            control.listen(4)
            control.settimeout(0.1)
            state = {'owner_pid': args.owner, 'owner_birth': args.birth,
                     'guardian_pid': os.getpid(), 'server_pid': server.pid,
                     'port': port, 'control_port': control.getsockname()[1],
                     'token': secrets.token_hex(16), 'data_directory': str(data)}
            (directory / 'state.json').write_text(json.dumps(state))
            pending = directory / 'port.pending'
            pending.write_text(f'{args.owner} {os.getpid()} {port}\n')
            pending.replace(directory / 'port')
            next_check = 0
            while not requested:
                if time.monotonic() >= next_check:
                    if identity(args.owner) != args.birth:
                        break
                    next_check = time.monotonic() + 0.25
                if server.poll() is not None:
                    raise RuntimeError('shared mongod exited before CTest cleanup')
                try:
                    connection, _ = control.accept()
                except socket.timeout:
                    continue
                connection.settimeout(1)
                message = connection.recv(128)
                if message != f'STOP {state["token"]}\n'.encode():
                    connection.close()
                    continue
                reply = connection
                break
    except Exception as error:
        # Keep diagnostics outside the data/ready directory; never publish readiness on failure.
        Path(str(directory) + '.error').write_text(str(error) + '\n')
        raise
    finally:
        stop(initializer)
        stop(server)
        if registry is not None:
            registry.unlink(missing_ok=True)
        retain_server_log(directory)
        shutil.rmtree(directory)
        if not Path(str(directory) + ".error").exists():
            Path(str(directory) + ".log").unlink(missing_ok=True)
        if reply is not None:
            reply.sendall(b'stopped\n')
            reply.close()


def require_ancestor(owner):
    ancestor = os.getppid()
    for _ in range(64):
        if ancestor == owner:
            return
        if ancestor <= 1:
            break
        parent = subprocess.run(['ps', '-p', str(ancestor), '-o', 'ppid='],
                                capture_output=True, text=True)
        try:
            ancestor = int(parent.stdout.strip()) if parent.returncode == 0 else 0
        except ValueError:
            break
    raise RuntimeError('CTest MongoDB owner is not an ancestor')


def state(root, owner):
    directory = location(root, owner)
    if not (directory / 'port').is_file():
        raise RuntimeError('shared MongoDB fixture is not ready for this CTest owner')
    value = json.loads((directory / 'state.json').read_text())
    if value['owner_pid'] != owner or identity(owner) != value['owner_birth']:
        raise RuntimeError('shared MongoDB fixture owner no longer matches')
    os.kill(value['guardian_pid'], 0)
    os.kill(value['server_pid'], 0)
    return value


def setup(args):
    directory = location(args.root, args.owner)
    directory.parent.mkdir(parents=True, exist_ok=True)
    try:
        directory.mkdir()
    except FileExistsError:
        # CTest --repeat repeats setup too; reuse only this live, ready owner.
        require_ancestor(args.owner)
        value = state(args.root, args.owner)
        subprocess.run([str(args.initializer), f'127.0.0.1:{value["port"]}', '30000'],
                       check=True, timeout=30)
        return
    birth = identity(args.owner)
    if birth is None:
        directory.rmdir()
        raise RuntimeError('CTest owner is not alive')
    error_file = Path(str(directory) + '.error')
    error_file.unlink(missing_ok=True)
    with Path(str(directory) + '.log').open('w') as log:
        child = subprocess.Popen([
            sys.executable, str(Path(__file__).resolve()), 'daemon', str(args.root),
            str(args.source), str(args.initializer), '--owner', str(args.owner), '--birth', birth],
            stdin=subprocess.DEVNULL, stdout=log, stderr=log, start_new_session=True)
        end = time.monotonic() + 32
        try:
            while time.monotonic() < end:
                if (directory / 'port').exists():
                    state(args.root, args.owner)
                    return
                if child.poll() is not None:
                    raise RuntimeError(error_file.read_text() if error_file.exists() else 'MongoDB fixture guardian failed')
                time.sleep(0.02)
            raise RuntimeError('MongoDB fixture preparation deadline exceeded')
        except BaseException:
            # Guardian cleanup may need both leaf-process TERM/KILL budgets.
            stop(child, timeout=5)
            raise


def cleanup(args):
    directory = location(args.root, args.owner)
    if not directory.exists():
        return
    value = state(args.root, args.owner)
    with socket.create_connection(('127.0.0.1', value['control_port']), timeout=4) as control:
        control.sendall(f'STOP {value["token"]}\n'.encode())
        if control.recv(128) != b'stopped\n':
            raise RuntimeError('shared MongoDB cleanup did not finish')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['setup', 'cleanup', 'member', 'status', 'daemon'])
    parser.add_argument('root', type=Path)
    parser.add_argument('source', nargs='?', type=Path)
    parser.add_argument('initializer', nargs='?', type=Path)
    parser.add_argument('--owner', type=int, default=os.getppid())
    parser.add_argument('--birth')
    args = parser.parse_args()
    if args.action in ('setup', 'daemon') and (args.source is None or args.initializer is None):
        parser.error('setup requires source and initializer')
    try:
        if args.action == 'setup': setup(args)
        elif args.action == 'daemon': daemon(args)
        elif args.action == 'cleanup': cleanup(args)
        else:
            if args.action == 'member':
                require_ancestor(args.owner)
            value = state(args.root, args.owner)
            print(value['port'] if args.action == 'member' else json.dumps(value))
    except Exception as error:
        print(str(error), file=sys.stderr)
        return 1
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
