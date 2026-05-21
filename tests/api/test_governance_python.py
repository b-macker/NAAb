#!/usr/bin/env python3
"""NAAb Governance Python Bindings — Integration Tests

Requires libnaab-governance.so (shared library build).
On ARM64 Android/Termux, the library builds as .a (static) due to
bionic TLS limitations — these tests are skipped on that platform.
"""

import json
import os
import sys
import unittest

# Add project root so bindings can be imported
PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, PROJECT_ROOT)

# Skip on platforms where only static library is available
from bindings.python.naab_governance import GovernanceEngine, GovernanceViolation

_skip_reason = None
try:
    _test_eng = GovernanceEngine()
    del _test_eng
except (FileNotFoundError, OSError) as e:
    _skip_reason = str(e)

TEST_CONFIG = {
    "version": "3.0",
    "mode": "enforce",
    "restrictions": {
        "dangerous_calls": {"level": "hard"},
        "shell_injection": {"level": "hard"},
        "code_injection": {"level": "hard"},
    },
    "code_quality": {
        "semantic_checks": {
            "level": "hard",
            "check_imports": True,
            "check_dangerous_eval": True,
        },
        "no_secrets": {"level": "hard"},
        "no_unsafe_deserialization": {"level": "hard"},
        "no_sql_injection": {"level": "hard"},
    },
    "security": {"sandbox_level": "unrestricted"},
}


@unittest.skipIf(_skip_reason, f"Shared library not available: {_skip_reason}")
class TestGovernanceEngine(unittest.TestCase):
    def setUp(self):
        self.engine = GovernanceEngine()
        self.engine.load_config_dict(TEST_CONFIG)

    def test_version(self):
        self.assertTrue(len(self.engine.version) > 0)

    def test_is_active(self):
        self.assertTrue(self.engine.is_active)

    def test_scan_clean_code(self):
        result = self.engine.scan("python", "x = 42\nprint(x)", "test.py")
        self.assertIn("blocked", result)
        self.assertFalse(result["blocked"])

    def test_scan_code_injection(self):
        result = self.engine.scan(
            "python",
            "user_input = input()\nresult = eval(user_input)",
            "test.py",
        )
        self.assertTrue(result["blocked"])

    def test_scan_secrets(self):
        result = self.engine.scan(
            "python",
            'api_key = "AKIAIOSFODNN7EXAMPLE"',
            "test.py",
        )
        self.assertTrue(result["blocked"])

    def test_scan_or_raise_clean(self):
        result = self.engine.scan_or_raise("python", "x = 1", "test.py")
        self.assertFalse(result["blocked"])

    def test_scan_or_raise_blocked(self):
        with self.assertRaises(GovernanceViolation) as ctx:
            self.engine.scan_or_raise(
                "python", "eval('dangerous')", "test.py"
            )
        self.assertTrue(ctx.exception.result["blocked"])

    def test_json_report_structure(self):
        self.engine.scan("python", "x = 1", "test.py")
        report = self.engine.json_report
        self.assertIsInstance(report, dict)

    def test_sarif_report(self):
        self.engine.scan("python", "x = 1", "test.py")
        sarif = self.engine.sarif_report
        self.assertIsInstance(sarif, dict)

    def test_result_count(self):
        self.engine.scan("python", "x = 1", "test.py")
        count = self.engine.result_count
        self.assertIsInstance(count, int)

    def test_reset(self):
        self.engine.scan("python", "eval('x')", "test.py")
        self.assertGreater(self.engine.result_count, 0)
        self.engine.reset()
        self.assertEqual(self.engine.result_count, 0)

    def test_load_config_dict(self):
        eng = GovernanceEngine()
        eng.load_config_dict({
            "version": "3.0",
            "mode": "enforce",
            "restrictions": {"dangerous_calls": {"level": "hard"}},
        })
        self.assertTrue(eng.is_active)

    def test_multiple_engines(self):
        eng1 = GovernanceEngine()
        eng2 = GovernanceEngine()
        eng1.load_config_dict(TEST_CONFIG)

        self.assertTrue(eng1.is_active)
        self.assertFalse(eng2.is_active)

    def test_check_single(self):
        result = self.engine.check(
            "secrets", "python", 'key = "AKIAIOSFODNN7EXAMPLE"'
        )
        self.assertIn("check", result)
        self.assertEqual(result["check"], "secrets")
        self.assertTrue(result["blocked"])


if __name__ == "__main__":
    unittest.main()
