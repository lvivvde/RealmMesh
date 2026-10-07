from pathlib import Path
import json,re
root=Path(__file__).resolve().parent
counts=[];active=[];closed=[]
for label in ['r0','final','cache','checks','fallback','cold-r0','cold-final']:
    directory=root/(label+'-results');saved=directory/'stages.json'
    if not directory.exists():continue
    rows=json.loads(saved.read_text()) if saved.exists() else []
    counts.append(label+':'+str(len(rows)));names={r['name'] for r in rows}
    if rows:closed.append((saved.stat().st_mtime,label,rows[-1]))
    for log in directory.glob('*.log'):
        if log.stem in names:continue
        memory=directory/(log.stem+'.memory.jsonl')
        values=[json.loads(s) for s in memory.read_text().splitlines()] if memory.exists() else []
        event_count=len(list((directory/(log.stem+'-events')).glob('*.json')))
        progress=re.findall(r'(\d+)/(\d+)\s+Test\s+#\d+:\s+(.*?)\s+\.{2,}\s+(?:Passed|\*\*\*Failed)',log.read_text(errors='replace'))
        active.append(label+'/'+log.stem+' seconds='+str(len(values))+' events='+str(event_count)+(' available='+str(round(min(v['available_mib'] for v in values)))+'MiB' if values else '')+(' tests='+progress[-1][0]+'/'+progress[-1][1] if progress else ''))
print('saved',' '.join(counts))
if active:print('\n'.join(active))
elif closed:
    _,label,row=max(closed,key=lambda x:x[0]);print('last',label+'/'+row['name'],'exit='+str(row['exit']))
