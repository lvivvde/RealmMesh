#!/usr/bin/env python3
"""Preserve the interrupted clock-invalid group; it is not a complete formal run."""
import hashlib,json,subprocess,sys,tarfile
from pathlib import Path
root=Path(__file__).resolve().parent
with (root/'clock-invalid-export.stdout').open('w') as output:
    subprocess.run([sys.executable,'collect.py',str(root),str(root/'lima-clock-invalid-samples.json'),'lima'],check=True,stdout=output)
data=json.loads((root/'lima-clock-invalid-samples.json').read_text())
for name,entry in data['raw_manifest'].items():
    assert hashlib.sha256((root/name).read_bytes()).hexdigest()==entry['sha256'],name
assert not data['r0_source_hash_mismatches'] and not data['final_source_hash_mismatches']
assert data['stability_gate_passed'] is False
summary={'evidence_integrity_checks':'passed','raw_files_checked':len(data['raw_manifest']),'stage_counts':{g:len(v['rows']) for g,v in data['groups'].items()},'formal_run_complete':False,'stability_gate_passed':data['stability_gate_passed'],'current_correctness_gate_passed':data['current_correctness_gate_passed'],'excluded_from_final_pairs':True,'reason':'User requested VM time synchronization correction and every Linux group rerun; original partial group and failures preserved','stability_failures':data['stability_failures']}
(root/'clock-invalid-audit.json').write_text(json.dumps(summary,indent=2)+'\n')
items={p.name:p for p in [root/'lima-clock-invalid-samples.json',root/'clock-invalid-audit.json',root/'stability-issues.json']}
for p in (root/'clock-diagnostic').iterdir():
    if p.is_file() and p.suffix in ['.json','.txt','.log','.c']:items['clock-'+p.name]=p
for name in ['linux-formal-run.log','linux-finish-run.log','verification-run.log','verification-remaining-run.log','continuation-run.log']:items[name]=root/name
for name in ['stable-1.log','recover-1-noop.log','final-full.log']:items[name]=root/'checks-results'/name
for side in ['r0','final']:
    for i in range(1,4):items[side+'-hot-full-'+str(i)+'.log']=root/(side+'-results')/('hot-full-'+str(i)+'.log')
manifest={name:{'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'bytes':p.stat().st_size} for name,p in items.items()}
(root/'clock-invalid-shipment.json').write_text(json.dumps(manifest,indent=2)+'\n');items['shipment-manifest.json']=root/'clock-invalid-shipment.json'
with tarfile.open(root/'clock-invalid-evidence.tar.gz','w:gz') as archive:
    for name,p in items.items():archive.add(p,arcname=name,recursive=False)
print(json.dumps(summary,indent=2))
