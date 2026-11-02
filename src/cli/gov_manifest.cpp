// naab-gov manifest infer|sign|check -- the signed capability manifest
// (Bundle 11 PR 2, include/naab/capability_manifest.h).
//
//   infer PROGRAM [--output PATH] [--force]
//       Draft <stem>.manifest.json from what the program shows statically.
//   sign MANIFEST
//       Sign it with the approver's Ed25519 key (NAAB_SIGNING_KEY).
//   check PROGRAM
//       Would a run of PROGRAM under manifest.required accept its manifest?
//
// INFERENCE IS A DRAFT, NOT A GATE. It reads the program the way the run's
// gates classify it -- GovernanceEngine::filesystemAccessMode() for files, the
// http module gate, the env/process/agent gates, the shell-off rule for
// separate-process languages, inlineInterpreterCode() for process.run -- and
// lists what literal arguments name. What it cannot see (a computed path, host
// or agent name, effects inside a polyglot block, a module reached through an
// alias) it reports as a note, and the run refuses until a person adds it:
// the manifest fails closed, the draft only saves typing. A person reads the
// draft before signing it; that reading is the approval.
//
// Exit: 0 written / signed / accepted; 1 a signing failure (or `check` under
// mode audit); 3 `check` refused the manifest (the HARD block a run gets);
// 2 UNMEASURABLE (the walk met a node it does not know, so the draft
// would be incomplete; nothing is written); 4 usage, parse or I/O error.

#include "naab/ast.h"
#include "naab/bounded_read.h"
#include "naab/capability_manifest.h"
#include "naab/crypto_utils.h"
#include "naab/governance.h"
#include "naab/inline_interpreter.h"
#include "naab/language_descriptors.h"
#include "naab/lexer.h"
#include "naab/module_resolver.h"
#include "naab/parser.h"
#include "naab/stdlib.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace naab;

namespace {

// The text a run hashes: read exactly as naab-lang's read_file() reads it.
bool readProgramText(const std::string& path, std::string& out) {
    std::ifstream file(path);
    if (!file.is_open()) return false;
    std::stringstream buffer;
    buffer << file.rdbuf();
    out = buffer.str();
    return true;
}

std::unique_ptr<ast::Program> parseText(const std::string& text, const std::string& path,
                                        std::string& error) {
    try {
        lexer::Lexer lex(text);
        auto tokens = lex.tokenize();
        parser::Parser parser(tokens);
        parser.setSource(text, path);
        return parser.parseProgram();
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
}

// Languages that run inside naab-lang: never refused by the shell-off rule.
bool runsInProcess(const std::string& runtime) {
    if (runtime == "javascript" || runtime == "sql") return true;
#ifdef HAVE_PYBIND11
    if (runtime == "python") return true;
#endif
    return false;
}

// The host of a URL literal, lower-cased; "" when it names none.
std::string urlHost(const std::string& url) {
    auto scheme = url.find("://");
    if (scheme == std::string::npos) return "";
    std::string rest = url.substr(scheme + 3);
    auto end = rest.find_first_of("/?#");
    if (end != std::string::npos) rest = rest.substr(0, end);
    auto at = rest.rfind('@');
    if (at != std::string::npos) rest = rest.substr(at + 1);
    std::string host;
    if (!rest.empty() && rest[0] == '[') {
        auto close = rest.find(']');
        host = rest.substr(1, close == std::string::npos ? std::string::npos : close - 1);
    } else {
        host = rest.substr(0, rest.find(':'));
    }
    for (auto& c : host) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return host;
}

const std::set<std::string>& effectModules() {
    static const std::set<std::string> m = {
        "file", "io", "csv", "log", "path", "env", "http", "process", "agent", "codegen"};
    return m;
}

struct Inference {
    std::set<std::string> actions, languages, hosts, files_read, files_write, agents;
    std::map<std::string, std::string> modules;  // relative path -> sha256
    std::vector<std::string> notes;
    std::set<std::string> unwalked;              // node kinds the walk does not know
};

class Inferrer {
public:
    Inferrer(Inference& inf, fs::path base, stdlib::StdLib& lib)
        : inf_(inf), base_(std::move(base)), lib_(lib) {}

    void walkFile(ast::Program& prog, const fs::path& file) {
        const fs::path saved_dir = dir_;
        const auto saved_aliases = aliases_;
        dir_ = file.parent_path();
        aliases_.clear();
        program(prog);
        dir_ = saved_dir;
        aliases_ = saved_aliases;
    }

private:
    Inference& inf_;
    fs::path base_;
    fs::path dir_;
    stdlib::StdLib& lib_;
    std::map<std::string, std::string> aliases_;  // bound name -> stdlib module
    std::set<std::string> visiting_;

    void note(const std::string& n) {
        if (std::find(inf_.notes.begin(), inf_.notes.end(), n) == inf_.notes.end())
            inf_.notes.push_back(n);
    }

    const ast::LiteralExpr* stringLiteral(const ast::Expr* e) const {
        auto* lit = dynamic_cast<const ast::LiteralExpr*>(e);
        if (lit && lit->getLiteralKind() == ast::LiteralKind::String) return lit;
        return nullptr;
    }

    std::string moduleOf(const ast::Expr* e) const {
        auto* id = dynamic_cast<const ast::IdentifierExpr*>(e);
        if (!id) return "";
        auto it = aliases_.find(id->getName());
        if (it != aliases_.end()) return it->second;
        return effectModules().count(id->getName()) ? id->getName() : "";
    }

    void language(const std::string& tag) {
        const std::string rt = lang::runtimeLanguage(tag);
        inf_.languages.insert(rt);
        if (!runsInProcess(rt)) inf_.actions.insert("SHELL_EXEC");
    }

    // One stdlib call, classified as the run's gates classify it.
    void stdlibCall(const std::string& mod, const std::string& fn,
                    const std::vector<std::unique_ptr<ast::Expr>>* args, int line) {
        const size_t argc = args ? args->size() : 0;
        auto arg = [&](size_t i) -> const ast::Expr* {
            return (args && i < args->size()) ? (*args)[i].get() : nullptr;
        };
        const std::string where = "line " + std::to_string(line);

        const std::string fs_mode = governance::GovernanceEngine::filesystemAccessMode(mod, fn);
        if (!fs_mode.empty()) {
            const auto* a0 = stringLiteral(arg(0));
            const bool console = mod == "log" && a0 &&
                (a0->getValue() == "stderr" || a0->getValue() == "stdout");
            if (!console) {
                inf_.actions.insert(fs_mode == "write" ? "FS_WRITE" : "FS_READ");
                auto& list = (fs_mode == "write") ? inf_.files_write : inf_.files_read;
                if (a0) list.insert(a0->getValue());
                else if (argc > 0) note(mod + "." + fn + " at " + where + ": the path is computed; add it under files");
                else if (mod == "env" && fn == "load_dotenv") inf_.files_read.insert(".env");
                if ((fn == "copy" || fn == "move") && argc > 1) {
                    const auto* a1 = stringLiteral(arg(1));
                    if (a1 && a0) {
                        inf_.files_write.insert(governance::GovernanceEngine::copyMoveDestination(
                            a0->getValue(), a1->getValue()));
                    } else {
                        note(mod + "." + fn + " at " + where + ": the destination is computed; add it under files.write");
                    }
                }
            }
        }
        if (mod == "http") {
            inf_.actions.insert("NET_CONNECT");
            const auto* a0 = stringLiteral(arg(0));
            const std::string host = a0 ? urlHost(a0->getValue()) : "";
            if (!host.empty()) inf_.hosts.insert(host);
            else if (argc > 0) note("http." + fn + " at " + where + ": the host is computed; add it under hosts");
        } else if (mod == "env") {
            static const std::set<std::string> reads = {
                "get", "get_int", "get_float", "get_bool", "list", "get_all", "has"};
            static const std::set<std::string> writes = {"set", "set_var", "delete_var", "load_dotenv"};
            if (reads.count(fn)) inf_.actions.insert("ENV_READ");
            if (writes.count(fn)) inf_.actions.insert("ENV_WRITE");
        } else if (mod == "process") {
            if (fn == "run" || fn == "kill") inf_.actions.insert("SHELL_EXEC");
            if (fn == "run") {
                const auto* cmd = stringLiteral(arg(0));
                auto* list = dynamic_cast<const ast::ListExpr*>(arg(1));
                if (cmd && list) {
                    std::vector<std::string> argv;
                    bool literal = true;
                    for (const auto& e : list->getElements()) {
                        const auto* s = stringLiteral(e.get());
                        if (!s) { literal = false; break; }
                        argv.push_back(s->getValue());
                    }
                    std::string lang_name, code;
                    if (literal && stdlib::inlineInterpreterCode(cmd->getValue(), argv, lang_name, code))
                        language(lang_name);
                } else if (argc > 0) {
                    note("process.run at " + where + ": the command is computed; a language it runs inline must be added");
                }
            }
        } else if (mod == "agent") {
            static const std::set<std::string> sends = {
                "send", "run", "batch", "fan_out", "pipeline", "propose"};
            if (sends.count(fn)) inf_.actions.insert("AGENT_SEND");
            if (fn == "create") {
                const auto* a0 = stringLiteral(arg(0));
                if (a0) inf_.agents.insert(a0->getValue());
                else if (argc > 0) note("agent.create at " + where + ": the agent name is computed; add it under agents");
            }
        } else if (mod == "codegen") {
            if (fn == "run" || fn == "run_with_args" || fn == "run_strict") {
                const auto* a0 = stringLiteral(arg(0));
                if (a0) language(a0->getValue());
                else if (argc > 0) note("codegen." + fn + " at " + where + ": the language is computed; add it under languages");
                note("codegen." + fn + " at " + where + ": effects of generated code are not inferred");
            }
        }
    }

    // An import of `spec` from the current file. A stdlib name binds an alias;
    // a file module is pinned and walked, as the VM's OP_IMPORT resolves it.
    void importSpec(const std::string& spec, const std::string& bind, int line) {
        std::string bare = spec;
        if (bare.size() > 5 && bare.compare(bare.size() - 5, 5, ".naab") == 0) bare.resize(bare.size() - 5);
        auto slash = bare.rfind('/');
        if (slash != std::string::npos) bare = bare.substr(slash + 1);
        if (lib_.hasModule(bare)) {
            if (!bind.empty() && effectModules().count(bare)) aliases_[bind] = bare;
            return;
        }
        modules::ModuleResolver resolver;
        auto resolved = resolver.resolve(spec, dir_);
        if (!resolved) {
            note("import \"" + spec + "\" at line " + std::to_string(line) + ": not found; a module must be listed under modules");
            return;
        }
        const std::string canon = modules::ModuleResolver::canonicalizePath(*resolved);
        // The import itself is a filesystem read (A25): it needs FS_READ.
        inf_.actions.insert("FS_READ");
        // Keyed relative to the program's directory, as the run resolves it.
        const fs::path rel = fs::path(canon).lexically_relative(base_);
        const std::string key = rel.empty() ? canon : rel.generic_string();
        auto text = readFileBounded(canon);
        if (!text) {
            note("import \"" + spec + "\": could not be read");
            return;
        }
        inf_.modules[key] = security::CryptoUtils::sha256(*text);
        if (!visiting_.insert(canon).second) return;  // walked already (or a cycle)
        std::string err;
        auto prog = parseText(*text, canon, err);
        if (!prog) {
            note("import \"" + spec + "\": does not parse; its effects are not inferred");
            return;
        }
        walkFile(*prog, fs::path(canon));
    }

    void program(ast::Program& p) {
        for (auto& u : p.getModuleUses()) stmt(u.get());
        for (auto& i : p.getModuleImports()) stmt(i.get());
        for (auto& u : p.getImports()) node(u.get());
        for (auto& r : p.getRuntimeDecls()) stmt(r.get());
        for (auto& e : p.getExports()) stmt(e.get());
        for (auto& f : p.getFunctions()) function(f.get());
        for (auto& s : p.getStructs()) (void)s;
        if (p.getMainBlock()) stmt(p.getMainBlock()->getBody());
    }

    void function(ast::FunctionDecl* f) {
        if (!f) return;
        for (auto& prm : f->getParams())
            if (prm.default_value && *prm.default_value) expr(prm.default_value->get());
        stmt(f->getBody());
    }

    void node(ast::ASTNode* n) {
        if (!n) return;
        if (auto* u = dynamic_cast<ast::UseStatement*>(n)) {
            const std::string& id = u->getBlockId();
            if (id.rfind("./", 0) == 0 || id.rfind("../", 0) == 0) importSpec(id, u->getAlias(), u->getLocation().line);
            else note("use " + id + ": a block-library block's language and effects are not inferred");
            return;
        }
        if (auto* s = dynamic_cast<ast::Stmt*>(n)) { stmt(s); return; }
        if (auto* e = dynamic_cast<ast::Expr*>(n)) { expr(e); return; }
        inf_.unwalked.insert("node kind " + std::to_string(static_cast<int>(n->getKind())));
    }

    void stmt(ast::Stmt* s) {
        if (!s) return;
        using namespace ast;
        if (auto* c = dynamic_cast<CompoundStmt*>(s)) { for (auto& x : c->getStatements()) stmt(x.get()); return; }
        if (auto* e = dynamic_cast<ExprStmt*>(s)) { expr(e->getExpr()); return; }
        if (auto* r = dynamic_cast<ReturnStmt*>(s)) { expr(r->getExpr()); return; }
        if (auto* i = dynamic_cast<IfStmt*>(s)) { expr(i->getCondition()); stmt(i->getThenBranch()); stmt(i->getElseBranch()); return; }
        if (auto* f = dynamic_cast<ForStmt*>(s)) { expr(f->getIter()); stmt(f->getBody()); return; }
        if (auto* w = dynamic_cast<WhileStmt*>(s)) { expr(w->getCondition()); stmt(w->getBody()); return; }
        if (dynamic_cast<BreakStmt*>(s) || dynamic_cast<ContinueStmt*>(s)) return;
        if (auto* v = dynamic_cast<VarDeclStmt*>(s)) { expr(v->getInit()); return; }
        if (auto* d = dynamic_cast<DestructureStmt*>(s)) { expr(d->getInit()); return; }
        if (auto* t = dynamic_cast<TryStmt*>(s)) {
            stmt(t->getTryBody());
            if (t->getCatchClause()) stmt(t->getCatchClause()->body.get());
            stmt(t->getFinallyBody());
            return;
        }
        if (auto* t = dynamic_cast<ThrowStmt*>(s)) { expr(t->getExpr()); return; }
        if (auto* fd = dynamic_cast<FunctionDeclStmt*>(s)) { function(fd->getDecl()); return; }
        if (dynamic_cast<StructDeclStmt*>(s)) return;
        if (auto* rt = dynamic_cast<RuntimeDeclStmt*>(s)) { language(rt->getLanguage()); return; }
        if (auto* ex = dynamic_cast<ExportStmt*>(s)) {
            function(ex->getFunctionDecl());
            if (ex->getVarDecl()) stmt(ex->getVarDecl());
            expr(ex->getExpr());
            return;
        }
        if (auto* im = dynamic_cast<ImportStmt*>(s)) {
            importSpec(im->getModulePath(), im->isWildcard() ? im->getWildcardAlias() : "",
                       im->getLocation().line);
            return;
        }
        if (auto* mu = dynamic_cast<ModuleUseStmt*>(s)) {
            const std::string& path = mu->getModulePath();
            std::string bind = mu->hasAlias() ? mu->getAlias() : path.substr(path.find_last_of('.') + 1);
            static const std::set<std::string> kPrelude = {
                "io", "json", "http", "collections", "string", "array", "math",
                "time", "env", "csv", "regex", "crypto", "file", "debug",
                "bolo", "path", "dict", "log", "uuid", "validate", "process",
                "agent", "codegen", "governance", "orchestra"};
            if (kPrelude.count(path)) {
                if (effectModules().count(path)) aliases_[bind] = path;
            } else if (path.rfind("BLOCK-", 0) == 0) {
                note("use " + path + ": a block-library block's language and effects are not inferred");
            } else {
                std::string file_path = path;
                for (auto& c : file_path) if (c == '.') c = '/';
                importSpec(file_path, bind, mu->getLocation().line);
            }
            return;
        }
        inf_.unwalked.insert("statement kind " + std::to_string(static_cast<int>(s->getKind())));
    }

    void expr(ast::Expr* e) {
        if (!e) return;
        using namespace ast;
        if (auto* c = dynamic_cast<CallExpr*>(e)) {
            if (auto* m = dynamic_cast<MemberExpr*>(c->getCallee())) {
                const std::string mod = moduleOf(m->getObject());
                if (!mod.empty()) stdlibCall(mod, m->getMember(), &c->getArgs(), c->getLocation().line);
                else expr(m->getObject());
            } else {
                expr(c->getCallee());
            }
            for (auto& a : c->getArgs()) expr(a.get());
            return;
        }
        if (auto* m = dynamic_cast<MemberExpr*>(e)) {
            // `env.get_all` with no parentheses: the VM CALLS it.
            const std::string mod = moduleOf(m->getObject());
            if (!mod.empty()) stdlibCall(mod, m->getMember(), nullptr, m->getLocation().line);
            else expr(m->getObject());
            return;
        }
        if (auto* b = dynamic_cast<BinaryExpr*>(e)) { expr(b->getLeft()); expr(b->getRight()); return; }
        if (auto* u = dynamic_cast<UnaryExpr*>(e)) { expr(u->getOperand()); return; }
        if (dynamic_cast<IdentifierExpr*>(e) || dynamic_cast<LiteralExpr*>(e)) return;
        if (auto* d = dynamic_cast<DictExpr*>(e)) { for (auto& kv : d->getEntries()) { expr(kv.first.get()); expr(kv.second.get()); } return; }
        if (auto* l = dynamic_cast<ListExpr*>(e)) { for (auto& x : l->getElements()) expr(x.get()); return; }
        if (auto* r = dynamic_cast<RangeExpr*>(e)) { expr(r->getStart()); expr(r->getEnd()); return; }
        if (auto* s = dynamic_cast<StructLiteralExpr*>(e)) { for (auto& fi : s->getFieldInits()) expr(fi.second.get()); return; }
        if (auto* ic = dynamic_cast<InlineCodeExpr*>(e)) {
            language(ic->getLanguage());
            note("<<" + ic->getLanguage() + ">> at line " + std::to_string(ic->getLocation().line) +
                 ": effects inside a polyglot block are not inferred");
            return;
        }
        if (auto* i = dynamic_cast<IfExpr*>(e)) { expr(i->getCondition()); expr(i->getThenExpr()); expr(i->getElseExpr()); return; }
        if (auto* t = dynamic_cast<TryCatchExpr*>(e)) { expr(t->getTryExpr()); expr(t->getCatchExpr()); return; }
        if (auto* t = dynamic_cast<ThrowExpr*>(e)) { expr(t->getExpr()); return; }
        if (auto* l = dynamic_cast<LambdaExpr*>(e)) {
            for (auto& prm : l->getParams())
                if (prm.default_value && *prm.default_value) expr(prm.default_value->get());
            stmt(l->getBody());
            return;
        }
        if (auto* m = dynamic_cast<MatchExpr*>(e)) {
            expr(m->getSubject());
            for (auto& arm : m->getArms()) { expr(arm.pattern.get()); expr(arm.guard.get()); expr(arm.body.get()); }
            return;
        }
        if (auto* a = dynamic_cast<AwaitExpr*>(e)) { expr(a->getExpr()); return; }
        if (auto* y = dynamic_cast<YieldExpr*>(e)) { expr(y->getExpr()); return; }
        inf_.unwalked.insert("expression kind " + std::to_string(static_cast<int>(e->getKind())));
    }
};

int usage() {
    std::cerr <<
        "Usage:\n"
        "  naab-gov manifest infer PROGRAM [--output PATH] [--force]\n"
        "  naab-gov manifest sign MANIFEST\n"
        "  naab-gov manifest check PROGRAM\n";
    return 4;
}

int cmdInfer(const std::vector<std::string>& args) {
    std::string program_path, output;
    bool force = false;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--output" && i + 1 < args.size()) output = args[++i];
        else if (args[i] == "--force") force = true;
        else if (!args[i].empty() && args[i][0] == '-') return usage();
        else if (program_path.empty()) program_path = args[i];
        else return usage();
    }
    if (program_path.empty()) return usage();

    std::string text;
    if (!readProgramText(program_path, text)) {
        std::cerr << "naab-gov manifest: cannot read " << program_path << "\n";
        return 4;
    }
    std::string perr;
    auto prog = parseText(text, program_path, perr);
    if (!prog) {
        std::cerr << "naab-gov manifest: " << program_path << " does not parse: " << perr << "\n";
        return 4;
    }

    const fs::path abs = fs::absolute(program_path).lexically_normal();
    fs::path base = abs.parent_path();
    try { base = fs::weakly_canonical(base); } catch (...) {}

    Inference inf;
    stdlib::StdLib lib;
    Inferrer walker(inf, base, lib);
    walker.walkFile(*prog, abs);

    if (!inf.unwalked.empty()) {
        std::cout << "UNMEASURABLE: the walk met nodes it does not know, so the draft would be incomplete:\n";
        for (const auto& k : inf.unwalked) std::cout << "  " << k << "\n";
        std::cout << "Nothing was written.\n";
        return 2;
    }

    // The filesystem mode has no write-only setting: FS_WRITE without FS_READ
    // switches the filesystem off, as a lease does. files.read still bounds
    // what may be read.
    if (inf.actions.count("FS_WRITE") && !inf.actions.count("FS_READ")) {
        inf.actions.insert("FS_READ");
        inf.notes.push_back("FS_READ is listed with FS_WRITE: the filesystem has no write-only mode; "
                            "files.read still bounds what is read");
    }

    // The project's governance plugins are read through the import gate,
    // which asks for FS_READ (see the manifest's notes).
    {
        governance::GovernanceEngine gov;
        if (gov.discoverAndLoad(base.string()) && !gov.getRules().governance_plugins.empty()) {
            inf.actions.insert("FS_READ");
            inf.notes.push_back("govern.json loads governance plugins: their files are read through the "
                                "import gate, which needs FS_READ");
        }
    }

    governance::CapabilityManifest m;
    m.program_file = abs.filename().string();
    m.program_sha256 = security::CryptoUtils::sha256(text);
    m.modules = inf.modules;
    m.actions.assign(inf.actions.begin(), inf.actions.end());
    m.languages.assign(inf.languages.begin(), inf.languages.end());
    m.hosts.assign(inf.hosts.begin(), inf.hosts.end());
    m.files_read.assign(inf.files_read.begin(), inf.files_read.end());
    m.files_write.assign(inf.files_write.begin(), inf.files_write.end());
    m.agents.assign(inf.agents.begin(), inf.agents.end());

    if (output.empty()) output = governance::capabilityManifestPathFor(abs.string());
    std::error_code ec;
    if (fs::exists(output, ec) && !force) {
        std::cerr << "naab-gov manifest: " << output << " exists; pass --force to replace it\n";
        return 4;
    }
    const std::string body = governance::serializeCapabilityManifest(m, inf.notes);
    {
        std::ofstream out(output, std::ios::binary | std::ios::trunc);
        if (!out.is_open() || !(out << body)) {
            std::cerr << "naab-gov manifest: cannot write " << output << "\n";
            return 4;
        }
    }
    // The old signature, if any, signed other text: remove it, so a stale
    // .sig can never be read as approval of this draft.
    fs::remove(output + ".sig", ec);

    std::cout << "Drafted " << output << "\n";
    std::cout << "  actions:   " << m.actions.size() << "\n"
              << "  languages: " << m.languages.size() << "\n"
              << "  hosts:     " << m.hosts.size() << "\n"
              << "  files:     " << m.files_read.size() << " read, " << m.files_write.size() << " write\n"
              << "  agents:    " << m.agents.size() << "\n"
              << "  modules:   " << m.modules.size() << "\n";
    if (!inf.notes.empty()) {
        std::cout << "Not inferred (" << inf.notes.size() << "); a run refuses each until it is added by hand:\n";
        for (const auto& n : inf.notes) std::cout << "  - " << n << "\n";
    }
    std::cout << "Review the draft, then have it signed: naab-gov manifest sign " << output << "\n";
    return 0;
}

int cmdSign(const std::vector<std::string>& args) {
    if (args.size() != 1) return usage();
    const char* key = std::getenv("NAAB_SIGNING_KEY");
    if (!key || !*key) {
        std::cerr << "naab-gov manifest: a capability manifest is signed with the approver's Ed25519 key;\n"
                     "  set NAAB_SIGNING_KEY to the private key file.\n";
        return 1;
    }
    std::string text;
    if (!readProgramText(args[0], text)) {
        std::cerr << "naab-gov manifest: cannot read " << args[0] << "\n";
        return 4;
    }
    governance::CapabilityManifest m;
    std::string err;
    if (!governance::parseCapabilityManifest(text, governance::GovernanceEngine::leaseActions(), m, err)) {
        std::cerr << "naab-gov manifest: " << args[0] << " is not a capability manifest: " << err << "\n";
        return 4;
    }
    return governance::GovernanceEngine::signFile(args[0]) ? 0 : 1;
}

int cmdCheck(const std::vector<std::string>& args) {
    if (args.size() != 1) return usage();
    std::string text;
    if (!readProgramText(args[0], text)) {
        std::cerr << "naab-gov manifest: cannot read " << args[0] << "\n";
        return 4;
    }
    governance::GovernanceEngine gov;
    const fs::path dir = fs::absolute(args[0]).parent_path();
    if (!gov.discoverAndLoad(dir.string())) {
        std::cerr << "naab-gov manifest: no govern.json loads for " << args[0] << "\n";
        return 4;
    }
    if (!gov.getRules().manifest.required) {
        std::cout << "manifest.required is not set: a run of " << args[0] << " reads no manifest\n";
        return 0;
    }
    const std::string err = gov.requireCapabilityManifest(args[0], text);
    if (!err.empty()) {
        std::cout << err << "\n";
        return 1;
    }
    std::cout << "ACCEPTED: the manifest is signed by a trusted key and approves this program text\n";
    return 0;
}

}  // namespace

int cmdManifest(const std::vector<std::string>& args, const char* /*argv0*/) {
    if (args.empty()) return usage();
    const std::vector<std::string> rest(args.begin() + 1, args.end());
    if (args[0] == "infer") return cmdInfer(rest);
    if (args[0] == "sign") return cmdSign(rest);
    if (args[0] == "check") return cmdCheck(rest);
    return usage();
}
