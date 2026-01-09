#!/usr/bin/env python3
"""
BLOCK-PY-EVAL: Python Expression Evaluator Block
Simple block that evaluates Python expressions
"""

def eval(code_str):
    """Evaluate a Python expression and return the result"""
    try:
        return eval(code_str)
    except Exception as e:
        return f"Error: {e}"

def exec_with_context(code_str, context):
    """Evaluate Python expression with variable context"""
    try:
        return eval(code_str, {}, context)
    except Exception as e:
        return f"Error: {e}"
