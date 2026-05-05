#include "naab/compiler.h"
#include "naab/governance.h"
#include "naab/lexer.h"
#include "naab/parser.h"
#include <stdexcept>
#include <unordered_set>
#include <fstream>

namespace naab {
namespace vm {

// ============================================================================
// Compiler lifecycle
// ============================================================================

Compiler::Compiler() = default;
Compiler::~Compiler() = default;

CompiledFunction* Compiler::newFunction(const std::string& name) {
    auto fn = std::make_unique<CompiledFunction>();
    fn->name = name;
    fn->source_file = source_file_;
    CompiledFunction* raw = fn.get();
    compiled_functions_.push_back(std::move(fn));
    return raw;
}

void Compiler::beginFunction(CompiledFunction* fn) {
    auto* state = new CompilerState();
    state->function = fn;
    state->enclosing = current_;
    current_ = state;

    // Reserve slot 0 for the function itself (or "this" in methods)
    addLocal("");
    markInitialized();
}

CompiledFunction* Compiler::endFunction() {
    // Emit implicit return null (unless last instruction is already a RETURN)
    auto& code = currentChunk().code;
    bool already_returns = false;
    if (!code.empty()) {
        OpCode last_op = decodeOp(code.back());
        if (last_op == OpCode::OP_RETURN || last_op == OpCode::OP_RETURN_NULL) {
            already_returns = true;
        }
    }
    if (!already_returns) {
        emitOp(OpCode::OP_RETURN_NULL, 0);
    }

    CompiledFunction* fn = current_->function;
    fn->local_count = static_cast<int>(current_->locals.size());
    fn->upvalues = current_->upvalues;

    // Populate debug info: slot index → variable name
    fn->local_names.resize(current_->locals.size());
    for (size_t i = 0; i < current_->locals.size(); i++) {
        fn->local_names[i] = current_->locals[i].name;
    }

    CompilerState* old = current_;
    current_ = current_->enclosing;
    delete old;

    return fn;
}

// ============================================================================
// Compilation entry points
// ============================================================================

CompiledFunction* Compiler::compile(ast::Program& program, const std::string& source_file) {
    source_file_ = source_file;

    CompiledFunction* fn = newFunction("<script>");
    fn->source_file = source_file;
    beginFunction(fn);

    // Visit the program
    program.accept(*this);

    return endFunction();
}

CompiledFunction* Compiler::compileModule(ast::Program& program, const std::string& source_file) {
    source_file_ = source_file;
    skip_main_ = true;  // Skip main blocks when compiling modules

    CompiledFunction* fn = newFunction("<module>");
    fn->source_file = source_file;
    beginFunction(fn);

    program.accept(*this);

    skip_main_ = false;
    return endFunction();
}

// ============================================================================
// Scope management
// ============================================================================

void Compiler::beginScope() {
    current_->scope_depth++;
}

void Compiler::endScope() {
    current_->scope_depth--;

    // Pop locals that went out of scope
    while (!current_->locals.empty() &&
           current_->locals.back().depth > current_->scope_depth) {
        if (current_->locals.back().is_captured) {
            emitWide(OpCode::OP_CLOSE_UPVALUE,
                     static_cast<uint32_t>(current_->locals.size() - 1), 0);
        } else {
            emitOp(OpCode::OP_POP, 0);
        }
        current_->locals.pop_back();
    }
}

int Compiler::addLocal(const std::string& name) {
    CompilerState::Local local;
    local.name = name;
    local.depth = -1;  // Not yet initialized
    local.is_captured = false;
    current_->locals.push_back(std::move(local));
    return static_cast<int>(current_->locals.size()) - 1;
}

void Compiler::markInitialized() {
    if (!current_->locals.empty()) {
        current_->locals.back().depth = current_->scope_depth;
    }
}

int Compiler::resolveLocal(CompilerState* state, const std::string& name) {
    for (int i = static_cast<int>(state->locals.size()) - 1; i >= 0; i--) {
        if (state->locals[static_cast<size_t>(i)].name == name) {
            return i;
        }
    }
    return -1;
}

int Compiler::resolveUpvalue(CompilerState* state, const std::string& name) {
    if (state->enclosing == nullptr) return -1;

    // Check enclosing function's locals
    int local = resolveLocal(state->enclosing, name);
    if (local != -1) {
        state->enclosing->locals[static_cast<size_t>(local)].is_captured = true;
        // Add upvalue descriptor
        for (size_t i = 0; i < state->upvalues.size(); i++) {
            if (state->upvalues[i].index == static_cast<uint8_t>(local) &&
                state->upvalues[i].is_local) {
                return static_cast<int>(i);
            }
        }
        UpvalueDesc desc;
        desc.index = static_cast<uint8_t>(local);
        desc.is_local = true;
        state->upvalues.push_back(desc);
        return static_cast<int>(state->upvalues.size()) - 1;
    }

    // Check enclosing function's upvalues (recursive)
    int upvalue = resolveUpvalue(state->enclosing, name);
    if (upvalue != -1) {
        for (size_t i = 0; i < state->upvalues.size(); i++) {
            if (state->upvalues[i].index == static_cast<uint8_t>(upvalue) &&
                !state->upvalues[i].is_local) {
                return static_cast<int>(i);
            }
        }
        UpvalueDesc desc;
        desc.index = static_cast<uint8_t>(upvalue);
        desc.is_local = false;
        state->upvalues.push_back(desc);
        return static_cast<int>(state->upvalues.size()) - 1;
    }

    return -1;
}

// ============================================================================
// Emission helpers
// ============================================================================

void Compiler::emitOp(OpCode op, int line) {
    currentChunk().emit(encodeWide(op, 0), line);
}

void Compiler::emitWide(OpCode op, uint32_t arg, int line) {
    currentChunk().emit(encodeWide(op, arg), line);
}

void Compiler::emit3(OpCode op, uint8_t a, uint8_t b, uint8_t c, int line) {
    currentChunk().emit(encode3(op, a, b, c), line);
}

int Compiler::emitJump(OpCode op, int line) {
    return currentChunk().emitJump(op, line);
}

void Compiler::patchJump(int offset) {
    currentChunk().patchJump(offset);
}

void Compiler::emitLoop(int loop_start, int line) {
    int offset = currentChunk().codeSize() - loop_start + 1;
    if (offset > 0xFFFFFF) {
        throw std::runtime_error("Loop body too large");
    }
    emitWide(OpCode::OP_JUMP_BACK, static_cast<uint32_t>(offset), line);
}

int Compiler::makeConstant(interpreter::NaabVal value) {
    return currentChunk().addConstant(std::move(value));
}

int Compiler::identifierConstant(const std::string& name) {
    return makeConstant(interpreter::NaabVal::makeString(name));
}

void Compiler::emitGetVariable(const std::string& name, int line) {
    int slot = resolveLocal(current_, name);
    if (slot != -1) {
        emitWide(OpCode::OP_GET_LOCAL, static_cast<uint32_t>(slot), line);
    } else {
        int upvalue = resolveUpvalue(current_, name);
        if (upvalue != -1) {
            emitWide(OpCode::OP_GET_UPVALUE, static_cast<uint32_t>(upvalue), line);
        } else {
            emitWide(OpCode::OP_GET_GLOBAL, static_cast<uint32_t>(identifierConstant(name)), line);
        }
    }
}

void Compiler::emitSetVariable(const std::string& name, int line) {
    int slot = resolveLocal(current_, name);
    if (slot != -1) {
        emitWide(OpCode::OP_SET_LOCAL, static_cast<uint32_t>(slot), line);
    } else {
        int upvalue = resolveUpvalue(current_, name);
        if (upvalue != -1) {
            emitWide(OpCode::OP_SET_UPVALUE, static_cast<uint32_t>(upvalue), line);
        } else {
            emitWide(OpCode::OP_SET_GLOBAL, static_cast<uint32_t>(identifierConstant(name)), line);
        }
    }
}

// ============================================================================
// Function compilation helper
// ============================================================================

void Compiler::compileFunctionBody(const std::string& name,
                                   const std::vector<ast::Parameter>& params,
                                   ast::Stmt* body, bool is_async, int line) {
    CompiledFunction* fn = newFunction(name);
    fn->arity = 0;
    fn->max_arity = static_cast<int>(params.size());
    fn->is_async = is_async;
    fn->source_line = line;

    // Detect if function body contains yield → mark as generator
    std::function<bool(ast::Stmt*)> containsYield = [&](ast::Stmt* stmt) -> bool {
        if (!stmt) return false;
        if (auto* compound = dynamic_cast<ast::CompoundStmt*>(stmt)) {
            for (auto& s : compound->getStatements()) {
                if (containsYield(s.get())) return true;
            }
        } else if (auto* expr_stmt = dynamic_cast<ast::ExprStmt*>(stmt)) {
            if (dynamic_cast<ast::YieldExpr*>(expr_stmt->getExpr())) return true;
        } else if (auto* if_stmt = dynamic_cast<ast::IfStmt*>(stmt)) {
            if (containsYield(if_stmt->getThenBranch())) return true;
            if (if_stmt->getElseBranch() && containsYield(if_stmt->getElseBranch())) return true;
        } else if (auto* for_stmt = dynamic_cast<ast::ForStmt*>(stmt)) {
            if (containsYield(for_stmt->getBody())) return true;
        } else if (auto* while_stmt = dynamic_cast<ast::WhileStmt*>(stmt)) {
            if (containsYield(while_stmt->getBody())) return true;
        } else if (auto* try_stmt = dynamic_cast<ast::TryStmt*>(stmt)) {
            if (containsYield(try_stmt->getTryBody())) return true;
            if (try_stmt->getCatchClause())
                if (containsYield(try_stmt->getCatchClause()->body.get())) return true;
        }
        return false;
    };
    fn->is_generator = containsYield(body);

    // Count required params (those without defaults)
    for (auto& param : params) {
        if (!param.default_value.has_value()) {
            fn->arity++;
        }
    }

    beginFunction(fn);

    // Parameters become local variables in order
    for (auto& param : params) {
        addLocal(param.name);
        markInitialized();
    }

    // Compile the body
    beginScope();
    body->accept(*this);

    // Implicit return: detect last statement type and emit return value before endScope.
    // Tree-walker returns the last evaluated value when no explicit return is present.
    bool emitted_implicit_return = false;
    auto* compound = dynamic_cast<ast::CompoundStmt*>(body);
    if (compound && !compound->getStatements().empty()) {
        auto& stmts = compound->getStatements();
        auto* last_stmt = stmts.back().get();
        // Skip if last statement is already a return
        if (!dynamic_cast<ast::ReturnStmt*>(last_stmt)) {
            if (auto* var_decl = dynamic_cast<ast::VarDeclStmt*>(last_stmt)) {
                // VarDeclStmt: value is in the local we just defined. Push it and return.
                int slot = resolveLocal(current_, var_decl->getName());
                if (slot >= 0) {
                    emitWide(OpCode::OP_GET_LOCAL, static_cast<uint32_t>(slot), line);
                    emitOp(OpCode::OP_RETURN, line);
                    emitted_implicit_return = true;
                }
            } else if (dynamic_cast<ast::ExprStmt*>(last_stmt)) {
                // ExprStmt: the OP_POP at the end discarded the value.
                // Replace it with OP_RETURN.
                auto& code = currentChunk().code;
                if (!code.empty()) {
                    OpCode last_op = decodeOp(code.back());
                    if (last_op == OpCode::OP_POP) {
                        // Replace opcode byte (high 8 bits) with OP_RETURN
                        code.back() = encodeWide(OpCode::OP_RETURN, code.back() & 0x00FFFFFF);
                        emitted_implicit_return = true;
                    }
                }
            }
        }
    }

    endScope();

    // endFunction emits OP_RETURN_NULL (unless we already emitted a return)
    CompiledFunction* compiled = endFunction();

    // Emit OP_CLOSURE with the function constant
    int fn_idx = makeConstant(interpreter::NaabVal::makeVMClosure(
        std::make_shared<VMClosure>(compiled, std::vector<std::shared_ptr<ObjUpvalue>>{})));
    emitWide(OpCode::OP_CLOSURE, static_cast<uint32_t>(fn_idx), line);

    // Emit upvalue descriptors following the closure instruction
    // Each upvalue is encoded as a 32-bit instruction: [is_local:8][index:24]
    for (auto& upval : compiled->upvalues) {
        uint32_t desc = (upval.is_local ? (1u << 24) : 0u) | upval.index;
        currentChunk().emit(desc, line);
    }
}

// ============================================================================
// Visitor implementations — Phase 1: stubs + Phase 2: core
// ============================================================================

void Compiler::visit(ast::Program& node) {
    // Visit all top-level declarations in order
    for (auto& import : node.getImports()) {
        import->accept(*this);
    }
    for (auto& use : node.getModuleUses()) {
        use->accept(*this);
    }
    for (auto& imp : node.getModuleImports()) {
        imp->accept(*this);
    }
    for (auto& exp : node.getExports()) {
        exp->accept(*this);
    }
    for (auto& strct : node.getStructs()) {
        strct->accept(*this);
    }
    for (auto& enm : node.getEnums()) {
        enm->accept(*this);
    }
    for (auto& iface : node.getInterfaces()) {
        iface->accept(*this);
    }
    for (auto& func : node.getFunctions()) {
        func->accept(*this);
    }
    // S11: Top-level runtime declarations
    for (auto& rd : node.getRuntimeDecls()) {
        rd->accept(*this);
    }
    if (node.getMainBlock() && !skip_main_) {
        node.getMainBlock()->accept(*this);
    }
}

void Compiler::visit(ast::MainBlock& node) {
    beginScope();
    node.getBody()->accept(*this);
    endScope();
}

void Compiler::visit(ast::CompoundStmt& node) {
    // Pre-hoist: declare all local function names so mutual recursion works
    for (auto& stmt : node.getStatements()) {
        if (auto* fn_stmt = dynamic_cast<ast::FunctionDeclStmt*>(stmt.get())) {
            int existing = resolveLocal(current_, fn_stmt->getDecl()->getName());
            if (existing == -1) {
                emitOp(OpCode::OP_NULL, fn_stmt->getDecl()->getLocation().line);
                addLocal(fn_stmt->getDecl()->getName());
                markInitialized();
            }
        }
    }
    for (auto& stmt : node.getStatements()) {
        stmt->accept(*this);
    }
}

void Compiler::visit(ast::ExprStmt& node) {
    node.getExpr()->accept(*this);
    emitOp(OpCode::OP_POP, node.getLocation().line);
}

void Compiler::visit(ast::ReturnStmt& node) {
    if (node.getExpr()) {
        // Pre-flight: if return expression is tainted, mark this function as returning tainted data
        if (governance_ && governance_->isActive() && current_ && current_->function) {
            if (exprContainsTaint(node.getExpr())) {
                tainted_functions_[current_->function->name] = true;
            }
        }
        node.getExpr()->accept(*this);
        emitOp(OpCode::OP_RETURN, node.getLocation().line);
    } else {
        emitOp(OpCode::OP_RETURN_NULL, node.getLocation().line);
    }
}

void Compiler::visit(ast::VarDeclStmt& node) {
    int line = node.getLocation().line;
    bool rhs_tainted = false;
    if (node.getInit()) {
        // Pre-flight: check if RHS expression is statically tainted BEFORE compiling
        // (we need the AST, which is lost after accept())
        if (governance_ && governance_->isActive()) {
            rhs_tainted = exprContainsTaint(node.getInit());
            // Check if RHS is a sanitizer call (clears taint)
            if (rhs_tainted) {
                if (auto* call = dynamic_cast<ast::CallExpr*>(node.getInit())) {
                    if (auto* id = dynamic_cast<ast::IdentifierExpr*>(call->getCallee())) {
                        if (governance_->isSanitizer(id->getName())) rhs_tainted = false;
                    }
                    if (auto* mem = dynamic_cast<ast::MemberExpr*>(call->getCallee())) {
                        std::string full;
                        if (auto* oid = dynamic_cast<ast::IdentifierExpr*>(mem->getObject()))
                            full = oid->getName() + ".";
                        full += mem->getMember();
                        if (governance_->isSanitizer(full)) rhs_tainted = false;
                    }
                }
            }
        }
        node.getInit()->accept(*this);
        // Deep copy lists/dicts for value semantics (let arr2 = arr1 → independent copies)
        emitOp(OpCode::OP_COPY_VALUE, line);
    } else {
        emitOp(OpCode::OP_NULL, line);
    }

    // Pre-flight: emit OP_GOV_TAINT_MARK to set taint on shadow taint stack
    if (rhs_tainted) {
        emitOp(OpCode::OP_GOV_TAINT_MARK, line);
        // Finding G: check tainted RHS against sink policy at assignment point
        int tca_idx = identifierConstant(node.getName());
        emitWide(OpCode::OP_GOV_TAINT_CHECK_ASSIGN, static_cast<uint32_t>(tca_idx), line);
        markVarTainted(node.getName());
    } else if (governance_ && governance_->isActive()) {
        clearVarTaint(node.getName());
    }

    if (current_->scope_depth > 0) {
        // Local variable
        addLocal(node.getName());
        markInitialized();
    } else {
        // Global variable
        int name_idx = identifierConstant(node.getName());
        emitWide(OpCode::OP_DEFINE_GLOBAL, static_cast<uint32_t>(name_idx), line);
    }
}

void Compiler::visit(ast::LiteralExpr& node) {
    int line = node.getLocation().line;

    switch (node.getLiteralKind()) {
        case ast::LiteralKind::Int: {
            try {
                int64_t val = std::stoll(node.getValue());
                if (val > INT32_MAX || val < INT32_MIN) {
                    // Exceeds 32-bit int range — promote to double
                    int idx = makeConstant(interpreter::NaabVal::makeDouble(static_cast<double>(val)));
                    emitWide(OpCode::OP_CONST, static_cast<uint32_t>(idx), line);
                } else {
                    int idx = makeConstant(interpreter::NaabVal::makeInt(static_cast<int>(val)));
                    emitWide(OpCode::OP_CONST, static_cast<uint32_t>(idx), line);
                }
            } catch (const std::out_of_range&) {
                double val = std::stod(node.getValue());
                int idx = makeConstant(interpreter::NaabVal::makeDouble(val));
                emitWide(OpCode::OP_CONST, static_cast<uint32_t>(idx), line);
            }
            break;
        }
        case ast::LiteralKind::Float: {
            double val = std::stod(node.getValue());
            int idx = makeConstant(interpreter::NaabVal::makeDouble(val));
            emitWide(OpCode::OP_CONST, static_cast<uint32_t>(idx), line);
            break;
        }
        case ast::LiteralKind::String: {
            const std::string& raw = node.getValue();
            // Check for string interpolation ${...}
            if (raw.find("${") != std::string::npos) {
                // Split into literal parts and expressions, compile as concatenation.
                // Strategy: build result string on stack via OP_ADD.
                // For each expression, call string(expr) to convert to string first.
                // string() is a global builtin.

                // Collect segments: literal strings and expression texts
                struct Segment { bool is_expr; std::string text; };
                std::vector<Segment> segments;
                size_t i = 0;
                while (i < raw.size()) {
                    if (raw[i] == '$' && i + 1 < raw.size() && raw[i + 1] == '{') {
                        i += 2; // skip ${
                        int depth = 1;
                        std::string expr_text;
                        while (i < raw.size() && depth > 0) {
                            if (raw[i] == '{') depth++;
                            else if (raw[i] == '}') { depth--; if (depth == 0) break; }
                            expr_text += raw[i++];
                        }
                        if (i < raw.size()) i++; // skip }
                        segments.push_back({true, expr_text});
                    } else {
                        std::string text;
                        while (i < raw.size() && !(raw[i] == '$' && i + 1 < raw.size() && raw[i + 1] == '{')) {
                            text += raw[i++];
                        }
                        if (!text.empty()) segments.push_back({false, text});
                    }
                }

                if (segments.empty()) {
                    int idx = makeConstant(interpreter::NaabVal::makeString(""));
                    emitWide(OpCode::OP_CONST, static_cast<uint32_t>(idx), line);
                } else {
                    // Compile first segment
                    bool have_value = false;
                    for (size_t si = 0; si < segments.size(); si++) {
                        auto& seg = segments[si];
                        if (seg.is_expr) {
                            // Emit: string(expr) — push string builtin, push expr, call
                            int str_fn = identifierConstant("string");
                            emitWide(OpCode::OP_GET_GLOBAL, static_cast<uint32_t>(str_fn), line);
                            // Compile the expression inline
                            naab::lexer::Lexer expr_lexer(seg.text);
                            auto expr_tokens = expr_lexer.tokenize();
                            naab::parser::Parser expr_parser(expr_tokens);
                            auto expr_ast = expr_parser.parseExpression();
                            expr_ast->accept(*this);
                            // Call string(expr) — 1 arg
                            emit3(OpCode::OP_CALL, 1, 0, 0, line);
                        } else {
                            int tidx = makeConstant(interpreter::NaabVal::makeString(seg.text));
                            emitWide(OpCode::OP_CONST, static_cast<uint32_t>(tidx), line);
                        }
                        if (have_value) {
                            emitOp(OpCode::OP_ADD, line); // concat with previous
                        }
                        have_value = true;
                    }
                }
            } else {
                int idx = makeConstant(interpreter::NaabVal::makeString(raw));
                emitWide(OpCode::OP_CONST, static_cast<uint32_t>(idx), line);
            }
            break;
        }
        case ast::LiteralKind::Bool: {
            if (node.getValue() == "true") {
                emitOp(OpCode::OP_TRUE, line);
            } else {
                emitOp(OpCode::OP_FALSE, line);
            }
            break;
        }
        case ast::LiteralKind::Null: {
            emitOp(OpCode::OP_NULL, line);
            break;
        }
    }
}

void Compiler::visit(ast::IdentifierExpr& node) {
    emitGetVariable(node.getName(), node.getLocation().line);
}

void Compiler::visit(ast::UnaryExpr& node) {
    int line = node.getLocation().line;

    // Constant folding: -42 → OP_CONST(-42), !true → OP_CONST(false)
    if (auto* lit = dynamic_cast<ast::LiteralExpr*>(node.getOperand())) {
        if (node.getOp() == ast::UnaryOp::Neg && lit->getLiteralKind() == ast::LiteralKind::Int) {
            try {
                int v = std::stoi(lit->getValue());
                if (v == INT32_MIN) goto no_fold_unary;  // -INT_MIN overflows
                int ci = makeConstant(interpreter::NaabVal::makeInt(-v));
                emitWide(OpCode::OP_CONST, static_cast<uint32_t>(ci), line);
                return;
            } catch (...) { /* too large, fall through */ }
        }
        if (node.getOp() == ast::UnaryOp::Neg && lit->getLiteralKind() == ast::LiteralKind::Float) {
            int ci = makeConstant(interpreter::NaabVal::makeDouble(-std::stod(lit->getValue())));
            emitWide(OpCode::OP_CONST, static_cast<uint32_t>(ci), line);
            return;
        }
        if (node.getOp() == ast::UnaryOp::Not && lit->getLiteralKind() == ast::LiteralKind::Bool) {
            int ci = makeConstant(interpreter::NaabVal::makeBool(lit->getValue() != "true"));
            emitWide(OpCode::OP_CONST, static_cast<uint32_t>(ci), line);
            return;
        }
    }

    no_fold_unary:
    node.getOperand()->accept(*this);

    switch (node.getOp()) {
        case ast::UnaryOp::Neg: emitOp(OpCode::OP_NEG, line); break;
        case ast::UnaryOp::Not: emitOp(OpCode::OP_NOT, line); break;
        case ast::UnaryOp::Pos: /* no-op */ break;
    }
}

void Compiler::visit(ast::BinaryExpr& node) {
    int line = node.getLocation().line;

    // Short-circuit And/Or
    if (node.getOp() == ast::BinaryOp::And) {
        node.getLeft()->accept(*this);
        int jump = emitJump(OpCode::OP_JUMP_IF_FALSE, line);
        emitOp(OpCode::OP_POP, line);
        node.getRight()->accept(*this);
        patchJump(jump);
        return;
    }

    if (node.getOp() == ast::BinaryOp::Or) {
        node.getLeft()->accept(*this);
        int jump = emitJump(OpCode::OP_JUMP_IF_TRUE, line);
        emitOp(OpCode::OP_POP, line);
        node.getRight()->accept(*this);
        patchJump(jump);
        return;
    }

    // Null coalesce: a ?? b
    // Stack discipline: exactly one result value at end
    if (node.getOp() == ast::BinaryOp::NullCoalesce) {
        node.getLeft()->accept(*this);          // push a
        emitOp(OpCode::OP_DUP, line);           // push copy for null check
        int jump = emitJump(OpCode::OP_JUMP_IF_NULL, line); // pop copy; jump if null
        // Not null path: stack has [a, a_copy_pushed_back_by_JUMP_IF_NULL]
        // Pop the extra copy, keep original
        emitOp(OpCode::OP_POP, line);           // pop extra (JUMP_IF_NULL pushed back)
        int end_jump = emitJump(OpCode::OP_JUMP, line);
        // Null path: JUMP_IF_NULL consumed the copy and jumped, stack has [a]
        patchJump(jump);
        emitOp(OpCode::OP_POP, line);           // discard original null value
        node.getRight()->accept(*this);          // push b
        patchJump(end_jump);
        return;
    }

    // Assignment
    if (node.getOp() == ast::BinaryOp::Assign) {
        // Pre-flight taint analysis on RHS
        bool assign_tainted = false;
        if (governance_ && governance_->isActive()) {
            assign_tainted = exprContainsTaint(node.getRight());
        }
        node.getRight()->accept(*this);
        if (assign_tainted) {
            emitOp(OpCode::OP_GOV_TAINT_MARK, line);
        }
        // Determine assignment target
        if (auto* ident = dynamic_cast<ast::IdentifierExpr*>(node.getLeft())) {
            if (assign_tainted) {
                // Finding G: check tainted RHS against sink policy at assignment point
                int tca_idx = identifierConstant(ident->getName());
                emitWide(OpCode::OP_GOV_TAINT_CHECK_ASSIGN, static_cast<uint32_t>(tca_idx), line);
                markVarTainted(ident->getName());
            } else if (governance_ && governance_->isActive()) clearVarTaint(ident->getName());
            emitSetVariable(ident->getName(), line);
        } else if (auto* member = dynamic_cast<ast::MemberExpr*>(node.getLeft())) {
            member->getObject()->accept(*this);
            int name_idx = identifierConstant(member->getMember());
            emitWide(OpCode::OP_SET_MEMBER, static_cast<uint32_t>(name_idx), line);
        } else if (auto* sub = dynamic_cast<ast::BinaryExpr*>(node.getLeft())) {
            if (sub->getOp() == ast::BinaryOp::Subscript) {
                sub->getLeft()->accept(*this);   // obj
                sub->getRight()->accept(*this);  // index
                emitOp(OpCode::OP_SET_INDEX, line);
            }
        }
        return;
    }

    // Pipeline: x |> fn  → fn(x), or x |> fn(a,b) → fn(x, a, b)
    if (node.getOp() == ast::BinaryOp::Pipeline) {
        if (auto* call = dynamic_cast<ast::CallExpr*>(node.getRight())) {
            // Right side is a call: x |> fn(a, b) → fn(x, a, b)
            call->getCallee()->accept(*this);   // push fn
            node.getLeft()->accept(*this);       // push x (first arg)
            for (auto& arg : call->getArgs()) {
                arg->accept(*this);              // push remaining args
            }
            int argc = 1 + static_cast<int>(call->getArgs().size());
            emit3(OpCode::OP_CALL, static_cast<uint8_t>(argc), 0, 0, line);
        } else {
            // Right side is identifier: x |> fn → fn(x)
            node.getRight()->accept(*this);  // push fn
            node.getLeft()->accept(*this);   // push x
            emit3(OpCode::OP_CALL, 1, 0, 0, line);
        }
        return;
    }

    // Subscript: obj[index]
    if (node.getOp() == ast::BinaryOp::Subscript) {
        node.getLeft()->accept(*this);
        node.getRight()->accept(*this);
        emitOp(OpCode::OP_GET_INDEX, line);
        return;
    }

    // Constant folding: evaluate binary ops on literals at compile time
    auto* left_lit = dynamic_cast<ast::LiteralExpr*>(node.getLeft());
    auto* right_lit = dynamic_cast<ast::LiteralExpr*>(node.getRight());
    if (left_lit && right_lit) {
        // Int op Int (only for values that fit in int)
        if (left_lit->getLiteralKind() == ast::LiteralKind::Int &&
            right_lit->getLiteralKind() == ast::LiteralKind::Int) {
            int a, b;
            try { a = std::stoi(left_lit->getValue()); b = std::stoi(right_lit->getValue()); }
            catch (...) { goto no_fold; }  // too large for int, skip folding
            switch (node.getOp()) {
                case ast::BinaryOp::Add: {
                    long long r = (long long)a + b;
                    if (r > INT32_MAX || r < INT32_MIN) break;  // overflow, skip folding → runtime will catch
                    int ci = makeConstant(interpreter::NaabVal::makeInt((int)r)); emitWide(OpCode::OP_CONST, static_cast<uint32_t>(ci), line);
                } return;
                case ast::BinaryOp::Sub: {
                    long long r = (long long)a - b;
                    if (r > INT32_MAX || r < INT32_MIN) break;  // overflow
                    int ci = makeConstant(interpreter::NaabVal::makeInt((int)r)); emitWide(OpCode::OP_CONST, static_cast<uint32_t>(ci), line);
                } return;
                case ast::BinaryOp::Mul: {
                    long long r = (long long)a * b;
                    if (r > INT32_MAX || r < INT32_MIN) break;  // overflow
                    int ci = makeConstant(interpreter::NaabVal::makeInt((int)r)); emitWide(OpCode::OP_CONST, static_cast<uint32_t>(ci), line);
                } return;
                case ast::BinaryOp::Div:
                    if (b != 0) { { int ci = makeConstant(interpreter::NaabVal::makeInt(a / b)); emitWide(OpCode::OP_CONST, static_cast<uint32_t>(ci), line); } return; }
                    break;
                case ast::BinaryOp::Mod:
                    if (b != 0) { { int ci = makeConstant(interpreter::NaabVal::makeInt(a % b)); emitWide(OpCode::OP_CONST, static_cast<uint32_t>(ci), line); } return; }
                    break;
                case ast::BinaryOp::Lt: { int ci = makeConstant(interpreter::NaabVal::makeBool(a < b)); emitWide(OpCode::OP_CONST, static_cast<uint32_t>(ci), line); } return;
                case ast::BinaryOp::Le: { int ci = makeConstant(interpreter::NaabVal::makeBool(a <= b)); emitWide(OpCode::OP_CONST, static_cast<uint32_t>(ci), line); } return;
                case ast::BinaryOp::Gt: { int ci = makeConstant(interpreter::NaabVal::makeBool(a > b)); emitWide(OpCode::OP_CONST, static_cast<uint32_t>(ci), line); } return;
                case ast::BinaryOp::Ge: { int ci = makeConstant(interpreter::NaabVal::makeBool(a >= b)); emitWide(OpCode::OP_CONST, static_cast<uint32_t>(ci), line); } return;
                case ast::BinaryOp::Eq: { int ci = makeConstant(interpreter::NaabVal::makeBool(a == b)); emitWide(OpCode::OP_CONST, static_cast<uint32_t>(ci), line); } return;
                case ast::BinaryOp::Ne: { int ci = makeConstant(interpreter::NaabVal::makeBool(a != b)); emitWide(OpCode::OP_CONST, static_cast<uint32_t>(ci), line); } return;
                default: break;
            }
        }
        // String + String concatenation
        if (left_lit->getLiteralKind() == ast::LiteralKind::String &&
            right_lit->getLiteralKind() == ast::LiteralKind::String &&
            node.getOp() == ast::BinaryOp::Add) {
            { int ci = makeConstant(interpreter::NaabVal::makeString(
                left_lit->getValue() + right_lit->getValue()));
              emitWide(OpCode::OP_CONST, static_cast<uint32_t>(ci), line); }
            return;
        }
    }
    no_fold:

    // Standard binary ops
    node.getLeft()->accept(*this);
    node.getRight()->accept(*this);

    switch (node.getOp()) {
        case ast::BinaryOp::Add: emitOp(OpCode::OP_ADD, line); break;
        case ast::BinaryOp::Sub: emitOp(OpCode::OP_SUB, line); break;
        case ast::BinaryOp::Mul: emitOp(OpCode::OP_MUL, line); break;
        case ast::BinaryOp::Div: emitOp(OpCode::OP_DIV, line); break;
        case ast::BinaryOp::Mod: emitOp(OpCode::OP_MOD, line); break;
        case ast::BinaryOp::Eq:  emitOp(OpCode::OP_EQ, line); break;
        case ast::BinaryOp::Ne:  emitOp(OpCode::OP_NE, line); break;
        case ast::BinaryOp::Lt:  emitOp(OpCode::OP_LT, line); break;
        case ast::BinaryOp::Le:  emitOp(OpCode::OP_LE, line); break;
        case ast::BinaryOp::Gt:  emitOp(OpCode::OP_GT, line); break;
        case ast::BinaryOp::Ge:  emitOp(OpCode::OP_GE, line); break;
        case ast::BinaryOp::In:  emitOp(OpCode::OP_IN, line); break;
        default:
            throw std::runtime_error("Unhandled binary operator in compiler");
    }
}

// ============================================================================
// Stubs — will be implemented in later phases
// ============================================================================

void Compiler::visit(ast::IfStmt& node) {
    int line = node.getLocation().line;
    node.getCondition()->accept(*this);
    int else_jump = emitJump(OpCode::OP_JUMP_IF_FALSE, line);
    emitOp(OpCode::OP_POP, line); // pop condition (true path)

    beginScope();
    node.getThenBranch()->accept(*this);
    endScope();

    if (node.getElseBranch()) {
        int end_jump = emitJump(OpCode::OP_JUMP, line);
        patchJump(else_jump);
        emitOp(OpCode::OP_POP, line); // pop condition (false path)
        beginScope();
        node.getElseBranch()->accept(*this);
        endScope();
        patchJump(end_jump);
    } else {
        // True path must jump over the false-path OP_POP
        int end_jump = emitJump(OpCode::OP_JUMP, line);
        patchJump(else_jump);
        emitOp(OpCode::OP_POP, line); // pop condition (false path)
        patchJump(end_jump);
    }
}

void Compiler::visit(ast::WhileStmt& node) {
    int line = node.getLocation().line;
    int loop_start = currentChunk().codeSize();

    // Push loop context for break/continue
    current_->loops.push_back({loop_start, {}, current_->scope_depth});

    node.getCondition()->accept(*this);
    int exit_jump = emitJump(OpCode::OP_JUMP_IF_FALSE, line);
    emitOp(OpCode::OP_POP, line); // pop condition (true path)

    beginScope();
    node.getBody()->accept(*this);
    endScope();

    emitLoop(loop_start, line);
    patchJump(exit_jump);
    emitOp(OpCode::OP_POP, line); // pop condition (false path, loop exit)

    // Patch all breaks
    auto& loop = current_->loops.back();
    for (int offset : loop.break_jumps) {
        patchJump(offset);
    }
    current_->loops.pop_back();
}

void Compiler::visit(ast::BreakStmt& node) {
    if (current_->loops.empty()) {
        throw std::runtime_error("Break outside of loop");
    }
    int line = node.getLocation().line;
    // Pop locals that are deeper than the loop's scope depth
    int loop_depth = current_->loops.back().scope_depth;
    int locals_to_pop = 0;
    for (int i = static_cast<int>(current_->locals.size()) - 1; i >= 0; i--) {
        if (current_->locals[i].depth > loop_depth) {
            locals_to_pop++;
        } else {
            break;
        }
    }
    if (locals_to_pop > 0) {
        emit3(OpCode::OP_POPN, static_cast<uint8_t>(locals_to_pop), 0, 0, line);
    }
    int jump = emitJump(OpCode::OP_JUMP, line);
    current_->loops.back().break_jumps.push_back(jump);
}

void Compiler::visit(ast::ContinueStmt& node) {
    if (current_->loops.empty()) {
        throw std::runtime_error("Continue outside of loop");
    }
    int line = node.getLocation().line;
    // Pop locals that are deeper than the loop's scope depth
    int loop_depth = current_->loops.back().scope_depth;
    int locals_to_pop = 0;
    for (int i = static_cast<int>(current_->locals.size()) - 1; i >= 0; i--) {
        if (current_->locals[i].depth > loop_depth) {
            locals_to_pop++;
        } else {
            break;
        }
    }
    if (locals_to_pop > 0) {
        emit3(OpCode::OP_POPN, static_cast<uint8_t>(locals_to_pop), 0, 0, line);
    }
    emitLoop(current_->loops.back().start, line);
}

void Compiler::visit(ast::IfExpr& node) {
    int line = node.getLocation().line;
    node.getCondition()->accept(*this);
    int else_jump = emitJump(OpCode::OP_JUMP_IF_FALSE, line);
    emitOp(OpCode::OP_POP, line); // pop condition (true path)
    node.getThenExpr()->accept(*this);
    int end_jump = emitJump(OpCode::OP_JUMP, line);
    patchJump(else_jump);
    emitOp(OpCode::OP_POP, line); // pop condition (false path)
    node.getElseExpr()->accept(*this);
    patchJump(end_jump);
}

void Compiler::visit(ast::TryCatchExpr& node) {
    int line = node.getLocation().line;

    // OP_TRY_BEGIN [catch_offset]
    int try_begin = emitJump(OpCode::OP_TRY_BEGIN, line);

    // Try expression — leaves value on stack
    node.getTryExpr()->accept(*this);

    // OP_TRY_END — pop exception handler
    emitOp(OpCode::OP_TRY_END, line);

    // Jump over catch block
    int skip_catch = emitJump(OpCode::OP_JUMP, line);

    // Catch block — exception value is on stack (pushed by VM on throw)
    patchJump(try_begin);

    // Bind error to local so catch expression can reference it
    beginScope();
    addLocal(node.getErrorName());
    markInitialized();
    // Stack: [..., error_val(local)]

    // Catch expression — leaves value on stack
    node.getCatchExpr()->accept(*this);
    // Stack: [..., error_val(local), catch_result]

    // Move catch_result under the local, then endScope pops the local
    emitOp(OpCode::OP_SWAP, line);
    endScope();  // pops error_val local
    // Stack: [..., catch_result]

    patchJump(skip_catch);
}

// Finding G fix: UseStatement is the block-loading form (use BLOCK-xyz).
// The old silent no-op caused confusing crashes later; throw at compile time instead.
// ModuleUseStmt (use math) is fully supported — see visit(ast::ModuleUseStmt&).
void Compiler::visit(ast::UseStatement& node) {
    std::string block_id = node.getBlockId();
    // File path imports — compile like ModuleUseStmt via OP_IMPORT
    if (block_id.find("./") == 0 || block_id.find("../") == 0) {
        int line = node.getLocation().line;
        int path_idx = makeConstant(interpreter::NaabVal::makeString(block_id));
        emitWide(OpCode::OP_IMPORT, static_cast<uint32_t>(path_idx), line);
        int name_idx = identifierConstant(node.getAlias());
        emitWide(OpCode::OP_DEFINE_GLOBAL, static_cast<uint32_t>(name_idx), line);
        return;
    }
    // Block-loading syntax — not supported in VM
    throw std::runtime_error(
        "Compiler error: 'use BLOCK-...' block-loading syntax is not supported in VM mode.\n\n"
        "  For stdlib modules use:   use math\n"
        "  For file modules use:     use mymodule\n"
        "  For block-loading syntax: run with --tree-walk\n"
    );
}

void Compiler::visit(ast::FunctionDecl& node) {
    int line = node.getLocation().line;

    // Governance: function body quality checks (oversimplification, complexity floor, etc.)
    if (governance_ && governance_->isActive() && line > 0 && !source_file_.empty()) {
        std::ifstream src_file(source_file_);
        if (src_file.is_open()) {
            std::vector<std::string> src_lines;
            std::string src_line;
            while (std::getline(src_file, src_line)) {
                src_lines.push_back(src_line);
            }
            src_file.close();
            if (line <= static_cast<int>(src_lines.size())) {
                std::string body_text;
                int brace_depth = 0;
                bool found_start = false;
                for (size_t i = static_cast<size_t>(line - 1); i < src_lines.size(); ++i) {
                    body_text += src_lines[i] + "\n";
                    for (char c : src_lines[i]) {
                        if (c == '{') { brace_depth++; found_start = true; }
                        if (c == '}') brace_depth--;
                    }
                    if (found_start && brace_depth <= 0) break;
                }
                std::string err = governance_->checkNaabFunctionBody(
                    node.getName(), body_text, line, source_file_);
                if (!err.empty()) {
                    throw std::runtime_error(err);
                }
            }
        }
    }

    // For local scope, declare before body so recursive self-references resolve
    bool is_local = (current_->scope_depth > 0);
    if (is_local) {
        addLocal(node.getName());
    }

    // Compile the function body into a new CompiledFunction
    compileFunctionBody(node.getName(), node.getParams(), node.getBody(),
                        node.isAsync(), node.getLocation().line);

    // The closure is now on the stack. Define it.
    if (!is_local) {
        int name_idx = identifierConstant(node.getName());
        emitWide(OpCode::OP_DEFINE_GLOBAL, static_cast<uint32_t>(name_idx),
                 node.getLocation().line);
    } else {
        markInitialized();
    }
}

void Compiler::visit(ast::StructDecl& node) {
    int line = node.getLocation().line;
    // Store struct definition as a dict with __struct_name__ and field defaults
    std::unordered_map<std::string, interpreter::NaabVal> def;
    def["__struct_name__"] = interpreter::NaabVal::makeString(node.getName());
    // Store field names as a list for validation
    std::vector<interpreter::NaabVal> field_names;
    for (auto& field : node.getFields()) {
        field_names.push_back(interpreter::NaabVal::makeString(field.name));
        def[field.name] = interpreter::NaabVal::makeNull(); // default
    }
    def["__fields__"] = interpreter::NaabVal::makeList(std::move(field_names));
    int idx = makeConstant(interpreter::NaabVal::makeDict(std::move(def)));
    emitWide(OpCode::OP_CONST, static_cast<uint32_t>(idx), line);
    int name_idx = identifierConstant(node.getName());
    emitWide(OpCode::OP_DEFINE_GLOBAL, static_cast<uint32_t>(name_idx), line);
}

void Compiler::visit(ast::EnumDecl& node) {
    int line = node.getLocation().line;
    // Store enum as a dict: variant_name -> integer value
    std::unordered_map<std::string, interpreter::NaabVal> variants;
    int next_val = 0;
    for (auto& v : node.getVariants()) {
        if (v.value.has_value()) next_val = v.value.value();
        variants[v.name] = interpreter::NaabVal::makeInt(next_val);
        next_val++;
    }
    variants["__enum_name__"] = interpreter::NaabVal::makeString(node.getName());
    int idx = makeConstant(interpreter::NaabVal::makeDict(std::move(variants)));
    emitWide(OpCode::OP_CONST, static_cast<uint32_t>(idx), line);
    int name_idx = identifierConstant(node.getName());
    emitWide(OpCode::OP_DEFINE_GLOBAL, static_cast<uint32_t>(name_idx), line);
}

void Compiler::visit(ast::InterfaceDecl& node) {
    // Interfaces are parsed but not enforced at runtime (same as tree-walker)
    (void)node;
}

void Compiler::visit(ast::FunctionDeclStmt& node) {
    auto* decl = node.getDecl();
    int line = decl->getLocation().line;

    // Governance: function body quality checks (oversimplification, complexity floor, etc.)
    if (governance_ && governance_->isActive() && line > 0 && !source_file_.empty()) {
        std::ifstream src_file(source_file_);
        if (src_file.is_open()) {
            std::vector<std::string> lines;
            std::string src_line;
            while (std::getline(src_file, src_line)) {
                lines.push_back(src_line);
            }
            src_file.close();
            if (line <= static_cast<int>(lines.size())) {
                std::string body_text;
                int brace_depth = 0;
                bool found_start = false;
                for (size_t i = static_cast<size_t>(line - 1); i < lines.size(); ++i) {
                    body_text += lines[i] + "\n";
                    for (char c : lines[i]) {
                        if (c == '{') { brace_depth++; found_start = true; }
                        if (c == '}') brace_depth--;
                    }
                    if (found_start && brace_depth <= 0) break;
                }
                std::string err = governance_->checkNaabFunctionBody(
                    decl->getName(), body_text, line, source_file_);
                if (!err.empty()) {
                    throw std::runtime_error(err);
                }
            }
        }
    }

    // Check if this function was pre-hoisted by CompoundStmt
    int existing = resolveLocal(current_, decl->getName());

    if (existing == -1) {
        // Not pre-hoisted: declare local BEFORE compiling body (self-recursion)
        addLocal(decl->getName());
    }

    compileFunctionBody(decl->getName(), decl->getParams(), decl->getBody(),
                        decl->isAsync(), line);

    if (existing != -1) {
        // Pre-hoisted: update the existing slot with the compiled closure
        emitWide(OpCode::OP_SET_LOCAL, static_cast<uint32_t>(existing), line);
        emitOp(OpCode::OP_POP, line); // SET_LOCAL leaves value on stack
    } else {
        // The compiled closure is on the stack — it occupies the local slot
        markInitialized();
    }
}

void Compiler::visit(ast::StructDeclStmt& node) {
    auto* decl = node.getDecl();
    int line = decl->getLocation().line;
    // Build struct definition dict
    std::unordered_map<std::string, interpreter::NaabVal> def;
    def["__struct_name__"] = interpreter::NaabVal::makeString(decl->getName());
    std::vector<interpreter::NaabVal> field_names;
    for (auto& field : decl->getFields()) {
        field_names.push_back(interpreter::NaabVal::makeString(field.name));
        def[field.name] = interpreter::NaabVal::makeNull();
    }
    def["__fields__"] = interpreter::NaabVal::makeList(std::move(field_names));
    int idx = makeConstant(interpreter::NaabVal::makeDict(std::move(def)));
    emitWide(OpCode::OP_CONST, static_cast<uint32_t>(idx), line);
    // Define as local in current scope
    addLocal(decl->getName());
    markInitialized();
}

void Compiler::visit(ast::RuntimeDeclStmt& node) {
    int line = node.getLocation().line;
    // Push runtime name and language as constants
    int name_idx = makeConstant(interpreter::NaabVal::makeString(node.getName()));
    int lang_idx = makeConstant(interpreter::NaabVal::makeString(node.getLanguage()));
    // Encode: name_idx in upper 12 bits, lang_idx in lower 12 bits
    uint32_t packed = ((static_cast<uint32_t>(name_idx) & 0xFFF) << 12) |
                      (static_cast<uint32_t>(lang_idx) & 0xFFF);
    emitWide(OpCode::OP_RUNTIME_START, packed, line);
}

void Compiler::visit(ast::DestructureStmt& node) {
    int line = node.getLocation().line;
    node.getInit()->accept(*this);  // push the source value

    // Reserve source as a hidden local so we can reference it by slot index
    addLocal(" __destr_src__");
    markInitialized();
    int source_slot = static_cast<int>(current_->locals.size()) - 1;

    auto& names = node.getNames();
    int rest_index = node.getRestIndex();

    if (node.getDestructureKind() == ast::DestructureStmt::Kind::Array) {
        for (size_t i = 0; i < names.size(); i++) {
            // Load source from its local slot
            emitWide(OpCode::OP_GET_LOCAL, static_cast<uint32_t>(source_slot), line);

            if (static_cast<int>(i) == rest_index) {
                // Rest element: arr.slice(i)
                emitWide(OpCode::OP_CONST, makeConstant(
                    interpreter::NaabVal::makeInt(static_cast<int>(i))), line);
                uint32_t slice_idx = static_cast<uint32_t>(identifierConstant("slice"));
                uint32_t method_arg = (slice_idx << 8) | 1;
                emitWide(OpCode::OP_CALL_METHOD, method_arg, line);
            } else {
                emitWide(OpCode::OP_CONST, makeConstant(
                    interpreter::NaabVal::makeInt(static_cast<int>(i))), line);
                emitOp(OpCode::OP_GET_INDEX, line);
            }
            addLocal(names[i]);
            markInitialized();
        }
    } else {
        // Dict destructuring: let {x, y} = expr
        // Use "get" method so missing keys return null instead of throwing
        for (auto& name : names) {
            emitWide(OpCode::OP_GET_LOCAL, static_cast<uint32_t>(source_slot), line);
            emitWide(OpCode::OP_CONST, makeConstant(
                interpreter::NaabVal::makeString(name)), line);
            uint32_t get_idx = static_cast<uint32_t>(identifierConstant("get"));
            uint32_t method_arg = (get_idx << 8) | 1;
            emitWide(OpCode::OP_CALL_METHOD, method_arg, line);
            addLocal(name);
            markInitialized();
        }
    }
    // Hidden local stays on stack — cleaned up when scope ends
}

void Compiler::visit(ast::ForStmt& node) {
    int line = node.getLocation().line;

    // Compile the iterable and convert to iterator state (list + index on stack)
    node.getIter()->accept(*this);
    // Pre-flight: if iterable expression is tainted, mark it so iterator elements inherit taint
    if (governance_ && governance_->isActive() && exprContainsTaint(node.getIter())) {
        emitOp(OpCode::OP_GOV_TAINT_MARK, line);
    }
    // arg: number of destructure vars (0 = no destructuring)
    // When > 0 and iterable is dict, OP_GET_ITER produces [key,value] pairs
    int destruct_vars = node.isDestructuring()
        ? static_cast<int>(node.getDestructureNames().size()) : 0;
    // Bit 23: flag that parens index form was used (for dict DX warning)
    // Lower 16 bits: destructure var count
    uint32_t iter_flag = static_cast<uint32_t>(destruct_vars);
    if (node.hasIndex()) iter_flag |= 0x800000u;
    emitWide(OpCode::OP_GET_ITER, iter_flag, line);

    // The iterator state occupies 2 stack slots: [list, index]
    // Reserve as anonymous locals so the stack accounting is correct
    addLocal("__iter_list__");
    markInitialized();
    addLocal("__iter_idx__");
    markInitialized();

    int loop_start = currentChunk().codeSize();
    current_->loops.push_back({loop_start, {}, current_->scope_depth});

    // OP_ITER_NEXT: advance or jump if done
    // Encode: [vars:8][jump:16] — jump offset patched later
    int num_vars = node.isDestructuring()
        ? static_cast<int>(node.getDestructureNames().size()) : 0;
    // Emit placeholder — we'll patch the jump offset
    int iter_next_offset = currentChunk().codeSize();
    uint32_t iter_arg = (static_cast<uint32_t>(num_vars) << 16) | 0; // jump=0 placeholder
    currentChunk().emit(encodeWide(OpCode::OP_ITER_NEXT, iter_arg), line);

    // Now the loop variable(s) are on the stack as locals
    beginScope();

    if (node.isDestructuring()) {
        for (auto& name : node.getDestructureNames()) {
            addLocal(name);
            markInitialized();
        }
    } else {
        addLocal(node.getVar());
        markInitialized();
    }

    // U2: If index variable requested, push current index (idx - 1, since ITER_NEXT already incremented)
    if (node.hasIndex()) {
        int idx_slot = resolveLocal(current_, "__iter_idx__");
        emitWide(OpCode::OP_GET_LOCAL, static_cast<uint32_t>(idx_slot), line);
        emitOp(OpCode::OP_CONST, line);
        int one_idx = makeConstant(interpreter::NaabVal::makeInt(1));
        currentChunk().code.back() = encodeWide(OpCode::OP_CONST, static_cast<uint32_t>(one_idx));
        emitOp(OpCode::OP_SUB, line);
        addLocal(node.getIndexVar());
        markInitialized();
    }

    // Compile body
    node.getBody()->accept(*this);
    endScope(); // pops loop variables

    // Jump back to loop start
    emitLoop(loop_start, line);

    // Patch the OP_ITER_NEXT jump to here (past the loop)
    int after_loop = currentChunk().codeSize();
    int jump_dist = after_loop - iter_next_offset - 1;
    // Re-encode the instruction with the correct jump offset
    uint32_t patched_arg = (static_cast<uint32_t>(num_vars) << 16) |
                           (static_cast<uint32_t>(jump_dist) & 0xFFFF);
    currentChunk().code[iter_next_offset] = encodeWide(OpCode::OP_ITER_NEXT, patched_arg);

    // Patch all breaks to here (before iterator cleanup, since break already popped body locals)
    auto& loop = current_->loops.back();
    for (int offset : loop.break_jumps) {
        patchJump(offset);
    }
    current_->loops.pop_back();

    // Pop iterator state (list + index)
    emitOp(OpCode::OP_POP, line);
    emitOp(OpCode::OP_POP, line);
    // Remove the anonymous iterator locals
    current_->locals.pop_back(); // __iter_idx__
    current_->locals.pop_back(); // __iter_list__
}

void Compiler::visit(ast::ImportStmt& node) {
    int line = node.getLocation().line;
    // For file imports: store module path as constant, emit OP_IMPORT
    int path_idx = identifierConstant(node.getModulePath());
    emitWide(OpCode::OP_IMPORT, static_cast<uint32_t>(path_idx), line);

    if (node.isWildcard()) {
        // import * from "module" — import all names
        if (!node.getWildcardAlias().empty()) {
            int alias_idx = identifierConstant(node.getWildcardAlias());
            emitWide(OpCode::OP_DEFINE_GLOBAL, static_cast<uint32_t>(alias_idx), line);
        } else {
            emitWide(OpCode::OP_IMPORT_WILDCARD, static_cast<uint32_t>(path_idx), line);
        }
    } else {
        // Named imports — module dict stays on stack, each OP_IMPORT_NAME
        // extracts a value without removing the module
        for (auto& item : node.getItems()) {
            int name_idx = identifierConstant(item.name);
            emitWide(OpCode::OP_IMPORT_NAME, static_cast<uint32_t>(name_idx), line);
            std::string bind_name = item.alias.empty() ? item.name : item.alias;
            int bind_idx = identifierConstant(bind_name);
            emitWide(OpCode::OP_DEFINE_GLOBAL, static_cast<uint32_t>(bind_idx), line);
        }
        // Pop the module dict after all names extracted
        emitOp(OpCode::OP_POP, line);
    }
}

void Compiler::visit(ast::ExportStmt& node) {
    // Compile the inner declaration — the export keyword just marks it for external use
    switch (node.getKind()) {
        case ast::ExportStmt::ExportKind::Function:
            if (node.getFunctionDecl()) {
                node.getFunctionDecl()->accept(*this);
            }
            break;
        case ast::ExportStmt::ExportKind::Variable:
            if (node.getVarDecl()) {
                node.getVarDecl()->accept(*this);
            }
            break;
        case ast::ExportStmt::ExportKind::Struct:
            if (node.getStructDecl()) {
                node.getStructDecl()->accept(*this);
            }
            break;
        case ast::ExportStmt::ExportKind::Enum:
            if (node.getEnumDecl()) {
                node.getEnumDecl()->accept(*this);
            }
            break;
        case ast::ExportStmt::ExportKind::DefaultExpr:
            if (node.getExpr()) {
                node.getExpr()->accept(*this);
                int name_idx = identifierConstant("default");
                emitWide(OpCode::OP_DEFINE_GLOBAL, static_cast<uint32_t>(name_idx),
                         node.getLocation().line);
            }
            break;
    }
}

void Compiler::visit(ast::TryStmt& node) {
    int line = node.getLocation().line;

    // OP_TRY_BEGIN [catch_offset] — jump to catch on exception
    int try_begin = emitJump(OpCode::OP_TRY_BEGIN, line);

    // Compile try body
    beginScope();
    node.getTryBody()->accept(*this);
    endScope();

    // OP_TRY_END — pop the exception handler
    emitOp(OpCode::OP_TRY_END, line);

    // Jump over catch block
    int skip_catch = emitJump(OpCode::OP_JUMP, line);

    // Patch try_begin to jump here (catch block)
    patchJump(try_begin);

    if (node.getCatchClause()) {
        beginScope();
        // The exception value is on the stack (pushed by VM on throw)
        addLocal(node.getCatchClause()->error_name);
        markInitialized();
        node.getCatchClause()->body->accept(*this);
        endScope();
    }

    patchJump(skip_catch);

    // Finally block (always runs)
    if (node.hasFinally()) {
        beginScope();
        node.getFinallyBody()->accept(*this);
        endScope();
    }
}

void Compiler::visit(ast::ThrowStmt& node) {
    int line = node.getLocation().line;
    node.getExpr()->accept(*this);
    emitOp(OpCode::OP_THROW, line);
}

void Compiler::visit(ast::ThrowExpr& node) {
    int line = node.getLocation().line;
    node.getExpr()->accept(*this);
    emitOp(OpCode::OP_THROW, line);
}

void Compiler::visit(ast::ModuleUseStmt& node) {
    int line = node.getLocation().line;
    std::string module_path = node.getModulePath();
    // Match tree-walker: without alias, use last component of dotted path
    // "modules.identity" -> "identity", "math" -> "math"
    std::string bind_name;
    if (node.hasAlias()) {
        bind_name = node.getAlias();
    } else {
        bind_name = module_path;
        auto last_dot = module_path.find_last_of('.');
        if (last_dot != std::string::npos) {
            bind_name = module_path.substr(last_dot + 1);
        }
    }

    // Check if it's a known stdlib module name (prelude modules)
    static const std::unordered_set<std::string> stdlib_modules = {
        "io", "math", "string", "array", "file", "json", "http",
        "crypto", "time", "env", "debug", "os", "regex",
        "csv", "collections", "bolo", "path", "dict",
        "log", "uuid", "validate", "process"
    };
    if (stdlib_modules.count(module_path)) {
        // Stdlib module: emit marker string
        std::string marker = "__stdlib_module__:" + module_path;
        int marker_idx = makeConstant(interpreter::NaabVal::makeString(marker));
        emitWide(OpCode::OP_CONST, static_cast<uint32_t>(marker_idx), line);
        int name_idx = identifierConstant(bind_name);
        emitWide(OpCode::OP_DEFINE_GLOBAL, static_cast<uint32_t>(name_idx), line);
    } else if (module_path.find("BLOCK-") == 0) {
        // Block import: blocks are pre-loaded by main.cpp via setGlobal()
        // No codegen needed — the global is already set before execution
    } else {
        // File-based module: use OP_IMPORT to load the .naab file
        // Convert dot notation to path: "foo.bar" -> "foo/bar"
        std::string file_path = module_path;
        for (auto& c : file_path) {
            if (c == '.') c = '/';
        }
        int path_idx = makeConstant(interpreter::NaabVal::makeString(file_path));
        emitWide(OpCode::OP_IMPORT, static_cast<uint32_t>(path_idx), line);
        // Define the module dict as a global with the bind name
        int name_idx = identifierConstant(bind_name);
        emitWide(OpCode::OP_DEFINE_GLOBAL, static_cast<uint32_t>(name_idx), line);
    }
}

void Compiler::visit(ast::CallExpr& node) {
    int line = node.getLocation().line;

    // Check for method call pattern: obj.method(args)
    if (auto* member = dynamic_cast<ast::MemberExpr*>(node.getCallee())) {
        if (member->isOptional()) {
            // Optional chaining method call: obj?.method(args)
            // If obj is null, skip the entire call and push null
            member->getObject()->accept(*this);
            emitOp(OpCode::OP_DUP, line);
            int skip_call = emitJump(OpCode::OP_JUMP_IF_NULL, line);
            emitOp(OpCode::OP_POP, line); // pop dup (not null)
            // Compile args
            for (auto& arg : node.getArgs()) {
                arg->accept(*this);
            }
            // Emit method call
            int name_idx = identifierConstant(member->getMember());
            uint32_t packed = (static_cast<uint32_t>(name_idx) << 8) |
                              (static_cast<uint32_t>(node.getArgs().size()) & 0xFF);
            emitWide(OpCode::OP_CALL_METHOD, packed, line);
            int end = emitJump(OpCode::OP_JUMP, line);
            patchJump(skip_call);
            emitOp(OpCode::OP_POP, line); // pop null dup, leave original null
            patchJump(end);
            return;
        }
        // Compile the object
        member->getObject()->accept(*this);
        // Compile arguments
        for (auto& arg : node.getArgs()) {
            arg->accept(*this);
        }
        // Emit OP_CALL_METHOD with method name and argc
        int name_idx = identifierConstant(member->getMember());
        uint32_t packed = (static_cast<uint32_t>(name_idx) << 8) |
                          (static_cast<uint32_t>(node.getArgs().size()) & 0xFF);
        emitWide(OpCode::OP_CALL_METHOD, packed, line);
        return;
    }

    // Normal function call: compile callee, then args, then OP_CALL
    node.getCallee()->accept(*this);
    for (auto& arg : node.getArgs()) {
        arg->accept(*this);
    }
    emit3(OpCode::OP_CALL, static_cast<uint8_t>(node.getArgs().size()), 0, 0, line);
}

void Compiler::visit(ast::MemberExpr& node) {
    int line = node.getLocation().line;
    node.getObject()->accept(*this);
    if (node.isOptional()) {
        // Optional chaining: obj?.member → null if obj is null
        emitOp(OpCode::OP_DUP, line);
        int skip = emitJump(OpCode::OP_JUMP_IF_NULL, line);
        emitOp(OpCode::OP_POP, line); // pop dup (not null)
        int name_idx = identifierConstant(node.getMember());
        emitWide(OpCode::OP_GET_MEMBER, static_cast<uint32_t>(name_idx), line);
        int end = emitJump(OpCode::OP_JUMP, line);
        patchJump(skip);
        emitOp(OpCode::OP_POP, line); // pop null dup
        // original null remains on stack
        patchJump(end);
    } else {
        int name_idx = identifierConstant(node.getMember());
        emitWide(OpCode::OP_GET_MEMBER, static_cast<uint32_t>(name_idx), line);
    }
}

void Compiler::visit(ast::DictExpr& node) {
    int line = node.getLocation().line;
    auto& entries = node.getEntries();
    for (auto& kv : entries) {
        kv.first->accept(*this);   // key
        kv.second->accept(*this);  // value
    }
    emitWide(OpCode::OP_DICT, static_cast<uint32_t>(entries.size()), line);
}

void Compiler::visit(ast::ListExpr& node) {
    int line = node.getLocation().line;
    auto& elems = node.getElements();
    for (auto& elem : elems) {
        elem->accept(*this);
    }
    emitWide(OpCode::OP_LIST, static_cast<uint32_t>(elems.size()), line);
}

void Compiler::visit(ast::RangeExpr& node) {
    int line = node.getLocation().line;
    node.getStart()->accept(*this);
    node.getEnd()->accept(*this);
    emitWide(OpCode::OP_RANGE, node.isInclusive() ? 1u : 0u, line);
}

void Compiler::visit(ast::StructLiteralExpr& node) {
    int line = node.getLocation().line;
    // Look up the struct definition — handle qualified names like "types.Vec2"
    std::string name = node.getStructName();
    auto dot = name.find('.');
    if (dot != std::string::npos) {
        // Qualified: emit GET for module, then GET_MEMBER for struct
        std::string mod = name.substr(0, dot);
        std::string member = name.substr(dot + 1);
        emitGetVariable(mod, line);
        int member_idx = identifierConstant(member);
        emitWide(OpCode::OP_GET_MEMBER, static_cast<uint32_t>(member_idx), line);
    } else {
        emitGetVariable(name, line);
    }
    // OP_STRUCT_NEW: clone the struct def dict as a new instance
    emitWide(OpCode::OP_STRUCT_NEW, 0, line);
    // Initialize fields
    for (auto& [field_name, expr] : node.getFieldInits()) {
        expr->accept(*this);
        int field_idx = identifierConstant(field_name);
        emitWide(OpCode::OP_STRUCT_INIT_FIELD, static_cast<uint32_t>(field_idx), line);
    }
}

void Compiler::visit(ast::InlineCodeExpr& node) {
    int line = node.getLocation().line;
    // Store polyglot block info as a constant
    // At runtime, the VM dispatches to the appropriate language executor
    PolyglotBlockInfo info;
    info.language = node.getLanguage();
    info.code = node.getCode();
    info.return_type = node.getReturnType();

    // Compile bound variables — push them on stack
    for (auto& var : node.getBoundVariables()) {
        emitGetVariable(var, line);
    }

    // Store the PolyglotBlockInfo in constants as a dict
    std::unordered_map<std::string, interpreter::NaabVal> block;
    block["__polyglot__"] = interpreter::NaabVal::makeBool(true);
    block["language"] = interpreter::NaabVal::makeString(info.language);
    block["code"] = interpreter::NaabVal::makeString(info.code);
    block["return_type"] = interpreter::NaabVal::makeString(info.return_type);
    // Store bound var names
    std::vector<interpreter::NaabVal> var_names;
    for (auto& v : node.getBoundVariables()) {
        var_names.push_back(interpreter::NaabVal::makeString(v));
    }
    block["bound_vars"] = interpreter::NaabVal::makeList(std::move(var_names));

    int info_idx = makeConstant(interpreter::NaabVal::makeDict(std::move(block)));
    uint32_t packed = (static_cast<uint32_t>(info_idx) << 8) |
                      (static_cast<uint32_t>(node.getBoundVariables().size()) & 0xFF);
    emitWide(OpCode::OP_POLYGLOT, packed, line);
}

void Compiler::visit(ast::LambdaExpr& node) {
    compileFunctionBody("<lambda>", node.getParams(), node.getBody(),
                        false, node.getLocation().line);
    // Closure value is now on the stack
}

void Compiler::visit(ast::MatchExpr& node) {
    int line = node.getSubject()->getLocation().line;

    // Subject goes on stack as anonymous local
    node.getSubject()->accept(*this);
    addLocal("__match_subject__");
    markInitialized();
    int subject_slot = static_cast<int>(current_->locals.size()) - 1;

    std::vector<int> end_jumps;

    for (auto& arm : node.getArms()) {
        if (arm.pattern == nullptr) {
            // Wildcard arm
            if (arm.guard) {
                // Guarded wildcard: _ if cond => body
                arm.guard->accept(*this);
                int no_guard = emitJump(OpCode::OP_JUMP_IF_FALSE, line);
                emitOp(OpCode::OP_POP, line); // pop guard result (true)
                arm.body->accept(*this);
                end_jumps.push_back(emitJump(OpCode::OP_JUMP, line));
                patchJump(no_guard);
                emitOp(OpCode::OP_POP, line); // pop guard result (false)
            } else {
                // Unguarded wildcard: always matches
                arm.body->accept(*this);
                end_jumps.push_back(emitJump(OpCode::OP_JUMP, line));
            }
        } else {
            // Check if pattern is a variable binding (IdentifierExpr)
            auto* ident_pattern = dynamic_cast<ast::IdentifierExpr*>(arm.pattern.get());
            bool is_binding = ident_pattern != nullptr;  // any identifier is a binding

            // Check for array destructure pattern: [x, y] => ...
            auto* list_pattern = dynamic_cast<ast::ListExpr*>(arm.pattern.get());

            if (list_pattern) {
                // Array destructure pattern in match arm
                // Strategy: type check → length check → bind ALL elements as locals
                // → check literals → check guard → body
                // Each failure path cleans its own stack, jumps to next_arm.
                auto& elems = list_pattern->getElements();
                int num_elems = static_cast<int>(elems.size());

                // Type check: type(subject) == "array"
                int type_idx = identifierConstant("type");
                emitWide(OpCode::OP_GET_GLOBAL, static_cast<uint32_t>(type_idx), line);
                emitWide(OpCode::OP_GET_LOCAL, static_cast<uint32_t>(subject_slot), line);
                emit3(OpCode::OP_CALL, 1, 0, 0, line);
                emitWide(OpCode::OP_CONST, makeConstant(
                    interpreter::NaabVal::makeString("array")), line);
                emitOp(OpCode::OP_EQ, line);
                int not_array = emitJump(OpCode::OP_JUMP_IF_FALSE, line);
                emitOp(OpCode::OP_POP, line); // pop EQ true

                // Length check: subject.length() == num_elems
                emitWide(OpCode::OP_GET_LOCAL, static_cast<uint32_t>(subject_slot), line);
                uint32_t len_idx = static_cast<uint32_t>(identifierConstant("length"));
                uint32_t len_packed = (len_idx << 8) | 0;
                emitWide(OpCode::OP_CALL_METHOD, len_packed, line);
                emitWide(OpCode::OP_CONST, makeConstant(
                    interpreter::NaabVal::makeInt(num_elems)), line);
                emitOp(OpCode::OP_EQ, line);
                int no_len_match = emitJump(OpCode::OP_JUMP_IF_FALSE, line);
                emitOp(OpCode::OP_POP, line); // pop EQ true

                // Bind ALL elements as locals (even literals — check values after)
                int first_binding_slot = static_cast<int>(current_->locals.size());
                for (int i = 0; i < num_elems; i++) {
                    emitWide(OpCode::OP_GET_LOCAL, static_cast<uint32_t>(subject_slot), line);
                    emitWide(OpCode::OP_CONST, makeConstant(
                        interpreter::NaabVal::makeInt(i)), line);
                    emitOp(OpCode::OP_GET_INDEX, line);

                    auto* elem_ident = dynamic_cast<ast::IdentifierExpr*>(elems[i].get());
                    std::string name = elem_ident ? elem_ident->getName()
                        : (" __match_elem_" + std::to_string(i) + "__");
                    addLocal(name);
                    markInitialized();
                }

                // Check literal values by comparing bound locals
                std::vector<int> check_fail_jumps;
                for (int i = 0; i < num_elems; i++) {
                    auto* elem_lit = dynamic_cast<ast::LiteralExpr*>(elems[i].get());
                    if (elem_lit) {
                        emitWide(OpCode::OP_GET_LOCAL,
                            static_cast<uint32_t>(first_binding_slot + i), line);
                        elems[i]->accept(*this);
                        emitOp(OpCode::OP_EQ, line);
                        check_fail_jumps.push_back(
                            emitJump(OpCode::OP_JUMP_IF_FALSE, line));
                        emitOp(OpCode::OP_POP, line); // pop EQ true
                    }
                }

                // Guard check
                if (arm.guard) {
                    arm.guard->accept(*this);
                    check_fail_jumps.push_back(
                        emitJump(OpCode::OP_JUMP_IF_FALSE, line));
                    emitOp(OpCode::OP_POP, line); // pop guard true
                }

                // Compile body — result goes on stack above bindings
                arm.body->accept(*this);

                // Match path cleanup: store result in first binding slot, pop rest
                if (num_elems > 0) {
                    emitWide(OpCode::OP_SET_LOCAL,
                        static_cast<uint32_t>(first_binding_slot), line);
                    for (int i = 0; i < num_elems; i++) {
                        emitOp(OpCode::OP_POP, line);
                    }
                }
                end_jumps.push_back(emitJump(OpCode::OP_JUMP, line));

                // == Failure paths (each cleans its own stack, jumps to next_arm) ==

                // Literal/guard failure: pop false + all bindings
                int skip_check_fail = -1;
                if (!check_fail_jumps.empty()) {
                    for (int j : check_fail_jumps) patchJump(j);
                    emitOp(OpCode::OP_POP, line); // pop EQ/guard false
                    for (int i = 0; i < num_elems; i++) {
                        emitOp(OpCode::OP_POP, line); // pop bindings
                    }
                    skip_check_fail = emitJump(OpCode::OP_JUMP, line);
                }

                // Length check failure: pop false (no bindings exist)
                patchJump(no_len_match);
                emitOp(OpCode::OP_POP, line); // pop EQ false
                int skip_len_fail = emitJump(OpCode::OP_JUMP, line);

                // Type check failure: pop false (no bindings exist)
                patchJump(not_array);
                emitOp(OpCode::OP_POP, line); // pop EQ false

                // next_arm: all failure paths converge here
                if (skip_check_fail != -1) patchJump(skip_check_fail);
                patchJump(skip_len_fail);

                // Compile-time: remove all bindings from compiler locals (once)
                for (int i = 0; i < num_elems; i++) {
                    current_->locals.pop_back();
                }
            } else if (is_binding) {
                // Variable binding: bind subject to name, then optionally check guard
                emitWide(OpCode::OP_GET_LOCAL, static_cast<uint32_t>(subject_slot), line);
                addLocal(ident_pattern->getName());
                markInitialized();

                if (arm.guard) {
                    arm.guard->accept(*this);
                    int no_guard = emitJump(OpCode::OP_JUMP_IF_FALSE, line);
                    emitOp(OpCode::OP_POP, line); // pop guard result (true)

                    arm.body->accept(*this);
                    // Stack: [..., binding_var, result]
                    int binding_slot = static_cast<int>(current_->locals.size()) - 1;
                    emitWide(OpCode::OP_SET_LOCAL, static_cast<uint32_t>(binding_slot), line);
                    emitOp(OpCode::OP_POP, line);
                    end_jumps.push_back(emitJump(OpCode::OP_JUMP, line));

                    patchJump(no_guard);
                    emitOp(OpCode::OP_POP, line); // pop guard result (false)
                    emitOp(OpCode::OP_POP, line); // pop binding variable
                } else {
                    // No guard — always matches (catch-all binding)
                    arm.body->accept(*this);
                    // Stack: [..., binding_var, result]
                    int binding_slot = static_cast<int>(current_->locals.size()) - 1;
                    emitWide(OpCode::OP_SET_LOCAL, static_cast<uint32_t>(binding_slot), line);
                    emitOp(OpCode::OP_POP, line);
                    end_jumps.push_back(emitJump(OpCode::OP_JUMP, line));
                }

                // Single compile-time cleanup of binding local
                current_->locals.pop_back();
            } else {
                // Compare subject == pattern
                emitWide(OpCode::OP_GET_LOCAL, static_cast<uint32_t>(subject_slot), line);
                arm.pattern->accept(*this);
                emitOp(OpCode::OP_EQ, line);

                if (arm.guard) {
                    int no_match = emitJump(OpCode::OP_JUMP_IF_FALSE, line);
                    emitOp(OpCode::OP_POP, line); // pop EQ result (true)
                    arm.guard->accept(*this);
                    int no_guard = emitJump(OpCode::OP_JUMP_IF_FALSE, line);
                    emitOp(OpCode::OP_POP, line); // pop guard result (true)
                    arm.body->accept(*this);
                    end_jumps.push_back(emitJump(OpCode::OP_JUMP, line));
                    patchJump(no_guard);
                    emitOp(OpCode::OP_POP, line); // pop guard result (false)
                    patchJump(no_match);
                    emitOp(OpCode::OP_POP, line); // pop EQ result (false)
                } else {
                    int no_match = emitJump(OpCode::OP_JUMP_IF_FALSE, line);
                    emitOp(OpCode::OP_POP, line); // pop EQ result (true)
                    arm.body->accept(*this);
                    end_jumps.push_back(emitJump(OpCode::OP_JUMP, line));
                    patchJump(no_match);
                    emitOp(OpCode::OP_POP, line); // pop EQ result (false)
                }
            }
        }
    }

    // Check if any arm is an unguarded wildcard (guaranteed catch-all).
    // A guarded wildcard (_ if cond) might not match, so it doesn't count.
    // An unguarded binding pattern (x => ...) without guard also always matches.
    bool has_catch_all = false;
    for (auto& arm : node.getArms()) {
        if (arm.pattern == nullptr && !arm.guard) {
            has_catch_all = true; break;
        }
        // Bare identifier without guard is also a catch-all (binds and always matches)
        if (arm.pattern && !arm.guard) {
            if (dynamic_cast<ast::IdentifierExpr*>(arm.pattern.get())) {
                has_catch_all = true; break;
            }
        }
    }

    if (!has_catch_all) {
        // No wildcard arm: if we reach here, no arm matched → throw
        int err_idx = makeConstant(interpreter::NaabVal::makeString(
            "Match error: no pattern matched the value"));
        emitWide(OpCode::OP_CONST, static_cast<uint32_t>(err_idx), line);
        emitOp(OpCode::OP_THROW, line);
    }

    for (int j : end_jumps) {
        patchJump(j);
    }

    // Stack: [..., subject@N, result@N+1]
    // Copy result into subject slot, then pop the duplicate
    emitWide(OpCode::OP_SET_LOCAL, static_cast<uint32_t>(subject_slot), line);
    emitOp(OpCode::OP_POP, line);
    // Stack: [..., result@N] — subject slot now holds the match result
    current_->locals.pop_back();
}

void Compiler::visit(ast::AwaitExpr& node) {
    int line = node.getLocation().line;
    node.getExpr()->accept(*this);
    emitOp(OpCode::OP_AWAIT, line);
}

void Compiler::visit(ast::YieldExpr& node) {
    int line = node.getLocation().line;
    node.getExpr()->accept(*this);
    emitOp(OpCode::OP_YIELD, line);
}

// ============================================================================
// Pre-flight compile-time taint analysis
// Mirrors tree-walker's expressionContainsTaint() for static taint detection.
// This catches cases the runtime shadow taint stack misses:
// - Both branches of if-expressions
// - ${var} in string interpolation
// - Pipeline expressions containing tainted vars
// - Await expressions wrapping tainted calls
// ============================================================================

bool Compiler::exprContainsTaint(ast::Expr* expr) {
    if (!expr || !governance_ || !governance_->isActive()) return false;

    // IdentifierExpr: check if variable is in compile-time taint set
    if (auto* id = dynamic_cast<ast::IdentifierExpr*>(expr)) {
        return tainted_vars_.count(id->getName()) > 0;
    }

    // CallExpr: check if callee is a taint source (e.g., env.get)
    if (auto* call = dynamic_cast<ast::CallExpr*>(expr)) {
        if (auto* member = dynamic_cast<ast::MemberExpr*>(call->getCallee())) {
            std::string full_name;
            if (auto* obj_id = dynamic_cast<ast::IdentifierExpr*>(member->getObject())) {
                full_name = obj_id->getName() + ".";
            }
            full_name += member->getMember();
            if (governance_->isTaintSource(full_name)) return true;
            // Check if callee is a sanitizer (clears taint)
            if (governance_->isSanitizer(full_name)) return false;
        }
        if (auto* id = dynamic_cast<ast::IdentifierExpr*>(call->getCallee())) {
            if (governance_->isTaintSource(id->getName())) return true;
            // Sanitizer functions clear taint
            if (governance_->isSanitizer(id->getName())) return false;
            // Check if this is a known tainted function (returns tainted data)
            if (tainted_functions_.count(id->getName()) > 0) return true;
        }
        // Check if any argument is tainted (taint flows through function calls)
        for (auto& arg : call->getArgs()) {
            if (exprContainsTaint(arg.get())) return true;
        }
        return false;
    }

    // BinaryExpr: either operand tainted → result tainted
    if (auto* bin = dynamic_cast<ast::BinaryExpr*>(expr)) {
        return exprContainsTaint(bin->getLeft()) || exprContainsTaint(bin->getRight());
    }

    // UnaryExpr: taint passes through
    if (auto* un = dynamic_cast<ast::UnaryExpr*>(expr)) {
        return exprContainsTaint(un->getOperand());
    }

    // IfExpr: EITHER branch could execute → check BOTH (static analysis)
    if (auto* ife = dynamic_cast<ast::IfExpr*>(expr)) {
        return exprContainsTaint(ife->getCondition()) ||
               exprContainsTaint(ife->getThenExpr()) ||
               exprContainsTaint(ife->getElseExpr());
    }

    // LiteralExpr(String): scan ${var} for tainted variable references
    if (auto* lit = dynamic_cast<ast::LiteralExpr*>(expr)) {
        if (lit->getLiteralKind() == ast::LiteralKind::String) {
            const std::string& raw = lit->getValue();
            size_t pos = 0;
            while ((pos = raw.find("${", pos)) != std::string::npos) {
                pos += 2;
                int depth = 1;
                std::string expr_text;
                size_t i = pos;
                while (i < raw.size() && depth > 0) {
                    if (raw[i] == '{') depth++;
                    else if (raw[i] == '}') { depth--; if (depth == 0) break; }
                    expr_text += raw[i]; i++;
                }
                // Extract identifiers from interpolated expression
                std::string word;
                for (char c : expr_text) {
                    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
                        word += c;
                    } else {
                        if (!word.empty() && tainted_vars_.count(word) > 0) return true;
                        word.clear();
                    }
                }
                if (!word.empty() && tainted_vars_.count(word) > 0) return true;
                pos = i + 1;
            }
        }
        return false;
    }

    // AwaitExpr: taint passes through
    if (auto* aw = dynamic_cast<ast::AwaitExpr*>(expr)) {
        return exprContainsTaint(aw->getExpr());
    }

    // MemberExpr: taint from object propagates
    if (auto* mem = dynamic_cast<ast::MemberExpr*>(expr)) {
        return exprContainsTaint(mem->getObject());
    }

    // ListExpr: any element tainted → list tainted
    if (auto* list = dynamic_cast<ast::ListExpr*>(expr)) {
        for (auto& elem : list->getElements()) {
            if (exprContainsTaint(elem.get())) return true;
        }
        return false;
    }

    // DictExpr: any value tainted → dict tainted
    if (auto* dict = dynamic_cast<ast::DictExpr*>(expr)) {
        for (auto& entry : dict->getEntries()) {
            if (exprContainsTaint(entry.second.get())) return true;
        }
        return false;
    }

    // InlineCodeExpr (polyglot): check if polyglot_output is a taint source
    if (auto* ice = dynamic_cast<ast::InlineCodeExpr*>(expr)) {
        if (governance_->isTaintSource("polyglot_output") ||
            governance_->isTaintSource("polyglot_output:" + ice->getLanguage()))
            return true;
        return false;
    }

    return false;
}

} // namespace vm
} // namespace naab
