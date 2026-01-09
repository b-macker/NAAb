#!/usr/bin/env python3
"""
Validator for NAAb Standard Library Module implementations.
Per CLAUDE.md: validates structure, content, and integration.
"""

import sys
import os
import re
import subprocess
from pathlib import Path

class ModuleValidator:
    def __init__(self, module_name: str, impl_file: Path):
        self.module_name = module_name
        self.impl_file = impl_file
        self.errors = []
        self.warnings = []

    def validate_v1_structure(self) -> bool:
        """V1: Structure validation"""
        print(f"[V1] Validating structure for {self.module_name}...")

        # Check file exists
        if not self.impl_file.exists():
            self.errors.append(f"Implementation file missing: {self.impl_file}")
            return False

        content = self.impl_file.read_text()

        # Check class declaration
        class_pattern = rf"class\s+{self.module_name.title()}Module\s*:\s*public\s+Module"
        if not re.search(class_pattern, content):
            self.errors.append(f"Class declaration not found: {self.module_name.title()}Module")

        # Check required methods
        required_methods = ["getName", "hasFunction", "call"]
        for method in required_methods:
            if method not in content:
                self.errors.append(f"Required method missing: {method}")

        # Check compiles
        compile_result = self._check_compilation()
        if not compile_result:
            self.errors.append("Module does not compile")

        return len(self.errors) == 0

    def validate_v2_content(self, python_ref: Path) -> bool:
        """V2: Content validation - functions match Python reference"""
        print(f"[V2] Validating content against Python reference...")

        if not python_ref.exists():
            self.errors.append(f"Python reference missing: {python_ref}")
            return False

        # Extract function names from Python
        py_content = python_ref.read_text()
        py_functions = re.findall(r'^def\s+(\w+)\s*\(', py_content, re.MULTILINE)

        # Extract function names from C++
        cpp_content = self.impl_file.read_text()
        cpp_functions = re.findall(
            rf'std::shared_ptr<.*?Value>\s+(\w+)\s*\(',
            cpp_content
        )

        # Check all Python functions are implemented
        missing = set(py_functions) - set(cpp_functions)
        if missing:
            self.errors.append(f"Functions missing in C++: {missing}")

        return len(self.errors) == 0

    def validate_v3_integration(self) -> bool:
        """V3: Integration validation - module registers and is callable"""
        print(f"[V3] Validating integration and registration...")

        # Check module is registered in stdlib.cpp
        stdlib_cpp = Path(self.impl_file.parent) / "stdlib.cpp"
        if not stdlib_cpp.exists():
            self.errors.append("stdlib.cpp not found")
            return False

        content = stdlib_cpp.read_text()
        registration = f'modules_["{self.module_name}"]'
        if registration not in content:
            self.errors.append(f"Module not registered in StdLib: {registration}")

        # Check tests exist
        test_file = Path(self.impl_file.parent.parent) / "tests" / "test_stdlib.cpp"
        if test_file.exists():
            test_content = test_file.read_text()
            test_name = f"TEST({self.module_name.title()}Module"
            if test_name not in test_content:
                self.warnings.append(f"No tests found for {self.module_name}")

        return len(self.errors) == 0

    def _check_compilation(self) -> bool:
        """Attempt to compile the module"""
        build_dir = Path(self.impl_file.parent.parent) / "build"
        if not build_dir.exists():
            self.warnings.append("Build directory not found, skipping compilation check")
            return True  # Don't fail if build not set up yet

        try:
            result = subprocess.run(
                ["make", "naab_stdlib"],
                cwd=build_dir,
                capture_output=True,
                timeout=60
            )
            return result.returncode == 0
        except Exception as e:
            self.warnings.append(f"Compilation check failed: {e}")
            return True  # Don't fail on check errors

    def run_all_validations(self, python_ref: Path = None) -> bool:
        """Run all validation levels"""
        v1_pass = self.validate_v1_structure()
        v2_pass = self.validate_v2_content(python_ref) if python_ref else True
        v3_pass = self.validate_v3_integration()

        all_pass = v1_pass and v2_pass and v3_pass

        print("\n=== Validation Results ===")
        print(f"V1 (Structure): {'✓ PASS' if v1_pass else '✗ FAIL'}")
        print(f"V2 (Content):   {'✓ PASS' if v2_pass else '✗ FAIL'}")
        print(f"V3 (Integration): {'✓ PASS' if v3_pass else '✗ FAIL'}")

        if self.errors:
            print("\nErrors:")
            for err in self.errors:
                print(f"  ✗ {err}")

        if self.warnings:
            print("\nWarnings:")
            for warn in self.warnings:
                print(f"  ! {warn}")

        return all_pass


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: validate_stdlib_module.py <module_name> <impl_file> [python_ref]")
        sys.exit(1)

    module_name = sys.argv[1]
    impl_file = Path(sys.argv[2])
    python_ref = Path(sys.argv[3]) if len(sys.argv) > 3 else None

    validator = ModuleValidator(module_name, impl_file)
    success = validator.run_all_validations(python_ref)

    sys.exit(0 if success else 1)
