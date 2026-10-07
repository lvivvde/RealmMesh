"""Diagnostic subprocess/sleep timings without substituting CMake or ccache."""
import argparse
import json
from pathlib import Path
import runpy
import subprocess
import sys
import time

p=argparse.ArgumentParser(); p.add_argument('--source',required=True); p.add_argument('--out',required=True)
a=p.parse_args(); root=Path(a.source).resolve(); out=Path(a.out).resolve(); out.mkdir(parents=True,exist_ok=True)
rows=[]; real_run=subprocess.run; real_sleep=time.sleep
def run(*args,**kwargs):
 start=time.monotonic()
 try:
  result=real_run(*args,**kwargs)
  return result
 finally:
  command=args[0] if args else kwargs.get('args')
  rows.append(dict(kind='process',args=command,wall_s=time.monotonic()-start))
def sleep(seconds):
 start=time.monotonic()
 real_sleep(seconds)
 rows.append(dict(kind='sleep',requested_s=seconds,wall_s=time.monotonic()-start))
subprocess.run=run; time.sleep=sleep
sys.argv=[str(root/'tests/scripts/ccache_test.py'),'--source',str(root),'--work',str(out/'fixtures'),'--native']
start=time.monotonic()
try:
 runpy.run_path(sys.argv[0],run_name='__main__')
finally:
 (out/'native-python-events.json').write_text(json.dumps(dict(wall_s=time.monotonic()-start,events=rows),indent=2)+'\n')
