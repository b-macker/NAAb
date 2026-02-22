#pragma once

// NAAb Auto-Formatter - AST-based code formatter
// Implements consistent code style across all NAAb code

#include <sstream>
#include <string>
#include <vector>
#include <memory>

// Forward declarations for AST types
namespace naab {
namespace ast {
    // Base classes
    class Expr;
    class Stmt;

    // Top-level
    class Program;
    class UseStatement;
    class FunctionDecl;
    class MainBlock;
    class StructDecl;
    class EnumDecl;

    // Statements
    class CompoundStmt;
    class ExprStmt;
    class ReturnStmt;
    class IfStmt;
    class ForStmt;
    class WhileStmt;
    class BreakStmt;
    class ContinueStmt;
    class VarDeclStmt;
    class ExportStmt;
    class TryStmt;
    class ThrowStmt;
    class ModuleUseStmt;

    // Expressions
    class BinaryExpr;
    class UnaryExpr;
    class CallExpr;
    class MemberExpr;
    class IdentifierExpr;
    class LiteralExpr;
    class DictExpr;
    class ListExpr;
    class RangeExpr;
    class StructLiteralExpr;
    class InlineCodeExpr;

    // Types and helpers
    struct Type;
    struct Parameter;
    struct StructField;
}
}

namespace naab {
namespace formatter {

// Formatting style options
enum class SemicolonStyle {
    Never,      // Never add semicolons
    Always,     // Always add semicolons
    AsNeeded    // Add only when required (multi-statement lines)
};

enum class BraceStyle {
    SameLine,    // K&R/Egyptian: fn name() {
    NextLine     // Allman: fn name()\n{
};

enum class WrappingStyle {
    Auto,        // Wrap when line too long
    Always,      // Always wrap
    Never        // Never wrap
};

// Formatter configuration options
struct FormatterOptions {
    // Indentation
    size_t indent_width = 4;

    // Line length
    size_t max_line_length = 100;

    // Semicolons
    SemicolonStyle semicolons = SemicolonStyle::Never;

    // Braces
    BraceStyle function_brace_style = BraceStyle::SameLine;
    BraceStyle control_flow_brace_style = BraceStyle::SameLine;

    // Trailing commas
    bool trailing_commas = true;  // Add trailing commas in multi-line lists

    // Blank lines
    size_t blank_lines_between_declarations = 1;
    size_t blank_lines_between_sections = 2;

    // Spacing
    bool space_before_function_paren = false;
    bool space_in_empty_parens = false;

    // Wrapping
    WrappingStyle wrap_function_params = WrappingStyle::Auto;
    WrappingStyle wrap_struct_fields = WrappingStyle::Auto;
    WrappingStyle wrap_array_elements = WrappingStyle::Auto;
    bool align_wrapped_params = true;

    // Factory methods
    static FormatterOptions defaults();
    static FormatterOptions fromFile(const std::string& path);
    static FormatterOptions fromToml(const std::string& toml_content);
};

// Formatting context - tracks current state during formatting
class FormatterContext {
public:
    FormatterContext(const FormatterOptions& options);

    // Indentation management
    void increaseIndent();
    void decreaseIndent();
    size_t getCurrentIndent() const { return current_indent_; }

    // Line position tracking
    void resetLinePosition();
    void advancePosition(size_t chars);
    size_t getCurrentLinePosition() const { return current_line_pos_; }

    // Line counting
    void incrementLineCount();
    size_t getCurrentLine() const { return current_line_; }

private:
    const FormatterOptions& options_;
    size_t current_indent_ = 0;
    size_t current_line_pos_ = 0;
    size_t current_line_ = 1;
};

// Main formatter class - uses AST visitor pattern
class Formatter {
public:
    explicit Formatter(const FormatterOptions& options = FormatterOptions::defaults());

    // Main entry points
    std::string format(const std::string& source_code);
    std::string format(const std::string& source_code, const std::string& filename);
    std::string formatProgram(const ast::Program& program);

    // Get last error
    const std::string& getLastError() const { return last_error_; }
    bool hasError() const { return !last_error_.empty(); }

private:
    // Visitor methods for AST nodes
    void visitProgram(const ast::Program& node);
    void visitUseStatement(const ast::UseStatement& node);
    void visitFunctionDecl(const ast::FunctionDecl& node);
    void visitMainBlock(const ast::MainBlock& node);
    void visitStructDecl(const ast::StructDecl& node);
    void visitEnumDecl(const ast::EnumDecl& node);

    // Statements
    void visitCompoundStmt(const ast::CompoundStmt& node);
    void visitExprStmt(const ast::ExprStmt& node);
    void visitReturnStmt(const ast::ReturnStmt& node);
    void visitIfStmt(const ast::IfStmt& node);
    void visitForStmt(const ast::ForStmt& node);
    void visitWhileStmt(const ast::WhileStmt& node);
    void visitBreakStmt(const ast::BreakStmt& node);
    void visitContinueStmt(const ast::ContinueStmt& node);
    void visitVarDeclStmt(const ast::VarDeclStmt& node);
    void visitExportStmt(const ast::ExportStmt& node);
    void visitTryStmt(const ast::TryStmt& node);
    void visitThrowStmt(const ast::ThrowStmt& node);
    void visitModuleUseStmt(const ast::ModuleUseStmt& node);

    // Expressions
    void visitBinaryExpr(const ast::BinaryExpr& node);
    void visitUnaryExpr(const ast::UnaryExpr& node);
    void visitCallExpr(const ast::CallExpr& node);
    void visitMemberExpr(const ast::MemberExpr& node);
    void visitIdentifierExpr(const ast::IdentifierExpr& node);
    void visitLiteralExpr(const ast::LiteralExpr& node);
    void visitDictExpr(const ast::DictExpr& node);
    void visitListExpr(const ast::ListExpr& node);
    void visitRangeExpr(const ast::RangeExpr& node);
    void visitStructLiteralExpr(const ast::StructLiteralExpr& node);
    void visitInlineCodeExpr(const ast::InlineCodeExpr& node);

    // Helper functions for types and parameters
    void visitType(const ast::Type& type);
    void visitParameter(const ast::Parameter& param);
    void visitStructField(const ast::StructField& field);

    // Helper for visiting any expression node
    void visitExpressionNode(ast::Expr* expr);

    // Output helpers
    void write(const std::string& text);
    void writeLine(const std::string& text = "");
    void writeIndent();
    void writeSpace();
    void writeNewline();
    void writeBlankLines(size_t count);
    void writeSemicolon();  // Conditional based on style

    // Line breaking and wrapping
    bool shouldBreakLine(size_t estimated_length);
    size_t estimateLength(const std::string& text);
    size_t estimateParamListLength(const std::vector<ast::Parameter>& params);

    // Comma-separated lists
    template<typename T, typename Func>
    void writeCommaSeparatedList(const std::vector<T>& items, Func visitor, bool multiline = false);

    FormatterOptions options_;
    FormatterContext context_;
    std::ostringstream output_;
    std::string last_error_;
};

} // namespace formatter
} // namespace naab

