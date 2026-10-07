"""Bounded diagnostics of the other three build contracts, unchanged assertions."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

p=argparse.ArgumentParser()
for key in ('source','out','probe-bin','binary-dir'): p.add_argument('--'+key,required=True)
a=p.parse_args(); root=Path(a.source).resolve(); out=Path(a.out).resolve(); out.mkdir(parents=True,exist_ok=True)
real=shutil.which('cmake'); real_ctest=shutil.which('ctest'); samples=[]
for side in ('baseline','profile'):
 for suite in ('build-dir','build-graph','dependency-isolation'):
  stem=side+'-'+suite; tool=real if side=='baseline' else str(Path(a.probe_bin)/'cmake')
  env=os.environ.copy()
  if side=='profile': env.update(REALMMESH_PROBE_LOG=str(out/(stem+'.jsonl')),REALMMESH_PROBE_REAL_CMAKE=real,REALMMESH_PROBE_REAL_CTEST=real_ctest)
  if suite=='build-dir': cmd=['bash',str(root/'tests/scripts/build_dir_info_test.sh'),str(root),tool]
  else:
   script='build_graph_test.cmake' if suite=='build-graph' else 'dependency_isolation_test.cmake'
   cmd=[real,'-DREALM_MESH_TEST_CMAKE_COMMAND='+tool,'-DREALM_MESH_TEST_SOURCE_DIR='+str(root),'-DREALM_MESH_TEST_WORK_DIR='+str(out/(stem+'-fixture'))]
   if suite=='build-graph': cmd+=['-DREALM_MESH_TEST_BINARY_DIR='+a.binary_dir,'-DREALM_MESH_TEST_GENERATOR=Ninja']
   cmd+=['-P',str(root/'tests/cmake'/script)]
  start=time.monotonic()
  with (out/(stem+'.log')).open('w') as log: r=subprocess.run(cmd,env=env,stdout=log,stderr=subprocess.STDOUT)
  row=dict(side=side,suite=suite,command=cmd,wall_s=time.monotonic()-start,exit=r.returncode)
  samples.append(row); (out/'other-samples.json').write_text(json.dumps(samples,indent=2)+'\n')
  print(json.dumps(row),flush=True)
  if r.returncode: raise SystemExit(r.returncode)
