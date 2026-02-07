// NAAb Auto-Formatter Implementation
// AST-based code formatter with configurable style rules

#include "formatter.h"
#include "naab/ast.h"
#include "naab/parser.h"
#include "naab/lexer.h"
#include <fmt/core.h>
#include <algorithm>
#include <toml++/toml.h>

namespace naab {
namespace formatter {

// ============================================================================
// Helper Functions
// ============================================================================

using naab::ast::BinaryOp;
using naab::ast::UnaryOp;

static std::string binaryOpToString(BinaryOp op) {
    switch (op) {
        case BinaryOp::Add: return "+";
        case BinaryOp::Sub: return "-";
        case BinaryOp::Mul: return "*";
        case BinaryOp::Div: return "/";
        case BinaryOp::Mod: return "%";
        case BinaryOp::Eq: return "==";
        case BinaryOp::Ne: return "!=";
        case BinaryOp::Lt: return "<";
        case BinaryOp::Le: return "<=";
        case BinaryOp::Gt: return ">";
        case BinaryOp::Ge: return ">=";
        case BinaryOp::And: return "and";
        case BinaryOp::Or: return "or";
        case BinaryOp::Assign: return "=";
        case BinaryOp::Pipeline: return "|>";
        case BinaryOp::Subscript: return "[]";
        default: return "?";
    }
}

static std::string unaryOpToString(UnaryOp op) {
    switch (op) {
        case UnaryOp::Not: return "not ";
        case UnaryOp::Neg: return "-";
        case UnaryOp::Pos: return "+";
        default: return "?";
    }
}

// ============================================================================
// FormatterOptions Implementation
// ============================================================================

FormatterOptions FormatterOptions::defaults() {
    return FormatterOptions{};
}

FormatterOptions FormatterOptions::fromFile(const std::string& path) {
    FormatterOptions opts = defaults();  // Start with defaults

    try {
        auto config = toml::parse_file(path);

        // [style] section
        if (config.contains("style")) {
            auto& style = *config["style"].as_table();
            if (style["indent_width"]) {
                opts.indent_width = style["indent_width"].value_or(4);
            }
            if (style["max_line_length"]) {
                opts.max_line_length = style["max_line_length"].value_or(100);
            }
            if (style["semicolons"]) {
                std::string semicolon_str = style["semicolons"].value_or("never");
                if (semicolon_str == "always") opts.semicolons = SemicolonStyle::Always;
                else if (semicolon_str == "as-needed") opts.semicolons = SemicolonStyle::AsNeeded;
                else opts.semicolons = SemicolonStyle::Never;
            }
            if (style["trailing_commas"]) {
                opts.trailing_commas = style["trailing_commas"].value_or(true);
            }
        }

        // [braces] section
        if (config.contains("braces")) {
            auto& braces = *config["braces"].as_table();
            if (braces["function_brace_style"]) {
                std::string style = braces["function_brace_style"].value_or("same_line");
                opts.function_brace_style = (style == "next_line") ? BraceStyle::NextLine : BraceStyle::SameLine;
            }
            if (braces["control_flow_brace_style"]) {
                std::string style = braces["control_flow_brace_style"].value_or("same_line");
                opts.control_flow_brace_style = (style == "next_line") ? BraceStyle::NextLine : BraceStyle::SameLine;
            }
        }

        // [spacing] section
        if (config.contains("spacing")) {
            auto& spacing = *config["spacing"].as_table();
            if (spacing["blank_lines_between_declarations"]) {
                opts.blank_lines_between_declarations = spacing["blank_lines_between_declarations"].value_or(1);
            }
            if (spacing["blank_lines_between_sections"]) {
                opts.blank_lines_between_sections = spacing["blank_lines_between_sections"].value_or(2);
            }
            if (spacing["space_before_function_paren"]) {
                opts.space_before_function_paren = spacing["space_before_function_paren"].value_or(false);
            }
            if (spacing["space_in_empty_parens"]) {
                opts.space_in_empty_parens = spacing["space_in_empty_parens"].value_or(false);
            }
        }

        // [wrapping] section
        if (config.contains("wrapping")) {
            auto& wrapping = *config["wrapping"].as_table();
            if (wrapping["wrap_function_params"]) {
                std::string style = wrapping["wrap_function_params"].value_or("auto");
                if (style == "always") opts.wrap_function_params = WrappingStyle::Always;
                else if (style == "never") opts.wrap_function_params = WrappingStyle::Never;
                else opts.wrap_function_params = WrappingStyle::Auto;
            }
            if (wrapping["wrap_struct_fields"]) {
                std::string style = wrapping["wrap_struct_fields"].value_or("auto");
                if (style == "always") opts.wrap_struct_fields = WrappingStyle::Always;
                else if (style == "never") opts.wrap_struct_fields = WrappingStyle::Never;
                else opts.wrap_struct_fields = WrappingStyle::Auto;
            }
            if (wrapping["wrap_array_elements"]) {
                std::string style = wrapping["wrap_array_elements"].value_or("auto");
                if (style == "always") opts.wrap_array_elements = WrappingStyle::Always;
                else if (style == "never") opts.wrap_array_elements = WrappingStyle::Never;
                else opts.wrap_array_elements = WrappingStyle::Auto;
            }
            if (wrapping["align_wrapped_params"]) {
                opts.align_wrapped_params = wrapping["align_wrapped_params"].value_or(true);
            }
        }

    } catch (const toml::parse_error& err) {
        // If config file is invalid, fall back to defaults
        fmt::print(stderr, "Warning: Failed to parse {}: {}\n", path, err.what());
        fmt::print(stderr, "Using default formatting options.\n");
    }

    return opts;
}

// ============================================================================
// FormatterContext Implementation
// ============================================================================

FormatterContext::FormatterContext(const FormatterOptions& options)
    : options_(options) {}

void FormatterContext::increaseIndent() {
    current_indent_ += options_.indent_width;
}

void FormatterContext::decreaseIndent() {
    if (current_indent_ >= options_.indent_width) {
        current_indent_ -= options_.indent_width;
    }
}

void FormatterContext::resetLinePosition() {
    current_line_pos_ = current_indent_;
}

void FormatterContext::advancePosition(size_t chars) {
    current_line_pos_ += chars;
}

void FormatterContext::incrementLineCount() {
    current_line_++;
    resetLinePosition();
}

// ============================================================================
// Formatter Implementation
// ============================================================================

Formatter::Formatter(const FormatterOptions& options)
    : options_(options), context_(options) {}

std::string Formatter::format(const std::string& source_code) {
    return format(source_code, "<input>");
}

std::string Formatter::format(const std::string& source_code, const std::string& filename) {
    try {
        // Tokenize
        lexer::Lexer lexer(source_code);
        auto tokens = lexer.tokenize();

        // Parse
        parser::Parser parser(tokens);
        parser.setSource(source_code, filename);
        auto program = parser.parseProgram();

        // Format
        return formatProgram(*program);
    } catch (const std::exception& e) {
        last_error_ = e.what();
        return "";
    }
}

std::string Formatter::formatProgram(const ast::Program& program) {
    output_.str("");  // Clear output
    output_.clear();
    // Note: context_ cannot be reassigned due to reference member, but it gets reset on next use

    visitProgram(program);

    return output_.str();
}

// ============================================================================
// Output Helper Functions
// ============================================================================

void Formatter::write(const std::string& text) {
    output_ << text;
    context_.advancePosition(text.length());
}

void Formatter::writeLine(const std::string& text) {
    if (!text.empty()) {
        write(text);
    }
    writeNewline();
}

void Formatter::writeIndent() {
    std::string indent(context_.getCurrentIndent(), ' ');
    output_ << indent;
    context_.advancePosition(indent.length());
}

void Formatter::writeSpace() {
    write(" ");
}

void Formatter::writeNewline() {
    output_ << "\n";
    context_.incrementLineCount();
}

void Formatter::writeBlankLines(size_t count) {
    for (size_t i = 0; i < count; ++i) {
        writeNewline();
    }
}

void Formatter::writeSemicolon() {
    if (options_.semicolons == SemicolonStyle::Always) {
        write(";");
    }
    // For Never and AsNeeded, don't add semicolons (parser handles this)
}

bool Formatter::shouldBreakLine(size_t estimated_length) {
    return (context_.getCurrentLinePosition() + estimated_length) > options_.max_line_length;
}

size_t Formatter::estimateLength(const std::string& text) {
    return text.length();
}

size_t Formatter::estimateParamListLength(const std::vector<ast::Parameter>& params) {
    size_t total = 2;  // For parentheses
    for (size_t i = 0; i < params.size(); ++i) {
        total += params[i].name.length() + 2;  // name + ": "
        // Add estimated type length (simplified)
        total += 10;  // Average type name length
        if (i < params.size() - 1) {
            total += 2;  // ", "
        }
    }
    return total;
}

// ============================================================================
// AST Visitor Methods - Program and Declarations
// ============================================================================

void Formatter::visitProgram(const ast::Program& node) {
    bool first = true;
    size_t section_count = 0;

    // Format use statements
    for (const auto& use_stmt : node.getImports()) {
        if (!first) {
            writeBlankLines(options_.blank_lines_between_declarations);
        }
        visitUseStatement(*use_stmt);
        first = false;
        section_count++;
    }

    // Format module use statements
    for (const auto& mod_use : node.getModuleUses()) {
        if (!first) {
            writeBlankLines(options_.blank_lines_between_declarations);
        }
        visitModuleUseStmt(*mod_use);
        first = false;
    }

    // Blank lines between sections (use statements and declarations)
    if (section_count > 0 && (!node.getStructs().empty() || !node.getEnums().empty() || !node.getFunctions().empty())) {
        writeBlankLines(options_.blank_lines_between_sections);
    }

    // Format struct declarations
    for (const auto& struct_decl : node.getStructs()) {
        if (!first) {
            writeBlankLines(options_.blank_lines_between_declarations);
        }
        visitStructDecl(*struct_decl);
        first = false;
    }

    // Format enum declarations
    for (const auto& enum_decl : node.getEnums()) {
        if (!first) {
            writeBlankLines(options_.blank_lines_between_declarations);
        }
        visitEnumDecl(*enum_decl);
        first = false;
    }

    // Blank lines before functions
    if (!node.getFunctions().empty() && !first) {
        writeBlankLines(options_.blank_lines_between_sections);
    }

    // Format function declarations
    for (const auto& func : node.getFunctions()) {
        if (!first) {
            writeBlankLines(options_.blank_lines_between_declarations);
        }
        visitFunctionDecl(*func);
        first = false;
    }

    // Blank lines before main block
    if (node.getMainBlock() && !first) {
        writeBlankLines(options_.blank_lines_between_sections);
    }

    // Format main block
    if (node.getMainBlock()) {
        visitMainBlock(*node.getMainBlock());
    }
}

void Formatter::visitUseStatement(const ast::UseStatement& node) {
    writeIndent();
    write("use ");
    write(node.getBlockId());
    write(" as ");
    write(node.getAlias());
    writeNewline();
}

void Formatter::visitModuleUseStmt(const ast::ModuleUseStmt& node) {
    writeIndent();
    write("use ");
    write(node.getModulePath());

    if (node.hasAlias()) {
        write(" as ");
        write(node.getAlias());
    }

    writeNewline();
}

void Formatter::visitFunctionDecl(const ast::FunctionDecl& node) {
    writeIndent();

    // Function keyword
    write("fn ");

    // Function name
    write(node.getName());

    // Generic parameters
    const auto& type_params = node.getTypeParams();
    if (!type_params.empty()) {
        write("<");
        for (size_t i = 0; i < type_params.size(); ++i) {
            write(type_params[i]);
            if (i < type_params.size() - 1) {
                write(", ");
            }
        }
        write(">");
    }

    // Parameters - check if needs wrapping
    const auto& params = node.getParams();
    bool multiline_params = shouldBreakLine(estimateParamListLength(params));

    if (multiline_params && !params.empty()) {
        // Multi-line params
        write("(");
        writeNewline();
        context_.increaseIndent();

        for (size_t i = 0; i < params.size(); ++i) {
            writeIndent();
            visitParameter(params[i]);
            if (i < params.size() - 1 || options_.trailing_commas) {
                write(",");
            }
            writeNewline();
        }

        context_.decreaseIndent();
        writeIndent();
        write(")");
    } else {
        // Single line params
        write("(");
        for (size_t i = 0; i < params.size(); ++i) {
            visitParameter(params[i]);
            if (i < params.size() - 1) {
                write(", ");
            }
        }
        write(")");
    }

    // Return type
    if (node.getReturnType().kind != ast::TypeKind::Void) {
        write(" -> ");
        visitType(node.getReturnType());
    }

    // Body
    write(" ");
    if (node.getBody()) {
        visitCompoundStmt(*dynamic_cast<ast::CompoundStmt*>(node.getBody()));
    }

    writeNewline();
}

void Formatter::visitMainBlock(const ast::MainBlock& node) {
    writeIndent();
    write("main ");

    if (node.getBody()) {
        visitCompoundStmt(*dynamic_cast<ast::CompoundStmt*>(node.getBody()));
    }

    writeNewline();
}

void Formatter::visitStructDecl(const ast::StructDecl& node) {
    writeIndent();
    write("struct ");
    write(node.getName());

    // Generic parameters
    const auto& type_params = node.getTypeParams();
    if (!type_params.empty()) {
        write("<");
        for (size_t i = 0; i < type_params.size(); ++i) {
            write(type_params[i]);
            if (i < type_params.size() - 1) {
                write(", ");
            }
        }
        write(">");
    }

    write(" {");
    writeNewline();

    context_.increaseIndent();

    const auto& fields = node.getFields();
    for (size_t i = 0; i < fields.size(); ++i) {
        writeIndent();
        visitStructField(fields[i]);
        if (i < fields.size() - 1 || options_.trailing_commas) {
            write(",");
        }
        writeNewline();
    }

    context_.decreaseIndent();
    writeIndent();
    write("}");
    writeNewline();
}

void Formatter::visitEnumDecl(const ast::EnumDecl& node) {
    writeIndent();
    write("enum ");
    write(node.getName());
    write(" {");
    writeNewline();

    context_.increaseIndent();

    const auto& variants = node.getVariants();
    for (size_t i = 0; i < variants.size(); ++i) {
        writeIndent();
        write(variants[i].name);

        if (variants[i].value.has_value()) {
            write(" = ");
            write(std::to_string(*variants[i].value));
        }

        if (i < variants.size() - 1 || options_.trailing_commas) {
            write(",");
        }
        writeNewline();
    }

    context_.decreaseIndent();
    writeIndent();
    write("}");
    writeNewline();
}

// ============================================================================
// AST Visitor Methods - Statements
// ============================================================================

void Formatter::visitCompoundStmt(const ast::CompoundStmt& node) {
    write("{");
    writeNewline();

    context_.increaseIndent();

    const auto& stmts = node.getStatements();
    for (const auto& stmt : stmts) {
        // Visit statement based on its type
        switch (stmt->getKind()) {
            case ast::NodeKind::ExprStmt:
                visitExprStmt(*dynamic_cast<ast::ExprStmt*>(stmt.get()));
                break;
            case ast::NodeKind::ReturnStmt:
                visitReturnStmt(*dynamic_cast<ast::ReturnStmt*>(stmt.get()));
                break;
            case ast::NodeKind::IfStmt:
                visitIfStmt(*dynamic_cast<ast::IfStmt*>(stmt.get()));
                break;
            case ast::NodeKind::ForStmt:
                visitForStmt(*dynamic_cast<ast::ForStmt*>(stmt.get()));
                break;
            case ast::NodeKind::WhileStmt:
                visitWhileStmt(*dynamic_cast<ast::WhileStmt*>(stmt.get()));
                break;
            case ast::NodeKind::BreakStmt:
                visitBreakStmt(*dynamic_cast<ast::BreakStmt*>(stmt.get()));
                break;
            case ast::NodeKind::ContinueStmt:
                visitContinueStmt(*dynamic_cast<ast::ContinueStmt*>(stmt.get()));
                break;
            case ast::NodeKind::VarDeclStmt:
                visitVarDeclStmt(*dynamic_cast<ast::VarDeclStmt*>(stmt.get()));
                break;
            case ast::NodeKind::ExportStmt:
                visitExportStmt(*dynamic_cast<ast::ExportStmt*>(stmt.get()));
                break;
            case ast::NodeKind::TryStmt:
                visitTryStmt(*dynamic_cast<ast::TryStmt*>(stmt.get()));
                break;
            case ast::NodeKind::ThrowStmt:
                visitThrowStmt(*dynamic_cast<ast::ThrowStmt*>(stmt.get()));
                break;
            case ast::NodeKind::CompoundStmt:
                writeIndent();
                visitCompoundStmt(*dynamic_cast<ast::CompoundStmt*>(stmt.get()));
                writeNewline();
                break;
            default:
                // Unknown statement type
                writeIndent();
                write("/* unknown statement */");
                writeNewline();
                break;
        }
    }

    context_.decreaseIndent();
    writeIndent();
    write("}");
}

void Formatter::visitExprStmt(const ast::ExprStmt& node) {
    writeIndent();

    if (node.getExpr()) {
        // Visit expression based on its type
        auto* expr = node.getExpr();
        switch (expr->getKind()) {
            case ast::NodeKind::BinaryExpr:
                visitBinaryExpr(*dynamic_cast<ast::BinaryExpr*>(expr));
                break;
            case ast::NodeKind::UnaryExpr:
                visitUnaryExpr(*dynamic_cast<ast::UnaryExpr*>(expr));
                break;
            case ast::NodeKind::CallExpr:
                visitCallExpr(*dynamic_cast<ast::CallExpr*>(expr));
                break;
            case ast::NodeKind::MemberExpr:
                visitMemberExpr(*dynamic_cast<ast::MemberExpr*>(expr));
                break;
            case ast::NodeKind::IdentifierExpr:
                visitIdentifierExpr(*dynamic_cast<ast::IdentifierExpr*>(expr));
                break;
            case ast::NodeKind::LiteralExpr:
                visitLiteralExpr(*dynamic_cast<ast::LiteralExpr*>(expr));
                break;
            case ast::NodeKind::DictExpr:
                visitDictExpr(*dynamic_cast<ast::DictExpr*>(expr));
                break;
            case ast::NodeKind::ListExpr:
                visitListExpr(*dynamic_cast<ast::ListExpr*>(expr));
                break;
            case ast::NodeKind::RangeExpr:
                visitRangeExpr(*dynamic_cast<ast::RangeExpr*>(expr));
                break;
            case ast::NodeKind::StructLiteralExpr:
                visitStructLiteralExpr(*dynamic_cast<ast::StructLiteralExpr*>(expr));
                break;
            case ast::NodeKind::InlineCodeExpr:
                visitInlineCodeExpr(*dynamic_cast<ast::InlineCodeExpr*>(expr));
                break;
            default:
                write("/* unknown expression */");
                break;
        }
    }

    writeSemicolon();
    writeNewline();
}

void Formatter::visitReturnStmt(const ast::ReturnStmt& node) {
    writeIndent();
    write("return");

    if (node.getExpr()) {
        writeSpace();
        // Visit expression (same logic as ExprStmt)
        auto* expr = node.getExpr();
        switch (expr->getKind()) {
            case ast::NodeKind::BinaryExpr:
                visitBinaryExpr(*dynamic_cast<ast::BinaryExpr*>(expr));
                break;
            case ast::NodeKind::CallExpr:
                visitCallExpr(*dynamic_cast<ast::CallExpr*>(expr));
                break;
            case ast::NodeKind::IdentifierExpr:
                visitIdentifierExpr(*dynamic_cast<ast::IdentifierExpr*>(expr));
                break;
            case ast::NodeKind::LiteralExpr:
                visitLiteralExpr(*dynamic_cast<ast::LiteralExpr*>(expr));
                break;
            default:
                write("/* expression */");
                break;
        }
    }

    writeSemicolon();
    writeNewline();
}

void Formatter::visitIfStmt(const ast::IfStmt& node) {
    writeIndent();
    write("if ");

    // Condition (simplified - just handle basic cases)
    if (node.getCondition()) {
        auto* cond = node.getCondition();
        if (cond->getKind() == ast::NodeKind::BinaryExpr) {
            visitBinaryExpr(*dynamic_cast<ast::BinaryExpr*>(cond));
        } else if (cond->getKind() == ast::NodeKind::IdentifierExpr) {
            visitIdentifierExpr(*dynamic_cast<ast::IdentifierExpr*>(cond));
        } else {
            write("/* condition */");
        }
    }

    writeSpace();

    // Then branch
    if (node.getThenBranch()) {
        if (node.getThenBranch()->getKind() == ast::NodeKind::CompoundStmt) {
            visitCompoundStmt(*dynamic_cast<ast::CompoundStmt*>(node.getThenBranch()));
        }
    }

    // Else branch
    if (node.getElseBranch()) {
        write(" else ");

        if (node.getElseBranch()->getKind() == ast::NodeKind::CompoundStmt) {
            visitCompoundStmt(*dynamic_cast<ast::CompoundStmt*>(node.getElseBranch()));
        } else if (node.getElseBranch()->getKind() == ast::NodeKind::IfStmt) {
            // else if
            visitIfStmt(*dynamic_cast<ast::IfStmt*>(node.getElseBranch()));
        }
    }

    writeNewline();
}

void Formatter::visitForStmt(const ast::ForStmt& node) {
    writeIndent();
    write("for ");
    write(node.getVar());
    write(" in ");

    // Iterator expression (simplified)
    if (node.getIter()) {
        auto* iter = node.getIter();
        if (iter->getKind() == ast::NodeKind::IdentifierExpr) {
            visitIdentifierExpr(*dynamic_cast<ast::IdentifierExpr*>(iter));
        } else if (iter->getKind() == ast::NodeKind::RangeExpr) {
            visitRangeExpr(*dynamic_cast<ast::RangeExpr*>(iter));
        } else {
            write("/* iterator */");
        }
    }

    writeSpace();

    // Body
    if (node.getBody() && node.getBody()->getKind() == ast::NodeKind::CompoundStmt) {
        visitCompoundStmt(*dynamic_cast<ast::CompoundStmt*>(node.getBody()));
    }

    writeNewline();
}

void Formatter::visitWhileStmt(const ast::WhileStmt& node) {
    writeIndent();
    write("while ");

    // Condition (simplified)
    if (node.getCondition()) {
        auto* cond = node.getCondition();
        if (cond->getKind() == ast::NodeKind::BinaryExpr) {
            visitBinaryExpr(*dynamic_cast<ast::BinaryExpr*>(cond));
        } else {
            write("/* condition */");
        }
    }

    writeSpace();

    // Body
    if (node.getBody() && node.getBody()->getKind() == ast::NodeKind::CompoundStmt) {
        visitCompoundStmt(*dynamic_cast<ast::CompoundStmt*>(node.getBody()));
    }

    writeNewline();
}

void Formatter::visitBreakStmt(const ast::BreakStmt& node) {
    (void)node;  // Unused parameter
    writeIndent();
    write("break");
    writeSemicolon();
    writeNewline();
}

void Formatter::visitContinueStmt(const ast::ContinueStmt& node) {
    (void)node;  // Unused parameter
    writeIndent();
    write("continue");
    writeSemicolon();
    writeNewline();
}

void Formatter::visitVarDeclStmt(const ast::VarDeclStmt& node) {
    writeIndent();
    write("let ");
    write(node.getName());

    // Type annotation
    if (node.getType().has_value()) {
        write(": ");
        visitType(*node.getType());
    }

    // Initializer
    if (node.getInit()) {
        write(" = ");
        auto* init = node.getInit();

        // Visit initializer expression
        switch (init->getKind()) {
            case ast::NodeKind::LiteralExpr:
                visitLiteralExpr(*dynamic_cast<ast::LiteralExpr*>(init));
                break;
            case ast::NodeKind::BinaryExpr:
                visitBinaryExpr(*dynamic_cast<ast::BinaryExpr*>(init));
                break;
            case ast::NodeKind::CallExpr:
                visitCallExpr(*dynamic_cast<ast::CallExpr*>(init));
                break;
            case ast::NodeKind::IdentifierExpr:
                visitIdentifierExpr(*dynamic_cast<ast::IdentifierExpr*>(init));
                break;
            case ast::NodeKind::ListExpr:
                visitListExpr(*dynamic_cast<ast::ListExpr*>(init));
                break;
            case ast::NodeKind::DictExpr:
                visitDictExpr(*dynamic_cast<ast::DictExpr*>(init));
                break;
            case ast::NodeKind::StructLiteralExpr:
                visitStructLiteralExpr(*dynamic_cast<ast::StructLiteralExpr*>(init));
                break;
            default:
                write("/* initializer */");
                break;
        }
    }

    writeSemicolon();
    writeNewline();
}

void Formatter::visitExportStmt(const ast::ExportStmt& node) {
    writeIndent();
    write("export ");

    switch (node.getKind()) {
        case ast::ExportStmt::ExportKind::Function:
            if (node.getFunctionDecl()) {
                visitFunctionDecl(*node.getFunctionDecl());
            }
            break;
        case ast::ExportStmt::ExportKind::Variable:
            if (node.getVarDecl()) {
                visitVarDeclStmt(*node.getVarDecl());
            }
            break;
        case ast::ExportStmt::ExportKind::Struct:
            if (node.getStructDecl()) {
                visitStructDecl(*node.getStructDecl());
            }
            break;
        case ast::ExportStmt::ExportKind::Enum:
            if (node.getEnumDecl()) {
                visitEnumDecl(*node.getEnumDecl());
            }
            break;
        default:
            write("/* export */");
            writeNewline();
            break;
    }
}

void Formatter::visitTryStmt(const ast::TryStmt& node) {
    writeIndent();
    write("try ");

    if (node.getTryBody()) {
        visitCompoundStmt(*node.getTryBody());
    }

    if (node.getCatchClause()) {
        write(" catch (");
        write(node.getCatchClause()->error_name);
        write(") ");
        visitCompoundStmt(*node.getCatchClause()->body.get());
    }

    if (node.hasFinally() && node.getFinallyBody()) {
        write(" finally ");
        visitCompoundStmt(*node.getFinallyBody());
    }

    writeNewline();
}

void Formatter::visitThrowStmt(const ast::ThrowStmt& node) {
    writeIndent();
    write("throw ");

    if (node.getExpr()) {
        auto* expr = node.getExpr();
        if (expr->getKind() == ast::NodeKind::LiteralExpr) {
            visitLiteralExpr(*dynamic_cast<ast::LiteralExpr*>(expr));
        } else if (expr->getKind() == ast::NodeKind::IdentifierExpr) {
            visitIdentifierExpr(*dynamic_cast<ast::IdentifierExpr*>(expr));
        } else {
            write("/* expression */");
        }
    }

    writeSemicolon();
    writeNewline();
}

// ============================================================================
// Expression Visitors (Phase 1.2)
// ============================================================================

void Formatter::visitBinaryExpr(const ast::BinaryExpr& node) {
    // Format: left op right
    // Handle operator precedence by parenthesizing when needed

    auto* left = node.getLeft();
    auto* right = node.getRight();

    // Visit left operand
    if (left) {
        visitExpressionNode(left);
    }

    // Operator with spaces
    writeSpace();
    write(binaryOpToString(node.getOp()));
    writeSpace();

    // Visit right operand
    if (right) {
        visitExpressionNode(right);
    }
}

void Formatter::visitUnaryExpr(const ast::UnaryExpr& node) {
    // Format: op operand (e.g., -x, !flag, ~bits)
    write(unaryOpToString(node.getOp()));

    auto* operand = node.getOperand();
    if (operand) {
        visitExpressionNode(operand);
    }
}

void Formatter::visitCallExpr(const ast::CallExpr& node) {
    // Format: callee(args) or callee<types>(args)

    // Callee
    auto* callee = node.getCallee();
    if (callee) {
        visitExpressionNode(callee);
    }

    // Generic type arguments
    const auto& type_args = node.getTypeArguments();
    if (!type_args.empty()) {
        write("<");
        for (size_t i = 0; i < type_args.size(); ++i) {
            visitType(type_args[i]);
            if (i < type_args.size() - 1) {
                write(", ");
            }
        }
        write(">");
    }

    // Arguments
    const auto& args = node.getArgs();

    // Check if we need multi-line formatting
    size_t estimated_length = args.size() * 10;  // Rough estimate per argument

    bool multiline = shouldBreakLine(estimated_length);

    if (multiline && args.size() > 2) {
        write("(\n");
        context_.increaseIndent();

        for (size_t i = 0; i < args.size(); ++i) {
            writeIndent();
            visitExpressionNode(args[i].get());

            if (i < args.size() - 1) {
                write(",");
            } else if (options_.trailing_commas) {
                write(",");
            }

            writeNewline();
        }

        context_.decreaseIndent();
        writeIndent();
        write(")");
    } else {
        // Single-line formatting
        write("(");
        for (size_t i = 0; i < args.size(); ++i) {
            visitExpressionNode(args[i].get());
            if (i < args.size() - 1) {
                write(", ");
            }
        }
        write(")");
    }
}

void Formatter::visitMemberExpr(const ast::MemberExpr& node) {
    // Format: object.member

    auto* object = node.getObject();
    if (object) {
        visitExpressionNode(object);
    }

    write(".");
    write(node.getMember());
}

void Formatter::visitIdentifierExpr(const ast::IdentifierExpr& node) {
    write(node.getName());
}

void Formatter::visitLiteralExpr(const ast::LiteralExpr& node) {
    // Format literal based on its kind
    switch (node.getLiteralKind()) {
        case ast::LiteralKind::Int:
        case ast::LiteralKind::Float:
        case ast::LiteralKind::Bool:
            write(node.getValue());
            break;

        case ast::LiteralKind::String:
            write("\"");
            write(node.getValue());
            write("\"");
            break;

        case ast::LiteralKind::Null:
            write("null");
            break;

        default:
            write(node.getValue());
            break;
    }
}

void Formatter::visitDictExpr(const ast::DictExpr& node) {
    // Format: {"key": value, ...} or multi-line

    const auto& entries = node.getEntries();

    if (entries.empty()) {
        write("{}");
        return;
    }

    // Decide single-line vs multi-line
    bool multiline = entries.size() > 3 || shouldBreakLine(entries.size() * 15);

    if (multiline) {
        write("{\n");
        context_.increaseIndent();

        for (size_t i = 0; i < entries.size(); ++i) {
            writeIndent();
            write("\"");
            visitExpressionNode(entries[i].first.get());  // Key expression
            write("\": ");

            visitExpressionNode(entries[i].second.get());  // Value expression

            if (i < entries.size() - 1) {
                write(",");
            } else if (options_.trailing_commas) {
                write(",");
            }

            writeNewline();
        }

        context_.decreaseIndent();
        writeIndent();
        write("}");
    } else {
        // Single-line
        write("{");
        for (size_t i = 0; i < entries.size(); ++i) {
            write("\"");
            visitExpressionNode(entries[i].first.get());  // Key expression
            write("\": ");

            visitExpressionNode(entries[i].second.get());  // Value expression

            if (i < entries.size() - 1) {
                write(", ");
            }
        }
        write("}");
    }
}

void Formatter::visitListExpr(const ast::ListExpr& node) {
    // Format: [elem1, elem2, ...] or multi-line

    const auto& elements = node.getElements();

    if (elements.empty()) {
        write("[]");
        return;
    }

    // Decide single-line vs multi-line
    bool multiline = elements.size() > 3 || shouldBreakLine(elements.size() * 10);

    if (multiline) {
        write("[\n");
        context_.increaseIndent();

        for (size_t i = 0; i < elements.size(); ++i) {
            writeIndent();
            visitExpressionNode(elements[i].get());

            if (i < elements.size() - 1) {
                write(",");
            } else if (options_.trailing_commas) {
                write(",");
            }

            writeNewline();
        }

        context_.decreaseIndent();
        writeIndent();
        write("]");
    } else {
        // Single-line
        write("[");
        for (size_t i = 0; i < elements.size(); ++i) {
            visitExpressionNode(elements[i].get());
            if (i < elements.size() - 1) {
                write(", ");
            }
        }
        write("]");
    }
}

void Formatter::visitRangeExpr(const ast::RangeExpr& node) {
    // Format: start..end or start..=end

    auto* start = node.getStart();
    if (start) {
        visitExpressionNode(start);
    }

    if (node.isInclusive()) {
        write("..=");
    } else {
        write("..");
    }

    auto* end = node.getEnd();
    if (end) {
        visitExpressionNode(end);
    }
}

void Formatter::visitStructLiteralExpr(const ast::StructLiteralExpr& node) {
    // Format: StructName { field1: value1, ... } or multi-line

    write(node.getStructName());
    writeSpace();

    const auto& field_inits = node.getFieldInits();

    if (field_inits.empty()) {
        write("{}");
        return;
    }

    // Decide single-line vs multi-line
    bool multiline = field_inits.size() > 2 || shouldBreakLine(field_inits.size() * 20);

    if (multiline) {
        write("{\n");
        context_.increaseIndent();

        for (size_t i = 0; i < field_inits.size(); ++i) {
            writeIndent();
            write(field_inits[i].first);  // Field name
            write(": ");

            visitExpressionNode(field_inits[i].second.get());  // Field value

            if (i < field_inits.size() - 1) {
                write(",");
            } else if (options_.trailing_commas) {
                write(",");
            }

            writeNewline();
        }

        context_.decreaseIndent();
        writeIndent();
        write("}");
    } else {
        // Single-line
        write("{ ");
        for (size_t i = 0; i < field_inits.size(); ++i) {
            write(field_inits[i].first);  // Field name
            write(": ");

            visitExpressionNode(field_inits[i].second.get());  // Field value

            if (i < field_inits.size() - 1) {
                write(", ");
            }
        }
        write(" }");
    }
}

void Formatter::visitInlineCodeExpr(const ast::InlineCodeExpr& node) {
    // Format: <<language[vars] code >>
    // Preserve polyglot block formatting as-is

    write("<<");
    write(node.getLanguage());

    const auto& bound_vars = node.getBoundVariables();
    if (!bound_vars.empty()) {
        write("[");
        for (size_t i = 0; i < bound_vars.size(); ++i) {
            write(bound_vars[i]);
            if (i < bound_vars.size() - 1) {
                write(", ");
            }
        }
        write("]");
    }

    writeNewline();

    // Write code as-is (preserve formatting inside polyglot blocks)
    write(node.getCode());

    writeNewline();
    write(">>");
}

// Helper method to visit any expression node
void Formatter::visitExpressionNode(ast::Expr* expr) {
    if (!expr) return;

    switch (expr->getKind()) {
        case ast::NodeKind::BinaryExpr:
            visitBinaryExpr(*dynamic_cast<ast::BinaryExpr*>(expr));
            break;
        case ast::NodeKind::UnaryExpr:
            visitUnaryExpr(*dynamic_cast<ast::UnaryExpr*>(expr));
            break;
        case ast::NodeKind::CallExpr:
            visitCallExpr(*dynamic_cast<ast::CallExpr*>(expr));
            break;
        case ast::NodeKind::MemberExpr:
            visitMemberExpr(*dynamic_cast<ast::MemberExpr*>(expr));
            break;
        case ast::NodeKind::IdentifierExpr:
            visitIdentifierExpr(*dynamic_cast<ast::IdentifierExpr*>(expr));
            break;
        case ast::NodeKind::LiteralExpr:
            visitLiteralExpr(*dynamic_cast<ast::LiteralExpr*>(expr));
            break;
        case ast::NodeKind::DictExpr:
            visitDictExpr(*dynamic_cast<ast::DictExpr*>(expr));
            break;
        case ast::NodeKind::ListExpr:
            visitListExpr(*dynamic_cast<ast::ListExpr*>(expr));
            break;
        case ast::NodeKind::RangeExpr:
            visitRangeExpr(*dynamic_cast<ast::RangeExpr*>(expr));
            break;
        case ast::NodeKind::StructLiteralExpr:
            visitStructLiteralExpr(*dynamic_cast<ast::StructLiteralExpr*>(expr));
            break;
        case ast::NodeKind::InlineCodeExpr:
            visitInlineCodeExpr(*dynamic_cast<ast::InlineCodeExpr*>(expr));
            break;
        default:
            write("/* unknown expression */");
            break;
    }
}

// ============================================================================
// Helper Functions - Types and Parameters
// ============================================================================

void Formatter::visitType(const ast::Type& type) {
    switch (type.kind) {
        case ast::TypeKind::Int:
            write("int");
            break;
        case ast::TypeKind::Float:
            write("float");
            break;
        case ast::TypeKind::String:
            write("string");
            break;
        case ast::TypeKind::Bool:
            write("bool");
            break;
        case ast::TypeKind::Void:
            write("void");
            break;
        case ast::TypeKind::Any:
            write("any");
            break;
        case ast::TypeKind::List:
            write("list<");
            if (type.element_type) {
                visitType(*type.element_type);
            } else {
                write("any");
            }
            write(">");
            break;
        case ast::TypeKind::Dict:
            write("dict<");
            if (type.key_value_types) {
                visitType(type.key_value_types->first);
                write(", ");
                visitType(type.key_value_types->second);
            } else {
                write("string, any");
            }
            write(">");
            break;
        case ast::TypeKind::Struct:
            if (!type.module_prefix.empty()) {
                write(type.module_prefix);
                write(".");
            }
            write(type.struct_name);
            break;
        case ast::TypeKind::Enum:
            write(type.enum_name);
            break;
        case ast::TypeKind::Function:
            write("function");
            break;
        default:
            write("unknown");
            break;
    }

    if (type.is_nullable) {
        write("?");
    }
}

void Formatter::visitParameter(const ast::Parameter& param) {
    write(param.name);
    write(": ");
    visitType(param.type);

    // Default value not yet supported in formatter
}

void Formatter::visitStructField(const ast::StructField& field) {
    write(field.name);
    write(": ");
    visitType(field.type);
}

} // namespace formatter
} // namespace naab
