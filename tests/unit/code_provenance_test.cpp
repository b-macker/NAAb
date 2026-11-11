// The code-provenance ledger (Bundle 14, include/naab/code_provenance.h).
//
// The NAAb-level arms -- what an execution event records for code a model
// wrote -- are tests/governance_v4/test_code_provenance.sh. These isolate what
// a program cannot reach cheaply: the bound, what eviction does to a match,
// and records made from several threads at once.

#include <gtest/gtest.h>

#include "naab/code_provenance.h"

#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using naab::governance::CodeProvenanceLedger;
using naab::governance::ModelOutputOrigin;
using naab::governance::addCodeProvenanceFields;

namespace {

ModelOutputOrigin origin(const std::string& reply) {
    ModelOutputOrigin o;
    o.model = "m";
    o.request_sha256 = "req";
    o.content_sha256 = naab::security::CryptoUtils::sha256(reply);
    o.config_name = "worker";
    o.handle_id = 1;
    o.turn_at_request = 0;
    o.derivation = "response";
    return o;
}

std::unordered_map<std::string, std::string> fieldsFor(const std::string& code) {
    std::unordered_map<std::string, std::string> f;
    addCodeProvenanceFields(f, code);
    return f;
}

class CodeProvenance : public ::testing::Test {
protected:
    void SetUp() override { CodeProvenanceLedger::instance().clearForTesting(); }
    void TearDown() override { CodeProvenanceLedger::instance().clearForTesting(); }
};

TEST_F(CodeProvenance, RecordedTextMatchesAndOtherTextDoesNot) {
    CodeProvenanceLedger::instance().record("print(1)", origin("print(1)"));
    auto hit = fieldsFor("print(1)");
    EXPECT_EQ(hit["code_attribution"], "model_output_match");
    EXPECT_EQ(hit["origin_model"], "m");
    auto miss = fieldsFor("print(1) ");
    EXPECT_EQ(miss["code_attribution"], "unattributed");
    EXPECT_EQ(miss.count("origin_model"), 0u);
    EXPECT_NE(miss["code_sha256"], hit["code_sha256"]);
}

TEST_F(CodeProvenance, EmptyTextIsNeverRecordedOrMatched) {
    CodeProvenanceLedger::instance().record("", origin(""));
    EXPECT_EQ(CodeProvenanceLedger::instance().size(), 0u);
    EXPECT_EQ(fieldsFor("")["code_attribution"], "unattributed");
}

TEST_F(CodeProvenance, TheLedgerIsBoundedAndEvictionOnlyLosesAMatch) {
    auto& L = CodeProvenanceLedger::instance();
    L.record("first", origin("first"));
    ASSERT_EQ(fieldsFor("first")["code_attribution"], "model_output_match");
    for (size_t i = 0; i < CodeProvenanceLedger::kMaxEntries; i++)
        L.record("t" + std::to_string(i), origin("r" + std::to_string(i)));
    EXPECT_EQ(L.size(), CodeProvenanceLedger::kMaxEntries);
    // The oldest entry is gone: its text reads unattributed, never as some
    // other entry's origin.
    EXPECT_EQ(fieldsFor("first")["code_attribution"], "unattributed");
    // The newest is still there (control: the ledger did not empty itself).
    EXPECT_EQ(fieldsFor("t" + std::to_string(CodeProvenanceLedger::kMaxEntries - 1))
                  ["code_attribution"], "model_output_match");
}

TEST_F(CodeProvenance, DerivedTextIsRecordedOnlyFromAKnownInput) {
    auto& L = CodeProvenanceLedger::instance();
    L.record("reply with code", origin("reply with code"));
    L.recordDerived("reply with code", "code", "extract_code");
    auto f = fieldsFor("code");
    EXPECT_EQ(f["code_attribution"], "model_output_match");
    EXPECT_EQ(f["origin_derivation"], "extract_code");
    // Text taken out of something no model wrote stays unattributed.
    L.recordDerived("the program's own text", "own", "extract_code");
    EXPECT_EQ(fieldsFor("own")["code_attribution"], "unattributed");
}

TEST_F(CodeProvenance, TheFirstOriginIsKeptAndDistinctRepliesCounted) {
    auto& L = CodeProvenanceLedger::instance();
    ModelOutputOrigin a = origin("reply A"); a.model = "model-a";
    ModelOutputOrigin b = origin("reply B"); b.model = "model-b";
    L.record("same", a);
    L.record("same", b);
    L.record("same", b);   // the same reply again is not a third
    auto f = fieldsFor("same");
    EXPECT_EQ(f["origin_model"], "model-a");
    EXPECT_EQ(f["origin_distinct_replies"], "2");
}

TEST_F(CodeProvenance, ConcurrentRecordsAndLookupsKeepTheBound) {
    auto& L = CodeProvenanceLedger::instance();
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; t++) {
        threads.emplace_back([t, &L] {
            for (int i = 0; i < 3000; i++) {
                const std::string text = "t" + std::to_string(t) + "-" + std::to_string(i);
                L.record(text, origin(text));
                (void)fieldsFor(text);
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(L.size(), CodeProvenanceLedger::kMaxEntries);
}

}  // namespace
