#!/usr/bin/env python3
"""Run remaining verification sequentially in the owned frozen benchmark root."""
import argparse,datetime,hashlib,json,os,shutil,subprocess,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parent
args=argparse.ArgumentParser();args.add_argument('--wait-pid',type=int);options=args.parse_args()

def phase(name,argv,cwd=ROOT,env=None):
    print(datetime.datetime.now(datetime.timezone.utc).isoformat(),name,'start',flush=True)
    with (ROOT/(name+'-run.log')).open('w') as output:
        result=subprocess.run(argv,cwd=cwd,env=env,stdout=output,stderr=subprocess.STDOUT)
    print(datetime.datetime.now(datetime.timezone.utc).isoformat(),name,'exit',result.returncode,flush=True)
    if result.returncode:raise SystemExit(result.returncode)

if options.wait_pid:
    while True:
        try:os.kill(options.wait_pid,0)
        except ProcessLookupError:break
        time.sleep(5)
for side,count in [('r0',42),('final',42),('cache',7)]:
    values=json.loads((ROOT/(side+'-results')/'stages.json').read_text())
    if len(values)!=count or any(row['exit'] for row in values):raise SystemExit('primary evidence incomplete: '+side)
phase('verification',[sys.executable,'checks.py','stable','recovery','syntax','full','fallback'])
revision=json.loads((ROOT/'revisions.json').read_text())['final']
metadata=ROOT/'git-metadata'
subprocess.run(['git','clone','--no-checkout',str(ROOT/'product.bundle'),str(metadata)],check=True)
shutil.move(str(metadata/'.git'),str(ROOT/'final/.git'))
metadata.rmdir()
subprocess.run(['git','read-tree',revision],cwd=ROOT/'final',check=True)
subprocess.run(['git','update-ref','--no-deref','HEAD',revision],cwd=ROOT/'final',check=True)
status=subprocess.check_output(['git','status','--porcelain'],cwd=ROOT/'final',text=True)
(ROOT/'acceptance-git-status.txt').write_text(status)
if status.strip()!='?? .tools':raise SystemExit('unexpected acceptance source status: '+status)
platform='mac' if sys.platform=='darwin' else 'lima'
env=os.environ.copy()
if platform=='lima':
    env['TMPDIR']=str(ROOT/'tmp');env['REALMMESH_LINUX_ACCEPTANCE_SKIP_BUILD']='1'
script='scripts/run-'+('macos' if platform=='mac' else 'linux')+'-login-acceptance.sh'
phase(platform+'-acceptance',['bash',script,'--preset','dev','--jobs','8' if platform=='mac' else '2'],cwd=ROOT/'final',env=env)
shutil.copytree(ROOT/'final/build/dev-ninja/acceptance',ROOT/('mac-acceptance' if platform=='mac' else 'linux-acceptance'))
phase('cold-complete',[sys.executable,'cold-complete.py'])
phase('collect',[sys.executable,'collect.py',str(ROOT),str(ROOT/(platform+'-samples.json')),platform])
phase('audit',[sys.executable,'audit.py',str(ROOT/(platform+'-samples.json'))])
(ROOT/'completed.json').write_text(json.dumps({'platform':platform,'revision':revision,'completed_utc':datetime.datetime.now(datetime.timezone.utc).isoformat()},indent=2)+'\n')
print('All frozen phases completed',flush=True)
