"""Recompute explicit-tool attribution from the preserved diagnostic events."""
import argparse
from collections import defaultdict
import json
from pathlib import Path

p=argparse.ArgumentParser();p.add_argument('raw',type=Path);p.add_argument('out',type=Path);a=p.parse_args()
result={'scope':'explicitly wrapped tool calls; diagnostic samples, not performance acceptance','platforms':{}}
for platform,folders in [('mac',('mac','mac-other')),('linux',('linux-disk-complete','linux-disk-other-current-graph'))]:
 suites={}; totals=defaultdict(lambda:{'calls':0,'wall_s':0.0}); repeated_calls=0;repeated_s=0.0
 for folder in folders:
  for f in sorted((a.raw/folder).glob('profile-*.jsonl')):
   rows=[json.loads(line) for line in f.read_text().splitlines()]
   groups=defaultdict(lambda:{'calls':0,'wall_s':0.0});seen=set();again=[]
   for row in sorted(rows,key=lambda r:r['start']):
    key=row['tool']+'-'+row['kind'];groups[key]['calls']+=1;groups[key]['wall_s']+=row['wall_s']
    totals[key]['calls']+=1;totals[key]['wall_s']+=row['wall_s']
    if row['kind']=='configure':
     args=row['args']; identity=args[args.index('-B')+1] if '-B' in args else row['cwd']+':'+args[args.index('--preset')+1]
     if identity in seen: again.append(row)
     seen.add(identity)
   repeated_calls+=len(again);repeated_s+=sum(r['wall_s'] for r in again)
   suites[f.stem[8:]]=dict(groups=groups,repeated_binary_dir_requests=len(again),repeated_binary_dir_wall_s=sum(r['wall_s'] for r in again))
 native=json.loads((a.raw/folders[0]/'native-python-events.json').read_text());ngroups=defaultdict(lambda:{'calls':0,'wall_s':0.0})
 for row in native['events']:
  if row['kind']=='sleep': key='safety-wait'
  else:
   args=row['args'];name=Path(args[0]).name
   if name=='cmake': key='cmake-build' if '--build' in args else 'cmake-configure'
   elif name=='ccache': key='ccache-inspect' if '--inspect' in args else 'ccache-stats' if '--print-stats' in args else 'ccache-maintenance'
   elif len(args)>1 and str(args[1]).endswith('/tools/build-bench/measure.py'):key='unwrapped-measure-cli'
   else:key=name
  ngroups[key]['calls']+=1;ngroups[key]['wall_s']+=row['wall_s']
 result['platforms'][platform]=dict(suites=suites,totals=totals,repeated_binary_dir_requests=repeated_calls,repeated_binary_dir_wall_s=repeated_s,native_python=dict(wall_s=native['wall_s'],groups=ngroups))
a.out.write_text(json.dumps(result,indent=2)+'\n')
