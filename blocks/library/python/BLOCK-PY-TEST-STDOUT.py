"""Test block for Python stdout verification"""

def test_stdout():
    """Test basic print output"""
    print("Hello from Python stdout!")
    print("This should be visible if stdout fix works")
    return "Function returned successfully"

def print_numbers():
    """Print numbers 1-5 to stdout"""
    print("Counting: 1, 2, 3, 4, 5")
    for i in range(1, 6):
        print(f"Number: {i}")
    return "Done counting"

def test_math():
    """Test math operations with output"""
    import sys
    print(f"Python version: {sys.version}")
    x = 42
    y = 58
    result = x + y
    print(f"{x} + {y} = {result}")
    return result
