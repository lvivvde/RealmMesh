#!/usr/bin/env python3
"""Observe the owned Linux benchmark clock; never adjust it."""
import argparse,json,signal,subprocess,time
from pathlib import Path
parser=argparse.ArgumentParser();parser.add_argument('out');parser.add_argument('--duration',type=float);parser.add_argument('--frequency-reader',required=True);parser.add_argument('--probe-mtime',action='store_true');args=parser.parse_args()
out=Path(args.out);out.mkdir(parents=True,exist_ok=True)
stop=False

def finish(signum,frame):
    global stop
    stop=True
signal.signal(signal.SIGTERM,finish);signal.signal(signal.SIGINT,finish)
start=time.monotonic();previous=None;backwards=[];steps=[];count=0;frequency=[];next_frequency=0
with (out/'timeline.jsonl').open('w') as stream:
    while not stop and (args.duration is None or time.monotonic()-start<args.duration):
        row={'wall_ns':time.time_ns(),'monotonic_ns':time.monotonic_ns(),'raw_ns':time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)}
        if args.probe_mtime:
            tick=out/'natural-write';tick.write_text(str(count));row['mtime_ns']=tick.stat().st_mtime_ns
        if previous:
            if row['wall_ns']<previous['wall_ns'] or (args.probe_mtime and row['mtime_ns']<previous['mtime_ns']):backwards.append({'previous':previous,'current':row})
            shift=(row['wall_ns']-previous['wall_ns'])-(row['monotonic_ns']-previous['monotonic_ns'])
            if abs(shift)>50000000:steps.append({'previous':previous,'current':row,'wall_vs_monotonic_shift_ns':shift})
        stream.write(json.dumps(row)+'\n');stream.flush();previous=row;count+=1
        if time.monotonic()-start>=next_frequency:
            result=subprocess.run([args.frequency_reader],capture_output=True,text=True,check=True)
            frequency.append({'monotonic_elapsed_s':time.monotonic()-start,'reading':result.stdout.strip()});next_frequency+=60
        time.sleep(.1)
frequency.append({'monotonic_elapsed_s':time.monotonic()-start,'reading':subprocess.run([args.frequency_reader],capture_output=True,text=True,check=True).stdout.strip()})
summary={'samples':count,'elapsed_s':time.monotonic()-start,'backward_steps':backwards,'wall_steps_over_50ms':steps,'frequency_readings':frequency,'chrony_active':subprocess.run(['systemctl','is-active','chrony'],capture_output=True,text=True).stdout.strip(),'chrony_enabled':subprocess.run(['systemctl','is-enabled','chrony'],capture_output=True,text=True).stdout.strip(),'guestagent_active':subprocess.run(['systemctl','is-active','lima-guestagent'],capture_output=True,text=True).stdout.strip()}
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary,indent=2))
raise SystemExit(bool(backwards or steps))
