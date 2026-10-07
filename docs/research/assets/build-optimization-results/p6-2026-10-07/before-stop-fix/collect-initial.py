#!/usr/bin/env python3
import hashlib,json,re,statistics,sys
from pathlib import Path
ROOT=Path(sys.argv[1]);DEST=Path(sys.argv[2]);PLATFORM=sys.argv[3]

def norm(value):
    if isinstance(value,str):
        for src,dst in [(str(ROOT),'<bench>'),('/private/tmp/realmmesh-p5/deps-src','<prepared-deps>'),('/home/edwin.guest/code/.bench/p5-20261006/deps-src','<prepared-deps>'),('/Users/edwin/Projects/RealmMesh','<checkout>'),('/home/edwin.guest/code/RealmMesh','<checkout>')]:value=value.replace(src,dst)
        return value
    if isinstance(value,list):return [norm(x) for x in value]
    if isinstance(value,dict):return {norm(k):norm(v) for k,v in value.items()}
    return value

def rows(directory):
    p=directory/'stages.json'
    if not p.exists():return []
    values=json.loads(p.read_text())
    for row in values:
        log=(directory/row['log']).read_text(errors='replace')
        row['sodium_compile_requests']=len(re.findall(r'^\s+CC\s+.+\.lo$',log,re.M))
        row['sodium_assembly_requests']=len(re.findall(r'^\s+CCAS\s+.+\.lo$',log,re.M))
        row['archive_count']=len(re.findall(r'Linking (?:C|CXX) static library',log))
        row['skipped_log_lines']=[x for x in log.splitlines() if '***Skipped' in x or '***Not Run' in x]
        a=row.get('ccache_before') or {};b=row.get('ccache_after') or {}
        row['cache_delta']=({k:b.get(k,0)-a.get(k,0) for k in set(a)|set(b)} if row.get('ccache_before') is not None and row.get('ccache_after') is not None else None)
        row['log_sha256']=hashlib.sha256((directory/row['log']).read_bytes()).hexdigest()
    return values

def stats(a):return {'n':len(a),'values':a,'median':statistics.median(a),'min':min(a),'max':max(a)}

def metric(row,kind):
    if kind in ['cold-build','lua']:
        return next(x['wall_s'] for x in row['steps'] if x['label']=='build')
    return row['wall_s']

data={'platform':PLATFORM,'revisions':json.loads((ROOT/'revisions.json').read_text()),'groups':{}}
for side in ['r0','final','cache','checks','fallback']:
    directory=ROOT/(side+'-results')
    if not directory.exists():continue
    data['groups'][side]={'rows':rows(directory),'environment':json.loads((directory/'environment.json').read_text()) if (directory/'environment.json').exists() else []}
    inventory=directory/'test-inventory.json'
    if inventory.exists():data['groups'][side]['inventory']=json.loads(inventory.read_text())

data['measurement_tool_revision']=(ROOT/'tool-revision.txt').read_text().strip()
data['comparisons']={}
for kind,count in [('cold-build',3),('unit',5),('cpp',5),('lua',5),('hot-full',3)]:
    sides={s:[r for r in data['groups'][s]['rows'] if re.fullmatch(kind+r'-\d+',r['name']) and not r['warmup']] for s in ['r0','final']}
    if not all(len(v)>=count for v in sides.values()):continue
    values={s:[metric(r,kind) for r in sorted(v,key=lambda r:int(r['name'].rsplit('-',1)[-1]))] for s,v in sides.items()}
    pairs=[{'pair':i+1,'before_s':a,'after_s':b,'delta_s':b-a,'reduction':1-b/a} for i,(a,b) in enumerate(zip(values['r0'],values['final']))]
    data['comparisons'][kind]={'before':stats(values['r0']),'after':stats(values['final']),'pairs':pairs,'reduction':1-statistics.median(values['final'])/statistics.median(values['r0']),'faster_pairs':sum(x['after_s']<x['before_s'] for x in pairs)}

if 'cache' in data['groups']:
    modes={m:[r for r in data['groups']['cache']['rows'] if re.fullmatch('cache-'+m+r'-\d+',r['name'])] for m in ['OFF','ON']}
    if all(len(v)==3 for v in modes.values()):
        v={m:[r['wall_s'] for r in sorted(a,key=lambda r:r['name'])] for m,a in modes.items()}
        data['cache_comparison']={'OFF':stats(v['OFF']),'ON':stats(v['ON']),'reduction':1-statistics.median(v['ON'])/statistics.median(v['OFF']),'pairs':[{'pair':i+1,'off_s':a,'on_s':b,'reduction':1-b/a} for i,(a,b) in enumerate(zip(v['OFF'],v['ON']))]}

for side in ['r0','final']:
    data[side+'_source_manifest']=json.loads((ROOT/(side+'-source-manifest.json')).read_text())
    data[side+'_source_hash_mismatches']=[name for name,digest in data[side+'_source_manifest'].items() if hashlib.sha256((ROOT/side/name).read_bytes()).hexdigest()!=digest]

data['test_scope']={}
if all('inventory' in data['groups'].get(s,{}) for s in ['r0','final']):
    old={t['name'] for t in data['groups']['r0']['inventory']['tests']}
    new={t['name'] for t in data['groups']['final']['inventory']['tests']}
    added=new-old
    data['test_scope']={'common_count':len(old&new),'removed':sorted(old-new),'added':sorted(added),'hot_decomposition':[]}
    pattern=re.compile(r'^\s*\d+/\d+\s+Test\s+#\d+:\s*(.*?)\s+\.{2,}\s+Passed\s+([0-9.]+)\s+sec',re.M)
    for side in ['r0','final']:
        for row in data['groups'][side]['rows']:
            if row['name'].startswith('hot-full-'):
                log=(ROOT/(side+'-results')/row['log']).read_text()
                duration={n:float(t) for n,t in pattern.findall(log)}
                data['test_scope']['hot_decomposition'].append({'side':side,'sample':row['name'],'parsed_tests':len(duration),'common_sum_s':sum(t for n,t in duration.items() if n in old),'added_sum_s':sum(t for n,t in duration.items() if n in added),'added_tests':{n:duration[n] for n in sorted(added) if n in duration},'note':'CTest 日志单例时间舍入到0.01秒；只用于成本分解，不合成主计时或宣称同合集通过。'})
    data['test_scope']['strict_full_collection_comparable']=old==new

cache_keys={'CMAKE_BUILD_TYPE','CMAKE_C_COMPILER','CMAKE_CXX_COMPILER','CMAKE_C_FLAGS','CMAKE_CXX_FLAGS','CMAKE_C_FLAGS_DEBUG','CMAKE_CXX_FLAGS_DEBUG','CMAKE_OSX_ARCHITECTURES','CMAKE_OSX_SYSROOT','CMAKE_OSX_DEPLOYMENT_TARGET','CMAKE_GENERATOR','REALMMESH_CCACHE','REALMMESH_CCACHE_DIR','REALMMESH_CCACHE_MAX_SIZE','REALMMESH_CCACHE_RESOLVED_EXECUTABLE','MSQUIC_LIBRARY','OPENSSL_INCLUDE_DIR','OPENSSL_SSL_LIBRARY','OPENSSL_CRYPTO_LIBRARY','BUILD_VERSION'}
data['end_cmake_caches']={}
for side in ['r0','final']:
    for cache in (ROOT/side/'build').glob('*/CMakeCache.txt'):
        values={}
        for line in cache.read_text().splitlines():
            match=re.match(r'^([^:#]+):[^=]+=(.*)$',line)
            if match and match[1] in cache_keys:values[match[1]]=match[2]
        data['end_cmake_caches'][str(cache.relative_to(ROOT))]=values

data['pilot']={}
for side in ['r0','final']:
    d=ROOT/'pilot-before-tool-fix'/(side+'-results')
    if d.exists():data['pilot'][side]={'rows':rows(d),'note':'工具修复前试测及切换中断，不计正式组；不完整阶段的原始文件仍保留。'}

allrows=[row for group in data['groups'].values() for row in group['rows']]
mem=[r['memory'] for r in allrows if r.get('memory')]
data['resources']={'max_tree_rss_mib':max(x['max_tree_rss_mib'] for x in mem),'min_available_mib':min(x['min_available_mib'] for x in mem),'max_pressure_level':max((x['max_pressure_level'] for x in mem if x['max_pressure_level'] is not None),default=None),'max_swap_growth_mib':max((x['swap_growth_mib'] for x in mem if x['swap_growth_mib'] is not None),default=None),'oom_kills':sum(x['oom_kills'] for x in mem if x['oom_kills'] is not None),'nonzero_rows':[r['name'] for r in allrows if r['exit']]}
# Preserve a complete checksum catalogue; raw outputs stay in the owned directory.
raw={}
for directory in [ROOT/(s+'-results') for s in ['r0','final','cache','checks','fallback']]+[ROOT/'pilot-before-tool-fix']:
    if directory.exists():
        for p in directory.rglob('*'):
            if p.is_file():raw[str(p.relative_to(ROOT))]={'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'bytes':p.stat().st_size}
data['raw_manifest']=raw
DEST.parent.mkdir(parents=True,exist_ok=True);DEST.write_text(json.dumps(norm(data),ensure_ascii=False,indent=1)+'\n')
print(json.dumps({'comparisons':data['comparisons'],'cache':data.get('cache_comparison'),'resources':data['resources']},ensure_ascii=False,indent=1))
