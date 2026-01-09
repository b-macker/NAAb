#!/bin/bash
# Quick refactor - change include and remove class wrapper
MODULE=$1
FILE=$2

# 1. Change include
sed -i 's|#include "naab/stdlib.h"|#include "naab/stdlib_new_modules.h"|' "$FILE"

# 2. Add module implementation prefix to hasFunction and call
sed -i "s/bool hasFunction/bool ${MODULE}::hasFunction/" "$FILE"
sed -i "s/std::shared_ptr<interpreter::Value> call(/std::shared_ptr<interpreter::Value> ${MODULE}::call(/" "$FILE"

# 3. Remove class declaration line and private: line
sed -i "/^class ${MODULE} : public Module {$/d" "$FILE"
sed -i "/^public:$/d" "$FILE"
sed -i "/^private:$/d" "$FILE"

# 4. Remove getName override
sed -i '/getName() const override/,/}/d' "$FILE"

# 5. Remove closing }; before namespace closing
sed -i '/^};$/d' "$FILE"

# 6. Change [this] to [] in lambdas
sed -i 's/\[this\]/[]/g' "$FILE"

echo "Refactored $MODULE in $FILE"
