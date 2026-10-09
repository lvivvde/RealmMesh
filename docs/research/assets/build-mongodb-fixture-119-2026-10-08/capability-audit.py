#!/usr/bin/env python3
"""Confirm real QUIC capability from recorded configure output and test commands."""
import json,re,sys
from pathlib import Path
root=Path(sys.argv[1]).resolve();platform=sys.argv[2];result={'platform':platform,'sources':{}}
for side in ('r0','before','after'):
 directory=root/(side+'-results')
 env=json.loads((directory/'environment.json').read_text())
 inventory=json.loads((directory/'test-inventory.json').read_text())['tests']
 tests=[t['name'] for t in inventory if Path(t['command'][0]).name=='quic_transport_test']
 assert len(tests)==1, (side,tests)
 rows=[r for r in json.loads((directory/'stages.json').read_text()) if re.fullmatch('hot-full-[0-9]+',r['name'])]
 logs={}
 for row in rows:
  lines=(directory/row['log']).read_text().splitlines()
  enabled=[l for l in lines if 'realm_network: QUIC transport enabled' in l]
  assert enabled,(side,row['name'])
  for test in tests:assert any(test in l and 'Passed' in l for l in lines),(side,row['name'],test)
  logs[row['log']]=enabled
 result['sources'][side]={'environment_captured_quic':env['quic_transport_enabled'],'msquic_library':env['cmake_cache']['MSQUIC_LIBRARY'],'real_quic_tests':tests,'configure_logs':logs}
result['passed']=True
print(json.dumps(result,indent=2))
