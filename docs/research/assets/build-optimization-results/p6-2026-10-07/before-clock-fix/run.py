#!/usr/bin/env python3
"""P6 measurement driver; use only with the owned archived trees beside this file."""
import argparse, hashlib, json, os, subprocess, sys
from pathlib import Path
ROOT=Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT/'tool'))
import measure
MAC=sys.platform=='darwin'
JOBS=8 if MAC else 2
DEPS=Path('/private/tmp/realmmesh-p5/deps-src') if MAC else Path('/home/edwin.guest/code/.bench/p5-20261006/deps-src')
REFS=json.loads((ROOT/'revisions.json').read_text())

def bench(side,jobs=None,mode='OFF',out=None):
    extra=['-DOPENSSL_INCLUDE_DIR=/opt/homebrew/opt/openssl@3/include'] if MAC else []
    args=argparse.Namespace(source=str(ROOT/side),out=str(ROOT/(out or side+'-results')),launcher=str(ROOT/'launcher'),
        build_dir=None,deps_dir=str(DEPS),preset='dev',jobs=jobs or (1 if side=='r0' else JOBS),
        cache_mode=mode,cmake_arg=extra,env=[] if MAC else ['TMPDIR='+str(ROOT/'tmp')],
        name_prefix='',commit=REFS[side]+(' + correctness-only patch' if side=='r0' else ''),
        label='P6 '+sys.platform+' '+side,samples=5,sample_start=1,no_warmup=True,long_samples=3,comment_samples=0,probe=['lua-hpp'])
    b=measure.Bench(args)
    b.ctest_cmd=lambda *extra:['ctest','--preset','dev','-j','1',*extra]
    return b

def save_env(b,scenario):
    p=b.out/'environment.json'; env=json.loads(p.read_text()) if p.exists() else []
    env.append(b.environment([scenario]));p.write_text(json.dumps(env,indent=2)+'\n')

def ordered(i):return ['r0','final'] if i%2 else ['final','r0']

def cold():
    for i in range(1,4):
        for side in ordered(i):
            b=bench(side);b.fresh_build_dir()
            row=b.run('cold-build-'+str(i),[b.prepare_step(),('configure',b.configure_cmd()),('build',b.build_cmd())])
            save_env(b,'cold-build');b.require(row,'prepare','configure','build')
    for side in ['r0','final']:
        b=bench(side);b.scenario_repro();save_env(b,'repro')
        p=b.out/'test-inventory.json'
        data=subprocess.check_output(['ctest','--preset','dev','--show-only=json-v1'],cwd=b.source,env=b.env,text=True)
        p.write_text(data)

def short():
    for scenario in ['unit','cpp','lua']:
        for i in range(0,6):
            for side in ordered(i or 1):
                b=bench(side,jobs=JOBS if scenario=='lua' else None)
                warm=i==0;suffix='warmup' if warm else str(i)
                if scenario=='unit':
                    steps=[('configure',b.configure_cmd()),('build',b.build_cmd()),('test',b.ctest_cmd('-L','^unit$'))] if side=='r0' else [('test-fast',b.fast_cmd())]
                    row=b.run('unit-'+suffix,steps,warmup=warm)
                    b.require(row,'configure','build','test','test-fast')
                else:
                    path=measure.PROBES['lua-cpp' if scenario=='cpp' else 'lua-hpp']
                    prefix=scenario+'-'+suffix
                    steps=(lambda:[('build',b.build_cmd())]) if scenario=='lua' else (
                        (lambda:[('configure',b.configure_cmd()),('build',b.build_cmd()),('test',b.ctest_cmd('-L','^unit$'))]) if side=='r0' else (lambda:[('test-fast',b.fast_cmd())]))
                    plan=[measure.EditSample(prefix,i,warm,'token',prefix+'-reset')]
                    b.run_edits(path,scenario,plan,steps)
                save_env(b,scenario)

def hot(start=1,end=3):
    for i in range(start,end+1):
        for side in ordered(i):
            b=bench(side)
            row=b.run('hot-full-'+str(i),[('configure',b.configure_cmd()),('build',b.build_cmd()),('test',b.ctest_cmd())])
            save_env(b,'hot-full')
            if row['exit']:raise SystemExit('full correctness failure retained; inspect before proceeding')

def cache():
    b=bench('final',mode='ON',out='cache-results');b.fresh_build_dir()
    row=b.run('cache-fill',[b.prepare_step(),('configure',b.configure_cmd()),('build',b.build_cmd())]);save_env(b,'fill');b.require(row,'build')
    for i in range(1,4):
        for mode in (['OFF','ON'] if i%2 else ['ON','OFF']):
            b=bench('final',mode=mode,out='cache-results');b.fresh_build_dir()
            row=b.run('cache-'+mode+'-'+str(i),[b.prepare_step(),('configure',b.configure_cmd()),('build',b.build_cmd())])
            save_env(b,'cache');b.require(row,'prepare','configure','build')

if __name__=='__main__':
    (ROOT/'tmp').mkdir(exist_ok=True)
    measure.exit_on_sigterm()
    for phase in sys.argv[1:] or ['cold','short','hot','cache']:
        globals()[phase]()
