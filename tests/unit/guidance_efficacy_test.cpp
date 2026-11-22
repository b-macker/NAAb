// Guidance efficacy (Bundle 20 PR 4, include/naab/guidance_efficacy.h).
//
// The end-to-end arms -- deliveries a real run makes, judged at the next run's
// start and by `naab-gov efficacy` -- are tests/governance_v4/test_guidance_efficacy.sh.
// These pin the judge's rules on event sequences a run cannot easily produce
// on demand: an intervention on another handle, an unaccepted reload, a window
// across two models, the held-out arm, the tally arithmetic.
//
// Each expected value is written out by hand from the rule in the header, and
// the holdout vectors were computed outside the engine (Python hashlib:
// int(sha256("run-1:0")[:13], 16) / 16**13), so the draw is not checked
// against itself.

#include <gtest/gtest.h>

#include "naab/guidance_efficacy.h"

#include <set>
#include <string>
#include <vector>

namespace gd = naab::guidance;

namespace {

gd::Event ev(const std::string& type, const std::string& handle,
             std::map<std::string, std::string> f, const std::string& run = "r1") {
    gd::Event e;
    e.type = type;
    e.run = run;
    e.handle = handle;
    e.f = std::move(f);
    return e;
}

gd::Event delivered(const std::string& trigger, int turn = 2, const std::string& handle = "1") {
    return ev("GUIDANCE_DELIVERED", handle,
              {{"channel", "agent"}, {"trigger", trigger}, {"turn", std::to_string(turn)},
               {"delivery_id", "g1"}, {"section_key", std::string(32, 'a')}});
}

gd::Event turn(int t, const std::string& penalties, const std::string& model = "m1",
               const std::string& analyzed = "true", const std::string& handle = "1") {
    return ev("CDD_TURN", handle,
              {{"turn", std::to_string(t)}, {"analyzed", analyzed},
               {"penalties_detail", penalties}, {"model", model}});
}

std::string only(const std::vector<gd::Event>& events) {
    auto out = gd::judge(events);
    if (out.size() != 1) return "outcomes=" + std::to_string(out.size());
    return out[0].result;
}

}  // namespace

TEST(GuidanceEfficacy, HoldoutDrawMatchesAnOutsideComputation) {
    EXPECT_NEAR(gd::holdoutDraw("run-1", 0), 0.3165320551882922, 1e-12);
    EXPECT_NEAR(gd::holdoutDraw("run-1", 1), 0.6359865478757789, 1e-12);
    EXPECT_NEAR(gd::holdoutDraw("1795370149894-8151", 2), 0.0076001394145541745, 1e-12);
    for (unsigned long long i = 0; i < 200; ++i) {
        const double d = gd::holdoutDraw("any-run", i);
        EXPECT_GE(d, 0.0);
        EXPECT_LT(d, 1.0);
    }
}

TEST(GuidanceEfficacy, EveryTableEntryIsClassifiedOnce) {
    std::set<std::string> seen;
    for (const auto& [type, cls] : gd::eventClassTable()) {
        EXPECT_TRUE(seen.insert(type).second) << "listed twice: " << type;
        const std::string c = cls;
        EXPECT_TRUE(c == "intervention" || c == "observation") << type << " -> " << c;
    }
    EXPECT_EQ(gd::eventClass("NOT_AN_EVENT"), nullptr);
    EXPECT_STREQ(gd::eventClass("MANDATE_INJECTION"), "intervention");
    EXPECT_STREQ(gd::eventClass("CDD_TURN"), "observation");
}

TEST(GuidanceEfficacy, RecurredWhenTheSignalPaysAgain) {
    EXPECT_EQ(only({delivered("signal:response_repetition"),
                    turn(3, "response_repetition=0.1500")}), "recurred");
}

TEST(GuidanceEfficacy, QuietAfterThreeAnalysedTurnsWithoutIt) {
    EXPECT_EQ(only({delivered("signal:response_repetition"),
                    turn(3, "circular=0.1000"), turn(4, ""), turn(5, "")}), "quiet");
}

TEST(GuidanceEfficacy, TheTelemetryLabelIsReadForAConfigKey) {
    // circular_actions is the config key; CDD_TURN prints "circular".
    EXPECT_EQ(only({delivered("signal:circular_actions"), turn(3, "circular=0.1000")}), "recurred");
    EXPECT_EQ(only({delivered("signal:circular_actions"),
                    turn(3, "circular_actions=0.1000"), turn(4, ""), turn(5, "")}), "quiet");
}

TEST(GuidanceEfficacy, OnlyAnalysedTurnsCount) {
    // Two analysed quiet turns and one interval-skipped row that re-shows the
    // old penalty: neither recurred nor quiet -- the run ended first.
    EXPECT_EQ(only({delivered("signal:response_repetition"),
                    turn(3, ""), turn(4, "response_repetition=0.1500", "m1", "false"), turn(5, "")}),
              "incomplete");
}

TEST(GuidanceEfficacy, TurnsBeforeTheDeliveryAreNotInItsWindow) {
    EXPECT_EQ(only({delivered("signal:response_repetition", 5),
                    turn(4, "response_repetition=0.1500"), turn(6, ""), turn(7, ""), turn(8, "")}),
              "quiet");
}

TEST(GuidanceEfficacy, AnInterventionMakesTheWindowUndetermined) {
    EXPECT_EQ(only({delivered("signal:response_repetition"),
                    ev("MANDATE_INJECTION", "1", {}),
                    turn(3, "response_repetition=0.1500")}), "undetermined");
    // ... but only on the same handle.
    EXPECT_EQ(only({delivered("signal:response_repetition"),
                    ev("MANDATE_INJECTION", "2", {}),
                    turn(3, "response_repetition=0.1500")}), "recurred");
    // ... and an intervention with no handle is run-wide.
    EXPECT_EQ(only({delivered("signal:response_repetition"),
                    ev("AGENT_CHALLENGE_PASS", "", {}),
                    turn(3, "")}), "undetermined");
}

TEST(GuidanceEfficacy, OnlyAnAcceptedReloadIntervenes) {
    EXPECT_EQ(only({delivered("signal:response_repetition"),
                    ev("CONFIG_ADJUSTMENT", "", {{"accepted", "false"}}),
                    turn(3, "response_repetition=0.1500")}), "recurred");
    EXPECT_EQ(only({delivered("signal:response_repetition"),
                    ev("CONFIG_ADJUSTMENT", "", {{"accepted", "true"}}),
                    turn(3, "response_repetition=0.1500")}), "undetermined");
}

TEST(GuidanceEfficacy, AnotherDeliveryWithTheSameRequestIsNotASecondIntervention) {
    auto outs = gd::judge({delivered("signal:response_repetition"),
                           ev("GUIDANCE_DELIVERED", "1", {{"channel", "agent"}, {"trigger", "start"},
                                                          {"turn", "2"}, {"delivery_id", "g2"}}),
                           turn(3, "response_repetition=0.1500")});
    ASSERT_EQ(outs.size(), 2u);
    EXPECT_EQ(outs[0].result, "recurred");
    EXPECT_EQ(outs[1].result, "not_judged");   // a start trigger
    // A terminal delivery reaches no agent: it is neither an intervention in
    // the window nor an outcome of its own.
    EXPECT_EQ(only({delivered("signal:response_repetition"),
                    ev("GUIDANCE_DELIVERED", "", {{"channel", "terminal"}, {"trigger", "violation:x"}}),
                    turn(3, "response_repetition=0.1500")}), "recurred");
    // A pulled section may be passed to any agent: run-wide.
    EXPECT_EQ(only({delivered("signal:response_repetition"),
                    ev("GUIDANCE_DELIVERED", "", {{"channel", "script"}, {"trigger", "pull"}}),
                    turn(3, "response_repetition=0.1500")}), "undetermined");
}

TEST(GuidanceEfficacy, AWindowAcrossTwoModelsIsMixed) {
    EXPECT_EQ(only({delivered("signal:response_repetition"),
                    turn(3, "", "primary"), turn(4, "", "backup"), turn(5, "", "backup")}),
              "mixed_model");
}

TEST(GuidanceEfficacy, TheHeldOutArmIsJudgedTheSameWay) {
    auto outs = gd::judge({ev("GUIDANCE_WITHHELD", "1",
                              {{"channel", "agent"}, {"reason", "holdout"}, {"holdout_ordinal", "4"},
                               {"trigger", "signal:response_repetition"}, {"turn", "2"}}),
                           turn(3, "response_repetition=0.1500")});
    ASSERT_EQ(outs.size(), 1u);
    EXPECT_EQ(outs[0].arm, "held_out");
    EXPECT_EQ(outs[0].id, "h4");
    EXPECT_EQ(outs[0].result, "recurred");
    // A cooldown or a budget is not an arm.
    EXPECT_TRUE(gd::judge({ev("GUIDANCE_WITHHELD", "1",
                              {{"channel", "agent"}, {"reason", "cooldown"},
                               {"trigger", "signal:response_repetition"}, {"turn", "2"}})}).empty());
}

TEST(GuidanceEfficacy, AValidationTriggerIsJudgedByTheNextValidation) {
    EXPECT_EQ(only({delivered("validation_failed"),
                    ev("VALIDATION_RECORDED", "1", {{"passed", "true"}})}), "resolved");
    EXPECT_EQ(only({delivered("validation_failed"),
                    ev("VALIDATION_RECORDED", "1", {{"passed", "false"}})}), "recurred");
    EXPECT_EQ(only({delivered("validation_failed")}), "incomplete");
}

TEST(GuidanceEfficacy, AnotherRunsEventsAreNotInTheWindow) {
    EXPECT_EQ(only({delivered("signal:response_repetition"),
                    turn(3, "response_repetition=0.1500", "m1", "true", "1"),
                    }), "recurred");
    std::vector<gd::Event> other = {delivered("signal:response_repetition")};
    auto t = turn(3, "response_repetition=0.1500");
    t.run = "r2";
    other.push_back(t);
    EXPECT_EQ(only(other), "incomplete");
}

TEST(GuidanceEfficacy, TalliesAddAndMergeByTheLargerCount) {
    std::vector<gd::SectionTally> tallies;
    auto outs = gd::judge({delivered("signal:response_repetition"), turn(3, "response_repetition=0.1")});
    gd::addOutcomes(tallies, outs);
    gd::addOutcomes(tallies, outs);
    ASSERT_EQ(tallies.size(), 1u);
    EXPECT_EQ(tallies[0].delivered, 2);
    EXPECT_EQ(tallies[0].recurred, 2);
    EXPECT_TRUE(gd::plausibleTally(tallies[0]));
    EXPECT_TRUE(gd::neverResolved(tallies[0]));

    // Two runs that each folded the same outcomes: the larger count, never the sum.
    std::vector<gd::SectionTally> other = tallies;
    other[0].delivered = 3;
    other[0].quiet = 1;
    gd::mergeTallies(tallies, other);
    EXPECT_EQ(tallies[0].delivered, 3);
    EXPECT_EQ(tallies[0].recurred, 2);
    EXPECT_EQ(tallies[0].quiet, 1);
    EXPECT_FALSE(gd::neverResolved(tallies[0]));   // one window was quiet

    gd::SectionTally bad = tallies[0];
    bad.key = "not-hex";
    EXPECT_FALSE(gd::plausibleTally(bad));
    bad = tallies[0];
    bad.recurred = 100;   // more outcomes than deliveries
    EXPECT_FALSE(gd::plausibleTally(bad));

    gd::SectionTally one;
    one.key = std::string(32, 'b');
    one.delivered = one.recurred = 1;
    EXPECT_FALSE(gd::neverResolved(one));   // below kUnresolvedLineMin judged deliveries
}
