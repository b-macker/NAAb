#!/usr/bin/env python3
"""
Refactor stdlib module implementations to fix vtable errors.
Transforms from full class definitions to method implementations.
"""

import re
import sys

def refactor_module(input_file, output_file, module_name):
    with open(input_file, 'r') as f:
        content = f.read()

    # Replace include
    content = content.replace('#include "naab/stdlib.h"', '#include "naab/stdlib_new_modules.h"')

    # Extract namespace opening
    namespace_match = re.search(r'(namespace naab \{\s*namespace stdlib \{)', content)
    if not namespace_match:
        print(f"Error: Could not find namespace declaration")
        return False

    # Find class definition start
    class_match = re.search(rf'class {module_name} : public Module \{{', content)
    if not class_match:
        print(f"Error: Could not find class {module_name}")
        return False

    # Find the closing brace of the class (last }; before namespace closing)
    # This is tricky - we need to find the matching closing brace

    # Extract everything before class definition
    before_class = content[:class_match.start()]

    # Extract everything after class definition start
    after_class_start = content[class_match.end():]

    # Find class closing (};\n)
    class_end_match = re.search(r'\n\};\s*\n\s*\} // namespace stdlib', after_class_start)
    if not class_end_match:
        print(f"Error: Could not find class closing brace")
        return False

    class_body = after_class_start[:class_end_match.start()]
    after_class = after_class_start[class_end_match.start():]

    # Extract public and private sections
    public_match = re.search(r'public:(.*?)private:', class_body, re.DOTALL)
    if not public_match:
        # No private section, all is public
        public_section = class_body
        private_section = ""
    else:
        public_section = public_match.group(1)
        private_section = class_body[public_match.end()-len('private:'):]

    # Extract methods from public section (skip getName override - it's inline in header)
    # We need hasFunction and call

    # Build new file
    output = []
    output.append(before_class.rstrip())
    output.append("")

    # Add forward declarations for helper functions
    # Extract helper function signatures from private section
    helper_sigs = re.findall(r'^\s*((?:std::)?(?:\w+(?:::\w+)*(?:<[^>]+>)?)\s+\w+\([^)]*\))', private_section, re.MULTILINE)
    if helper_sigs:
        output.append("// Forward declarations of helper functions")
        for sig in helper_sigs:
            # Make it static
            output.append(f"static {sig};")
        output.append("")

    # Extract hasFunction implementation
    hasfunction_match = re.search(r'bool hasFunction\(const std::string& name\) const override \{(.*?)\n    \}', public_section, re.DOTALL)
    if hasfunction_match:
        hasfunction_body = hasfunction_match.group(1)
        output.append(f"bool {module_name}::hasFunction(const std::string& name) const {{")
        output.append(hasfunction_body)
        output.append("}")
        output.append("")

    # Extract call implementation
    call_match = re.search(r'std::shared_ptr<interpreter::Value> call\(\s*const std::string& function_name,\s*const std::vector<std::shared_ptr<interpreter::Value>>& args\) override \{(.*?)\n    \}', public_section, re.DOTALL)
    if call_match:
        call_body = call_match.group(1)
        output.append(f"std::shared_ptr<interpreter::Value> {module_name}::call(")
        output.append("    const std::string& function_name,")
        output.append("    const std::vector<std::shared_ptr<interpreter::Value>>& args) {")
        output.append(call_body)
        output.append("}")
        output.append("")

    # Process private section - convert methods to static functions
    # Remove "private:" marker
    private_section = re.sub(r'^\s*private:\s*\n', '', private_section, flags=re.MULTILINE)

    # Replace [this] with [] in lambdas (since they're now static)
    private_section = re.sub(r'\[this\]', '[]', private_section)

    # Don't add ClassName:: prefix to private methods - they're now static
    output.append("// Helper functions")
    output.append(private_section.rstrip())

    # Add namespace closing
    output.append("")
    output.append("} // namespace stdlib")
    output.append("} // namespace naab")
    output.append("")

    # Write output
    with open(output_file, 'w') as f:
        f.write('\n'.join(output))

    print(f"Refactored {module_name} successfully")
    return True

if __name__ == "__main__":
    if len(sys.argv) != 4:
        print("Usage: refactor_module.py <input_file> <output_file> <ModuleName>")
        sys.exit(1)

    success = refactor_module(sys.argv[1], sys.argv[2], sys.argv[3])
    sys.exit(0 if success else 1)
