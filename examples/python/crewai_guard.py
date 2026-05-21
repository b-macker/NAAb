#!/usr/bin/env python3
"""
CrewAI Governance Guard

Pre/post task governance scanning for CrewAI agent tasks. Works standalone
(no CrewAI required for the demo).

Usage:
    python3 crewai_guard.py

With CrewAI installed:
    from crewai_guard import NaabGovernanceGuard

    guard = NaabGovernanceGuard()

    task = Task(
        description="Write a data processing function",
        agent=coding_agent,
        callback=guard.task_callback,
    )
"""

import sys
import os

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, PROJECT_ROOT)

from bindings.python.naab_governance import GovernanceEngine, GovernanceViolation

DEFAULT_POLICY = {
    "version": "3.0",
    "mode": "enforce",
    "restrictions": {
        "dangerous_calls": {"level": "hard"},
        "code_injection": {"level": "hard"},
        "shell_injection": {"level": "hard"},
    },
    "code_quality": {
        "no_secrets": {"level": "hard"},
        "semantic_checks": {"level": "hard"},
    },
    "security": {"sandbox_level": "standard"},
}


class NaabGovernanceGuard:
    """CrewAI-compatible governance guard.

    Scans code outputs from agent tasks through NAAb governance
    before they are passed to the next task or returned to the user.
    """

    def __init__(self, policy=None):
        self.engine = GovernanceEngine()
        self.engine.load_config_dict(policy or DEFAULT_POLICY)
        self.scan_count = 0
        self.block_count = 0

    def validate_output(self, output, language="python"):
        """Validate task output. Returns (valid, reason) tuple.

        Compatible with CrewAI's output validation pattern.
        """
        result = self.engine.scan(language, output, "crewai_task")
        self.scan_count += 1
        if result["blocked"]:
            self.block_count += 1
            violations = result.get("violations", [])
            reason = violations[0]["message"] if violations else "Governance violation"
            return False, reason
        return True, ""

    def task_callback(self, task_output):
        """CrewAI task callback — scans output after task completes.

        Usage:
            task = Task(..., callback=guard.task_callback)
        """
        if hasattr(task_output, 'raw'):
            code = task_output.raw
        else:
            code = str(task_output)

        valid, reason = self.validate_output(code)
        if not valid:
            raise GovernanceViolation({
                "blocked": True,
                "error": reason,
                "source": "crewai_task_callback",
            })
        return task_output

    def scan_or_raise(self, code, language="python", source="crewai_task"):
        """Scan and raise GovernanceViolation if blocked."""
        self.engine.scan_or_raise(language, code, source)
        self.scan_count += 1

    @property
    def stats(self):
        return {
            "scans": self.scan_count,
            "blocked": self.block_count,
            "pass_rate": f"{(self.scan_count - self.block_count) / max(self.scan_count, 1):.0%}",
        }


def main():
    """Standalone demo — no CrewAI required."""
    guard = NaabGovernanceGuard()

    print("NAAb Governance CrewAI Guard Demo")
    print(f"Engine version: {guard.engine.version}\n")

    # Simulate agent task outputs
    task_outputs = [
        ("Data processor", "python",
         "def process(data):\n    return [x * 2 for x in data]\nresult = process([1,2,3])"),
        ("Malicious cleanup", "python",
         "import shutil\nshutil.rmtree('/')"),
        ("API client", "python",
         "import requests\nresponse = requests.get('https://api.example.com/data')\nprint(response.json())"),
        ("Credential theft", "python",
         'password = "sk-ant-api03-REAL_KEY_HERE_12345678901234567890"'),
        ("Shell command", "python",
         "import subprocess\nsubprocess.run(['ls', '-la'], check=True)"),
    ]

    for label, lang, code in task_outputs:
        print(f"--- Task: {label} ---")
        valid, reason = guard.validate_output(code, lang)
        if valid:
            print(f"    PASS")
        else:
            print(f"    BLOCKED: {reason[:80]}...")

    print(f"\nGuard stats: {guard.stats}")


if __name__ == "__main__":
    main()
