#!/usr/bin/env python3
"""Prepared-source cold complete entry, independent from build-only target samples."""
import run
for i in range(1,4):
    for side in run.ordered(i):
        b=run.bench(side,out='cold-'+side+'-results')
        b.fresh_build_dir()
        row=b.run('cold-complete-'+str(i),[b.prepare_step(),('configure',b.configure_cmd()),('build',b.build_cmd()),('test',b.ctest_cmd())])
        run.save_env(b,'cold-complete')
        if row['exit'] or row.get('tests_total')!=(649 if side=='r0' else 667) or row.get('tests_failed')!=0:
            raise SystemExit('cold complete correctness failure retained: '+side+' '+row['name'])
