#!/usr/bin/env python3
import argparse,json,os,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT/'tool'))
import measure
MAC=sys.platform=='darwin'
R0=Path('/private/tmp/realmmesh-p6-fixed/r0') if MAC else Path('/home/edwin.guest/code/.bench/p6-clock-fixed-20261007/r0')
DEPS=Path('/private/tmp/realmmesh-p5/deps-src') if MAC else Path('/home/edwin.guest/code/.bench/p5-20261006/deps-src')
def bench(side):
 source=R0 if side=='r0' else ROOT/side
 env=[] if MAC else ['TMPDIR='+str(ROOT/'tmp-disk')]
 args=argparse.Namespace(source=str(source),out=str(ROOT/(side+'-results')),launcher=str(ROOT/'launcher'),build_dir=None,deps_dir=str(DEPS),preset='dev',jobs=(1 if side=='r0' else (8 if MAC else 2)),cache_mode='OFF',cmake_arg=(['-DOPENSSL_INCLUDE_DIR=/opt/homebrew/opt/openssl@3/include'] if MAC else []),env=env,name_prefix='',commit=side,label='119 MongoDB '+sys.platform+' '+side,samples=5,sample_start=1,no_warmup=True,long_samples=3,comment_samples=0,probe=[])
 b=measure.Bench(args)
 if side=='r0':b.launcher=str(R0.parent/'launcher')
 b.ctest_cmd=lambda *extra:['ctest','--preset','dev','-j','1',*extra]
 return b

def setup(side):
 b=bench(side)
 (b.build_dir/measure.SODIUM_DOWNLOAD_DIR).mkdir(parents=True,exist_ok=True)
 row=b.run('setup',[b.prepare_step(),('configure',b.configure_cmd()),('build',b.build_cmd())],warmup=True)
 b.require(row,'prepare','configure','build')
 b.scenario_repro()
 inventory=subprocess.check_output(['ctest','--preset','dev','--show-only=json-v1'],cwd=b.source,env=b.env,text=True)
 (b.out/'test-inventory.json').write_text(inventory)
 (b.out/'environment.json').write_text(json.dumps(b.environment(['hot-full']),indent=2)+'\n')
 print('READY '+side,flush=True)

def refresh(side):
 b=bench(side)
 row=b.run('post-review-refresh',[('configure',b.configure_cmd()),('build',b.build_cmd())],warmup=True)
 b.require(row,'configure','build')
 for i in range(1,4):
  row=b.run('post-review-stable-'+str(i),[('configure',b.configure_cmd()),('build',b.build_cmd())],warmup=True)
  b.require(row,'configure','build')
  assert row['compile_count']==row['link_count']==0
 (b.out/'test-inventory.json').write_text(subprocess.check_output(['ctest','--preset','dev','--show-only=json-v1'],cwd=b.source,env=b.env,text=True))
 (b.out/'environment.json').write_text(json.dumps(b.environment(['hot-full']),indent=2)+'\n')

def sample(side,index):
 b=bench(side)
 row=b.run('hot-full-'+str(index),[('configure',b.configure_cmd()),('build',b.build_cmd()),('test',b.ctest_cmd())])
 if row['exit']:raise SystemExit('full correctness failure retained')
 if row['compile_count'] or row['link_count']:raise SystemExit('hot entry unexpectedly compiled; retained')

if __name__=='__main__':
 measure.exit_on_sigterm()
 (ROOT/'tmp-disk').mkdir(exist_ok=True)
 if sys.argv[1]=='setup':setup(sys.argv[2])
 elif sys.argv[1]=='r0-stable':
  b=bench('r0');b.scenario_repro()
  (b.out/'test-inventory.json').write_text(subprocess.check_output(['ctest','--preset','dev','--show-only=json-v1'],cwd=b.source,env=b.env,text=True))
  (b.out/'environment.json').write_text(json.dumps(b.environment(['hot-full']),indent=2)+'\n')
 elif sys.argv[1]=='refresh':refresh(sys.argv[2])
 elif sys.argv[1]=='sample':sample(sys.argv[2],int(sys.argv[3]))
 elif sys.argv[1]=='formal':
  for i,order in enumerate([['r0','before','after'],['after','before','r0'],['before','r0','after']],1):
   for side in order:sample(side,i)
