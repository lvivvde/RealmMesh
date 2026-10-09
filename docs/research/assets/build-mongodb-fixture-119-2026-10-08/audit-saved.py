#!/usr/bin/env python3
"""Audit saved #119 full-entry evidence without running a build or original sources."""
import hashlib
import json
import re
import statistics
import sys
from pathlib import Path

root = Path(sys.argv[1]).resolve()
platform = sys.argv[2]
saved = json.loads((root / (platform + '-samples.json')).read_text())
for name, record in saved['raw_manifest'].items():
    path = root / name
    assert path.stat().st_size == record['bytes'], name
    assert hashlib.sha256(path.read_bytes()).hexdigest() == record['sha256'], name
samples = {}
inventories = {}
formal = []
for side in ('r0', 'before', 'after'):
    directory = root / (side + '-results')
    rows = json.loads((directory / 'stages.json').read_text())
    inventory = json.loads((directory / 'test-inventory.json').read_text())['tests']
    inventories[side] = [(t['name'], next(p['value'] for p in t['properties'] if p['name'] == 'LABELS')) for t in inventory]
    samples[side] = sorted((r for r in rows if re.fullmatch(r'hot-full-[0-9]+', r['name'])), key=lambda r: int(r['name'].rsplit('-', 1)[1]))
    assert len(samples[side]) >= 3
    for row in samples[side]:
        log = (directory / row['log']).read_text()
        summaries = re.findall(r'100% tests passed(?:, 0 tests failed)? out of (\d+)', log)
        assert summaries == [str(len(inventory))], (side, row['name'], summaries)
        assert not re.search(r'\*\*\*(?:Skipped|Not Run)', log)
        assert row['exit'] == row['tests_failed'] == row['compile_count'] == row['link_count'] == 0
        assert row['tests_total'] == len(inventory)
        assert not re.search(r'Linking (?:C|CXX) static library|^\s+CC(?:AS)?\s+.+\.lo$', log, re.M)
        assert [s['label'] for s in row['steps']] == ['configure', 'build', 'test']
        assert all(s['exit'] == 0 for s in row['steps'])
        assert not list((directory / row['events']).glob('*.json'))
        formal.append(row)
assert inventories['before'] == inventories['after']

def stats(values):
    return dict(n=len(values), values=values, median=statistics.median(values), min=min(values), max=max(values))

result = {'stats': {side: stats([r['wall_s'] for r in rows]) for side, rows in samples.items()}}
pairs = [{'group': i + 1, 'before_s': a['wall_s'], 'after_s': b['wall_s'], 'saved_s': a['wall_s'] - b['wall_s']} for i, (a, b) in enumerate(zip(samples['before'], samples['after']))]
result['current_gain'] = {'pairs': pairs, 'reduction': 1 - result['stats']['after']['median'] / result['stats']['before']['median'], 'faster_pairs': sum(p['saved_s'] > 0 for p in pairs)}
result['current_gain']['passed'] = result['current_gain']['reduction'] > 0 and result['current_gain']['faster_pairs'] > len(pairs) / 2
limit = result['stats']['r0']['median'] * 1.05
result['r0_cap'] = {'limit_s': limit, 'ratio': result['stats']['after']['median'] / result['stats']['r0']['median'], 'headroom_s': limit - result['stats']['after']['median'], 'passed': result['stats']['after']['median'] <= limit}
mem = [r['memory'] for r in formal]
if platform != 'mac':
    assert all(x['oom_kills'] is not None for x in mem), 'Linux OOM counters missing'
result['resources'] = {'max_tree_rss_mib': max(x['max_tree_rss_mib'] for x in mem), 'min_available_mib': min(x['min_available_mib'] for x in mem), 'max_swap_growth_mib': max(x['swap_growth_mib'] for x in mem), 'max_pressure_level': max((x['max_pressure_level'] for x in mem if x['max_pressure_level'] is not None), default=None), 'oom_kills': sum(x['oom_kills'] for x in mem if x['oom_kills'] is not None) if platform != 'mac' else None}
result['resource_passed'] = result['resources']['min_available_mib'] >= 1024 and result['resources']['max_swap_growth_mib'] == 0 and result['resources']['max_pressure_level'] in (None, 1) and result['resources']['oom_kills'] in (None, 0)
result['passed'] = result['current_gain']['passed'] and result['r0_cap']['passed'] and result['resource_passed']
for field, value in result.items():
    assert saved[field] == value, field
for phase in saved.get('clock', {}):
    clock = json.loads((root / 'environment-clock' / phase / 'summary.json').read_text())
    assert clock == saved['clock'][phase]
    assert not clock['backward_steps'] and not clock['wall_steps_over_50ms']
print(json.dumps({'platform': platform, 'raw_checksums': 'pass', 'full_entries': len(formal), 'source_files_rechecked': False, **result}, indent=2))
