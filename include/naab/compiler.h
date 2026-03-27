#pragma once
// NAAb Bytecode Compiler — AST to bytecode compilation
// Walks the AST via the visitor pattern, emitting bytecode into Chunks.

#include "ast.h"
#include "vm.h"

#include <string>
#include <vector>

namespace naab {

// Forward declarations
namespace governance { class GovernanceEngine; }

namespace vm {

class Compiler : public ast::ASTVisitor {
public:
    Compiler();
    ~Compiler();

    // Compile a full program -> top-level CompiledFunction
    CompiledFunction* compile(ast::Program& program, const std::string& source_file = "");

    // Compile a module -> CompiledFunction (for imports)
    CompiledFunction* compileModule(ast::Program& program, const std::string& source_file = "");

    // Governance-aware compilation
    void setGovernance(governance::GovernanceEngine* gov) { governance_ = gov; }

    const std::string& getLastError() const { return last_error_; }

    // Transfer ownership of all compiled functions (for keeping them alive after compiler dies)
    std::vector<std::unique_ptr<CompiledFunction>> takeCompiledFunctions() {
        return std::move(compiled_functions_);
    }

private:
    // Nested compiler state (one per function scope)
    struct CompilerState {
        CompiledFunction* function = nullptr;

        struct Local {
            std::string name;
            int depth = -1;         // Scope depth (-1 = uninitialized)
            bool is_captured = false;
        };
        std::vector<Local> locals;
        std::vector<UpvalueDesc> upvalues;
        int scope_depth = 0;
        CompilerState* enclosing = nullptr;

        // Loop break/continue tracking
        struct LoopContext {
            int start;                          // Loop start offset
            std::vector<int> break_jumps;       // Offsets to patch on break
            int scope_depth;                    // Depth at loop start
        };
        std::vector<LoopContext> loops;
    };

    CompilerState* current_ = nullptr;
    governance::GovernanceEngine* governance_ = nullptr;
    std::string last_error_;
    std::string source_file_;
    bool skip_main_ = false;  // True when compiling a module (skip main blocks)

    // Owned compiled functions (for memory management)
    std::vector<std::unique_ptr<CompiledFunction>> compiled_functions_;

    // Scope management
    void beginScope();
    void endScope();
    int resolveLocal(CompilerState* state, const std::string& name);
    int resolveUpvalue(CompilerState* state, const std::string& name);
    int addLocal(const std::string& name);
    void markInitialized();

    // Emission helpers
    Chunk& currentChunk() { return current_->function->chunk; }
    void emitOp(OpCode op, int line);
    void emitWide(OpCode op, uint32_t arg, int line);
    void emit3(OpCode op, uint8_t a, uint8_t b, uint8_t c, int line);
    int emitJump(OpCode op, int line);
    void patchJump(int offset);
    void emitLoop(int loop_start, int line);
    int makeConstant(interpreter::NaabVal value);
    int identifierConstant(const std::string& name);

    // Variable emission (resolves local vs upvalue vs global)
    void emitGetVariable(const std::string& name, int line);
    void emitSetVariable(const std::string& name, int line);

    // Compile a function body (shared by FunctionDecl, LambdaExpr, FunctionDeclStmt)
    void compileFunctionBody(const std::string& name,
                             const std::vector<ast::Parameter>& params,
                             ast::Stmt* body, bool is_async, int line);

    // Create a new CompiledFunction, return raw pointer (owned by compiled_functions_)
    CompiledFunction* newFunction(const std::string& name = "");

    // Begin compiling a new function body
    void beginFunction(CompiledFunction* fn);
    CompiledFunction* endFunction();

    // ========================================================================
    // Visitor implementations (AST -> bytecode)
    // ========================================================================
    void visit(ast::Program& node) override;
    void visit(ast::UseStatement& node) override;
    void visit(ast::FunctionDecl& node) override;
    void visit(ast::MainBlock& node) override;
    void visit(ast::StructDecl& node) override;
    void visit(ast::EnumDecl& node) override;
    void visit(ast::InterfaceDecl& node) override;
    void visit(ast::FunctionDeclStmt& node) override;
    void visit(ast::StructDeclStmt& node) override;
    void visit(ast::RuntimeDeclStmt& node) override;
    void visit(ast::DestructureStmt& node) override;
    void visit(ast::CompoundStmt& node) override;
    void visit(ast::ExprStmt& node) override;
    void visit(ast::ReturnStmt& node) override;
    void visit(ast::IfStmt& node) override;
    void visit(ast::ForStmt& node) override;
    void visit(ast::WhileStmt& node) override;
    void visit(ast::BreakStmt& node) override;
    void visit(ast::ContinueStmt& node) override;
    void visit(ast::VarDeclStmt& node) override;
    void visit(ast::ImportStmt& node) override;
    void visit(ast::ExportStmt& node) override;
    void visit(ast::TryStmt& node) override;
    void visit(ast::ThrowStmt& node) override;
    void visit(ast::ModuleUseStmt& node) override;
    void visit(ast::BinaryExpr& node) override;
    void visit(ast::UnaryExpr& node) override;
    void visit(ast::CallExpr& node) override;
    void visit(ast::MemberExpr& node) override;
    void visit(ast::IdentifierExpr& node) override;
    void visit(ast::LiteralExpr& node) override;
    void visit(ast::DictExpr& node) override;
    void visit(ast::ListExpr& node) override;
    void visit(ast::RangeExpr& node) override;
    void visit(ast::StructLiteralExpr& node) override;
    void visit(ast::InlineCodeExpr& node) override;
    void visit(ast::IfExpr& node) override;
    void visit(ast::LambdaExpr& node) override;
    void visit(ast::MatchExpr& node) override;
    void visit(ast::AwaitExpr& node) override;
    void visit(ast::YieldExpr& node) override;
};

} // namespace vm
} // namespace naab
