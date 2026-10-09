#!/usr/bin/env python3
import importlib.util,json
from pathlib import Path
root=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('paired',root/'paired-run.py');paired=importlib.util.module_from_spec(spec);spec.loader.exec_module(paired)
b=paired.bench('after');inventory=json.loads((b.out/'test-inventory.json').read_text())
command=next(t['command'] for t in inventory['tests'] if t['name']=='MongoFixtureTest');helper=Path(command[command.index('--initializer')+1]);assert helper.is_relative_to(b.build_dir)
b.out=root/'recovery-check-results';b.out.mkdir(exist_ok=True);b.rows=[];b.stages_path=b.out/'stages.json'
helper.unlink()
row=b.run('deleted-helper-consumer-recovery',[('build',['cmake','--build','--preset','dev','--parallel',str(b.args.jobs),'--target','player_data_store_test']),('test',['ctest','--preset','dev','-j','1','-R','^MongoFixtureTest$'])])
assert row['exit']==0 and helper.is_file() and row['tests_total']==1 and row['tests_failed']==0
row=b.run('recovery-noop',[('build',['cmake','--build','--preset','dev','--parallel',str(b.args.jobs),'--target','player_data_store_test'])]);assert row['exit']==0 and row['compile_count']==row['link_count']==0
