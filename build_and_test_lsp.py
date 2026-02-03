#!/usr/bin/env python3
import subprocess
import sys
import os

print("="*60)
print("Building naab-lsp...")
print("="*60)

# Build
result = subprocess.run(
    ['make', 'naab-lsp'],
    cwd='build',
    capture_output=True,
    text=True
)

if result.returncode != 0:
    print("BUILD FAILED!")
    print("\nSTDOUT:")
    print(result.stdout)
    print("\nSTDERR:")
    print(result.stderr)
    sys.exit(1)

print("✓ Build successful!")
print(f"\n{result.stdout[-500:]}")  # Last 500 chars

# Check binary exists
if not os.path.exists('build/naab-lsp'):
    print("✗ Binary not found!")
    sys.exit(1)

binary_size = os.path.getsize('build/naab-lsp')
print(f"✓ Binary created ({binary_size / 1024 / 1024:.1f} MB)")

print("\n" + "="*60)
print("Running LSP symbol test...")
print("="*60 + "\n")

# Run test
result = subprocess.run(
    ['python3', 'test_simple_symbols.py'],
    capture_output=True,
    text=True
)

print(result.stdout)
if result.stderr:
    print("STDERR:")
    print(result.stderr)

if result.returncode != 0:
    print(f"\n✗ Test failed with exit code {result.returncode}")
    sys.exit(1)

print("\n" + "="*60)
print("✓ All tests passed!")
print("="*60)
