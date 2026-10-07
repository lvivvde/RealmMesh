"""Run unchanged real contracts with and without tool timing instrumentation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time

p=argparse.ArgumentParser()
p.add_argument('--source', required=True)
p.add_argument('--out', required=True)
p.add_argument('--probe-bin', required=True)
a=p.parse_args()
source=Path(a.source).resolve(); out=Path(a.out).resolve(); out.mkdir(parents=True, exist_ok=True)
real={n:shutil.which(n) for n in ('cmake','ctest','ccache','ninja')}
data={'source':str(source),'platform':platform.platform(),'source_hashes':{},'tools':real,'samples':[]}
for name in ('tests/scripts/ccache_test.py','tests/scripts/test_fast_test.sh','scripts/test-fast.sh','scripts/test-watch.sh'):
 data['source_hashes'][name]=hashlib.sha256((source/name).read_bytes()).hexdigest()
for side in ('baseline','profile'):
 for suite in ('config','native','fast'):
  stem=side+'-'+suite; events=out/(stem+'.jsonl')
  env=os.environ.copy()
  cmake=real['cmake']
  if side=='profile':
   cmake=str(Path(a.probe_bin)/'cmake')
   env.update(REALMMESH_PROBE_LOG=str(events),REALMMESH_PROBE_REAL_CMAKE=real['cmake'],REALMMESH_PROBE_REAL_CTEST=real['ctest'])
  if suite=='fast':
   command=['bash',str(source/'tests/scripts/test_fast_test.sh'),str(source),cmake]
  else:
   command=[sys.executable,str(source/'tests/scripts/ccache_test.py'),'--source',str(source),'--work',str(out/'fixtures'),'--cmake',cmake]
   if suite=='native': command+=['--native','--ccache',real['ccache']]
  started=time.monotonic()
  with (out/(stem+'.log')).open('w') as log:
   r=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,env=env)
  row=dict(side=side,suite=suite,wall_s=time.monotonic()-started,exit=r.returncode,log=stem+'.log')
  data['samples'].append(row)
  (out/'samples.json').write_text(json.dumps(data,indent=2)+'\n')
  print(json.dumps(row),flush=True)
  if r.returncode: sys.exit(r.returncode)
