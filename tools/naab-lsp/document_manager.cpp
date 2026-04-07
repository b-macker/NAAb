#include "document_manager.h"
#include <sstream>
#include <algorithm>
#include <iostream>
#include <unistd.h>
#include <sys/wait.h>

namespace naab {
namespace lsp {

// ============================================================================
// Position
// ============================================================================

Position Position::fromJson(const json& j) {
    return Position{
        j["line"].get<int>(),
        j["character"].get<int>()
    };
}

json Position::toJson() const {
    return {
        {"line", line},
        {"character", character}
    };
}

// ============================================================================
// Range
// ============================================================================

Range Range::fromJson(const json& j) {
    return Range{
        Position::fromJson(j["start"]),
        Position::fromJson(j["end"])
    };
}

json Range::toJson() const {
    return {
        {"start", start.toJson()},
        {"end", end.toJson()}
    };
}

// ============================================================================
// Diagnostic
// ============================================================================

json Diagnostic::toJson() const {
    return {
        {"range", range.toJson()},
        {"severity", static_cast<int>(severity)},
        {"code", code},
        {"message", message},
        {"source", source}
    };
}

// ============================================================================
// Document
// ============================================================================

Document::Document(const std::string& uri, const std::string& text, int version)
    : uri_(uri), text_(text), version_(version) {
    parse();
    typeCheck();
    runGovernanceChecks();
}

void Document::update(const std::string& new_text, int new_version) {
    text_ = new_text;
    version_ = new_version;

    // Re-analyze
    parse();
    typeCheck();
    runGovernanceChecks();
}

void Document::parse() {
    std::cerr << "[Document::parse] Starting parse for " << uri_ << "\n";
    diagnostics_.clear();

    try {
        // Tokenize
        std::cerr << "[Document::parse] Tokenizing...\n";
        lexer::Lexer lexer(text_);
        tokens_ = lexer.tokenize();
        std::cerr << "[Document::parse] Got " << tokens_.size() << " tokens\n";

        // Parse
        std::cerr << "[Document::parse] Parsing...\n";
        parser::Parser parser(tokens_);
        parser.setSource(text_, uri_);
        ast_ = parser.parseProgram();
        std::cerr << "[Document::parse] AST created: " << (ast_ ? "yes" : "no") << "\n";

        // Build symbol table
        std::cerr << "[Document::parse] Calling buildSymbolTable...\n";
        buildSymbolTable();

        // Collect parse errors
        collectDiagnostics();

    } catch (const std::exception& e) {
        std::cerr << "[Document::parse] Exception caught: " << e.what() << "\n";
        // Add diagnostic for parse failure
        Diagnostic diag;
        diag.range = Range{{0, 0}, {0, 0}};
        diag.severity = DiagnosticSeverity::Error;
        diag.code = "parse-error";
        diag.message = std::string("Parse error: ") + e.what();
        diagnostics_.push_back(diag);
    } catch (...) {
        std::cerr << "[Document::parse] Unknown exception caught!\n";
        Diagnostic diag;
        diag.range = Range{{0, 0}, {0, 0}};
        diag.severity = DiagnosticSeverity::Error;
        diag.code = "parse-error";
        diag.message = "Unknown parse error";
        diagnostics_.push_back(diag);
    }
}

void Document::typeCheck() {
    if (!ast_) return;

    try {
        // Run type checker
        typecheck::TypeChecker checker;

        // Create a non-owning shared_ptr for type checker
        std::shared_ptr<ast::Program> ast_ptr(ast_.get(), [](ast::Program*){});
        auto type_errors = checker.check(ast_ptr);

        // Convert type errors to diagnostics
        for (const auto& err : type_errors) {
            Diagnostic diag;
            diag.range = Range{
                {static_cast<int>(err.line), static_cast<int>(err.column)},
                {static_cast<int>(err.line), static_cast<int>(err.column) + 1}
            };
            diag.severity = DiagnosticSeverity::Error;
            diag.code = "type-error";
            diag.message = err.message;
            diagnostics_.push_back(diag);
        }

    } catch (const std::exception& e) {
        // Type checker failure (should not happen)
        Diagnostic diag;
        diag.range = Range{{0, 0}, {0, 0}};
        diag.severity = DiagnosticSeverity::Error;
        diag.code = "type-check-error";
        diag.message = std::string("Type check error: ") + e.what();
        diagnostics_.push_back(diag);
    }
}

void Document::collectDiagnostics() {
    // TODO: Extract diagnostics from parser's error reporter
    // For now, if we got here, parsing succeeded
}

// Helper: convert file:// URI to filesystem path
static std::string uriToPath(const std::string& uri) {
    if (uri.substr(0, 7) == "file://") {
        return uri.substr(7);
    }
    return uri;
}

// Helper: parse a single governance error/warning block into a Diagnostic
static Diagnostic parseGovernanceBlock(const std::string& block) {
    Diagnostic diag;
    diag.source = "naab-governance";

    // Determine severity from tag
    if (block.find("[HARD-MANDATORY]") != std::string::npos) {
        diag.severity = DiagnosticSeverity::Error;
    } else if (block.find("[SOFT-MANDATORY]") != std::string::npos) {
        diag.severity = DiagnosticSeverity::Warning;
    } else if (block.find("[ADVISORY]") != std::string::npos) {
        diag.severity = DiagnosticSeverity::Hint;
    } else {
        diag.severity = DiagnosticSeverity::Warning;
    }

    // Extract the "what" message from first line
    // Format: "Governance error: <what> [TAG]"  or  "Governance warning: <what> [TAG]"
    std::string message;
    auto bracket_pos = block.find(" [");
    if (bracket_pos != std::string::npos) {
        auto colon_pos = block.find(": ");
        if (colon_pos != std::string::npos && colon_pos < bracket_pos) {
            message = block.substr(colon_pos + 2, bracket_pos - colon_pos - 2);
        }
    }
    if (message.empty()) {
        auto nl = block.find('\n');
        message = (nl != std::string::npos) ? block.substr(0, nl) : block;
    }
    diag.message = message;

    // Extract line number from "At: line N" or "At: <file>:N"
    int line = 0;
    auto at_pos = block.find("  At: ");
    if (at_pos != std::string::npos) {
        std::string at_str = block.substr(at_pos + 6);
        auto nl = at_str.find('\n');
        if (nl != std::string::npos) at_str = at_str.substr(0, nl);

        // Try "line N" format
        if (at_str.substr(0, 5) == "line ") {
            try { line = std::stoi(at_str.substr(5)); } catch (...) {}
        }
        // Try "<file>:N" format
        if (line == 0) {
            auto colon = at_str.rfind(':');
            if (colon != std::string::npos) {
                try { line = std::stoi(at_str.substr(colon + 1)); } catch (...) {}
            }
        }
    }

    // Extract rule for diagnostic code
    auto rule_pos = block.find("  Rule (govern.json): ");
    if (rule_pos != std::string::npos) {
        std::string rule_str = block.substr(rule_pos + 22);
        auto nl = rule_str.find('\n');
        if (nl != std::string::npos) rule_str = rule_str.substr(0, nl);
        diag.code = rule_str;
    } else {
        diag.code = "governance";
    }

    // Also check for taint tracking violation format:
    // "Taint tracking violation: variable 'x' ... at <file>:<line>"
    if (block.find("Taint tracking violation:") != std::string::npos) {
        diag.severity = DiagnosticSeverity::Error;
        diag.code = "taint-tracking";
        // Extract line from "at <file>:<line>"
        auto at_file_pos = block.rfind(" at ");
        if (at_file_pos != std::string::npos) {
            auto colon = block.rfind(':');
            if (colon != std::string::npos && colon > at_file_pos) {
                try { line = std::stoi(block.substr(colon + 1)); } catch (...) {}
            }
        }
    }

    // LSP uses 0-based lines; governance uses 1-based
    int lsp_line = (line > 0) ? line - 1 : 0;
    diag.range = Range{{lsp_line, 0}, {lsp_line, 1000}};

    return diag;
}

// Run naab-lang as subprocess and capture combined output
// Governance errors are thrown as exceptions and printed to stderr
static std::string runNaabGovernance(const std::string& naab_path,
                                      const std::string& file_path) {
    // V-LSP-003: use fork/execvp instead of popen to avoid shell injection.
    // file_path comes from an untrusted LSP URI; passing it through a shell
    // would allow metacharacters to execute arbitrary commands.
    int pipefd[2];
    if (pipe(pipefd) != 0) return "";

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return "";
    }

    if (pid == 0) {
        // Child: redirect stdout+stderr to pipe write end, then exec
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        const char* argv[] = { naab_path.c_str(), file_path.c_str(), nullptr };
        execvp(naab_path.c_str(), const_cast<char* const*>(argv));
        _exit(127);  // execvp failed
    }

    // Parent: read from pipe read end
    close(pipefd[1]);
    std::string result;
    char buffer[4096];
    ssize_t n;
    while ((n = read(pipefd[0], buffer, sizeof(buffer))) > 0) {
        result.append(buffer, static_cast<size_t>(n));
    }
    close(pipefd[0]);
    waitpid(pid, nullptr, 0);
    return result;
}

void Document::runGovernanceChecks() {
    std::string file_path = uriToPath(uri_);

    // Find naab-lang binary relative to naab-lsp
    // naab-lsp is at build/naab-lsp, naab-lang is at build/naab-lang
    if (naab_lang_path_.empty()) {
        // Try to find naab-lang in same directory as naab-lsp, or in PATH
        // Read /proc/self/exe to find our own path
        char self_path[4096];
        ssize_t len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
        if (len > 0) {
            self_path[len] = '\0';
            std::string self_dir(self_path);
            auto slash = self_dir.rfind('/');
            if (slash != std::string::npos) {
                naab_lang_path_ = self_dir.substr(0, slash) + "/naab-lang";
            }
        }
        // Fallback: try "naab-lang" in PATH
        if (naab_lang_path_.empty()) {
            naab_lang_path_ = "naab-lang";
        }
        std::cerr << "[Document] naab-lang path: " << naab_lang_path_ << "\n";
    }

    // Run governance checks via subprocess
    std::string output = runNaabGovernance(naab_lang_path_, file_path);
    if (output.empty()) return;

    std::cerr << "[Document] Governance output (" << output.size() << " bytes)\n";

    // Parse output: split on "Governance error:" or "Governance warning:" or "Taint tracking violation:"
    // Each block starts with one of these prefixes
    std::vector<std::string> blocks;
    std::string current_block;
    std::istringstream iss(output);
    std::string line;

    while (std::getline(iss, line)) {
        if ((line.find("Governance error:") == 0 ||
             line.find("Governance warning:") == 0 ||
             line.find("Taint tracking violation:") == 0) &&
            !current_block.empty()) {
            blocks.push_back(current_block);
            current_block.clear();
        }
        current_block += line + "\n";
    }
    if (!current_block.empty()) {
        blocks.push_back(current_block);
    }

    // Convert each block to a diagnostic
    for (const auto& block : blocks) {
        if (block.find("Governance error:") != std::string::npos ||
            block.find("Governance warning:") != std::string::npos ||
            block.find("Taint tracking violation:") != std::string::npos) {
            diagnostics_.push_back(parseGovernanceBlock(block));
        }
    }

    std::cerr << "[Document] Governance produced " << diagnostics_.size() << " total diagnostics\n";
}

// Forward declaration
static void extractVariablesFromStmt(ast::Stmt* stmt, semantic::SymbolTable& symbol_table, const std::string& uri);

// Helper function to convert ast::Type to string for symbol table
static std::string astTypeToString(const ast::Type& type) {
    switch (type.kind) {
        case ast::TypeKind::Void: return "void";
        case ast::TypeKind::Int: return "int";
        case ast::TypeKind::Float: return "float";
        case ast::TypeKind::String: return "string";
        case ast::TypeKind::Bool: return "bool";
        case ast::TypeKind::Any: return "any";
        case ast::TypeKind::Block: return "block";
        case ast::TypeKind::Struct:
            return type.struct_name.empty() ? "struct" : type.struct_name;
        case ast::TypeKind::Enum:
            return type.enum_name.empty() ? "enum" : type.enum_name;
        case ast::TypeKind::List:
            if (type.element_type) {
                return "list[" + astTypeToString(*type.element_type) + "]";
            }
            return "list";
        case ast::TypeKind::Dict:
            if (type.key_value_types) {
                return "dict[" + astTypeToString(type.key_value_types->first) + ", " +
                       astTypeToString(type.key_value_types->second) + "]";
            }
            return "dict";
        case ast::TypeKind::Function: return "function";
        case ast::TypeKind::TypeParameter:
            return type.type_parameter_name.empty() ? "T" : type.type_parameter_name;
        default: return "unknown";
    }
}

void Document::buildSymbolTable() {
    if (!ast_) {
        std::cerr << "[Document] AST is null, cannot build symbol table\n";
        return;
    }

    // Clear existing symbols
    symbol_table_ = semantic::SymbolTable();

    std::cerr << "[Document] Building symbol table for " << uri_ << "\n";

    // Walk AST and populate symbol table
    ast::Program* program = ast_.get();

    // Functions
    for (const auto& func : program->getFunctions()) {
        auto loc = func->getLocation();

        // Create function signature type string
        std::ostringstream sig;
        sig << "(";
        for (size_t i = 0; i < func->getParams().size(); ++i) {
            if (i > 0) sig << ", ";
            const auto& param = func->getParams()[i];
            sig << param.name << ": " << astTypeToString(param.type);
        }
        sig << ") -> " << astTypeToString(func->getReturnType());

        // AST lines are 1-based, LSP is 0-based — subtract 1
        semantic::Symbol symbol(
            func->getName(),
            semantic::SymbolKind::Function,
            sig.str(),
            semantic::SourceLocation(uri_, loc.line > 0 ? loc.line - 1 : 0, loc.column)
        );
        symbol_table_.define(func->getName(), std::move(symbol));
        std::cerr << "  Added function: " << func->getName() << " " << sig.str() << "\n";

        // TODO: Add parameters and local variables in function scope
    }

    // Structs
    for (const auto& struct_decl : program->getStructs()) {
        auto loc = struct_decl->getLocation();
        semantic::Symbol symbol(
            struct_decl->getName(),
            semantic::SymbolKind::Class,
            "struct",
            semantic::SourceLocation(uri_, loc.line > 0 ? loc.line - 1 : 0, loc.column)
        );
        symbol_table_.define(struct_decl->getName(), std::move(symbol));
        std::cerr << "  Added struct: " << struct_decl->getName() << "\n";
    }

    // Enums
    for (const auto& enum_decl : program->getEnums()) {
        auto loc = enum_decl->getLocation();
        semantic::Symbol symbol(
            enum_decl->getName(),
            semantic::SymbolKind::Enum,
            "enum",
            semantic::SourceLocation(uri_, loc.line > 0 ? loc.line - 1 : 0, loc.column)
        );
        symbol_table_.define(enum_decl->getName(), std::move(symbol));
        std::cerr << "  Added enum: " << enum_decl->getName() << "\n";
    }

    // Main block - extract top-level variables
    if (program->getMainBlock()) {
        extractVariablesFromStmt(program->getMainBlock()->getBody(), symbol_table_, uri_);
    }

    std::cerr << "[Document] Symbol table has " << symbol_table_.get_all_symbols().size() << " symbols\n";
}

// Helper to extract variable declarations from statements
static void extractVariablesFromStmt(ast::Stmt* stmt, semantic::SymbolTable& symbol_table, const std::string& uri) {
    if (!stmt) return;

    // Check if this is a VarDeclStmt
    if (stmt->getKind() == ast::NodeKind::VarDeclStmt) {
        ast::VarDeclStmt* var_decl = static_cast<ast::VarDeclStmt*>(stmt);
        auto loc = var_decl->getLocation();

        std::string type_str = "any";
        if (var_decl->getType().has_value()) {
            type_str = astTypeToString(*var_decl->getType());
        }

        // AST lines are 1-based, LSP is 0-based
        semantic::Symbol symbol(
            var_decl->getName(),
            semantic::SymbolKind::Variable,
            type_str,
            semantic::SourceLocation(uri, loc.line > 0 ? loc.line - 1 : 0, loc.column)
        );
        symbol_table.define(var_decl->getName(), std::move(symbol));
        std::cerr << "  Added variable: " << var_decl->getName() << ": " << type_str << "\n";
    }
    // If compound statement, walk all children
    else if (stmt->getKind() == ast::NodeKind::CompoundStmt) {
        ast::CompoundStmt* compound = static_cast<ast::CompoundStmt*>(stmt);
        for (const auto& child_stmt : compound->getStatements()) {
            extractVariablesFromStmt(child_stmt.get(), symbol_table, uri);
        }
    }
    // TODO: Handle if statements, for loops, etc. that have nested scopes
}

std::string Document::getLineText(int line) const {
    std::istringstream iss(text_);
    std::string line_text;

    for (int i = 0; i <= line && std::getline(iss, line_text); ++i) {
        if (i == line) {
            return line_text;
        }
    }

    return "";
}

Position Document::offsetToPosition(size_t offset) const {
    int line = 0;
    int character = 0;

    for (size_t i = 0; i < offset && i < text_.size(); ++i) {
        if (text_[i] == '\n') {
            ++line;
            character = 0;
        } else {
            ++character;
        }
    }

    return Position{line, character};
}

size_t Document::positionToOffset(const Position& pos) const {
    std::istringstream iss(text_);
    std::string line_text;
    size_t offset = 0;

    for (int line = 0; line < pos.line && std::getline(iss, line_text); ++line) {
        offset += line_text.length() + 1;  // +1 for newline
    }

    offset += static_cast<size_t>(pos.character);
    return offset;
}

// ============================================================================
// DocumentManager
// ============================================================================

DocumentManager::DocumentManager() = default;

void DocumentManager::open(const std::string& uri, const std::string& text, int version) {
    documents_[uri] = std::make_unique<Document>(uri, text, version);
}

void DocumentManager::update(const std::string& uri, const std::string& text, int version) {
    auto it = documents_.find(uri);
    if (it != documents_.end()) {
        it->second->update(text, version);
    }
}

void DocumentManager::close(const std::string& uri) {
    documents_.erase(uri);
}

Document* DocumentManager::getDocument(const std::string& uri) {
    auto it = documents_.find(uri);
    return (it != documents_.end()) ? it->second.get() : nullptr;
}

bool DocumentManager::hasDocument(const std::string& uri) const {
    return documents_.find(uri) != documents_.end();
}

std::vector<Document*> DocumentManager::getAllDocuments() {
    std::vector<Document*> docs;
    for (auto& [uri, doc] : documents_) {
        docs.push_back(doc.get());
    }
    return docs;
}

} // namespace lsp
} // namespace naab
