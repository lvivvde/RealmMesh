#!/usr/bin/env python3
"""Real CTest lifecycle contracts for the shared MongoDB fixture."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

parser = argparse.ArgumentParser()
parser.add_argument('--source', required=True, type=Path)
parser.add_argument('--initializer', required=True, type=Path)
parser.add_argument('--probe', required=True, type=Path)
ARGS, rest = parser.parse_known_args()
MODULE = ARGS.source / 'tests/scripts/mongodb_ctest_fixture.py'

class MongoCtestFixtureContract(unittest.TestCase):
    def test_prepares_real_primary_and_cleans_up_after_ctest(self):
        with tempfile.TemporaryDirectory(prefix='mongodb-ctest-contract-') as directory:
            root = Path(directory)
            state = root / 'fixture'
            consumer = root / 'consume.py'
            consumer.write_text('''import json,os,subprocess,sys
from pathlib import Path
module,root,source,record=sys.argv[1:]
snapshot=json.loads(subprocess.check_output([sys.executable,module,'status',root,'--owner',str(os.getppid())],text=True))
Path(record).write_text(json.dumps(snapshot))
shell=subprocess.check_output(['bash','-c','realmmesh_mongodb_root="$1"; source "$1/scripts/lib/mongodb-tools.sh"; realmmesh_mongosh_binary','bash',source],text=True).strip()
hello=subprocess.check_output([shell,'mongodb://127.0.0.1:'+str(snapshot['port'])+'/?directConnection=true','--quiet','--norc','--eval',"const h=db.hello();print(h.setName+':'+h.isWritablePrimary)"],text=True).strip()
assert hello=='rs0:true',hello
''')
            cmake = f'''cmake_minimum_required(VERSION 3.20)
project(mongodb_ctest_contract NONE)
enable_testing()
add_test(NAME Prepare COMMAND "{os.sys.executable}" "{MODULE}" setup "{state}" "{ARGS.source}" "{ARGS.initializer}")
set_tests_properties(Prepare PROPERTIES FIXTURES_SETUP mongo TIMEOUT 35)
add_test(NAME Observe COMMAND "{os.sys.executable}" "{consumer}" "{MODULE}" "{state}" "{ARGS.source}" "{root / 'observed.json'}")
set_tests_properties(Observe PROPERTIES FIXTURES_REQUIRED mongo TIMEOUT 15)
add_test(NAME Cleanup COMMAND "{os.sys.executable}" "{MODULE}" cleanup "{state}")
set_tests_properties(Cleanup PROPERTIES FIXTURES_CLEANUP mongo TIMEOUT 5)
'''
            (root / 'CMakeLists.txt').write_text(cmake)
            subprocess.run(['cmake','-S',str(root),'-B',str(root/'build')],check=True,capture_output=True,text=True)
            result=subprocess.run(['ctest','--test-dir',str(root/'build'),'-j','1','--output-on-failure'],capture_output=True,text=True,timeout=55)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            observed=json.loads((root/'observed.json').read_text())
            with self.assertRaises(ProcessLookupError): os.kill(observed['server_pid'],0)
            self.assertFalse(Path(observed['data_directory']).exists())
            self.assertEqual(list(state.iterdir()),[])

    def test_distinct_databases_are_isolated_on_the_same_real_backend(self):
        with tempfile.TemporaryDirectory(prefix='mongodb-isolation-contract-') as directory:
            root = Path(directory)
            state = root / 'fixture'
            consumer = root / 'consume.py'
            consumer.write_text('''import json,os,subprocess,sys
module,root,probe=sys.argv[1:]
snapshot=json.loads(subprocess.check_output([sys.executable,module,'status',root,'--owner',str(os.getppid())],text=True))
env=os.environ.copy();env['REALMMESH_TEST_MONGODB_SESSION_DIRECTORY']=root;env['REALMMESH_TEST_MONGODB_OWNER']=str(os.getppid())
expected='127.0.0.1:'+str(snapshot['port'])
def observe(database,script):
    return subprocess.check_output([probe,'--shared',database,script],env=env,text=True).strip()
assert observe('fixture_alpha',"print(db.hello().me);db.examples.insertOne({value:'alpha'});print(db.examples.countDocuments({}))")==expected+'\\n1'
assert observe('fixture_beta',"print(db.hello().me);print(db.examples.countDocuments({}))")==expected+'\\n0'
assert observe('fixture_alpha',"print(db.examples.countDocuments({value:'alpha'}))")=='1'
''')
            (root / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(mongodb_isolation_contract NONE)
enable_testing()
add_test(NAME Prepare COMMAND "{os.sys.executable}" "{MODULE}" setup "{state}" "{ARGS.source}" "{ARGS.initializer}")
set_tests_properties(Prepare PROPERTIES FIXTURES_SETUP mongo TIMEOUT 35)
add_test(NAME Observe COMMAND "{os.sys.executable}" "{consumer}" "{MODULE}" "{state}" "{ARGS.probe}")
set_tests_properties(Observe PROPERTIES FIXTURES_REQUIRED mongo TIMEOUT 30)
add_test(NAME Cleanup COMMAND "{os.sys.executable}" "{MODULE}" cleanup "{state}")
set_tests_properties(Cleanup PROPERTIES FIXTURES_CLEANUP mongo TIMEOUT 5)
''')
            subprocess.run(['cmake','-S',str(root),'-B',str(root/'build')],check=True,capture_output=True,text=True)
            result=subprocess.run(['ctest','--test-dir',str(root/'build'),'-j','1','--output-on-failure'],capture_output=True,text=True,timeout=70)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_failed_preparation_blocks_consumers_and_reclaims_real_server(self):
        with tempfile.TemporaryDirectory(prefix='mongodb-failed-ctest-') as directory:
            root = Path(directory)
            state = root / 'fixture'
            marker = root / 'consumer-ran'
            wrapper = root / 'wrong-set-mongod.py'
            real = subprocess.check_output(['bash','-c','realmmesh_mongodb_root="$1"; source "$1/scripts/lib/mongodb-tools.sh"; realmmesh_mongod_binary','bash',str(ARGS.source)],text=True).strip()
            wrapper.write_text('#!/usr/bin/env python3\nimport os,sys\nfrom pathlib import Path\n'
                               + f'Path({str(root / "server-pid")!r}).write_text(str(os.getpid()))\n'
                               + 'args=sys.argv[1:];args[args.index("--replSet")+1]="wrong-set"\n'
                               + f'os.execv({real!r},[{real!r},*args])\n')
            wrapper.chmod(0o755)
            (root/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(mongodb_failure_contract NONE)
enable_testing()
add_test(NAME Prepare COMMAND "{os.sys.executable}" "{MODULE}" setup "{state}" "{ARGS.source}" "{ARGS.initializer}")
set_tests_properties(Prepare PROPERTIES FIXTURES_SETUP mongo TIMEOUT 35 ENVIRONMENT "REALMMESH_MONGOD_BINARY={wrapper}")
add_test(NAME Observe COMMAND "{os.sys.executable}" -c "from pathlib import Path;Path(r'{marker}').touch()")
set_tests_properties(Observe PROPERTIES FIXTURES_REQUIRED mongo)
add_test(NAME Cleanup COMMAND "{os.sys.executable}" "{MODULE}" cleanup "{state}")
set_tests_properties(Cleanup PROPERTIES FIXTURES_CLEANUP mongo TIMEOUT 5)
''')
            subprocess.run(['cmake','-S',str(root),'-B',str(root/'build')],check=True,capture_output=True,text=True)
            result=subprocess.run(['ctest','--test-dir',str(root/'build'),'-j','1','--output-on-failure'],capture_output=True,text=True,timeout=50)
            self.assertNotEqual(result.returncode,0)
            self.assertFalse(marker.exists())
            with self.assertRaises(ProcessLookupError): os.kill(int((root/'server-pid').read_text()),0)
            self.assertEqual([p for p in state.iterdir() if p.is_dir()],[])
            self.assertIn('MongoDB fixture initializer failed',result.stdout)

    def test_killed_ctest_owner_reclaims_shared_processes_and_data(self):
        with tempfile.TemporaryDirectory(prefix='mongodb-killed-ctest-') as directory:
            root = Path(directory)
            state = root / 'fixture'
            consumer = root / 'consume.py'
            consumer.write_text('''import json,os,socket,subprocess,sys,time
from pathlib import Path
module,root,record=sys.argv[1:]
snapshot=json.loads(subprocess.check_output([sys.executable,module,'status',root,'--owner',str(os.getppid())],text=True))
Path(record).write_text(json.dumps(snapshot))
end=time.monotonic()+15
while time.monotonic()<end:
    try:
        with socket.create_connection(('127.0.0.1',snapshot['port']),timeout=.1): pass
    except OSError: break
    time.sleep(.02)
''')
            (root/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(mongodb_killed_contract NONE)
enable_testing()
add_test(NAME Prepare COMMAND "{os.sys.executable}" "{MODULE}" setup "{state}" "{ARGS.source}" "{ARGS.initializer}")
set_tests_properties(Prepare PROPERTIES FIXTURES_SETUP mongo TIMEOUT 35)
add_test(NAME Observe COMMAND "{os.sys.executable}" "{consumer}" "{MODULE}" "{state}" "{root / 'observed.json'}")
set_tests_properties(Observe PROPERTIES FIXTURES_REQUIRED mongo TIMEOUT 20)
add_test(NAME Cleanup COMMAND "{os.sys.executable}" "{MODULE}" cleanup "{state}")
set_tests_properties(Cleanup PROPERTIES FIXTURES_CLEANUP mongo TIMEOUT 5)
''')
            subprocess.run(['cmake','-S',str(root),'-B',str(root/'build')],check=True,capture_output=True,text=True)
            with (root/'ctest.log').open('w') as log:
                process=subprocess.Popen(['ctest','--test-dir',str(root/'build'),'-j','1'],stdout=log,stderr=log)
                try:
                    deadline=time.monotonic()+10
                    while not (root/'observed.json').exists():
                        if process.poll() is not None or time.monotonic()>deadline:
                            self.fail('real shared fixture was not prepared: '+(root/'ctest.log').read_text())
                        time.sleep(.02)
                    observed=json.loads((root/'observed.json').read_text())
                    process.kill();process.wait(timeout=2)
                    deadline=time.monotonic()+6
                    while Path(observed['data_directory']).exists() and time.monotonic()<deadline:
                        time.sleep(.02)
                    self.assertFalse(Path(observed['data_directory']).exists())
                    for pid in [observed['server_pid'],observed['guardian_pid']]:
                        deadline=time.monotonic()+2
                        while time.monotonic()<deadline:
                            try: os.kill(pid,0)
                            except ProcessLookupError: break
                            time.sleep(.02)
                        with self.assertRaises(ProcessLookupError): os.kill(pid,0)
                    self.assertEqual(list(state.iterdir()),[])
                finally:
                    if process.poll() is None:
                        process.kill();process.wait()

if __name__=='__main__':
    unittest.main(argv=[__file__,*rest],verbosity=2)
