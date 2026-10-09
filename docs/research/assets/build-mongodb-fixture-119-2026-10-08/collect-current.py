#!/usr/bin/env python3
"""Recompute the two approved #119 gates from complete raw stages."""
import hashlib,json,re,statistics,sys
from pathlib import Path
root=Path(sys.argv[1]).resolve();dest=Path(sys.argv[2]);platform=sys.argv[3]
r0=Path('/private/tmp/realmmesh-p6-fixed/r0') if platform=='mac' else Path('/home/edwin.guest/code/.bench/p6-clock-fixed-20261007/r0')
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def stats(values):return dict(n=len(values),values=values,median=statistics.median(values),min=min(values),max=max(values))
def identity(tests):return [(t['name'],next(p['value'] for p in t['properties'] if p['name']=='LABELS')) for t in tests]
data={'platform':platform,'revisions':json.loads((root/'revisions.json').read_text()),'groups':{},'source_mismatches':{},'raw_manifest':{}}
formal=[]
for side in ('r0','before','after'):
 directory=root/(side+'-results');rows=json.loads((directory/'stages.json').read_text())
 inventory=json.loads((directory/'test-inventory.json').read_text())['tests']
 samples=sorted([row for row in rows if re.fullmatch('hot-full-[0-9]+',row['name'])],key=lambda row:int(row['name'].rsplit('-',1)[1]))
 assert len(samples)>=3
 for row in samples:
  text=(directory/row['log']).read_text()
  row['skipped_log_lines']=[line for line in text.splitlines() if '***Skipped' in line or '***Not Run' in line]
  row['archive_count']=len(re.findall(r'Linking (?:C|CXX) static library',text))
  row['sodium_compile_requests']=len(re.findall(r'^\s+CC(?:AS)?\s+.+\.lo$',text,re.M))
  assert row['exit']==0 and row['tests_failed']==0 and row['tests_total']==len(inventory)
  assert not row['skipped_log_lines']
  assert row['compile_count']==row['link_count']==row['archive_count']==row['sodium_compile_requests']==0
  formal.append(row)
 data['groups'][side]={'rows':rows,'samples':samples,'inventory':inventory,'environment':json.loads((directory/'environment.json').read_text())}
 source=r0 if side=='r0' else root/side
 manifest=json.loads((root/(side+'-source-manifest.json')).read_text())
 data['source_mismatches'][side]=[name for name,sha in manifest.items() if digest(source/name)!=sha]
 assert not data['source_mismatches'][side]
 for p in directory.rglob('*'):
  if p.is_file():data['raw_manifest'][str(p.relative_to(root))]={'sha256':digest(p),'bytes':p.stat().st_size}
before=data['groups']['before'];after=data['groups']['after'];old=data['groups']['r0']
assert identity(before['inventory'])==identity(after['inventory'])
data['same_current_collection']=True
data['collection_counts']={side:len(group['inventory']) for side,group in data['groups'].items()}
oldnames={x['name'] for x in old['inventory']};newnames={x['name'] for x in after['inventory']}
data['r0_collection_difference']={'strict_same_collection':oldnames==newnames,'removed':sorted(oldnames-newnames),'added':sorted(newnames-oldnames)}
data['stats']={side:stats([r['wall_s'] for r in group['samples']]) for side,group in data['groups'].items()}
pairs=[{'group':i+1,'before_s':a['wall_s'],'after_s':b['wall_s'],'saved_s':a['wall_s']-b['wall_s']} for i,(a,b) in enumerate(zip(before['samples'],after['samples']))]
data['current_gain']={'pairs':pairs,'reduction':1-data['stats']['after']['median']/data['stats']['before']['median'],'faster_pairs':sum(p['saved_s']>0 for p in pairs)}
data['current_gain']['passed']=data['current_gain']['reduction']>0 and data['current_gain']['faster_pairs']>len(pairs)/2
limit=data['stats']['r0']['median']*1.05
data['r0_cap']={'limit_s':limit,'ratio':data['stats']['after']['median']/data['stats']['r0']['median'],'headroom_s':limit-data['stats']['after']['median'],'passed':data['stats']['after']['median']<=limit}
mem=[r['memory'] for r in formal]
if platform!='mac':assert all(x['oom_kills'] is not None for x in mem), 'Linux OOM counters missing; cannot validate resources'
data['resources']={'max_tree_rss_mib':max(x['max_tree_rss_mib'] for x in mem),'min_available_mib':min(x['min_available_mib'] for x in mem),'max_swap_growth_mib':max(x['swap_growth_mib'] for x in mem),'max_pressure_level':max((x['max_pressure_level'] for x in mem if x['max_pressure_level'] is not None),default=None),'oom_kills':sum(x['oom_kills'] for x in mem if x['oom_kills'] is not None) if platform!='mac' else None}
clock=root/'environment-clock'
if clock.exists():
 data['clock']={phase:json.loads((clock/phase/'summary.json').read_text()) for phase in ('preflight','monitor')}
 assert all(not x['backward_steps'] and not x['wall_steps_over_50ms'] for x in data['clock'].values())
 data['clock_passed']=True
 for p in clock.rglob('*'):
  if p.is_file():data['raw_manifest'][str(p.relative_to(root))]={'sha256':digest(p),'bytes':p.stat().st_size}
data['resource_passed']=data['resources']['min_available_mib']>=1024 and data['resources']['max_swap_growth_mib']==0 and (data['resources']['max_pressure_level'] in (None,1)) and (data['resources']['oom_kills'] in (None,0))
data['passed']=data['current_gain']['passed'] and data['r0_cap']['passed'] and data['resource_passed']
dest.write_text(json.dumps(data,indent=2)+'\n')
print(json.dumps({k:data[k] for k in ('platform','stats','current_gain','r0_cap','resources','passed')},indent=2))
