"""Native fixture comparison: identical assertions, isolated cache per case."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import sys
import time

parser = argparse.ArgumentParser()
parser.add_argument('--source', required=True)
parser.add_argument('--out', required=True)
args = parser.parse_args()
here = Path(__file__).resolve().parent
out = Path(args.out).resolve()
out.mkdir(parents=True, exist_ok=True)
source = Path(args.source).resolve()
tool = shutil.which('ccache')
data = {'platform': platform.platform(), 'python': sys.version,
        'source': str(source), 'scripts': {}, 'versions': {}, 'samples': []}
for name in ('before', 'after'):
    script = here / f'ccache-paired-{name}.py'
    data['scripts'][name] = hashlib.sha256(script.read_bytes()).hexdigest()
for name, command in [('ccache', [tool, '--version']), ('cmake', ['cmake', '--version']),
                      ('ninja', ['ninja', '--version']), ('compiler', ['cc', '--version'])]:
    data['versions'][name] = subprocess.check_output(command, text=True).strip()

def save():
    (out / 'samples.json').write_text(json.dumps(data, indent=2) + '\n')

for index in range(6):
    for side in (('before', 'after') if index % 2 == 0 else ('after', 'before')):
        name = f'{index}-{side}'
        command = [sys.executable, str(here / f'ccache-paired-{side}.py'),
                   '--source', str(source), '--work', str(out / 'fixtures'),
                   '--native', '--ccache', tool]
        started = time.monotonic()
        result = subprocess.run(command, capture_output=True, text=True)
        elapsed = time.monotonic() - started
        (out / f'{name}.log').write_text(result.stdout + result.stderr)
        data['samples'].append({'pair': index, 'side': side, 'warmup': index == 0,
                                'wall_s': elapsed, 'exit': result.returncode,
                                'log': f'{name}.log'})
        save()
        print(f'{name}: {elapsed:.3f}s exit {result.returncode}', flush=True)
        if result.returncode:
            raise SystemExit(result.returncode)
data['summary'] = {}
for side in ('before', 'after'):
    values = [row['wall_s'] for row in data['samples'] if row['side'] == side and not row['warmup']]
    data['summary'][side] = {'n': len(values), 'median': statistics.median(values),
                             'min': min(values), 'max': max(values)}
data['summary']['seconds_saved'] = (data['summary']['before']['median'] -
                                    data['summary']['after']['median'])
data['summary']['faster_pairs'] = sum(
    next(r['wall_s'] for r in data['samples'] if r['pair'] == i and r['side'] == 'after') <
    next(r['wall_s'] for r in data['samples'] if r['pair'] == i and r['side'] == 'before')
    for i in range(1, 6))
save()
