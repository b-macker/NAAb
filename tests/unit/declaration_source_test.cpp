// The per-function governance checks read the text an engine PARSED
// (include/naab/declaration_source.h, docs/open-investigations.md A45).
//
// Two halves. The extraction rules, as pure functions -- each one a defect a
// single engine's copy had. And both engines given a program whose file name
// does not exist on disk, which is what REST ("api-request.naab") and
// libnaab's Context::eval ("<embedded>") hand them: the checks must still run.
// The VM half is invisible from the CLI, where the file on disk IS the text
// parsed, so it is tested here.

#include <gtest/gtest.h>

#include "naab/compiler.h"
#include "naab/declaration_source.h"
#include "naab/governance.h"
#include "naab/interpreter.h"
#include "naab/lexer.h"
#include "naab/parser.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

namespace fs = std::filesystem;
using naab::source::declarationText;
using naab::source::splitLines;

namespace {

std::vector<std::string> getlineSplit(const std::string& text) {
    std::istringstream in(text);
    std::vector<std::string> out;
    std::string ln;
    while (std::getline(in, ln)) out.push_back(ln);
    return out;
}

}  // namespace

// Line numbers are the AST's, so the split must be getline's exactly.
TEST(DeclarationSource, SplitLinesMatchesGetline) {
    for (const std::string text : {"", "a", "a\n", "a\n\nb", "\n", "a\r\nb\n", "x\n\n\n"}) {
        EXPECT_EQ(splitLines(text), getlineSplit(text)) << "input: [" << text << "]";
    }
}

// 11146ceb taught only the VM's copy to skip braces in strings.
TEST(DeclarationSource, BraceInStringDoesNotEndTheBody) {
    auto lines = splitLines("fn f(x) {\n    let s = \"}\"\n    // TODO: x\n    return x\n}\n");
    std::string body = declarationText(lines, 1, "f");
    EXPECT_NE(body.find("TODO"), std::string::npos) << body;
}

TEST(DeclarationSource, BraceInCommentDoesNotEndTheBody) {
    auto lines = splitLines("fn f(x) {\n    // a } here\n    // TODO: x\n    return x\n}\n");
    EXPECT_NE(declarationText(lines, 1, "f").find("TODO"), std::string::npos);
}

// 7d4fda0f's search looked for "fn NAME(" and, not finding it, started five
// lines early -- inside the previous function.
TEST(DeclarationSource, SpaceBeforeParenReadsThisFunction) {
    auto lines = splitLines(
        "fn helper() {\n    return 1\n}\nfn stub (x) {\n    // TODO: x\n    return x\n}\n");
    std::string body = declarationText(lines, 4, "stub");
    EXPECT_EQ(body.rfind("fn stub (x) {", 0), 0u) << body;
    EXPECT_NE(body.find("TODO"), std::string::npos) << body;
    EXPECT_EQ(body.find("helper"), std::string::npos) << body;
}

// When no line in the window declares the name, the AST line is the start.
TEST(DeclarationSource, NoDeclarationInWindowStartsAtTheAstLine) {
    auto lines = splitLines("fn helper() {\n    return 1\n}\nfn other(x) {\n    return x\n}\n");
    std::string body = declarationText(lines, 4, "absent");
    EXPECT_EQ(body.rfind("fn other(x) {", 0), 0u) << body;
}

// The reason the backward search exists: an AST line below `fn`.
TEST(DeclarationSource, AstLineBelowTheDeclarationFindsIt) {
    auto lines = splitLines("export fn g(x)\n{\n    // TODO: y\n    return x\n}\n");
    std::string body = declarationText(lines, 2, "g");
    EXPECT_EQ(body.rfind("export fn g(x)", 0), 0u) << body;
    EXPECT_NE(body.find("TODO"), std::string::npos) << body;
}

// A name that only CONTAINS the declared one is not it.
TEST(DeclarationSource, WholeWordName) {
    EXPECT_TRUE(naab::source::declaresFunction("fn stub(x) {", "stub"));
    EXPECT_TRUE(naab::source::declaresFunction("export async fn stub <T>(x)", "stub"));
    EXPECT_FALSE(naab::source::declaresFunction("fn stubby(x) {", "stub"));
    EXPECT_FALSE(naab::source::declaresFunction("let fnstub = 1", "stub"));
    EXPECT_FALSE(naab::source::declaresFunction("defn stub(x)", "stub"));
}

TEST(DeclarationSource, MainBlock) {
    auto lines = splitLines("fn a() {\n    return 1\n}\nmain {\n    // TODO: z\n}\n");
    std::string body = declarationText(lines, 4, "");
    EXPECT_EQ(body, "main {\n    // TODO: z\n}\n");
}

TEST(DeclarationSource, OutOfRange) {
    auto lines = splitLines("fn a() {\n}\n");
    EXPECT_EQ(declarationText(lines, 0, "a"), "");
    EXPECT_EQ(declarationText(lines, 3, "a"), "");
}

// ---------------------------------------------------------------------------
// Both engines, given a file name that does not exist on disk
// ---------------------------------------------------------------------------

namespace {

const char* kTodoProgram =
    "fn compute(x) {\n    // TODO: handle negative input\n    let y = x * 2\n    return y + 1\n}\n"
    "main {\n    print(compute(3))\n}\n";
const char* kCleanProgram =
    "fn compute(x) {\n    let y = x * 2\n    return y + 1\n}\n"
    "main {\n    print(compute(3))\n}\n";

// A project directory holding only govern.json, and an empty trust store so
// the unsigned config loads whatever keys this machine trusts.
class NotOnDisk : public ::testing::Test {
protected:
    fs::path dir_;
    fs::path trust_;
    std::string saved_trust_;
    bool had_trust_ = false;

    void SetUp() override {
        std::random_device rd;
        auto base = fs::temp_directory_path() / ("naab-declsrc-" + std::to_string(rd()));
        dir_ = base / "project";
        trust_ = base / "trust";
        fs::create_directories(dir_);
        fs::create_directories(trust_);
        std::ofstream(dir_ / "govern.json")
            << "{ \"version\": \"5.0\", \"mode\": \"enforce\","
               " \"security\": { \"sandbox_level\": \"elevated\" },"
               " \"code_quality\": { \"no_placeholders\": { \"enabled\": true, \"level\": \"hard\" } } }\n";
        if (const char* t = std::getenv("NAAB_TRUST_STORE_DIR")) { had_trust_ = true; saved_trust_ = t; }
        setenv("NAAB_TRUST_STORE_DIR", trust_.c_str(), 1);
    }
    void TearDown() override {
        if (had_trust_) setenv("NAAB_TRUST_STORE_DIR", saved_trust_.c_str(), 1);
        else unsetenv("NAAB_TRUST_STORE_DIR");
        std::error_code ec;
        fs::remove_all(dir_.parent_path(), ec);
    }
    std::string missingFile() const { return (dir_ / "not-on-disk.naab").string(); }

    static std::unique_ptr<naab::ast::Program> parse(const std::string& src) {
        naab::lexer::Lexer lexer(src);
        auto tokens = lexer.tokenize();
        naab::parser::Parser parser(tokens);
        return parser.parseProgram();
    }

    // The tree-walker, as REST and Context::eval drive it. Returns what() of
    // the error, or "" when the program ran.
    std::string runTreeWalker(const std::string& src) {
        auto program = parse(src);
        naab::interpreter::Interpreter interp;
        try {
            interp.setSourceCode(src, missingFile());
            interp.execute(*program);
        } catch (const std::exception& e) {
            return e.what();
        }
        return "";
    }

    // The VM compiler, as main.cpp / naab-gov lint / VM::importModule drive it.
    std::string compileVm(const std::string& src, bool give_text) {
        auto program = parse(src);
        naab::governance::GovernanceEngine engine;
        if (!engine.loadFromFile((dir_ / "govern.json").string())) return "CONFIG NOT LOADED";
        naab::vm::Compiler compiler;
        compiler.setGovernance(&engine);
        if (give_text) compiler.setSourceText(src);
        try {
            compiler.compile(*program, missingFile());
        } catch (const std::exception& e) {
            return e.what();
        }
        return "";
    }
};

bool mentionsPlaceholder(const std::string& s) {
    return s.find("laceholder") != std::string::npos || s.find("TODO") != std::string::npos;
}

}  // namespace

TEST_F(NotOnDisk, TreeWalkerChecksTheTextItWasGiven) {
    ASSERT_FALSE(fs::exists(missingFile()));
    std::string err = runTreeWalker(kTodoProgram);
    EXPECT_TRUE(mentionsPlaceholder(err)) << "ran, or failed otherwise: [" << err << "]";
}

TEST_F(NotOnDisk, TreeWalkerRunsACleanProgram) {
    EXPECT_EQ(runTreeWalker(kCleanProgram), "");
}

TEST_F(NotOnDisk, VmCompilerChecksTheTextItWasGiven) {
    ASSERT_FALSE(fs::exists(missingFile()));
    std::string err = compileVm(kTodoProgram, /*give_text=*/true);
    EXPECT_TRUE(mentionsPlaceholder(err)) << "compiled, or failed otherwise: [" << err << "]";
}

TEST_F(NotOnDisk, VmCompilerCompilesACleanProgram) {
    EXPECT_EQ(compileVm(kCleanProgram, /*give_text=*/true), "");
}

// The fallback, pinned: given no text the compiler reads the file, and a
// missing file has nothing to check -- so the text is what makes the
// difference in the arm above, not some other path.
TEST_F(NotOnDisk, VmCompilerWithoutTheTextReadsTheMissingFile) {
    EXPECT_EQ(compileVm(kTodoProgram, /*give_text=*/false), "");
}
