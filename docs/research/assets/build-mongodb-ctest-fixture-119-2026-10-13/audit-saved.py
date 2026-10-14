#!/usr/bin/env python3
"""只读复核已交付的原始文件、完整入口与双重门槛；不启动构建或服务。"""
import hashlib,json,re,statistics,sys
from pathlib import Path
root=Path(sys.argv[1]).resolve();platform=sys.argv[2]
def read(name):return json.loads((root/name).read_text())
def identity(inv):
    return [(t['name'],next(p['value'] for p in t['properties'] if p['name']=='LABELS')) for t in inv]
x=read(platform+'-samples.json');protocol=read('measurement-protocol.json')
for name,sha in protocol['tool_sha256'].items():
    assert hashlib.sha256((root/name).read_bytes()).hexdigest()==sha,name
for name,record in x['raw_manifest'].items():
    p=root/name
    assert p.stat().st_size==record['bytes'] and hashlib.sha256(p.read_bytes()).hexdigest()==record['sha256'],name
samples={};median={}
for side,g in x['groups'].items():
    folder=root/(side+'-results');rows=json.loads((folder/'stages.json').read_text())
    inv=json.loads((folder/'test-inventory.json').read_text())['tests']
    assert inv==g['inventory']
    env=json.loads((folder/'environment.json').read_text());assert env==g['environment']
    assert env['cache_mode']=='OFF' and env['quic_transport_enabled'] is True
    provenance=folder/'environment-capability-provenance.json'
    if provenance.exists():
        correction=json.loads(provenance.read_text())
        unresolved=(folder/'environment-capability-unresolved.json').read_bytes()
        assert hashlib.sha256(unresolved).hexdigest()==correction['original_sha256']
        assert hashlib.sha256((folder/'environment.json').read_bytes()).hexdigest()==correction['derived_sha256']
        original_env=json.loads(unresolved)
        assert original_env['quic_transport_enabled'] is None
        expected_env=dict(original_env)
        expected_env['quic_transport_enabled']=True
        assert env==expected_env
        assert correction['actual_configure_evidence']
        for evidence in correction['actual_configure_evidence']:
            raw=(folder/evidence['log']).read_bytes()
            assert hashlib.sha256(raw).hexdigest()==evidence['sha256']
            markers=[v for v in raw.decode(errors='replace').splitlines() if v.startswith('-- realm_network: QUIC transport')]
            assert markers and markers==evidence['markers'] and all('enabled (MsQuic found)' in v for v in markers)
    assert env['jobs']==(1 if side=='r0' else (8 if platform=='mac' else 2))
    assert env['cmake_cache']['CMAKE_GENERATOR']==('Unix Makefiles' if side=='r0' else 'Ninja')
    selected=sorted([r for r in rows if re.fullmatch(r'hot-full-[0-9]+',r['name'])],key=lambda r:int(r['name'].rsplit('-',1)[1]))
    assert len(selected) in (3,5)
    assert len(selected)==len(g['samples']), (side, 'summary sample count differs')
    for original,saved in zip(selected,g['samples']):
        assert all(saved[k]==v for k,v in original.items())
        assert original['exit']==0 and original['tests_failed']==0 and original['tests_total']==len(inv)
        assert original['compile_count']==original['link_count']==0
        assert [v['label'] for v in original['steps']]==['configure','build','test']
        assert all(v['exit']==0 for v in original['steps'])
        log=(folder/original['log']).read_text()
        assert re.search(r'100% tests passed(?:, 0 tests failed)? out of '+str(len(inv))+r'\b',log)
        assert '***Skipped' not in log and '***Not Run' not in log
        assert not re.search(r'Linking (?:C|CXX) static library',log)
        assert not re.search(r'^\s+CC(?:AS)?\s+.+\.lo$',log,re.M)
        assert len(re.findall(r'^\s*\d+/\d+ Test\s+#\d+:.+Passed',log,re.M))==len(inv)
        mem=original['memory'];assert mem['min_available_mib']>=1024 and mem['swap_growth_mib']==0
        assert mem['max_pressure_level']==1 if platform=='mac' else mem['oom_kills']==0
    assert [r['name'] for r in selected]==['hot-full-'+str(i) for i in range(1,len(selected)+1)]
    samples[side]=selected;median[side]=statistics.median(v['wall_s'] for v in selected)
    assert median[side]==x['stats'][side]['median']
assert identity(x['groups']['before']['inventory'])==identity(x['groups']['after']['inventory'])
assert all(not v for v in x['source_mismatches'].values())
n=len(samples['after']);assert all(len(v)==n for v in samples.values())
noisy=any((max(v['wall_s'] for v in rows[:3])-min(v['wall_s'] for v in rows[:3]))/statistics.median(v['wall_s'] for v in rows[:3])>.05 for rows in samples.values())
assert n==(5 if noisy else 3)
faster=sum(a['wall_s']<b['wall_s'] for a,b in zip(samples['after'],samples['before']))
gain=1-median['after']/median['before'];cap=1.05*median['r0']
assert gain>0 and faster>n/2 and median['after']<=cap
assert abs(gain-x['current_gain']['reduction'])<1e-12 and cap==x['r0_cap']['limit_s']
if platform=='lima':
    for phase in ('preflight','monitor'):
        clock=read('environment-clock/'+phase+'/summary.json')
        assert clock==x['clock'][phase]
        assert not clock['backward_steps'] and not clock['wall_steps_over_50ms']
        assert all('frequency_before_ppm=0.000000000 tick_before_us=10000' in v['reading'] for v in clock['frequency_readings'])
assert x['passed'] and x['resource_passed']
print(json.dumps({'platform':platform,'complete_groups':n,'median_s':median,'gain':gain,'faster_pairs':faster,'cap_s':cap,'headroom_s':cap-median['after'],'raw_files_verified':len(x['raw_manifest']),'passed':True},indent=2))
