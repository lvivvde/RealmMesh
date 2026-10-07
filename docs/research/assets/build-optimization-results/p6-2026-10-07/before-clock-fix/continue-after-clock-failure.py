#!/usr/bin/env python3
"""Continue correctness evidence while preserving the VM clock stability failures."""
import datetime,json,os,shutil,subprocess,sys
from pathlib import Path
import run,checks
ROOT=run.ROOT
b=run.bench('final',mode='ON',out='checks-results')
# The first stable and recovery pair already exist. Never overwrite their evidence.
assert [r['name'] for r in b.rows]==['stable-1','recover-1','recover-1-noop']
paths=['build.ninja','CMakeFiles/rules.ninja','.ninja_log']
for i in range(2,4):
    before=b.generated_version_headers()
    for relative in paths:
        target=b.build_dir/relative
        if target.exists():(b.out/('stable-'+str(i)+'-before-'+relative.replace('/','_'))).write_bytes(target.read_bytes())
    row=b.run('stable-'+str(i),[('configure',b.configure_cmd()),('build',b.build_cmd())]);checks.require(row)
    for relative in paths:
        target=b.build_dir/relative
        if target.exists():(b.out/('stable-'+str(i)+'-after-'+relative.replace('/','_'))).write_bytes(target.read_bytes())
    after=b.generated_version_headers()
    row['generated_version_headers_changed']=[p for p in set(before)|set(after) if before.get(p)!=after.get(p)]
    b.save()
for i,relative in enumerate([
    'third_party/sodium/install/include/sodium.h',
    'third_party/sodium/install/include/sodium/crypto_box.h',
    'proto/generated/realmmesh/edge/v1/edge.pb.h',
    'proto/generated/realmmesh/edge/v1/edge.pb.cc',
    'proto/librealm_protocol.a',
    'framework/cluster/CMakeFiles/realm_cluster.dir/src/budget_publisher.cpp.o'],2):
    target=b.build_dir/relative
    assert target.is_file(),relative
    row=b.run('recover-'+str(i),[('delete',['cmake','-E','rm',str(target)]),('build',b.build_cmd())],extra={'deleted':relative})
    checks.require(row);assert target.is_file(),relative
    checks.require(b.run('recover-'+str(i)+'-noop',[('build',b.build_cmd())]))
checks.require(b.run('missingdeps',[('check',['ninja','-C',str(b.build_dir),'-t','missingdeps'])]))
run.save_env(b,'clock-failure-continuation')
issues=[{'sample':r['name'],'compile_count':r['compile_count'],'link_count':r['link_count'],'generated_version_headers_changed':r.get('generated_version_headers_changed',[])} for r in b.rows if (r['name'].startswith('stable-') or r['name'].endswith('-noop')) and (r['compile_count'] or r['link_count'] or r.get('generated_version_headers_changed'))]
(ROOT/'stability-issues.json').write_text(json.dumps({'gate_passed':not issues,'issues':issues,'clock_diagnostic':'clock-diagnostic/clock-samples.json','note':'VM clock steps backwards; failures retained without normalizing file timestamps or changing global time services.'},indent=2)+'\n')
checks.syntax();checks.full();checks.fallback()
def phase(name,argv,cwd=ROOT,env=None):
    print(datetime.datetime.now(datetime.timezone.utc).isoformat(),name,'start',flush=True)
    with (ROOT/(name+'-run.log')).open('w') as output:
        result=subprocess.run(argv,cwd=cwd,env=env,stdout=output,stderr=subprocess.STDOUT)
    print(name,'exit',result.returncode,flush=True)
    if result.returncode:raise SystemExit(result.returncode)
# Reuse the original finisher's acceptance and cold-complete commands verbatim.
source=(ROOT/'finish-bench.py').read_text()
remaining=source[source.index("revision=json.loads"):source.index("phase('audit'")]
exec(compile(remaining,str(ROOT/'finish-bench.py'),'exec'),{'ROOT':ROOT,'sys':sys,'os':os,'json':json,'shutil':shutil,'subprocess':subprocess,'phase':lambda name,argv,cwd=ROOT,env=None: phase(name,argv,cwd,env)})
(ROOT/'completed.json').write_text(json.dumps({'platform':'lima','revision':run.REFS['final'],'completed_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'stability_gate_passed':not issues,'retained_blocker':'VM clock backward steps; see stability-issues.json'},indent=2)+'\n')
print('Remaining frozen evidence completed; stability blocker retained',flush=True)
