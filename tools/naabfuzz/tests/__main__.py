import sys
import unittest

from . import test_oracle_selfcheck

suite = unittest.defaultTestLoader.loadTestsFromModule(test_oracle_selfcheck)
result = unittest.TextTestRunner(verbosity=1).run(suite)
sys.exit(0 if result.wasSuccessful() else 1)
