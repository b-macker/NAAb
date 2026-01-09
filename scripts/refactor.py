#!/usr/bin/env python3
"""Refactor stdlib module implementations - simple but effective approach"""
import sys
import re

def refactor_file(filepath, module_name):
    with open(filepath, 'r') as f:
        lines = f.readlines()

    output = []
    i = 0
    in_class = False
    in_private = False
    class_depth = 0

    # Process line by line
    while i < len(lines):
        line = lines[i]

        # Replace include
        if '#include "naab/stdlib.h"' in line:
            output.append(line.replace('#include "naab/stdlib.h"', '#include "naab/stdlib_new_modules.h"'))
            i += 1
            continue

        # Found class definition - skip it and start processing
        if f'class {module_name} : public Module {{' in line:
            in_class = True
            # Add forward declarations placeholder - we'll extract helpers later
            output.append("// Forward declarations\n")
            output.append(f"// (helpers will be added here)\n\n")
            i += 1
            continue

        # Skip getName override - it's in header
        if in_class and 'getName() const override' in line:
            # Skip until closing brace
            while i < len(lines) and '}' not in lines[i]:
                i += 1
            i += 1  # Skip closing brace
            continue

        # Process hasFunction
        if in_class and 'bool hasFunction' in line:
            # Output as MathModule::hasFunction
            output.append(f"bool {module_name}::hasFunction(const std::string& name) const {{\n")
            i += 1
            # Copy body until closing brace
            brace_count = 1
            while i < len(lines) and brace_count > 0:
                if '{' in lines[i]:
                    brace_count += 1
                if '}' in lines[i]:
                    brace_count -= 1
                if brace_count > 0:
                    output.append(lines[i])
                i += 1
            output.append("}\n\n")
            continue

        # Process call
        if in_class and 'std::shared_ptr<interpreter::Value> call(' in line:
            output.append(f"std::shared_ptr<interpreter::Value> {module_name}::call(\n")
            i += 1
            # Next line should be parameters
            while i < len(lines) and 'override {' not in lines[i]:
                output.append(lines[i].replace('override {', '{'))
                i += 1
            if 'override {' in lines[i]:
                output.append(lines[i].replace('override {', '{'))
                i += 1
            # Copy body
            brace_count = 1
            while i < len(lines) and brace_count > 0:
                if '{' in lines[i]:
                    brace_count += 1
                if '}' in lines[i]:
                    brace_count -= 1
                if brace_count > 0:
                    output.append(lines[i])
                i += 1
            output.append("}\n\n")
            continue

        # Found private: section
        if in_class and 'private:' in line:
            in_private = True
            output.append("// Helper functions and implementations\n")
            i += 1
            continue

        # In private section - make everything static
        if in_private:
            # Skip class closing
            if '};' in line:
                in_class = False
                in_private = False
                i += 1
                continue

            # Replace [this] with []
            line = line.replace('[this]', '[]')
            output.append(line)
            i += 1
            continue

        # Not in class yet or after class
        if not in_class:
            output.append(line)

        i += 1

    # Write output
    with open(filepath, 'w') as f:
        f.writelines(output)

    print(f"Refactored {module_name}")

if __name__ == "__main__":
    modules = [
        ("src/stdlib/time_impl.cpp", "TimeModule"),
        ("src/stdlib/string_impl.cpp", "StringModule"),
        ("src/stdlib/array_impl.cpp", "ArrayModule"),
        ("src/stdlib/env_impl.cpp", "EnvModule"),
        ("src/stdlib/regex_impl.cpp", "RegexModule"),
        ("src/stdlib/crypto_impl.cpp", "CryptoModule"),
    ]

    for filepath, modname in modules:
        try:
            refactor_file(filepath, modname)
        except Exception as e:
            print(f"Error refactoring {modname}: {e}")
            import traceback
            traceback.print_exc()
