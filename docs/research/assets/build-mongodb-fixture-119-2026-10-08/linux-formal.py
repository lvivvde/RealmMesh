#!/usr/bin/env python3
import json,subprocess,sys
from pathlib import Path
root=Path(__file__).resolve().parent
preflight=['python3',str(root/'clock-monitor.py'),str(root/'environment-clock/preflight'),'--duration','45','--frequency-reader',str(root/'clock-frequency'),'--probe-mtime']
with (root/'linux-clock-preflight.stdout').open('w') as log:subprocess.run(preflight,stdout=log,stderr=subprocess.STDOUT,check=True)
x=json.loads((root/'environment-clock/preflight/summary.json').read_text());assert not x['backward_steps'] and not x['wall_steps_over_50ms']
assert all('frequency_before_ppm=0.000000000 tick_before_us=10000' in v['reading'] for v in x['frequency_readings'])
print('CLOCK PREFLIGHT PASS',flush=True)
with (root/'linux-clock-monitor.stdout').open('w') as log:
 monitor=subprocess.Popen(['python3',str(root/'clock-monitor.py'),str(root/'environment-clock/monitor'),'--frequency-reader',str(root/'clock-frequency')],stdout=log,stderr=subprocess.STDOUT)
 try:result=subprocess.run(['python3',str(root/'paired-run.py'),'formal'])
 finally:monitor.terminate();clock_exit=monitor.wait()
assert clock_exit==0
if result.returncode:raise SystemExit(result.returncode)
subprocess.run(['python3',str(root/'collect-current.py'),str(root),str(root/'lima-samples.json'),'lima'],check=True)
