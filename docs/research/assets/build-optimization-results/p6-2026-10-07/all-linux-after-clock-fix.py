#!/usr/bin/env python3
"""Run every frozen Linux phase with a read-only clock observer."""
import json,signal,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parent
preflight=json.loads((ROOT/'environment-clock/preflight/summary.json').read_text())
assert preflight['elapsed_s']>=45 and not preflight['backward_steps'] and not preflight['wall_steps_over_50ms']

def phase(argv,log):
    print('START',argv,flush=True)
    with (ROOT/log).open('w') as output:
        result=subprocess.run(argv,cwd=ROOT,stdout=output,stderr=subprocess.STDOUT)
    print('EXIT',result.returncode,argv,flush=True)
    if result.returncode:raise SystemExit(result.returncode)
with (ROOT/'clock-monitor-run.log').open('w') as output:
    monitor=subprocess.Popen([sys.executable,'clock-monitor.py','environment-clock/monitor','--frequency-reader','./clock-frequency'],cwd=ROOT,stdout=output,stderr=subprocess.STDOUT)
    try:
        phase([sys.executable,'run.py','cold','short','hot','cache'],'linux-formal-run.log')
        phase([sys.executable,'finish-bench.py'],'linux-finish-run.log')
    finally:
        monitor.send_signal(signal.SIGTERM)
        monitor_exit=monitor.wait(timeout=10)
    assert monitor_exit==0,monitor_exit
# All *-run.log files and clock timeline are closed before export.
phase([sys.executable,'collect.py',str(ROOT),str(ROOT/'lima-samples.json'),'lima'],'lima-export.stdout')
phase([sys.executable,'audit.py',str(ROOT/'lima-samples.json'),str(ROOT)],'lima-audit-final.txt')
phase([sys.executable,'package-evidence.py'],'package.stdout')
print('All Linux paired phases and final raw/clock audits completed',flush=True)
