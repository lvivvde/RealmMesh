#!/usr/bin/env python3
"""Repeat the complete frozen Linux series after preserving its sleep-interrupted epoch."""
import json,os,shutil,subprocess,time
from pathlib import Path
root=Path(__file__).resolve().parent
old=root/'clock-interrupted'
assert not old.exists(), 'do not overwrite an epoch'
old.mkdir()
for name in ('r0-results','before-results','after-results','environment-clock','linux-formal.stdout','linux-clock-preflight.stdout','linux-clock-monitor.stdout'):
 p=root/name
 if p.exists():p.rename(old/name)
for side in ('r0','before','after'):
 dest=root/(side+'-results');dest.mkdir()
 for name in ('compile-observer.cmake','environment.json','test-inventory.json'):
  shutil.copy2(old/(side+'-results')/name,dest/name)
(root/'excluded-epoch.json').write_text(json.dumps({'epoch':'clock-interrupted','excluded_complete_series':True,'reason':'17 realtime vs monotonic discontinuities aligned with host clamshell sleep; last full run exit8 (DevServices failure recovery, M3); no sample chosen by speed','replacement':'new full three groups in unchanged order/source/tools/caches/resources'},indent=2)+'\n')
with (root/'linux-clock-preflight.stdout').open('w') as log:
 subprocess.run(['python3',str(root/'clock-monitor.py'),str(root/'environment-clock/preflight'),'--duration','45','--frequency-reader',str(root/'clock-frequency'),'--probe-mtime'],stdout=log,stderr=subprocess.STDOUT,check=True)
x=json.loads((root/'environment-clock/preflight/summary.json').read_text())
assert not x['backward_steps'] and not x['wall_steps_over_50ms']
assert all('frequency_before_ppm=0.000000000 tick_before_us=10000' in v['reading'] for v in x['frequency_readings'])
print('CLOCK PREFLIGHT PASS',flush=True)
env=os.environ.copy();env['TMPDIR']=str(root/'tmp-disk')
with (root/'linux-clock-monitor.stdout').open('w') as log:
 monitor=subprocess.Popen(['python3',str(root/'clock-monitor.py'),str(root/'environment-clock/monitor'),'--frequency-reader',str(root/'clock-frequency')],stdout=log,stderr=subprocess.STDOUT)
 try:
  start=time.monotonic()
  with (root/'sleep-failure-replay.log').open('w') as replay:
   result=subprocess.run(['ctest','--preset','dev','-j','1','-R',r'^(DevServicesScriptTest\.FourProcessFailureRecovery|LoadgenIntegrationTest\.M3SmokeTenThousandTicketsAndConcurrentPolls)$'],cwd=root/'after',env=env,stdout=replay,stderr=subprocess.STDOUT)
  (root/'sleep-failure-replay.json').write_text(json.dumps({'wall_s':time.monotonic()-start,'exit':result.returncode,'source':'e59cc88b27c0b61955ae901ecad0d97819c8513c','role':'targeted correctness diagnosis; excluded from performance'},indent=2)+'\n')
  assert result.returncode==0, 'stable-clock replay failed; investigate retained log'
  print('TARGETED REPLAY PASS',flush=True)
  for side in ('r0','before','after'):
   subprocess.run(['python3',str(root/'paired-run.py'),'refresh',side],check=True)
  print('ALL THREE SOURCES STABLE',flush=True)
  subprocess.run(['python3',str(root/'paired-run.py'),'formal'],check=True)
 finally:
  monitor.terminate();clock_exit=monitor.wait()
assert clock_exit==0, 'clock observation invalid; preserve epoch'
subprocess.run(['python3',str(root/'collect-current.py'),str(root),str(root/'lima-samples.json'),'lima'],check=True)
