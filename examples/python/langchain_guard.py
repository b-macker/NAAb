#!/usr/bin/env python3
"""
Example: NAAb Governance as a LangChain Guard

Shows how to use NAAb governance scanning to validate LLM-generated
code before execution in a LangChain pipeline.

Usage:
    export NAAB_GOV_LIB=/path/to/libnaab-governance.so
    python3 langchain_guard.py
"""

import sys
import os

# Add project root
PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, PROJECT_ROOT)

from bindings.python.naab_governance import GovernanceEngine, GovernanceViolation


def main():
    # Create engine with inline policy
    engine = GovernanceEngine()
    engine.load_config_dict({
        "version": "3.0",
        "mode": "enforce",
        "restrictions": {
            "dangerous_calls": {"level": "hard"},
            "code_injection": {"level": "hard"},
            "shell_injection": {"level": "hard"},
        },
        "code_quality": {
            "no_secrets": {"level": "hard"},
            "semantic_checks": {"level": "hard", "check_dangerous_eval": True},
        },
        "security": {"sandbox_level": "unrestricted"},
    })

    print(f"NAAb Governance Engine v{engine.version}")
    print(f"Active: {engine.is_active}\n")

    # Simulate LLM-generated code blocks
    test_cases = [
        ("Safe computation", "python", "result = sum(range(100))\nprint(result)"),
        ("Dangerous eval", "python", "user_data = input()\nexec(user_data)"),
        ("Secret leak", "python", 'token = "ghp_ABCDEFghijklmnop1234567890abcdef"'),
        ("Safe JavaScript", "javascript", "const x = [1,2,3].map(n => n * 2);"),
    ]

    for label, lang, code in test_cases:
        print(f"--- {label} ({lang}) ---")
        try:
            result = engine.scan_or_raise(lang, code, f"{label}.{lang}")
            print(f"  PASS: {engine.result_count} checks passed")
        except GovernanceViolation as e:
            print(f"  BLOCKED: {e}")
        print()

    print("Done.")


if __name__ == "__main__":
    main()
