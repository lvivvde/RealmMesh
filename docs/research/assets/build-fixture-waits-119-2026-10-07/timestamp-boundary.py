"""Public ccache boundary diagnostic with a slightly future input mtime."""
import argparse
import os
from pathlib import Path
import runpy
import time
import unittest

parser = argparse.ArgumentParser()
parser.add_argument('--fixture', required=True)
args, _ = parser.parse_known_args()
namespace = runpy.run_path(args.fixture, run_name='fixture')


class TimestampBoundary(namespace['NativeCacheContract']):
    def runTest(self):
        # A copied/coarse-clock input can be newer than the first compiler
        # invocation. This modifies only diagnostic input metadata, never the
        # production launcher settings or the waiting implementation.
        header = self.src / 'value.h'
        timestamp = time.time() + 0.8
        os.utime(header, (timestamp, timestamp))
        self.enable()
        self.build_probe('12')
        before = self.stats()
        self.clean()
        self.build_probe('12')
        after = self.stats()
        self.assertEqual(after['direct_cache_hit'] - before['direct_cache_hit'], 2)
        self.assertEqual(after['cache_miss'], before['cache_miss'])


result = unittest.TextTestRunner(verbosity=2).run(unittest.TestSuite([TimestampBoundary()]))
raise SystemExit(0 if result.wasSuccessful() else 1)
