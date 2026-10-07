#!/opt/homebrew/opt/python@3.13/libexec/bin/python3
"""Time the real shell without recording connection strings or eval contents."""
import json
import os
import subprocess
import sys
import time

script=sys.argv[sys.argv.index('--eval')+1] if '--eval' in sys.argv else ''
kind='initiate' if 'rs.initiate(' in script else 'primary' if 'db.hello().isWritablePrimary' in script else 'eval'
started=time.monotonic(); status=subprocess.call([os.environ['REALMMESH_PROBE_REAL_MONGOSH'],*sys.argv[1:]])
row=dict(kind=kind,wall_s=time.monotonic()-started,exit=status)
fd=os.open(os.environ['REALMMESH_PROBE_MONGOSH_LOG'],os.O_WRONLY|os.O_CREAT|os.O_APPEND,0o600)
os.write(fd,(json.dumps(row)+'\n').encode()); os.close(fd)
sys.exit(status)
