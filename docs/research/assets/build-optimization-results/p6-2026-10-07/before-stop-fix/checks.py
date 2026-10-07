#!/usr/bin/env python3
import hashlib,json,shlex,subprocess,sys
from pathlib import Path
import run
ROOT=run.ROOT

def require(row):
    if row['exit']:raise SystemExit('check failed: '+row['name'])

def stable():
    b=run.bench('final',mode='ON',out='checks-results')
    before=b.generated_version_headers()
    for i in range(1,4):
        row=b.run('stable-'+str(i),[('configure',b.configure_cmd()),('build',b.build_cmd())])
        after=b.generated_version_headers()
        row['generated_version_headers_changed']=[p for p in set(before)|set(after) if before.get(p)!=after.get(p)]
        b.save();require(row)
        if row['compile_count'] or row['link_count'] or row['generated_version_headers_changed']:
            raise SystemExit('non-stable input: '+row['name'])
        before=after
    run.save_env(b,'stable')

def recovery():
    b=run.bench('final',mode='ON',out='checks-results')
    paths=[
      'third_party/sodium/install/lib/libsodium.a',
      'third_party/sodium/install/include/sodium.h',
      'third_party/sodium/install/include/sodium/crypto_box.h',
      'proto/generated/realmmesh/edge/v1/edge.pb.h',
      'proto/generated/realmmesh/edge/v1/edge.pb.cc',
      'proto/librealm_protocol.a',
      'framework/cluster/CMakeFiles/realm_cluster.dir/src/budget_publisher.cpp.o']
    for i,relative in enumerate(paths,1):
        target=b.build_dir/relative
        if not target.is_file():raise SystemExit('missing precondition: '+str(target))
        row=b.run('recover-'+str(i),[('delete',['cmake','-E','rm',str(target)]),('build',b.build_cmd())],extra={'deleted':relative})
        require(row)
        if not target.is_file():raise SystemExit('not recovered: '+relative)
        row=b.run('recover-'+str(i)+'-noop',[('build',b.build_cmd())]);require(row)
        if row['compile_count'] or row['link_count']:raise SystemExit('recovery not stable: '+relative)
    require(b.run('missingdeps',[('check',['ninja','-C',str(b.build_dir),'-t','missingdeps'])]))
    run.save_env(b,'recovery')

def syntax():
    b=run.bench('final',mode='ON',out='checks-results')
    commands=json.loads((b.build_dir/'compile_commands.json').read_text())
    selected=[c for c in commands if any(str(c['file']).endswith(x) for x in [
        'config_headers_test.cpp','gateway_headers_test.cpp','training_rule.cpp','startup_topology_lua.cpp',
        'gateway_config_lua.cpp','gateway_primary_transport.cpp','realm_sessions.cpp','main.cpp']) and '/deps-' not in c['file']]
    # Filter main.cpp to the actual service executable.
    selected=[c for c in selected if not c['file'].endswith('main.cpp') or '/apps/mesh_host/' in c['file']]
    steps=[]
    for c in selected:
        argv=shlex.split(c['command']);index=argv.index('-o');del argv[index:index+2];argv.remove('-c')
        argv+=['-fsyntax-only']
        steps.append(('syntax',argv))
    row=b.run('syntax',steps,extra={'syntax_sources':[c['file'] for c in selected]});require(row)
    run.save_env(b,'syntax')

def full():
    b=run.bench('final',mode='ON',out='checks-results')
    row=b.run('final-full',[('full',['scripts/build.sh','--preset','dev','--jobs',str(run.JOBS)])]);require(row)
    if row.get('tests_total')!=666 or row.get('tests_failed')!=0:raise SystemExit('unexpected current full inventory')
    run.save_env(b,'final-full')

def fallback():
    b=run.bench('final',mode='OFF',out='fallback-results');b.args.preset='dev-make'
    b.build_dir=run.measure.preset_binary_dir(b.source,'dev-make');b.fresh_build_dir()
    require(b.run('fallback-full',[b.prepare_step(),('configure',b.configure_cmd()),('full',['scripts/build.sh','--preset','dev-make','--jobs',str(run.JOBS)])]))
    run.save_env(b,'fallback-full')

if __name__=='__main__':
    for phase in sys.argv[1:] or ['stable','recovery','syntax','full','fallback']:
        globals()[phase]()
