#!/usr/bin/env python3
"""Emit a conservative native compiler cache bucket for GitHub Actions."""
import datetime
import hashlib
import os
from pathlib import Path
import platform
import subprocess


def command(*args, input=None):
    return subprocess.check_output(args, input=input, stderr=subprocess.STDOUT)


def cache_bucket(root):
    digest = hashlib.sha256()

    def add(name, value):
        digest.update(name.encode() + b'\0' + value + b'\0')

    add('policy', b'RealmMesh-ccache-v1-Debug-native-strict')
    add('workspace', str(root.resolve()).encode())  # no unchecked cross-path reuse
    add('platform', f'{platform.system()}-{platform.machine()}'.encode())
    for compiler in ('cc', 'c++'):
        path = Path(command('/usr/bin/which', compiler).decode().strip()).resolve()
        add(compiler, path.read_bytes())
        add(compiler + '-version', command(compiler, '--version'))
        add(compiler + '-target', command(compiler, '-dumpmachine'))
    # Includes the actual standard-library ABI and platform/compiler macros.
    add('stdlib', command('c++', '-dM', '-E', '-x', 'c++', '-include', 'string', '-', input=b''))
    if platform.system() == 'Darwin':
        add('sdk', command('xcrun', '--show-sdk-path') + command('xcrun', '--show-sdk-version'))
    else:
        add('sysroot', command('c++', '-print-sysroot'))
        add('os', Path('/etc/os-release').read_bytes())
    for tool in ('ccache', 'cmake', 'ninja'):
        add(tool, command(tool, '--version'))
    # Fixed dependency identities/options/patches, native flags and protocol inputs.
    paths = {root / 'CMakePresets.json', root / 'CMakeLists.txt'}
    paths.update((root / 'scripts').glob('install-*.sh'))
    for directory in ('third_party', 'cmake', 'proto', 'framework', 'game', 'apps', 'tests'):
        for path in (root / directory).rglob('*'):
            if path.is_file() and (path.name == 'CMakeLists.txt' or path.suffix in ('.cmake', '.patch', '.proto')):
                paths.add(path)
    for path in sorted(paths):
        add(str(path.relative_to(root)), path.read_bytes())
    return f'ccache-v1-{platform.system()}-{platform.machine()}-{digest.hexdigest()}'


if __name__ == '__main__':
    root = Path(__file__).resolve().parent.parent
    bucket = cache_bucket(root)
    # One immutable successful snapshot per compatible bucket/week, rather than
    # one 2 GiB archive per commit/run. GitHub evicts unused snapshots normally.
    week = datetime.datetime.now(datetime.timezone.utc).strftime('%G-W%V')
    lines = f'bucket={bucket}\nkey={bucket}-{week}\n'
    print(lines, end='')
    if os.environ.get('GITHUB_OUTPUT'):
        with open(os.environ['GITHUB_OUTPUT'], 'a') as output:
            output.write(lines)
