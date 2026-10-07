#!/opt/homebrew/opt/python@3.13/libexec/bin/python3
"""Diagnostic wrapper; overhead is excluded from formal performance samples."""
import json
import os
from pathlib import Path
import subprocess
import sys
import time

tool = Path(sys.argv[0]).name
real = os.environ['REALMMESH_PROBE_REAL_' + tool.upper()]
started = time.monotonic()
status = subprocess.call([real, *sys.argv[1:]])
ended = time.monotonic()
args = sys.argv[1:]
kind = ('build' if '--build' in args else 'script' if '-P' in args else 'configure') if tool == 'cmake' else ('discovery' if any(a.startswith('--show-only') or a == '-N' for a in args) else 'test')
row = dict(tool=tool, kind=kind, args=args, cwd=os.getcwd(), start=started,
           end=ended, wall_s=ended-started, exit=status, pid=os.getpid(), ppid=os.getppid())
fd = os.open(os.environ['REALMMESH_PROBE_LOG'], os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
os.write(fd, (json.dumps(row) + '\n').encode())
os.close(fd)
sys.exit(status)
