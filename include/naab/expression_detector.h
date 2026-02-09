#ifndef NAAB_EXPRESSION_DETECTOR_H
#define NAAB_EXPRESSION_DETECTOR_H

#include <string>

namespace naab {
namespace runtime {

/**
 * Detects if code is a simple expression vs a full program.
 * Used to automatically wrap expressions for return value capture.
 */
class ExpressionDetector {
public:
    // Detect if code is a simple expression vs full program
    static bool isExpression(const std::string& code, const std::string& language);

private:
    static bool isRustExpression(const std::string& code);
    static bool isRubyExpression(const std::string& code);
    static bool isGoExpression(const std::string& code);
    static bool isCSharpExpression(const std::string& code);

    // Helper: trim whitespace
    static std::string trim(const std::string& str);
};

} // namespace runtime
} // namespace naab

#endif // NAAB_EXPRESSION_DETECTOR_H
