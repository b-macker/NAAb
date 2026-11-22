#pragma once
// Guidance efficacy (Bundle 20 PR 4; docs/plan-live-guidance.md, "Measuring
// whether it helped").
//
// The owner's decision of 2026-10-12: whether a piece of guidance helped can
// only be known after the agent has used it, so NOTHING at runtime decides on
// it. A run records each delivery; this reads the telemetry afterwards and,
// per delivery, looks at the next kWindowTurns analysed drift turns of the
// same handle: did the signal that triggered it pay again (or, for a failed
// validation, did the next validation pass)? That is correlation. A window
// where something else was done to the agent -- another delivery, a reminder
// or correction, a challenge, a reset, an accepted config reload -- is
// UNDETERMINED, and a window that spans two models is MIXED_MODEL, because
// neither can be read as the guidance's effect. Causation needs arms: the
// optional holdout (`guidance.holdout`) withholds a reproducible fraction of
// would-be deliveries, which this judges the same way.
//
// One function, judge(), serves `naab-gov efficacy TELEMETRY` and the next
// run's startup (a run never reads its own outcomes); its per-section totals
// travel in the run history's carry (include/naab/run_history.h).
//
// No JSON here: nlohmann/json stays in .cpp files (CLAUDE.md).

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace naab {
namespace guidance {

constexpr int kWindowTurns = 3;            // analysed drift turns judged per delivery
constexpr size_t kMaxTallies = 100;        // sections a carry holds
constexpr int kUnresolvedLineMin = 2;      // judged deliveries before a section is named at run end

// The holdout draw for a run's Nth drawn candidate: a number in [0, 1) from
// the SHA-256 of "<run_id>:<N>" (its first 13 hex digits over 16^13). Anyone
// holding the telemetry can recompute it (`naab-gov replay`), so a recorded
// holdout cannot have been chosen after the fact.
double holdoutDraw(const std::string& run_id, unsigned long long ordinal);

// Every agent-telemetry event type, for the window rule: "intervention" (it
// changes what an agent is told or the state it is judged in) or
// "observation". nullptr: not in the table -- the guard suite
// (test_guidance_efficacy.sh GE-00) fails on a type the engine writes and this
// does not classify, so a new event cannot silently count as neither.
const char* eventClass(const std::string& event_type);
const std::vector<std::pair<const char*, const char*>>& eventClassTable();

// One telemetry event as the judge reads it: every scalar field as a string.
struct Event {
    std::string type, run, handle;
    std::map<std::string, std::string> f;
};
// The event types judge() reads (everything else can be skipped unparsed).
bool judgedEventType(const std::string& type);

struct Outcome {
    std::string run, id, arm;      // arm: "delivered" | "held_out"
    std::string handle, trigger, section_key, source, heading, model;
    int turn = -1;
    // resolved | recurred | quiet | undetermined | incomplete | mixed_model | not_judged
    std::string result;
    int window_turns = 0;          // analysed turns found (at most kWindowTurns)
};

// Judges every agent-channel delivery, and every held-out candidate, in
// `events` (file order; one run or several). A `signal:K` trigger: recurred
// when K paid on one of the window's turns before any intervention landed,
// quiet when it paid on none of kWindowTurns turns, incomplete when the run
// ended first. A `validation_failed` trigger: the handle's next recorded
// validation, resolved when it passed. Any other trigger is not_judged.
std::vector<Outcome> judge(const std::vector<Event>& events);

// Per-section totals of the DELIVERED arm, carried from run to run.
struct SectionTally {
    std::string key;               // GuidanceDelivery::section_key (32 hex)
    int delivered = 0, held_out = 0;
    int recurred = 0, quiet = 0, resolved = 0;
    int undetermined = 0, incomplete = 0, mixed_model = 0, not_judged = 0;
    int judged() const { return recurred + quiet + resolved; }
};
// Adds `outcomes` to `tallies` (a section not yet held is appended; the list
// keeps the most recently delivered kMaxTallies).
void addOutcomes(std::vector<SectionTally>& tallies, const std::vector<Outcome>& outcomes);
// Two runs at once each folded the same earlier outcomes: per section, the
// larger of each count (a count one reader saw; never a sum).
void mergeTallies(std::vector<SectionTally>& into, const std::vector<SectionTally>& other);
// A tally whose every field is a shape the engine writes.
bool plausibleTally(const SectionTally& t);
// The section the end of a run names: delivered this run, and every one of at
// least kUnresolvedLineMin judged earlier deliveries was followed by its
// trigger again.
bool neverResolved(const SectionTally& t);

}  // namespace guidance
}  // namespace naab
