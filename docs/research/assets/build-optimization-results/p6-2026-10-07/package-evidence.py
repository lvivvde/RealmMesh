#!/usr/bin/env python3
"""Package selected immutable logs; complete raw catalogue stays in the exported JSON."""
import hashlib,json,sys,tarfile
from pathlib import Path
root=Path(__file__).resolve().parent
platform='mac' if sys.platform=='darwin' else 'lima'
completed=json.loads((root/'completed.json').read_text())
assert completed['platform']==platform
items={platform+'-samples.json':root/(platform+'-samples.json')}
for side in ['r0','final']:
    for i in range(1,4):
        for scenario,directory in [('hot-full',side+'-results'),('cold-complete','cold-'+side+'-results')]:
            name=scenario+'-'+str(i)+'.log'
            items[platform+'-'+side+'-'+name]=root/directory/name
for name,directory,log in [('final-full','checks-results','final-full.log'),('fallback-full','fallback-results','fallback-full.log'),('missingdeps','checks-results','missingdeps.log')]:
    items[platform+'-'+name+'.log']=root/directory/log
acceptance=root/('mac-acceptance' if platform=='mac' else 'linux-acceptance')
for p in acceptance.iterdir():
    if p.is_file():items[p.name]=p
for name in ['audit-run.log','acceptance-git-status.txt','completed.json','verification-run.log','cold-complete-run.log']:
    items[platform+'-'+name]=root/name
for name in ['continuation-run.log','verification-remaining-run.log','stability-issues.json']:
    if (root/name).is_file():items[platform+'-'+name]=root/name
for name in ['stable-1.log','recover-1-noop.log']:
    if platform=='lima':items[platform+'-'+name]=root/'checks-results'/name
if (root/'clock-diagnostic').exists():
    for p in (root/'clock-diagnostic').iterdir():
        if p.is_file() and p.name!='last-tick':items[platform+'-clock-'+p.name]=p
for phase in ['preflight','monitor']:
    p=root/'environment-clock'/phase/'summary.json'
    if p.is_file():items[platform+'-clock-'+phase+'-summary.json']=p
for name in ['clock-preflight-run.log','clock-monitor-run.log','lima-audit-final.txt']:
    if (root/name).is_file():items[platform+'-'+name if not name.startswith(platform+'-') else name]=root/name
for name,p in items.items():
    if not p.is_file():raise SystemExit('missing evidence: '+str(p))
manifest={name:{'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'bytes':p.stat().st_size} for name,p in items.items()}
(root/'shipment-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
items[platform+'-shipment-manifest.json']=root/'shipment-manifest.json'
with tarfile.open(root/(platform+'-evidence.tar.gz'),'w:gz') as archive:
    for name,p in items.items():archive.add(p,arcname=name,recursive=False)
print('Packaged',platform,len(items),'files')
