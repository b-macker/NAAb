#pragma once
// Run history (Bundle 20): what earlier runs of this project refused, read at
// `naab-lang run` start from their tamper-evident telemetry.
//
// Coding agents iterate against NAAb -- run, get refused, edit, run again -- and
// each run used to start with no memory of the last, so governance could be
// satisfied without the problem being solved. The history gives the evaluator
// that memory, from evidence it already writes:
//
//   * a refusal of a function-body check records its subject (the function, its
//     file, the SHA-256 of its text and a code fingerprint) on the chained
//     RuleViolation row, and the refused text goes into the protected
//     `.naab_cache/` under its hash (the TRANSCRIPT_REF pattern: content outside
//     the chain, its hash inside);
//   * each run's telemetry carries a chained HISTORY_CARRY event (the refusals
//     still remembered, the recent repairs), and `.naab_cache/history.anchor.json`
//     names its offset and hash, so the next run reads one line rather than the
//     whole file;
//   * the next run reads that carry and the verified rows after it.
//
// The rule (docs/plan-bundles.md, Bundle 20): MEMORY POINTS, THE CHECK DECIDES.
// History only chooses where an existing check looks, or what engine-written
// fact is shown; every verdict is the real check under the current govern.json.
// A stale or forged history can add an honest check; it never invents or
// removes a refusal, and nothing skips work because of it.
//
// No JSON here: nlohmann/json stays in .cpp files (CLAUDE.md).
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "naab/guidance_efficacy.h"

namespace naab {
namespace history {

constexpr int kFingerprintVersion = 1;
constexpr size_t kMaxRemembered = 100;           // refusals a carry holds
constexpr size_t kMaxRepairs = 20;               // recent repairs a carry holds
constexpr int kForgetAfterRuns = 50;             // runs a refusal may go unseen
constexpr int kMinTokensWithoutName = 12;        // below this, a match needs the name too
constexpr size_t kSubjectMaxBytes = 64 * 1024;   // a stored subject's text
constexpr size_t kSubjectRunBudget = 4 * 1024 * 1024;
constexpr size_t kStoreMaxEntries = 2000;        // .naab_cache/*.subject.txt kept
constexpr size_t kForwardWindowBytes = 8u * 1024 * 1024;   // after the anchored carry
constexpr size_t kBackwardScanBytes = 64u * 1024 * 1024;   // with no anchor

// A NAAb function's text as tokens of the run's own lexer: comments and
// whitespace dropped, identifiers renamed in order of first use (members after
// `.` and anything followed by `.` -- a module or receiver -- kept), the
// function's own name replaced, string and number literals reduced to their
// kind, polyglot bodies kept with whitespace collapsed, a leading `export`
// dropped (a module function reads as the same function in the entry file).
// Equal fingerprints mean the same code under other local names and other
// literal values. Empty `fp` when the text does not lex. `stream_out`, when
// given, receives the text that was hashed (the caller checks it holds no
// secret before keeping the hash).
struct Fingerprint {
    std::string fp;
    int tokens = 0;
};
Fingerprint fingerprintNaab(const std::string& text, const std::string& own_name,
                            std::string* stream_out = nullptr);

// SHA-256 (hex) of the text with CRLF read as LF, as readSignedFile reads.
std::string lfSha256(const std::string& text);

// The function-body checks whose refusals history remembers, and (PR 3) a
// function's must_produce contract, `contracts.<fn>.must_produce`.
bool rememberedRule(const std::string& rule);
bool mustProduceRule(const std::string& rule);
// A rule whose subject must never be stored (secrets, PII): name only.
bool secretLikeRule(const std::string& rule);
// advisory < detect < soft < approval_required < hard; unknown = -1.
int levelRank(const std::string& level);

struct Remembered {
    std::string fp;
    int fpv = kFingerprintVersion;
    int tokens = 0;
    std::string rule, level, file, function, sha256;
    int line = 0;
    std::string origin_run, origin_hash;   // the run and chained row that refused it
    int unseen = 0;                         // runs since it was last scanned
};

struct Repair {
    std::string rule, level, file, function;
    std::string refused_run, repaired_run;
    std::string refused_sha256, repaired_sha256;
    // PR 3: for a must_produce repair, the 1-based golden case whose input and
    // expected value the passing change inserted as literals; 0 = none seen.
    // A fact, never a verdict: a legitimate base-case fix has the same diff.
    int golden_case = 0;
};

// PR 3: the first golden case (1-based) whose every argument and expected
// value -- scalars only, a number or a string -- appears among the literal
// tokens the passing text INSERTS relative to the refused text (the longest
// common subsequence of the two token streams, literal values kept); 0 when
// none does, when a case has a non-scalar value, or when either text does not
// lex or is too long to compare. `cases` holds each case's argument JSON texts
// and its expected JSON text.
int goldenCaseInserted(const std::string& refused_text, const std::string& passing_text,
                       const std::vector<std::pair<std::vector<std::string>, std::string>>& cases);

// A repair whose every field is a shape the engine writes: a remembered rule,
// a plain function name, a plain relative path, plain run ids. History lines
// are verified by their hash, but an unkeyed chain's hash can be recomputed by
// anyone who edits the file, so a value is shown only in a shape no forged
// line can turn into instructions or terminal control (PR 3's precedent lines).
bool displayableRepair(const Repair& r);
// The same shape check for a remembered refusal (Bundle 20 PR 4, the `history`
// guidance source): a remembered rule, a plain function name, a relative plain
// file, a plain run id, a known level and a line that is a small number.
bool displayableRemembered(const Remembered& r);

// A remembered refusal's code seen again in this run, somewhere else.
struct Sighting {
    Remembered origin;
    std::string file, function;   // where it was seen
};

struct RunHistory {
    std::vector<Remembered> remembered;
    std::vector<Repair> repairs;
    std::string carry_run;        // the run that wrote the carry read ("" = none)
    long long bytes_read = 0;
    int window_rows = 0;          // remembered refusals read after the carry
    bool from_anchor = false;
    bool keyed = false;
    std::string reset_reason;     // the anchor no longer matched the file
    std::string unread_reason;    // history could not be read at all
    // Bundle 20 PR 4: guidance efficacy. The carry's per-section totals, plus
    // the outcomes this reader judged: the carry-writer's own deliveries (from
    // `from_offset`, where its lines began, to its carry; at most
    // kForwardWindowBytes) and every delivery after the carry. A run never
    // judges its own; the next one does, once.
    std::vector<naab::guidance::SectionTally> tallies;
    int outcomes_judged = 0;      // deliveries judged by this reader
    long long carry_from_offset = -1;
};

// A remembered refusal the module function matches: the same fingerprint and
// version, and either at least kMinTokensWithoutName tokens or the same name
// (a refused `return []` stub must not match every library stub). nullptr: none.
const Remembered* matchMoved(const RunHistory& h, const Fingerprint& fp,
                             const std::string& function);
// Every remembered refusal the module function matches, by the same rule (a
// function can be remembered under more than one rule).
std::vector<const Remembered*> matchMovedAll(const RunHistory& h, const Fingerprint& fp,
                                             const std::string& function);

// Reads the history. `hash` recomputes a chained line's hash exactly as the
// writer did (HMAC when the chain is keyed). With an anchor that still matches,
// one line is read at its offset plus at most kForwardWindowBytes after it;
// without one, the last kBackwardScanBytes are scanned for the last carry.
// Only a carry or a refusal naming its subject is parsed, and only when its
// own hash recomputes; a line that does not verify is skipped (memory is
// lost, never wrong). Links are not required: they bind nothing an unkeyed
// chain's reader could not recompute, and a keyed chain's HMAC binds each line.
// `judge_guidance` false: the carry's guidance totals as written, nothing
// judged -- the merge of a concurrent run's carry, whose window holds this
// run's own deliveries (a run never judges its own; the next one does).
RunHistory readRunHistory(const std::string& telemetry_path,
                          const std::string& anchor_path,
                          const std::function<std::string(const std::string&)>& hash,
                          bool judge_guidance = true);

// A reset reason the reader writes (readRunHistory): `naab-gov history` shows
// a recorded reason only when it is one of these.
bool knownResetReason(const std::string& reason);

// One HISTORY_SUBJECT event read back: a remembered refusal's code met again.
struct SubjectEvent {
    std::string run, kind, rule, level, file, function;
    std::string origin_run, origin_file, origin_function;
};

// What `naab-gov history` reports: the history the next run would start from
// (the last carry in the file and the refusals after it, read exactly as a run
// reads it with no anchor), plus the HISTORY_SUBJECT and HISTORY_RESET events
// of the same region. Every event is used only when its own hash recomputes.
constexpr size_t kMaxReportSightings = 500;
struct HistoryReport {
    bool opened = false;
    RunHistory history;
    std::vector<SubjectEvent> sightings;   // file order, the last kMaxReportSightings
    std::vector<std::string> resets;       // file order
    long long history_lines = 0;   // history events and subject rows met
    long long verified = 0;        // ... whose own hash recomputed
    bool cut = false;              // the file is longer than kBackwardScanBytes
};
HistoryReport readHistoryReport(const std::string& telemetry_path,
                                const std::function<std::string(const std::string&)>& hash);

// What `naab-gov efficacy` reports (Bundle 20 PR 4): every agent delivery and
// held-out candidate in the last kBackwardScanBytes, judged by
// naab::guidance::judge() over the lines whose own hash recomputes.
struct EfficacyReport {
    bool opened = false;
    std::vector<naab::guidance::Outcome> outcomes;
    long long lines = 0;           // lines of a type the judge reads
    long long verified = 0;        // ... whose own hash recomputed
    bool cut = false;              // the file is longer than kBackwardScanBytes
    std::vector<std::string> holdouts;   // the holdout values recorded, as written
};
EfficacyReport readEfficacyReport(const std::string& telemetry_path,
                                  const std::function<std::string(const std::string&)>& hash);

// The millisecond time a run id ("<ms>-<pid>") starts with; -1 when it has none.
long long runTime(const std::string& run_id);

// Folds a carry another run wrote while this one ran into this run's own
// (`carry`, `repairs`), so two runs at once do not forget each other's
// refusals. A refusal only the other run holds is kept; one this run judged
// keeps this run's outcome; a remembered refusal the other run repaired after
// it was refused is dropped. Repairs are the union, newest first, capped.
void mergeConcurrent(const RunHistory& other, const std::string& this_run,
                     std::vector<Remembered>& carry, std::vector<Repair>& repairs);

// The HISTORY_CARRY fields and the anchor file, as JSON text.
std::string carryEntriesJson(const std::vector<Remembered>& entries);
std::string carryRepairsJson(const std::vector<Repair>& repairs);
std::string carryTalliesJson(const std::vector<naab::guidance::SectionTally>& tallies);
// One verified telemetry event as the efficacy judge reads it.
naab::guidance::Event efficacyEvent(const std::string& json_text);
std::string anchorJson(const std::string& telemetry_path, long long offset,
                       const std::string& hash, const std::string& run_id);

}  // namespace history
}  // namespace naab
