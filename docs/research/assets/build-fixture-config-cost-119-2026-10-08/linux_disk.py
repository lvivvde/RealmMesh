"""Rerun all Linux diagnostics and full correctness on owned disk TMPDIR."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

p=argparse.ArgumentParser(); p.add_argument('--root',required=True);p.add_argument('--full-source',required=True);a=p.parse_args()
root=Path(a.root).resolve();full=Path(a.full_source).resolve();tmp=root/'tmp-disk';tmp.mkdir(exist_ok=True)
env=os.environ.copy();env['TMPDIR']=str(tmp)
for k in list(env):
 if k.startswith('CCACHE_') or k in ('MAKEFLAGS','CMAKE_BUILD_PARALLEL_LEVEL','CTEST_PARALLEL_LEVEL') or ('MONGODB' in k and ('URI' in k or 'PASSWORD' in k)):env.pop(k)
(root/'disk-environment.txt').write_text(subprocess.check_output(['df','-h','/tmp',str(tmp)],text=True))
common=['--source',str(root/'source'),'--out',str(root/'linux-disk-complete')]
commands=[
 [sys.executable,str(root/'diagnose.py'),*common,'--probe-bin',str(root/'bin')],
 [sys.executable,str(root/'native_profile.py'),*common],
 [sys.executable,str(root/'other_contracts.py'),'--source',str(root/'source'),'--out',str(root/'linux-disk-other-current-graph'),'--probe-bin',str(root/'bin'),'--binary-dir',str(full/'build/dev-ninja')],
]
for i,cmd in enumerate(commands):
 with (root/f'linux-disk-driver-{i}.log').open('w') as log:r=subprocess.run(cmd,env=env,stdout=log,stderr=subprocess.STDOUT)
 print('disk diagnostic',i,'exit',r.returncode,flush=True)
 if r.returncode:raise SystemExit(r.returncode)
cmd=['./scripts/build.sh','--jobs','2'];start=time.monotonic()
with (root/'linux-disk-full.log').open('w') as log:r=subprocess.run(cmd,cwd=full,env=env,stdout=log,stderr=subprocess.STDOUT)
row=dict(command=cmd,root=str(full),tmpdir=str(tmp),wall_s=time.monotonic()-start,exit=r.returncode)
(root/'linux-disk-full.json').write_text(json.dumps(row,indent=2)+'\n');print(json.dumps(row),flush=True)
raise SystemExit(r.returncode)
