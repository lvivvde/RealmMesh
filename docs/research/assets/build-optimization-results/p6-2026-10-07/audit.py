#!/usr/bin/env python3
"""Validate evidence integrity separately from observed acceptance gates."""
import hashlib, json, sys
from pathlib import Path
p=json.loads(Path(sys.argv[1]).read_text())
for kind,n in [('cold-build',3),('unit',5),('cpp',5),('lua',5),('hot-full',3),('cold-complete',3)]:
    c=p['comparisons'][kind]
    assert c['before']['n']==c['after']['n']==len(c['pairs'])==n,kind
for side in ['r0','final']:
    assert not p[side+'_source_hash_mismatches'],side
    rows=p['groups'][side]['rows']
    assert len(rows)==42,(side,len(rows))
    for r in rows:
        assert r['exit']==0 and not r['skipped_log_lines'],(side,r['name'])
        if r['name'].startswith(('unit-','hot-full-')):
            assert r['compile_count']==r['link_count']==r['archive_count']==r['sodium_compile_requests']==r['sodium_assembly_requests']==0,(side,r['name'])
        if r['name'].startswith(('unit-','cpp-')):
            assert r['tests_total']==(496 if side=='r0' else 504) and r['tests_failed']==0,(side,r['name'])
        if r['name'].startswith('hot-full-'):
            assert r['tests_total']==(649 if side=='r0' else 667) and r['tests_failed']==0,(side,r['name'])
for side in ['r0','final']:
    rows=p['groups']['cold-'+side]['rows']
    assert len(rows)==3,(side,len(rows))
    for r in rows:
        assert r['exit']==0 and not r['skipped_log_lines']
        assert r['tests_total']==(649 if side=='r0' else 667) and r['tests_failed']==0
        assert r['compile_count']>700 and r['sodium_compile_requests']>100
for side in ['r0','final']:
    rows=p['groups'][side]['rows']
    for kind in ['cpp','lua']:
        selected=[r for r in rows if r['name'].startswith(kind+'-') and r['name'].rsplit('-',1)[-1].isdigit()]
        expected=1 if kind=='cpp' else (33 if side=='r0' else 10)
        assert len(selected)==5
        for r in selected:
            assert r['compile_count']==expected,(side,r['name'])
            assert sorted(r['compiled_sources'])==sorted(selected[0]['compiled_sources']),(side,r['name'])
checks=p['groups']['checks']['rows']
assert len(checks)==20,len(checks)
for r in checks:
    assert r['exit']==0 and not r['skipped_log_lines'],r['name']
    if r['name'].startswith('stable-'):
        assert r['generated_version_headers_changed']==[],r['name']
assert checks[-1]['tests_total']==667
assert checks[-1]['tests_failed']==0
assert len(next(r for r in checks if r['name']=='syntax')['syntax_sources'])==8
fallback=p['groups']['fallback']['rows']
assert len(fallback)==1 and fallback[0]['exit']==0
assert fallback[0]['tests_total']==667 and fallback[0]['tests_failed']==0 and not fallback[0]['skipped_log_lines']
cache=[r for r in p['groups']['cache']['rows'] if r['name']!='cache-fill']
assert len(cache)==6
for r in cache:
    assert r['exit']==0,r['name']
    assert sorted(r['compiled_sources'])==sorted(cache[0]['compiled_sources']),r['name']
    assert sorted(r['linked_outputs'])==sorted(cache[0]['linked_outputs']),r['name']
    assert r['sodium_compile_requests']==cache[0]['sodium_compile_requests']>0,r['name']
    if r['name'].startswith('cache-ON'):
        assert r['cache_delta']['direct_cache_hit']==r['compile_count'] and r['cache_delta']['cache_miss']==0,r['name']
observed_stability_failures=[r['name'] for r in checks if (r['name'].startswith('stable-') or r['name'].endswith('-noop')) and (r['compile_count'] or r['link_count'] or r['archive_count'] or r['sodium_compile_requests'] or r['sodium_assembly_requests'] or r.get('generated_version_headers_changed'))]
assert [r['sample'] for r in p['stability_failures']]==observed_stability_failures
assert p['stability_gate_passed']==(not observed_stability_failures)
assert p['test_execution_gate_passed'] is True
assert p['current_correctness_gate_passed']==(p['stability_gate_passed'] and (not p.get('clock_environment') or p['clock_environment_gate_passed'] is True))
if observed_stability_failures:
    assert p['platform']=='lima' and p['clock_backward_steps']
    assert all(r['wall_delta_ns']<0 and r['monotonic_delta_ns']>0 for r in p['clock_backward_steps'])
else:assert not p['stability_failures']
res=p['resources']
assert res['nonzero_rows']==[]
assert res['min_available_mib']>=1024
assert res['max_swap_growth_mib']==0
assert res['max_pressure_level'] in (None,1)
assert res['oom_kills'] in (None,0)
assert p['test_scope']['common_count']==649 and p['test_scope']['removed']==[] and len(p['test_scope']['added'])==18
assert all(not x for x in p['unexpected_appledouble_entries'].values())
assert p['test_scope']['strict_full_collection_comparable'] is False
assert p['test_scope']['unit_common_count']==496 and p['test_scope']['unit_removed']==[] and len(p['test_scope']['unit_added'])==8
assert p['quic_configuration_evidence'] and all(x['enabled'] for x in p['quic_configuration_evidence'])
for side in ['r0','final']:
    observed={x['sample'] for x in p['quic_configuration_evidence'] if x['side']==side and x['enabled']}
    assert {'cold-build-1','cold-build-2','cold-build-3'}<=observed,side
raw_checked=None
if len(sys.argv)>2:
    if p.get('clock_environment'):
        assert p['clock_environment_gate_passed'] is True
        preflight=p['clock_environment']['preflight']
        assert preflight and preflight['elapsed_s']>=45 and not preflight['backward_steps'] and not preflight['wall_steps_over_50ms']
    raw_root=Path(sys.argv[2])
    for name,entry in p['raw_manifest'].items():
        path=raw_root/name
        assert path.is_file() and hashlib.sha256(path.read_bytes()).hexdigest()==entry['sha256'],name
    raw_checked=len(p['raw_manifest'])
print(json.dumps({'raw_files_checked':raw_checked,'platform':p['platform'],'evidence_integrity_checks':'passed','clock_environment_gate_passed':p.get('clock_environment_gate_passed'),'stability_gate_passed':p['stability_gate_passed'],'stability_failure_samples':observed_stability_failures,'strict_full_collection_comparable':False,'first_batch_accepted':False,'current_correctness_gate_passed':p['current_correctness_gate_passed']},ensure_ascii=False))
