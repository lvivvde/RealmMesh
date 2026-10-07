"""Isolated Linux correctness build; no performance acceptance claim."""
import os
from pathlib import Path
import shutil
import subprocess
import time
import json

source = Path(__file__).resolve().parent
out = source.parent
previous = Path('/home/edwin.guest/code/.bench/p6-clock-fixed-20261007/final')
deps = Path('/home/edwin.guest/code/.bench/p5-20261006/deps-src')
tools = source / '.tools'
if not tools.exists():
    tools.symlink_to((previous / '.tools').resolve(), target_is_directory=True)
env = os.environ.copy()
for key in list(env):
    if key.startswith('CCACHE_') or key in ('MAKEFLAGS', 'CMAKE_BUILD_PARALLEL_LEVEL', 'CTEST_PARALLEL_LEVEL'):
        env.pop(key)
    if 'MONGODB' in key and ('URI' in key or 'PASSWORD' in key):
        env.pop(key)
(out / 'tmp').mkdir(exist_ok=True)
env['TMPDIR'] = str(out / 'tmp')
download = source / 'build/dev-ninja/third_party/sodium/sodium_external-prefix/src'
download.mkdir(parents=True, exist_ok=True)
shutil.copy2(deps / 'sodium-download/libsodium-1.0.22.tar.gz', download)
overrides = [f'-DFETCHCONTENT_SOURCE_DIR_{p.name[:-4].upper()}={p}'
             for p in sorted(deps.iterdir()) if p.is_dir() and p.name.endswith('-src')]
results = []
for name, cmd in [('configure', ['cmake', '--preset', 'dev', '-DREALMMESH_CCACHE=OFF', *overrides]),
                  ('full', ['./scripts/build.sh', '--jobs', '2'])]:
    started = time.monotonic()
    with (out / f'linux-{name}.log').open('w') as stream:
        result = subprocess.run(cmd, cwd=source, env=env, stdout=stream, stderr=subprocess.STDOUT)
    results.append({'name': name, 'exit': result.returncode, 'wall_s': time.monotonic() - started,
                    'command': cmd})
    (out / 'linux-full.json').write_text(json.dumps(results, indent=2) + '\n')
    print(results[-1], flush=True)
    if result.returncode:
        raise SystemExit(result.returncode)
