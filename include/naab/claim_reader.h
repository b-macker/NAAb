#pragma once
// The claim reader (Bundle 20 PR 5): what a model's reply says happened.
//
// One deterministic reading of a reply's final text, shared by the two
// signals that compare a reply's claims with what was observed:
//   S19 claim_result_reconciliation -- a claim about a tool against the
//       tool's recorded outcome (A202: it used to look words up in the
//       keyword set, where "done" is a stop word and a snake_case tool name
//       is split, and it read "not successful" as success);
//   S24 claim_validation -- a claim of success or a fix against the
//       validation the orchestration script records next.
// AGENT_CLAIM telemetry records each claim it reads (kind, span, negation,
// never the text), and `naab-gov replay --transcript` re-runs it.
//
// What it reads, and what it skips (table: tests/governance_v4/claim_reader.psv):
//   - Words from three lists: success ("completed", "done", "passed", ...),
//     fixed ("fixed", "resolved", ...) and failure ("failed", "error",
//     "unable", ...), compared whole and without case.
//   - Not read as claims: fenced code blocks, inline code, text in double
//     quotes, block-quoted lines, questions, a conditional clause ("if",
//     "unless", "whether"), and an infinitive ("to pass").
//   - Negation: "not", "never", "no", "without", a word ending in "n't",
//     "cannot", "unable" and "failed"/"fails" within three words before a
//     claim word, in its clause, flip what it says ("not successful" says it
//     did not work; "no errors" says it did).
//   - A modal ("should", "will", "would", "might", "may", "likely",
//     "hopefully") within three words before makes it a prediction.
//   - In a clause holding a fixed word, a failure NOUN is what was fixed, not
//     a claim ("fixed the error").
//   - A clause ends at , ; : and at "but", "however", "although", "though",
//     "yet", "while"; a sentence at . ! ? before a space and at a line break.
// Tool names are matched as whole names (letters, digits, '_', '-', '.'),
// without case, inside inline code and quotes too: a snake_case tool is one
// name, not three words.
//
// It reads; it decides nothing. Its errors lean towards no claim, or a
// mixed reply both signals leave alone -- the false-reassurance direction,
// never a false charge from a word it could not place.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace naab {
namespace claims {

enum class ClaimKind { Success, Fixed, Failure, Prediction };

const char* kindName(ClaimKind k);

struct Claim {
    ClaimKind kind = ClaimKind::Success;
    // What the claim says, after negation: true "it worked", false "it did
    // not". A prediction keeps the direction of the word it predicts.
    bool positive = true;
    bool negated = false;
    size_t begin = 0;      // byte span of the claim word in the reply
    size_t end = 0;
    size_t sentence = 0;   // index into ClaimReading::sentence_names
};

struct ClaimReading {
    std::vector<Claim> claims;
    // Per sentence, the names it holds (lowercased), code and quotes included.
    std::vector<std::vector<std::string>> sentence_names;
};

ClaimReading readClaims(std::string_view reply);

// What a reply says about one tool. A claim is about the tool when it is in a
// sentence that names the tool; when those sentences hold none, in the
// sentence right after one of them, provided that sentence names none of
// `all_tools`. Predictions are not about a tool's outcome and are left out.
struct ToolClaims {
    bool mentioned = false;
    bool positive = false;   // some claim says it worked
    bool negative = false;   // some claim says it did not
};
ToolClaims claimsAboutTool(const ClaimReading& r, std::string_view tool,
                           const std::vector<std::string>& all_tools);

// The reply as a whole: a positive claim and no negative one (predictions
// included) -- what S24 charges when a failed validation follows.
bool onlyPositiveClaims(const ClaimReading& r);

}  // namespace claims
}  // namespace naab
