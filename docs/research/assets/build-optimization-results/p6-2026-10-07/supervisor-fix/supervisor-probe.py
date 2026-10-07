#!/usr/bin/env python3
"""Temporary probe of the exact supervisor manager function and a minimal worker."""
import json,os,signal,subprocess,sys,tempfile,time
from pathlib import Path
source=Path(sys.argv[1]);n=int(sys.argv[2])
text=(source/'scripts/dev-services.sh').read_text()
function=text[text.index('supervise_services() {'):text.index('\nsupervise_service_group() {')]
if '--repeat-wait' in sys.argv:
 function=function.replace('realmmesh_worker_status=0\n        wait "${realmmesh_worker_pid}" || realmmesh_worker_status=$?', 'while :; do\n            realmmesh_worker_status=0\n            wait "${realmmesh_worker_pid}" || realmmesh_worker_status=$?\n            kill -0 "${realmmesh_worker_pid}" 2>/dev/null || break\n        done')
if '--trace' in sys.argv:
 function=function.replace('realmmesh_worker_pid=$!', 'realmmesh_worker_pid=$!\n    printf "[DEBUG-p6] worker=%s manager=%s\\n" "$realmmesh_worker_pid" "$BASHPID"')
 function=function.replace('if [[ "${realmmesh_shutdown_requested}" -ne 0 ]]; then', 'printf "[DEBUG-p6] first-wait=%s shutdown=%s\\n" "$realmmesh_worker_status" "$realmmesh_shutdown_requested"\n    if [[ "${realmmesh_shutdown_requested}" -ne 0 ]]; then')
 function=function.replace(': > "${realmmesh_supervisor_shutdown_file}"', ': > "${realmmesh_supervisor_shutdown_file}"\n        printf "[DEBUG-p6] published\\n"')
 function=function.replace('rm -f -- "${realmmesh_supervisor_shutdown_file}"\n    return', 'printf "[DEBUG-p6] second-wait=%s shutdown=%s\\n" "$realmmesh_worker_status" "$realmmesh_shutdown_requested"\n    if kill -0 "$realmmesh_worker_pid" 2>/dev/null; then printf "[DEBUG-p6] worker-alive\\n"; fi\n    rm -f -- "${realmmesh_supervisor_shutdown_file}"\n    return')
results=[]
for i in range(n):
 with tempfile.TemporaryDirectory(prefix='p6-supervisor-probe-') as tmp:
  root=Path(tmp);script=root/'probe.sh';log=root/'log'
  script.write_text('set -euo pipefail\nrealmmesh_supervisor_shutdown_file="$1/shutdown"\n'+function+"\nsupervise_service_group() {\n printf '%s\\n' \"$BASHPID\" > \"$1/worker.pid\"\n : > \"$1/ready\"\n sleep 0.005\n while [[ ! -f \"${realmmesh_supervisor_shutdown_file}\" ]]; do sleep 0.001; done\n sleep 0.05\n : > \"$1/stopped\"\n}\n"+'supervise_services "$1"\n')
  # The worker receives the same owned directory through the global.
  script.write_text(script.read_text().replace('"$1/worker.pid"','"'+str(root)+'/worker.pid"').replace('"$1/ready"','"'+str(root)+'/ready"').replace('"$1/stopped"','"'+str(root)+'/stopped"'))
  with log.open('w') as out:
   proc=subprocess.Popen(['bash',str(script),str(root)],stdout=out,stderr=subprocess.STDOUT,start_new_session=True)
   deadline=time.monotonic()+2
   while not (root/'ready').exists() and proc.poll() is None and time.monotonic()<deadline:time.sleep(.0001)
   until=time.monotonic()+.02
   while time.monotonic()<until and proc.poll() is None:
    try:os.kill(proc.pid,signal.SIGTERM)
    except ProcessLookupError:break
    time.sleep(.0001)
   try:code=proc.wait(timeout=.2)
   except subprocess.TimeoutExpired:code=None
   stopped=(root/'stopped').exists()
   if code!=0 or not stopped:
    worker=int((root/'worker.pid').read_text()) if (root/'worker.pid').exists() else None
    alive=worker is not None and Path(f'/proc/{worker}/stat').exists()
    result={'trial':i,'exit':code,'worker_stopped':stopped,'worker_alive':alive,'shutdown_exists':(root/'shutdown').exists(),'log':log.read_text()};results.append(result);print(json.dumps(result),flush=True)
   try:os.killpg(proc.pid,signal.SIGKILL)
   except ProcessLookupError:pass
   proc.wait()
 print('',end='',flush=True)
print(json.dumps({'trials':n,'failures':len(results),'results':results}),flush=True)
