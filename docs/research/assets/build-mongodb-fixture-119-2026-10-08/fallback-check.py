#!/usr/bin/env python3
import importlib.util,json,subprocess
from pathlib import Path
root=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('paired',root/'paired-run.py');paired=importlib.util.module_from_spec(spec);spec.loader.exec_module(paired)
b=paired.bench('after');b.args.preset='dev-make';b.build_dir=b.source/'build/dev-make';b.out=root/'make-check-results';b.out.mkdir(exist_ok=True);b.rows=[];b.stages_path=b.out/'stages.json'
(b.build_dir/paired.measure.SODIUM_DOWNLOAD_DIR).mkdir(parents=True,exist_ok=True)
row=b.run('make-helper-contract',[b.prepare_step(),('configure',b.configure_cmd()),('build',['cmake','--build','--preset','dev-make','--parallel',str(b.args.jobs),'--target','player_data_store_test','realmmesh_mongod_fixture_probe']),('test',['ctest','--preset','dev-make','-j','1','-R','^(MongoFixtureTest|PlayerDataStoreTest.AccountWithoutCharactersIsEligible)$'])])
assert row['exit']==0 and row['tests_total']==2 and row['tests_failed']==0
(b.out/'environment.json').write_text(json.dumps(b.environment(['make-helper-contract']),indent=2)+'\n')
