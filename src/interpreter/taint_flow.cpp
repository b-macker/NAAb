// NAAb -- the static information-flow analysis (see include/naab/taint_flow.h).
#include "naab/taint_flow.h"

#include "naab/string_interpolation.h"
#include "naab/taint_rule.h"

#include <algorithm>
#include <set>
#include <sstream>
#include <utility>

namespace naab {
namespace taint {

const char* kindName(FlowFinding::Kind k) {
    return k == FlowFinding::Kind::Value ? "value" : "control";
}

namespace {

using K = ast::NodeKind;

// A taint label. t: tainted at all. x: tainted through DATA -- the value is
// computed from a source (an explicit flow). t without x: tainted only through
// CONTROL -- set or reached on a path tainted data chose (an implicit flow).
// origin: the first place the taint came from, preferring a data origin; it is
// for the message only, no decision reads it.
struct Label {
    bool t = false;
    bool x = false;
    std::string origin;
    // The lambdas a variable may hold, so a call through it is followed.
    std::vector<ast::LambdaExpr*> fns;
    static Label data(std::string o) { Label l; l.t = l.x = true; l.origin = std::move(o); return l; }
    static Label control(std::string o) { Label l; l.t = true; l.origin = std::move(o); return l; }
};
Label join(const Label& a, const Label& b) {
    Label r;
    r.t = a.t || b.t;
    r.x = a.x || b.x;
    r.origin = a.x ? a.origin : b.x ? b.origin : a.t ? a.origin : b.origin;
    r.fns = a.fns;
    for (auto* f : b.fns)
        if (std::find(r.fns.begin(), r.fns.end(), f) == r.fns.end()) r.fns.push_back(f);
    return r;
}

using Scope = std::map<std::string, Label>;
struct State { std::vector<Scope> scopes; };

// Two states with the same scope structure (a branch point and its end).
State joinState(const State& a, const State& b) {
    State out = a;
    const size_t n = std::min(a.scopes.size(), b.scopes.size());
    for (size_t i = 0; i < n; ++i)
        for (const auto& [name, l] : b.scopes[i]) {
            auto it = out.scopes[i].find(name);
            if (it == out.scopes[i].end()) out.scopes[i][name] = l;
            else it->second = join(it->second, l);
        }
    return out;
}
bool sameTaint(const State& a, const State& b) {
    if (a.scopes.size() != b.scopes.size()) return false;
    for (size_t i = 0; i < a.scopes.size(); ++i) {
        if (a.scopes[i].size() != b.scopes[i].size()) return false;
        for (const auto& [name, l] : a.scopes[i]) {
            auto it = b.scopes[i].find(name);
            if (it == b.scopes[i].end() || it->second.t != l.t || it->second.x != l.x ||
                it->second.fns.size() != l.fns.size()) return false;
        }
    }
    return true;
}

// A try in progress: what was thrown inside it, and the control a throw (or a
// call on tainted data, which may throw because of it) happened under -- the
// catch runs on that path, and so does the rest of the try body after it.
struct TryFrame { Label thrown; Label cond; };

// The function being walked.
struct FnCtx {
    int id = 0;           // this activation, so a lambda knows where it was defined
    std::string name;
    std::string via;
    Label ret;            // what it returns (yields included)
    Label sticky;         // an early exit under tainted control: what follows runs only on that path
    Label throws;         // an uncaught throw under tainted control (the caller's control after the call)
    Label thrown_value;   // an uncaught throw of a tainted value (the caller's catch binding)
    std::vector<Label> loop_sticky;  // break/continue under tainted control, per enclosing loop
    std::vector<TryFrame> tries;
};

// diverges: every path through the function throws (it never returns).
struct Summary { Label ret, throws, thrown_value; bool diverges = false; bool fresh = false; };
struct LambdaResult { Label ret; bool diverges = false; };

constexpr int kMaxLoopIterations = 24;
constexpr int kMaxRounds = 8;
constexpr int kMaxDepth = 64;

bool isMutatingMethod(const std::string& m) {
    static const std::set<std::string> k = {
        "push", "append", "add", "unshift", "insert", "extend", "set", "put", "update", "merge"};
    return k.count(m) > 0;
}

class Analyzer {
public:
    Analyzer(ast::Program& p, const FlowPolicy& pol) : prog_(p), pol_(pol) {
        for (const auto& f : p.getFunctions()) fns_[f->getName()] = f.get();
        // A module's exports: its functions, and its top-level `export let`s,
        // which run before anything that imports it.
        for (const auto& ex : p.getExports()) {
            if (auto* fd = ex->getFunctionDecl()) fns_[fd->getName()] = fd;
            if (auto* vd = ex->getVarDecl()) exported_vars_.push_back(vd);
        }
        for (const auto& u : p.getModuleUses()) {
            std::string alias = u->getAlias();
            if (alias.empty()) {
                alias = u->getModulePath();
                auto slash = alias.find_last_of("/.");
                if (slash != std::string::npos) alias = alias.substr(slash + 1);
            }
            // `use env` names a standard-library module: its calls are the
            // configured sources and sinks, not code to follow.
            if (pol_.is_stdlib_module && pol_.is_stdlib_module(u->getModulePath())) continue;
            imported_.insert(alias);
        }
        for (const auto& u : p.getImports())  // `use BLOCK-ID as alias`: a registry block
            if (!u->getAlias().empty()) imported_.insert(u->getAlias());
        for (const auto& im : p.getModuleImports()) {
            for (const auto& item : im->getItems()) imported_.insert(item.alias.empty() ? item.name : item.alias);
            if (!im->getWildcardAlias().empty()) imported_.insert(im->getWildcardAlias());
        }
    }

    FlowReport run() {
        for (int round = 0; round < kMaxRounds; ++round) {
            for (auto& [k, s] : memo_) s.fresh = false;
            changed_ = false;
            st_ = State{};
            st_.scopes.emplace_back();
            ctx_ = FnCtx{};
            ctx_.id = ++next_ctx_id_;
            ctx_.name = "(module)";
            diverged_ = div_throw_ = false;
            for (auto* vd : exported_vars_) exec(vd, Label{});
            module_scope_ = st_;
            if (auto* mb = prog_.getMainBlock()) {
                ctx_ = FnCtx{};
                ctx_.id = ++next_ctx_id_;
                ctx_.name = "main";
                diverged_ = div_throw_ = false;
                exec(mb->getBody(), Label{});
            }
            // A function no call reached (a module's exports, dead code): its own
            // sources and sinks still count, with clean arguments.
            for (const auto& [fname, fdecl] : fns_) {
                const std::string prefix = fname + "@";
                bool reached = false;
                for (const auto& [k, s] : memo_)
                    if (k.compare(0, prefix.size(), prefix) == 0) { reached = true; break; }
                if (reached) continue;
                st_ = module_scope_;
                ctx_ = FnCtx{};
                ctx_.name = "(top level)";
                analyzeCall(fdecl, {}, Label{}, 0);
            }
            if (!changed_) { converged_ = true; break; }
        }
        if (!converged_) {
            report_.converged = false;
            blind("recursion that did not settle");
        }
        std::set<std::string> analysed;
        for (const auto& [k, s] : memo_) analysed.insert(k.substr(0, k.find('@')));
        report_.functions_analysed = static_cast<int>(analysed.size());
        std::stable_sort(report_.findings.begin(), report_.findings.end(),
                         [](const FlowFinding& a, const FlowFinding& b) { return a.line < b.line; });
        return std::move(report_);
    }

private:
    // --- names ---------------------------------------------------------------
    const Label* find(const std::string& n) const {
        for (auto it = st_.scopes.rbegin(); it != st_.scopes.rend(); ++it) {
            auto f = it->find(n);
            if (f != it->end()) return &f->second;
        }
        return nullptr;
    }
    bool declared(const std::string& n) const { return find(n) != nullptr; }
    Label get(const std::string& n) const { const Label* p = find(n); return p ? *p : Label{}; }
    void declare(const std::string& n, const Label& l) {
        if (st_.scopes.empty()) st_.scopes.emplace_back();
        st_.scopes.back()[n] = l;
    }
    void assign(const std::string& n, const Label& l) {
        for (auto it = st_.scopes.rbegin(); it != st_.scopes.rend(); ++it) {
            auto f = it->find(n);
            if (f != it->end()) { f->second = l; return; }
        }
        if (st_.scopes.empty()) st_.scopes.emplace_back();
        st_.scopes.front()[n] = l;
    }
    // A part of a value changed (a field, an element, a pushed item): the
    // variable keeps what it held and gains the new taint.
    void weakAssign(const std::string& n, const Label& l) {
        if (!l.t) return;
        assign(n, join(get(n), l));
    }

    // --- the shared rule ---------------------------------------------------
    // The rule asked twice: once over all taint, once over data taint alone.
    struct Hooks : RuleHooks {
        Analyzer& a;
        bool data_only;
        Hooks(Analyzer& an, bool d) : a(an), data_only(d) {}
        bool nameTainted(const std::string& n) override {
            Label l = a.get(n);
            return data_only ? l.x : l.t;
        }
        CallVerdict call(ast::CallExpr& c) override { return a.verdict(c, data_only); }
    };
    CallVerdict verdict(ast::CallExpr& c, bool data_only) {
        const std::string name = calleeName(c.getCallee());
        if (!name.empty()) {
            if (pol_.is_source && pol_.is_source(name)) return CallVerdict::Tainted;
            if (pol_.is_sanitizer && pol_.is_sanitizer(name)) return CallVerdict::Clean;
        }
        // What the callee returns beyond its arguments: a user function or a
        // callback that read a source, or returned under tainted control.
        auto it = call_extra_.find(&c);
        if (it != call_extra_.end() && (data_only ? it->second.x : it->second.t)) return CallVerdict::Tainted;
        return CallVerdict::Undecided;
    }
    Label valueOf(ast::Expr* e) {
        Hooks all(*this, false);
        if (!expressionTainted(e, all)) return Label{};
        Hooks data_only(*this, true);
        const bool x = expressionTainted(e, data_only);
        std::string o = originOf(e, x);
        if (o.empty()) o = "line " + std::to_string(e->getLocation().line);
        return x ? Label::data(o) : Label::control(o);
    }

    static std::vector<ast::Expr*> children(ast::Expr* e) {
        std::vector<ast::Expr*> c;
        if (!e) return c;
        switch (e->getKind()) {
            case K::BinaryExpr: {
                auto* b = static_cast<ast::BinaryExpr*>(e);
                c = {b->getLeft(), b->getRight()};
                break;
            }
            case K::UnaryExpr: c = {static_cast<ast::UnaryExpr*>(e)->getOperand()}; break;
            case K::CallExpr: {
                auto* call = static_cast<ast::CallExpr*>(e);
                for (const auto& a : call->getArgs()) c.push_back(a.get());
                c.push_back(call->getCallee());
                break;
            }
            case K::MemberExpr: c = {static_cast<ast::MemberExpr*>(e)->getObject()}; break;
            case K::DictExpr:
                for (const auto& kv : static_cast<ast::DictExpr*>(e)->getEntries()) {
                    c.push_back(kv.first.get());
                    c.push_back(kv.second.get());
                }
                break;
            case K::ListExpr:
                for (const auto& x : static_cast<ast::ListExpr*>(e)->getElements()) c.push_back(x.get());
                break;
            case K::RangeExpr: {
                auto* r = static_cast<ast::RangeExpr*>(e);
                c = {r->getStart(), r->getEnd()};
                break;
            }
            case K::StructLiteralExpr:
                for (const auto& f : static_cast<ast::StructLiteralExpr*>(e)->getFieldInits()) c.push_back(f.second.get());
                break;
            case K::IfExpr: {
                auto* i = static_cast<ast::IfExpr*>(e);
                c = {i->getCondition(), i->getThenExpr(), i->getElseExpr()};
                break;
            }
            case K::MatchExpr: {
                auto* m = static_cast<ast::MatchExpr*>(e);
                c.push_back(m->getSubject());
                for (auto& arm : m->getArms()) {
                    c.push_back(arm.pattern.get());
                    c.push_back(arm.guard.get());
                    c.push_back(arm.body.get());
                }
                break;
            }
            case K::AwaitExpr: c = {static_cast<ast::AwaitExpr*>(e)->getExpr()}; break;
            case K::YieldExpr: c = {static_cast<ast::YieldExpr*>(e)->getExpr()}; break;
            case K::ThrowExpr: c = {static_cast<ast::ThrowExpr*>(e)->getExpr()}; break;
            case K::TryCatchExpr: {
                auto* t = static_cast<ast::TryCatchExpr*>(e);
                c = {t->getTryExpr(), t->getCatchExpr()};
                break;
            }
            default: break;
        }
        return c;
    }

    // Where an expression's taint came from: its first tainted leaf (a data
    // leaf when data_only).
    std::string originOf(ast::Expr* e, bool data_only) {
        if (!e) return "";
        const int line = e->getLocation().line;
        switch (e->getKind()) {
            case K::IdentifierExpr: {
                Label l = get(static_cast<ast::IdentifierExpr*>(e)->getName());
                return (data_only ? l.x : l.t) ? l.origin : "";
            }
            case K::LiteralExpr: {
                auto* lit = static_cast<ast::LiteralExpr*>(e);
                if (lit->getLiteralKind() != ast::LiteralKind::String) return "";
                for (const auto& id : interp::interpolatedIdentifiers(lit->getValue())) {
                    Label l = get(id);
                    if (data_only ? l.x : l.t) return l.origin;
                }
                return "";
            }
            case K::CallExpr: {
                auto* c = static_cast<ast::CallExpr*>(e);
                const std::string name = calleeName(c->getCallee());
                if (!name.empty() && pol_.is_source && pol_.is_source(name))
                    return name + " at line " + std::to_string(line);
                auto it = call_extra_.find(c);
                if (it != call_extra_.end() && (data_only ? it->second.x : it->second.t)) return it->second.origin;
                break;
            }
            case K::InlineCodeExpr:
                return "a <<" + static_cast<ast::InlineCodeExpr*>(e)->getLanguage() + ">> block at line " +
                       std::to_string(line);
            default: break;
        }
        for (ast::Expr* c : children(e)) {
            std::string o = originOf(c, data_only);
            if (!o.empty()) return o;
        }
        return "";
    }

    // --- control -------------------------------------------------------------
    Label eff(const Label& pc) const {
        Label e = join(pc, ctx_.sticky);
        for (const auto& l : ctx_.loop_sticky) e = join(e, l);
        for (const auto& f : ctx_.tries) e = join(e, f.cond);
        return e;
    }
    static Label controlFrom(const Label& base, const Label& c, const std::string& what, int line) {
        if (base.t) return base;
        if (c.t) return Label::control(what + " at line " + std::to_string(line) + " (" + c.origin + ")");
        return Label{};
    }
    void blind(const std::string& what) { report_.blind_spots[what]++; }
    void record(FlowFinding::Kind kind, const std::string& sink, int line, const std::string& origin) {
        const std::string key = std::string(kindName(kind)) + "|" + sink + "|" + std::to_string(line) + "|" + ctx_.name;
        if (!finding_keys_.insert(key).second) return;
        FlowFinding f;
        f.kind = kind;
        f.sink = sink;
        f.line = line;
        f.function = ctx_.name;
        f.origin = origin;
        f.via = ctx_.via;
        report_.findings.push_back(std::move(f));
    }
    // A throw (or a call that may throw) under this control.
    void throwHere(const Label& value, const Label& e) {
        if (!ctx_.tries.empty()) {
            auto& f = ctx_.tries.back();
            f.thrown = join(f.thrown, join(value, e));
            if (e.t) f.cond = join(f.cond, e);
            return;
        }
        if (value.t) ctx_.thrown_value = join(ctx_.thrown_value, value);
        if (e.t) {
            ctx_.sticky = join(ctx_.sticky, e);
            ctx_.throws = join(ctx_.throws, e);
        }
    }
    void sinkCheck(const std::string& name, const Label& data, const Label& pc, int line) {
        if (name.empty() || !pol_.is_sink || !pol_.is_sink(name)) return;
        if (data.x) record(FlowFinding::Kind::Value, name, line, data.origin);
        else if (data.t) record(FlowFinding::Kind::Control, name, line, data.origin);
        else if (pc.t) record(FlowFinding::Kind::Control, name, line, pc.origin);
    }

    // --- functions -----------------------------------------------------------
    Summary analyzeCall(ast::FunctionDecl* fn, const std::vector<Label>& args, const Label& pc, int line) {
        std::ostringstream mask;
        for (size_t i = 0; i < fn->getParams().size(); ++i) {
            const Label* a = i < args.size() ? &args[i] : nullptr;
            mask << (a && a->x ? 'x' : a && a->t ? 'c' : '0');
            if (a) for (auto* f : a->fns) mask << ':' << static_cast<const void*>(f);
            mask << ',';
        }
        // A function declared inside another one reads the enclosing names:
        // the current ones when called where it was declared, else those it
        // captured there (joined over every place it is declared). They are
        // part of the call shape.
        Scope enclosing;
        const bool nested = fn_captured_.count(fn) > 0;
        if (nested) enclosing = fn_ctx_[fn] == ctx_.id ? flatten(st_) : fn_captured_[fn];
        for (const auto& [n, l] : enclosing)
            mask << n << '=' << (l.x ? 'x' : l.t ? 'c' : '0') << l.fns.size() << ';';
        std::ostringstream key;
        key << fn->getName() << "@" << static_cast<const void*>(fn) << "|" << mask.str() << "|" << (pc.t ? 1 : 0);
        const std::string k = key.str();
        {
            auto it = memo_.find(k);
            if (it != memo_.end() && (it->second.fresh || in_progress_.count(k))) return it->second;
        }
        if (depth_ >= kMaxDepth) {
            blind("calls nested deeper than the analysis follows");
            Summary s;
            s.ret = Label::data("a call nested too deep to follow, at line " + std::to_string(line));
            return s;
        }
        memo_.emplace(k, Summary{});
        in_progress_.insert(k);
        ++depth_;
        State saved_st = std::move(st_);
        FnCtx saved_ctx = std::move(ctx_);
        const bool saved_div = diverged_, saved_thr = div_throw_;
        diverged_ = div_throw_ = false;
        // The caller's statement is still being evaluated: the callee's own
        // statements reset stmt_ctrl_, and the caller's must survive them.
        Label saved_sc = std::move(stmt_ctrl_);
        stmt_ctrl_ = Label{};
        st_ = module_scope_;  // a function sees the module's names, not its caller's
        if (nested) st_.scopes.push_back(enclosing);
        st_.scopes.emplace_back();
        ctx_ = FnCtx{};
        ctx_.id = ++next_ctx_id_;
        ctx_.name = fn->getName();
        ctx_.via = line > 0 ? ("called from " + saved_ctx.name + " at line " + std::to_string(line)) : "";
        const auto& params = fn->getParams();
        for (size_t i = 0; i < params.size(); ++i) {
            Label l;
            if (i < args.size()) l = args[i];
            else if (params[i].default_value && *params[i].default_value) l = eval(params[i].default_value->get(), Label{});
            declare(params[i].name, l);
        }
        exec(fn->getBody(), pc);
        Summary out;
        out.ret = ctx_.ret;
        out.throws = ctx_.throws;
        out.thrown_value = ctx_.thrown_value;
        out.diverges = diverged_ && div_throw_;
        st_ = std::move(saved_st);
        ctx_ = std::move(saved_ctx);
        diverged_ = saved_div;
        div_throw_ = saved_thr;
        stmt_ctrl_ = std::move(saved_sc);
        --depth_;
        in_progress_.erase(k);
        Summary& m = memo_[k];
        if (out.ret.t != m.ret.t || out.ret.x != m.ret.x || out.throws.t != m.throws.t ||
            out.thrown_value.t != m.thrown_value.t || out.diverges != m.diverges)
            changed_ = true;
        m.diverges = out.diverges;
        m.ret = join(m.ret, out.ret);
        m.throws = join(m.throws, out.throws);
        m.thrown_value = join(m.thrown_value, out.thrown_value);
        m.fresh = true;
        return m;
    }
    // A function value's body. called: it runs here (a call through the name
    // that holds it, or a callback a call is given -- its parameters then get
    // the call's other data), so what it throws reaches this point. Not called:
    // it is defined and kept, and its body is walked for its own sources and
    // sinks with clean parameters; nothing it throws happens here.
    LambdaResult analyzeLambda(ast::LambdaExpr* lam, const Label& param, const Label& pc, int line, bool called) {
        if (depth_ >= kMaxDepth) {
            blind("calls nested deeper than the analysis follows");
            return {Label::data("a function value nested too deep to follow, at line " + std::to_string(line)), false};
        }
        ++depth_;
        const bool saved_div = diverged_, saved_thr = div_throw_;
        diverged_ = div_throw_ = false;
        Label saved_sc = std::move(stmt_ctrl_);  // as in analyzeCall
        stmt_ctrl_ = Label{};
        // Where it was made, it sees the current names (later assignments
        // included); anywhere else, the names it captured there.
        auto made = lam_ctx_.find(lam);
        const bool elsewhere = made != lam_ctx_.end() && made->second != ctx_.id;
        State saved_st;
        if (elsewhere) {
            saved_st = std::move(st_);
            st_ = State{};
            st_.scopes.push_back(captured_[lam]);
        }
        FnCtx saved = std::move(ctx_);
        ctx_ = FnCtx{};
        ctx_.id = ++next_ctx_id_;
        ctx_.name = saved.name + " (the function at line " + std::to_string(line) + ")";
        ctx_.via = saved.via;
        st_.scopes.emplace_back();
        for (const auto& p : lam->getParams()) declare(p.name, param);
        exec(lam->getBody(), pc);
        st_.scopes.pop_back();
        if (elsewhere) st_ = std::move(saved_st);
        LambdaResult r{ctx_.ret, diverged_ && div_throw_};
        Label throws = ctx_.throws, thrown = ctx_.thrown_value;
        ctx_ = std::move(saved);
        diverged_ = saved_div;
        div_throw_ = saved_thr;
        stmt_ctrl_ = std::move(saved_sc);
        --depth_;
        if (called && (throws.t || thrown.t)) throwHere(thrown, throws);
        return r;
    }
    // A lambda is made here: remember the names it can see (joined over every
    // place it is made), and which activation made it.
    Scope flatten(const State& st) const {
        Scope flat;
        for (const auto& sc : st.scopes)
            for (const auto& [n, l] : sc) flat[n] = l;  // inner scopes override
        return flat;
    }
    void captureFor(ast::LambdaExpr* lam) {
        Scope flat = flatten(st_);
        Scope& c = captured_[lam];
        for (const auto& [n, l] : flat) {
            auto it = c.find(n);
            if (it == c.end()) c[n] = l;
            else it->second = join(it->second, l);
        }
        lam_ctx_[lam] = ctx_.id;
    }
    void afterCall(const Summary& s) {
        if (s.throws.t || s.thrown_value.t) throwHere(s.thrown_value, s.throws);
        if (s.diverges) diverged_ = div_throw_ = true;  // the call never returns
    }

    // --- expressions ---------------------------------------------------------
    // Visit an expression as it runs under control pc: record the sinks it
    // calls, follow the functions it calls, apply its assignments. Returns its
    // value's label (the shared rule's verdict).
    Label eval(ast::Expr* e, const Label& pc) {
        if (!e) return Label{};
        const int line = e->getLocation().line;
        switch (e->getKind()) {
            case K::IdentifierExpr:
            case K::LiteralExpr:
                break;
            case K::BinaryExpr: {
                auto* b = static_cast<ast::BinaryExpr*>(e);
                switch (b->getOp()) {
                    case ast::BinaryOp::Assign: {
                        Label v = eval(b->getRight(), pc);
                        Label where = evalTargetParts(b->getLeft(), pc);
                        assignTarget(b->getLeft(), join(join(join(v, pc), where), stmt_ctrl_), line);
                        return v;
                    }
                    case ast::BinaryOp::And:
                    case ast::BinaryOp::Or:
                    case ast::BinaryOp::NullCoalesce: {
                        Label l = eval(b->getLeft(), pc);
                        State before = st_;
                        const bool ldiv = diverged_, lthr = div_throw_;
                        eval(b->getRight(), controlFrom(pc, l, "the left side of a short-circuit", line));
                        st_ = joinState(before, st_);
                        diverged_ = ldiv;
                        div_throw_ = lthr;
                        break;
                    }
                    case ast::BinaryOp::Pipeline: {
                        Label l = eval(b->getLeft(), pc);
                        if (auto* c = dynamic_cast<ast::CallExpr*>(b->getRight())) {
                            evalCall(c, pc, &l);
                        } else if (auto* id = dynamic_cast<ast::IdentifierExpr*>(b->getRight())) {
                            const std::string& n = id->getName();
                            auto fit = fns_.find(n);
                            if (!declared(n) && fit != fns_.end()) {
                                Summary s = analyzeCall(fit->second, {l}, pc, line);
                                afterCall(s);
                            } else if (!declared(n)) {
                                sinkCheck(n, l, pc, line);
                            }
                        } else {
                            eval(b->getRight(), pc);
                        }
                        break;
                    }
                    default:
                        eval(b->getLeft(), pc);
                        eval(b->getRight(), pc);
                }
                break;
            }
            case K::UnaryExpr:
                eval(static_cast<ast::UnaryExpr*>(e)->getOperand(), pc);
                break;
            case K::CallExpr:
                evalCall(static_cast<ast::CallExpr*>(e), pc, nullptr);
                break;
            case K::MemberExpr:
                eval(static_cast<ast::MemberExpr*>(e)->getObject(), pc);
                break;
            case K::DictExpr:
                for (const auto& kv : static_cast<ast::DictExpr*>(e)->getEntries()) {
                    eval(kv.first.get(), pc);
                    eval(kv.second.get(), pc);
                }
                break;
            case K::ListExpr:
                for (const auto& x : static_cast<ast::ListExpr*>(e)->getElements()) eval(x.get(), pc);
                break;
            case K::RangeExpr: {
                auto* r = static_cast<ast::RangeExpr*>(e);
                eval(r->getStart(), pc);
                eval(r->getEnd(), pc);
                break;
            }
            case K::StructLiteralExpr:
                for (const auto& f : static_cast<ast::StructLiteralExpr*>(e)->getFieldInits()) eval(f.second.get(), pc);
                break;
            case K::InlineCodeExpr: {
                auto* ic = static_cast<ast::InlineCodeExpr*>(e);
                Label data;
                for (const auto& v : ic->getBoundVariables()) data = join(data, get(v));
                const std::string sink = polyglotSinkType(ic->getLanguage());
                if (pol_.is_sink && pol_.is_sink(sink)) sinkCheck(sink, data, pc, line);
                else if (data.t || pc.t)
                    blind("a polyglot block given tainted data or run under tainted control, whose "
                          "language has no <lang>_exec sink configured (its effects are not analysed)");
                break;
            }
            case K::IfExpr: {
                auto* i = static_cast<ast::IfExpr*>(e);
                Label c = eval(i->getCondition(), pc);
                Label bp = controlFrom(pc, c, "the condition", line);
                State before = st_;
                diverged_ = false;
                eval(i->getThenExpr(), bp);
                const bool tdiv = diverged_, tthr = div_throw_;
                State then_st = std::move(st_);
                st_ = before;
                diverged_ = false;
                eval(i->getElseExpr(), bp);
                const bool ediv = i->getElseExpr() && diverged_, ethr = div_throw_;
                if (tdiv && !ediv) { /* only the else path continues: st_ is it */ }
                else if (ediv && !tdiv) st_ = std::move(then_st);
                else st_ = joinState(then_st, st_);
                diverged_ = tdiv && ediv;
                div_throw_ = diverged_ && tthr && ethr;
                break;
            }
            case K::LambdaExpr: {
                // A function value kept: its own sources and sinks are walked
                // now; the name that holds it carries it, so its calls are
                // followed where they happen.
                auto* lam = static_cast<ast::LambdaExpr*>(e);
                captureFor(lam);
                analyzeLambda(lam, Label{}, pc, line, false);
                Label l;
                l.fns.push_back(lam);
                return l;
            }
            case K::MatchExpr: {
                auto* m = static_cast<ast::MatchExpr*>(e);
                Label s = eval(m->getSubject(), pc);
                Label armpc = controlFrom(pc, s, "the match subject", line);
                State before = st_;
                State acc = st_;
                for (auto& arm : m->getArms()) {
                    st_ = before;
                    diverged_ = false;
                    st_.scopes.emplace_back();
                    Label here = armpc;
                    if (arm.pattern) {
                        if (auto* id = dynamic_cast<ast::IdentifierExpr*>(arm.pattern.get())) {
                            declare(id->getName(), s);  // a bare name binds the subject
                        } else if (auto* lp = dynamic_cast<ast::ListExpr*>(arm.pattern.get())) {
                            // A list pattern binds each bare-name element to an element
                            // of the subject, and compares the others with it; it used
                            // to be read as an expression, so `[a, b]` read the OUTER a
                            // (A121(a), as the shared rule's matchPatternBinders()).
                            for (const auto& el : lp->getElements()) {
                                if (auto* eid = dynamic_cast<ast::IdentifierExpr*>(el.get()))
                                    declare(eid->getName(), s);
                                else
                                    here = controlFrom(here, eval(el.get(), armpc), "a match pattern", line);
                            }
                        } else {
                            here = controlFrom(here, eval(arm.pattern.get(), armpc), "a match pattern", line);
                        }
                    }
                    if (arm.guard) {
                        Label g = eval(arm.guard.get(), here);
                        here = controlFrom(here, g, "a match guard", line);
                        armpc = controlFrom(armpc, g, "a match guard", line);  // later arms: this guard was false
                    }
                    eval(arm.body.get(), here);
                    st_.scopes.pop_back();
                    acc = joinState(acc, st_);
                }
                st_ = acc;
                diverged_ = div_throw_ = false;  // no arm may match; not followed further
                break;
            }
            case K::AwaitExpr:
                eval(static_cast<ast::AwaitExpr*>(e)->getExpr(), pc);
                break;
            case K::YieldExpr: {
                Label v = eval(static_cast<ast::YieldExpr*>(e)->getExpr(), pc);
                ctx_.ret = join(ctx_.ret, join(v, pc));
                break;
            }
            case K::ThrowExpr: {
                Label v = eval(static_cast<ast::ThrowExpr*>(e)->getExpr(), pc);
                throwHere(v, pc);
                diverged_ = div_throw_ = true;
                break;
            }
            case K::TryCatchExpr: {
                auto* tc = static_cast<ast::TryCatchExpr*>(e);
                State before = st_;
                ctx_.tries.push_back({});
                diverged_ = false;
                eval(tc->getTryExpr(), pc);
                const bool tdiv = diverged_ && div_throw_;
                TryFrame f = ctx_.tries.back();
                ctx_.tries.pop_back();
                State after_try = st_;
                st_ = joinState(before, after_try);
                Label cp = join(pc, f.cond);
                st_.scopes.emplace_back();
                declare(tc->getErrorName(), join(f.thrown, cp));
                diverged_ = false;
                eval(tc->getCatchExpr(), cp);
                const bool cdiv = diverged_, cthr = div_throw_;
                st_.scopes.pop_back();
                if (!tdiv) st_ = joinState(after_try, st_);
                // Whether the value is the try's or the catch's depends on the
                // throw's control -- unless the try always throws. The shared
                // rule does not model it, so it reaches the statement.
                if (!tdiv && f.cond.t) stmt_ctrl_ = join(stmt_ctrl_, f.cond);
                diverged_ = tdiv && cdiv;
                div_throw_ = diverged_ && cthr;
                break;
            }
            // Statements are not expressions; one here is not followed.
            case K::Program: case K::UseStatement: case K::FunctionDecl: case K::MainBlock:
            case K::StructDecl: case K::EnumDecl: case K::InterfaceDecl: case K::CompoundStmt:
            case K::ExprStmt: case K::ReturnStmt: case K::IfStmt: case K::ForStmt:
            case K::WhileStmt: case K::BreakStmt: case K::ContinueStmt: case K::VarDeclStmt:
            case K::ImportStmt: case K::ExportStmt: case K::TryStmt: case K::ThrowStmt:
            case K::ModuleUseStmt: case K::FunctionDeclStmt: case K::StructDeclStmt:
            case K::RuntimeDeclStmt: case K::DestructureStmt:
                blind("a node of an unexpected kind inside an expression");
                break;
        }
        return valueOf(e);
    }

    // The parts of an assignment target that run: a subscript's index, an
    // object that is not a plain name. The returned label is what choosing the
    // position depends on (`a[t] = 1` puts the change where t says).
    Label evalTargetParts(ast::Expr* target, const Label& pc) {
        if (auto* b = dynamic_cast<ast::BinaryExpr*>(target); b && b->getOp() == ast::BinaryOp::Subscript) {
            Label where = eval(b->getRight(), pc);
            if (!dynamic_cast<ast::IdentifierExpr*>(b->getLeft())) where = join(where, evalTargetParts(b->getLeft(), pc));
            return where;
        }
        if (auto* m = dynamic_cast<ast::MemberExpr*>(target)) {
            if (!dynamic_cast<ast::IdentifierExpr*>(m->getObject())) return evalTargetParts(m->getObject(), pc);
        }
        return Label{};
    }
    static std::string rootName(ast::Expr* target) {
        while (target) {
            if (auto* id = dynamic_cast<ast::IdentifierExpr*>(target)) return id->getName();
            if (auto* b = dynamic_cast<ast::BinaryExpr*>(target); b && b->getOp() == ast::BinaryOp::Subscript) {
                target = b->getLeft();
                continue;
            }
            if (auto* m = dynamic_cast<ast::MemberExpr*>(target)) { target = m->getObject(); continue; }
            break;
        }
        return "";
    }
    void assignTarget(ast::Expr* target, const Label& v, int line) {
        if (auto* id = dynamic_cast<ast::IdentifierExpr*>(target)) {
            assign(id->getName(), v);
            // The run's assignment sink, when taint_tracking.sinks lists it.
            if (pol_.is_sink && pol_.is_sink("variable.assign")) {
                if (v.x) record(FlowFinding::Kind::Value, "variable.assign", line, v.origin);
                else if (v.t) record(FlowFinding::Kind::Control, "variable.assign", line, v.origin);
            }
            return;
        }
        const std::string root = rootName(target);
        if (root.empty()) { blind("an assignment to a target the analysis does not follow"); return; }
        weakAssign(root, v);
    }

    void evalCall(ast::CallExpr* c, const Label& pc, const Label* pipe_left) {
        const int line = c->getLocation().line;
        ast::Expr* callee = c->getCallee();
        const std::string name = calleeName(callee);
        Label recv;
        bool on_value = false;   // a method on a value: not a stdlib call, so never a configured sink
        bool via_value = false;  // a call through a function value
        bool imported = false;
        std::string recv_var, method;
        if (auto* mem = dynamic_cast<ast::MemberExpr*>(callee)) {
            method = mem->getMember();
            if (auto* obj = dynamic_cast<ast::IdentifierExpr*>(mem->getObject())) {
                if (declared(obj->getName())) {
                    on_value = true;
                    recv_var = obj->getName();
                    recv = get(recv_var);
                } else if (imported_.count(obj->getName())) {
                    imported = true;
                }
            } else {
                recv = eval(mem->getObject(), pc);
                on_value = true;
            }
        } else if (auto* id = dynamic_cast<ast::IdentifierExpr*>(callee)) {
            if (declared(id->getName())) via_value = true;
            else if (imported_.count(id->getName())) imported = true;
        } else {
            eval(callee, pc);
            via_value = true;
        }

        // The call's arguments, by position (a lambda is a label that carries
        // it, so a user function's parameter can be called), and the function
        // values among them, which a stdlib call runs on its data.
        std::vector<Label> args;
        std::vector<ast::Expr*> fn_args;
        if (pipe_left) args.push_back(*pipe_left);
        for (const auto& a : c->getArgs()) {
            ast::Expr* x = a.get();
            if (x->getKind() == K::LambdaExpr) {
                captureFor(static_cast<ast::LambdaExpr*>(x));
                Label l;
                l.fns.push_back(static_cast<ast::LambdaExpr*>(x));
                args.push_back(l);
                fn_args.push_back(x);
                continue;
            }
            if (auto* id = dynamic_cast<ast::IdentifierExpr*>(x); id && !declared(id->getName()) && fns_.count(id->getName())) {
                args.push_back(Label{});  // a named function passed by reference carries no data
                fn_args.push_back(x);
                continue;
            }
            Label l = eval(x, pc);
            args.push_back(l);
            if (!l.fns.empty()) fn_args.push_back(x);
        }
        Label data = recv;
        for (const auto& l : args) data = join(data, l);
        Label extra;
        const bool to_user = !on_value && !via_value && !imported &&
                             dynamic_cast<ast::IdentifierExpr*>(callee) && fns_.count(name);
        if (!to_user) for (ast::Expr* f : fn_args) {
            if (auto* lam = dynamic_cast<ast::LambdaExpr*>(f)) {
                extra = join(extra, analyzeLambda(lam, data, pc, line, true).ret);
            } else if (!dynamic_cast<ast::IdentifierExpr*>(f) || declared(static_cast<ast::IdentifierExpr*>(f)->getName())) {
                // an argument that evaluates to function values
                Label held;
                if (auto* id = dynamic_cast<ast::IdentifierExpr*>(f)) held = get(id->getName());
                for (auto* lam : held.fns) extra = join(extra, analyzeLambda(lam, data, pc, line, true).ret);
            } else {
                auto* fn = fns_[static_cast<ast::IdentifierExpr*>(f)->getName()];
                std::vector<Label> fa(fn->getParams().size(), data);
                Summary s = analyzeCall(fn, fa, pc, line);
                // a callback may be called any number of times, so its divergence is not the call's
                if (s.throws.t || s.thrown_value.t) throwHere(s.thrown_value, s.throws);
                extra = join(extra, s.ret);
            }
        }

        bool user = false;
        if (!on_value && !via_value && !imported && dynamic_cast<ast::IdentifierExpr*>(callee)) {
            auto it = fns_.find(name);
            if (it != fns_.end()) {
                user = true;
                Summary s = analyzeCall(it->second, args, pc, line);
                afterCall(s);
                extra = join(extra, s.ret);
            }
        }
        if (via_value) {
            // A call through a name: follow the lambdas it may hold.
            std::vector<ast::LambdaExpr*> held;
            if (auto* id = dynamic_cast<ast::IdentifierExpr*>(callee)) held = get(id->getName()).fns;
            if (held.empty()) {
                blind("a call through a function value the analysis cannot resolve (the callee is not followed)");
            } else {
                bool all_diverge = true;
                for (auto* lam : held) {
                    LambdaResult r = analyzeLambda(lam, data, pc, line, true);
                    extra = join(extra, r.ret);
                    all_diverge = all_diverge && r.diverges;
                }
                if (all_diverge) diverged_ = div_throw_ = true;
            }
        }
        if (imported) blind("a call into an imported NAAb module (the callee is not followed)");
        if (on_value && runtimes_.count(recv_var)) blind("a method of a polyglot runtime handle (its code is not analysed)");

        if (!user && !via_value && !on_value && !imported) sinkCheck(name, data, pc, line);

        // A mutating method on a variable: it now holds what was put in it.
        if (on_value && !recv_var.empty() && isMutatingMethod(method)) {
            Label in;
            for (const auto& l : args) in = join(in, l);
            weakAssign(recv_var, join(in, pc));
        }
        call_extra_[c] = extra;
        // Inside a try: a call on tainted data may throw because of it.
        if (!ctx_.tries.empty() && data.t)
            ctx_.tries.back().cond = join(ctx_.tries.back().cond,
                Label::control("a call on tainted data at line " + std::to_string(line) + " that may throw (" + data.origin + ")"));
    }

    // --- statements ------------------------------------------------------------
    void exec(ast::Stmt* s, const Label& pc) {
        if (!s) return;
        const int line = s->getLocation().line;
        switch (s->getKind()) {
            case K::CompoundStmt:
                st_.scopes.emplace_back();
                for (const auto& x : static_cast<ast::CompoundStmt*>(s)->getStatements()) {
                    if (diverged_) break;  // every path here threw or returned: the rest does not run
                    exec(x.get(), pc);
                }
                st_.scopes.pop_back();
                break;
            case K::ExprStmt:
                stmt_ctrl_ = Label{};
                eval(static_cast<ast::ExprStmt*>(s)->getExpr(), eff(pc));
                break;
            case K::VarDeclStmt: {
                auto* v = static_cast<ast::VarDeclStmt*>(s);
                stmt_ctrl_ = Label{};
                Label e = eff(pc);
                Label val = eval(v->getInit(), e);
                declare(v->getName(), join(join(val, e), stmt_ctrl_));
                break;
            }
            case K::DestructureStmt: {
                auto* d = static_cast<ast::DestructureStmt*>(s);
                stmt_ctrl_ = Label{};
                Label e = eff(pc);
                Label val = join(eval(d->getInit(), e), e);
                val = join(val, stmt_ctrl_);
                for (const auto& n : d->getNames()) declare(n, val);
                break;
            }
            case K::ReturnStmt: {
                stmt_ctrl_ = Label{};
                Label e = eff(pc);
                Label v = eval(static_cast<ast::ReturnStmt*>(s)->getExpr(), e);
                ctx_.ret = join(ctx_.ret, join(join(v, e), stmt_ctrl_));
                if (e.t) ctx_.sticky = join(ctx_.sticky, e);
                diverged_ = true;
                div_throw_ = false;
                break;
            }
            case K::IfStmt: {
                auto* i = static_cast<ast::IfStmt*>(s);
                Label e = eff(pc);
                Label c = eval(i->getCondition(), e);
                Label bp = controlFrom(e, c, "the condition", line);
                State before = st_;
                diverged_ = false;
                exec(i->getThenBranch(), bp);
                const bool tdiv = diverged_, tthr = div_throw_;
                State then_st = std::move(st_);
                st_ = before;
                diverged_ = false;
                exec(i->getElseBranch(), bp);
                const bool ediv = i->getElseBranch() && diverged_, ethr = div_throw_;
                if (tdiv && !ediv) { /* only the else path continues */ }
                else if (ediv && !tdiv) st_ = std::move(then_st);
                else st_ = joinState(then_st, st_);
                diverged_ = tdiv && ediv;
                div_throw_ = diverged_ && tthr && ethr;
                break;
            }
            case K::WhileStmt: {
                auto* w = static_cast<ast::WhileStmt*>(s);
                ctx_.loop_sticky.push_back(Label{});
                for (int i = 0; i < kMaxLoopIterations; ++i) {
                    State before = st_;
                    bool sticky_before = ctx_.loop_sticky.back().t || ctx_.sticky.t;
                    Label e = eff(pc);
                    Label c = eval(w->getCondition(), e);
                    diverged_ = false;
                    exec(w->getBody(), controlFrom(e, c, "the loop condition", line));
                    st_ = joinState(before, st_);
                    bool sticky_after = ctx_.loop_sticky.back().t || ctx_.sticky.t;
                    if (i > 0 && sameTaint(before, st_) && sticky_before == sticky_after) break;
                    if (i == kMaxLoopIterations - 1) blind("a loop the analysis did not settle");
                }
                ctx_.loop_sticky.pop_back();
                diverged_ = div_throw_ = false;  // the loop may run no time at all
                break;
            }
            case K::ForStmt: {
                auto* f = static_cast<ast::ForStmt*>(s);
                Label e = eff(pc);
                Label it = eval(f->getIter(), e);
                ctx_.loop_sticky.push_back(Label{});
                for (int i = 0; i < kMaxLoopIterations; ++i) {
                    State before = st_;
                    bool sticky_before = ctx_.loop_sticky.back().t || ctx_.sticky.t;
                    Label bp = controlFrom(eff(pc), it, "the loop over a value", line);
                    st_.scopes.emplace_back();
                    Label elem = join(it, bp);
                    if (!f->getVar().empty()) declare(f->getVar(), elem);
                    if (!f->getIndexVar().empty()) declare(f->getIndexVar(), bp);
                    for (const auto& n : f->getDestructureNames()) declare(n, elem);
                    diverged_ = false;
                    exec(f->getBody(), bp);
                    st_.scopes.pop_back();
                    st_ = joinState(before, st_);
                    bool sticky_after = ctx_.loop_sticky.back().t || ctx_.sticky.t;
                    if (i > 0 && sameTaint(before, st_) && sticky_before == sticky_after) break;
                    if (i == kMaxLoopIterations - 1) blind("a loop the analysis did not settle");
                }
                ctx_.loop_sticky.pop_back();
                diverged_ = div_throw_ = false;
                break;
            }
            case K::BreakStmt:
            case K::ContinueStmt: {
                Label e = eff(pc);
                if (e.t && !ctx_.loop_sticky.empty())
                    ctx_.loop_sticky.back() = join(ctx_.loop_sticky.back(), e);
                diverged_ = true;
                div_throw_ = false;
                break;
            }
            case K::TryStmt: {
                auto* t = static_cast<ast::TryStmt*>(s);
                Label e = eff(pc);
                State before = st_;
                ctx_.tries.push_back({});
                diverged_ = false;
                exec(t->getTryBody(), e);
                const bool tdiv = diverged_, tthr = div_throw_;
                TryFrame f = ctx_.tries.back();
                ctx_.tries.pop_back();
                State after_try = st_;
                if (auto* cc = t->getCatchClause()) {
                    st_ = joinState(before, after_try);
                    Label cp = join(e, f.cond);
                    st_.scopes.emplace_back();
                    declare(cc->error_name, join(f.thrown, cp));
                    diverged_ = false;
                    exec(cc->body.get(), cp);
                    const bool cdiv = diverged_, cthr = div_throw_;
                    st_.scopes.pop_back();
                    // A try that always throws only continues through the catch.
                    if (!(tdiv && tthr)) st_ = joinState(after_try, st_);
                    diverged_ = tdiv && cdiv;
                    div_throw_ = diverged_ && cthr;
                } else {
                    if (f.thrown.t || f.cond.t) throwHere(f.thrown, f.cond);  // try/finally: on to the caller
                    diverged_ = tdiv;
                    div_throw_ = tthr;
                }
                if (t->getFinallyBody()) {
                    const bool kdiv = diverged_, kthr = div_throw_;
                    diverged_ = false;
                    exec(t->getFinallyBody(), e);
                    if (!diverged_) { diverged_ = kdiv; div_throw_ = kthr; }
                }
                break;
            }
            case K::ThrowStmt: {
                stmt_ctrl_ = Label{};
                Label e = eff(pc);
                Label v = eval(static_cast<ast::ThrowStmt*>(s)->getExpr(), e);
                throwHere(v, e);
                diverged_ = div_throw_ = true;
                break;
            }
            case K::FunctionDeclStmt: {
                auto* d = static_cast<ast::FunctionDeclStmt*>(s)->getDecl();
                if (!d) break;
                fns_[d->getName()] = d;
                // It reads the names around it (a nested function is a closure):
                // remember them, joined over every place it is declared. A
                // capture that gains taint changes summaries already made.
                Scope& c = fn_captured_[d];
                for (const auto& [n, l] : flatten(st_)) {
                    auto it = c.find(n);
                    if (it == c.end()) { c[n] = l; if (l.t) changed_ = true; continue; }
                    Label j = join(it->second, l);
                    if (j.t != it->second.t || j.x != it->second.x || j.fns.size() != it->second.fns.size())
                        changed_ = true;
                    it->second = j;
                }
                fn_ctx_[d] = ctx_.id;
                break;
            }
            case K::RuntimeDeclStmt:
                declare(static_cast<ast::RuntimeDeclStmt*>(s)->getName(), Label{});
                runtimes_.insert(static_cast<ast::RuntimeDeclStmt*>(s)->getName());
                break;
            case K::StructDeclStmt: case K::ImportStmt: case K::ExportStmt:
            case K::ModuleUseStmt: case K::UseStatement:
                break;  // no data moves
            case K::Program: case K::FunctionDecl: case K::MainBlock:
            case K::StructDecl: case K::EnumDecl: case K::InterfaceDecl:
            // Expressions are not statements; one here is not followed.
            case K::BinaryExpr: case K::UnaryExpr: case K::CallExpr: case K::MemberExpr:
            case K::IdentifierExpr: case K::LiteralExpr: case K::DictExpr: case K::ListExpr:
            case K::RangeExpr: case K::StructLiteralExpr: case K::InlineCodeExpr: case K::IfExpr:
            case K::LambdaExpr: case K::MatchExpr: case K::AwaitExpr: case K::YieldExpr:
            case K::TryCatchExpr: case K::ThrowExpr:
                blind("a statement of an unexpected kind");
                break;
        }
    }

    ast::Program& prog_;
    const FlowPolicy& pol_;
    std::map<std::string, ast::FunctionDecl*> fns_;
    std::set<std::string> imported_;
    std::set<std::string> runtimes_;
    std::vector<ast::VarDeclStmt*> exported_vars_;
    State module_scope_;
    State st_;
    FnCtx ctx_;
    std::map<std::string, Summary> memo_;
    std::set<std::string> in_progress_;
    std::map<const ast::CallExpr*, Label> call_extra_;
    std::set<std::string> finding_keys_;
    FlowReport report_;
    bool changed_ = false;
    bool converged_ = false;
    int depth_ = 0;
    // Reachability: every path to this point threw, returned or broke out
    // (div_throw_: and at least one way out of it was a throw on every path).
    bool diverged_ = false;
    bool div_throw_ = false;
    // Control the current statement's value depends on that the shared rule
    // does not model (whether a try expression's value came from its catch).
    Label stmt_ctrl_;
    int next_ctx_id_ = 0;
    std::map<const ast::LambdaExpr*, Scope> captured_;
    std::map<const ast::LambdaExpr*, int> lam_ctx_;
    std::map<const ast::FunctionDecl*, Scope> fn_captured_;
    std::map<const ast::FunctionDecl*, int> fn_ctx_;
};

}  // namespace

FlowReport analyzeProgram(ast::Program& program, const FlowPolicy& policy) {
    Analyzer a(program, policy);
    return a.run();
}

}  // namespace taint
}  // namespace naab
