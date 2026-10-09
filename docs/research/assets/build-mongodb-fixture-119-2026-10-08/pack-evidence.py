#!/usr/bin/env python3
"""Package only measurement evidence; exclude source/build trees, tools, data and binaries."""
import gzip
import hashlib
import json
import sys
import tarfile
from pathlib import Path
root = Path(sys.argv[1]).resolve()
dest = Path(sys.argv[2]).resolve()
platform = sys.argv[3]
assert not dest.exists()
allowed_suffixes = {'.json', '.jsonl', '.log', '.stdout', '.py', '.c', '.cpp', '.md', '.patch'}
paths = set()
for path in root.iterdir():
    if path.is_file() and path.suffix in allowed_suffixes and path.name not in {'linux-inputs.tar.gz','host-power-after-failure.log','report-draft.md','asset-readme.md','continuation-status.md'}:
        paths.add(path)
    elif path.is_dir() and (path.name.endswith('-results') or path.name in {'environment-clock', 'startup-variants', 'startup-journal', 'pr149-ci-acceptance','clock-interrupted'}):
        paths.update(p for p in path.rglob('*') if p.is_file() and not p.is_symlink())
for path in (root / 'tool').iterdir():
    if path.is_file() and path.suffix in {'.py', '.cpp', '.md'}:
        paths.add(path)
with dest.open('xb') as raw:
    with gzip.GzipFile(filename='', mode='wb', fileobj=raw, mtime=0, compresslevel=6) as stream:
        with tarfile.open(fileobj=stream, mode='w') as archive:
            for path in sorted(paths):
                archive.add(path, arcname=str(path.relative_to(root)), recursive=False)
print(json.dumps({'platform': platform, 'archive': dest.name, 'files': len(paths), 'bytes': dest.stat().st_size, 'sha256': hashlib.sha256(dest.read_bytes()).hexdigest()}, indent=2))
