#!/usr/bin/env python3
"""
LangChain Governance Middleware

Intercepts LLM-generated code in LangChain tool calls and scans it
through NAAb governance before execution. Works standalone (no LangChain
required for the demo).

Usage:
    python3 langchain_middleware.py

With LangChain installed:
    from langchain_middleware import make_governed_python_tool
    tool = make_governed_python_tool()
    agent = initialize_agent([tool], llm, agent="zero-shot-react-description")
"""

import sys
import os

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, PROJECT_ROOT)

from bindings.python.naab_governance import GovernanceEngine, GovernanceViolation

# Shared governance policy for all tool calls
POLICY = {
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
    "security": {"sandbox_level": "standard"},
}


def create_governance_guard(policy=None):
    """Create a governance guard function for wrapping tool calls."""
    engine = GovernanceEngine()
    engine.load_config_dict(policy or POLICY)

    def guard(code, language="python", source="langchain_tool"):
        """Scan code and raise GovernanceViolation if blocked."""
        result = engine.scan(language, code, source)
        if result["blocked"]:
            raise GovernanceViolation(result)
        return result

    return guard


def make_governed_python_tool(policy=None):
    """
    Wrap LangChain's PythonREPLTool with governance scanning.

    Returns a PythonREPLTool whose _run method scans code before execution.
    If LangChain is not installed, returns None.
    """
    try:
        from langchain_experimental.tools import PythonREPLTool
    except ImportError:
        return None

    guard = create_governance_guard(policy)
    tool = PythonREPLTool()
    original_run = tool._run

    def governed_run(query, **kwargs):
        guard(query, "python")  # Raises GovernanceViolation if blocked
        return original_run(query, **kwargs)

    tool._run = governed_run
    tool.description += " (governed by NAAb)"
    return tool


def main():
    """Standalone demo — no LangChain required."""
    guard = create_governance_guard()

    print(f"NAAb Governance LangChain Middleware Demo")
    print(f"Engine version: {GovernanceEngine().version}\n")

    test_cases = [
        ("Safe computation", "python", "result = sum(range(100))\nprint(result)"),
        ("Dangerous eval", "python", "user_data = input()\nexec(user_data)"),
        ("OS command injection", "python", "import os; os.system('rm -rf /')"),
        ("Secret leak", "python", 'api_key = "ghp_ABCDEFghijklmnop1234567890abcdef"'),
        ("Safe JavaScript", "javascript", "const x = [1,2,3].map(n => n * 2);"),
        ("JS eval injection", "javascript", "eval(document.location.hash)"),
    ]

    passed = 0
    blocked = 0
    for label, lang, code in test_cases:
        print(f"--- {label} ({lang}) ---")
        print(f"    Code: {code[:60]}{'...' if len(code) > 60 else ''}")
        try:
            guard(code, lang)
            print(f"    Result: PASS")
            passed += 1
        except GovernanceViolation:
            print(f"    Result: BLOCKED")
            blocked += 1

    print(f"\n{passed} passed, {blocked} blocked out of {len(test_cases)} checks")

    # Show LangChain integration status
    tool = make_governed_python_tool()
    if tool:
        print("\nLangChain PythonREPLTool available with governance wrapper")
    else:
        print("\nLangChain not installed — use: pip install langchain-experimental")


if __name__ == "__main__":
    main()
