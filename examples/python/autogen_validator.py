#!/usr/bin/env python3
"""
AutoGen Code Execution Validator

Validates LLM-generated code before AutoGen's code executor runs it.
Works standalone (no AutoGen required for the demo).

Usage:
    python3 autogen_validator.py

With AutoGen installed:
    from autogen_validator import NaabCodeValidator

    validator = NaabCodeValidator()

    # Register as a pre-execution hook
    agent.register_hook("process_code_block", validator.process_code_block)

    # Or validate manually
    allowed, reason = validator.validate(code, "python")
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
    "security": {"sandbox_level": "unrestricted"},
}


class NaabCodeValidator:
    """AutoGen-compatible code validator.

    Scans code blocks through NAAb governance before they are executed
    in AutoGen's code execution environment.
    """

    def __init__(self, policy=None):
        self.engine = GovernanceEngine()
        self.engine.load_config_dict(policy or DEFAULT_POLICY)

    def validate(self, code, language="python"):
        """Validate code. Returns (allowed: bool, reason: str).

        Compatible with AutoGen's code validation pattern.
        """
        result = self.engine.scan(language, code, "autogen_exec")
        if result["blocked"]:
            violations = result.get("violations", [])
            reason = violations[0]["message"] if violations else "Governance violation"
            return False, reason
        return True, ""

    def validate_or_raise(self, code, language="python"):
        """Validate and raise GovernanceViolation if blocked."""
        self.engine.scan_or_raise(language, code, "autogen_exec")

    def process_code_block(self, code, language="python"):
        """AutoGen hook-compatible method.

        Returns the code unmodified if it passes, raises if blocked.

        Usage with AutoGen:
            agent.register_hook("process_code_block", validator.process_code_block)
        """
        allowed, reason = self.validate(code, language)
        if not allowed:
            raise GovernanceViolation({
                "blocked": True,
                "error": reason,
                "source": "autogen_hook",
            })
        return code


def main():
    """Standalone demo — no AutoGen required."""
    validator = NaabCodeValidator()

    print("NAAb Governance AutoGen Validator Demo")
    print(f"Engine version: {validator.engine.version}\n")

    # Simulate code blocks from an AutoGen conversation
    code_blocks = [
        ("Data analysis", "python", """
import json
data = [{"name": "Alice", "score": 85}, {"name": "Bob", "score": 92}]
avg = sum(d["score"] for d in data) / len(data)
print(f"Average score: {avg}")
""".strip()),

        ("File system attack", "python", """
import os
for root, dirs, files in os.walk("/etc"):
    for f in files:
        os.remove(os.path.join(root, f))
""".strip()),

        ("Web request", "python", """
import urllib.request
response = urllib.request.urlopen("https://api.example.com/data")
data = response.read().decode("utf-8")
print(data[:100])
""".strip()),

        ("Eval injection", "python", """
user_input = input("Enter expression: ")
result = eval(user_input)
print(result)
""".strip()),

        ("Safe shell command", "shell", """
echo "Hello from shell"
ls -la /tmp
""".strip()),
    ]

    passed = 0
    blocked = 0

    for label, lang, code in code_blocks:
        print(f"--- {label} ({lang}) ---")
        allowed, reason = validator.validate(code, lang)
        if allowed:
            print(f"    ALLOWED")
            passed += 1
        else:
            print(f"    BLOCKED: {reason[:70].strip()}...")
            blocked += 1

    print(f"\nResults: {passed} allowed, {blocked} blocked out of {len(code_blocks)}")

    # Demonstrate the hook pattern
    print("\n--- Hook pattern demo ---")
    try:
        clean_code = validator.process_code_block("x = 42\nprint(x)")
        print(f"    Clean code passed through: {clean_code!r}")
    except GovernanceViolation:
        print("    Unexpected block!")

    try:
        validator.process_code_block("eval(input())")
        print("    Should have been blocked!")
    except GovernanceViolation:
        print("    Dangerous code correctly blocked by hook")


if __name__ == "__main__":
    main()
