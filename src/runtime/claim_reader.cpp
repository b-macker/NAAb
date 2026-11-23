// The claim reader (include/naab/claim_reader.h, Bundle 20 PR 5).
#include "naab/claim_reader.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>

namespace naab {
namespace claims {

const char* kindName(ClaimKind k) {
    switch (k) {
        case ClaimKind::Success:    return "success";
        case ClaimKind::Fixed:      return "fixed";
        case ClaimKind::Failure:    return "failure";
        case ClaimKind::Prediction: return "prediction";
    }
    return "unknown";
}

namespace {

// How a byte is read. Code and quotes still hold names; a fenced block or a
// block-quoted line holds nothing.
enum : char { kText = 0, kNamesOnly = 1, kSkip = 2 };

enum class Word { None, Success, Fixed, Failure };

struct WordInfo {
    Word word;
    bool needs_modal_or_negator;   // a base-form verb: "fix" is an intention, "won't fix" a claim
    bool object_noun;              // in a clause with a fixed word, what was fixed
};

const std::unordered_map<std::string, WordInfo>& wordTable() {
    static const std::unordered_map<std::string, WordInfo> t = {
        // success
        {"success", {Word::Success, false, false}},
        {"successful", {Word::Success, false, false}},
        {"successfully", {Word::Success, false, false}},
        {"succeeded", {Word::Success, false, false}},
        {"succeeds", {Word::Success, false, false}},
        {"succeed", {Word::Success, true, false}},
        {"completed", {Word::Success, false, false}},
        {"complete", {Word::Success, false, false}},
        {"completes", {Word::Success, false, false}},
        {"finished", {Word::Success, false, false}},
        {"done", {Word::Success, false, false}},
        {"created", {Word::Success, false, false}},
        {"wrote", {Word::Success, false, false}},
        {"written", {Word::Success, false, false}},
        {"saved", {Word::Success, false, false}},
        {"passed", {Word::Success, false, false}},
        {"passes", {Word::Success, false, false}},
        {"pass", {Word::Success, false, false}},
        {"passing", {Word::Success, false, false}},
        {"works", {Word::Success, false, false}},
        {"worked", {Word::Success, false, false}},
        {"work", {Word::Success, true, false}},
        {"found", {Word::Success, false, false}},
        {"returned", {Word::Success, false, false}},
        // fixed
        {"fixed", {Word::Fixed, false, false}},
        {"fixes", {Word::Fixed, false, false}},
        {"fix", {Word::Fixed, true, false}},
        {"resolved", {Word::Fixed, false, false}},
        {"resolves", {Word::Fixed, false, false}},
        {"resolve", {Word::Fixed, true, false}},
        {"repaired", {Word::Fixed, false, false}},
        {"corrected", {Word::Fixed, false, false}},
        {"patched", {Word::Fixed, false, false}},
        // failure
        {"error", {Word::Failure, false, true}},
        {"errors", {Word::Failure, false, true}},
        {"exception", {Word::Failure, false, true}},
        {"exceptions", {Word::Failure, false, true}},
        {"failure", {Word::Failure, false, true}},
        {"failures", {Word::Failure, false, true}},
        {"failing", {Word::Failure, false, true}},
        {"broken", {Word::Failure, false, true}},
        {"crash", {Word::Failure, false, true}},
        {"crashes", {Word::Failure, false, true}},
        {"timeout", {Word::Failure, false, true}},
        {"timeouts", {Word::Failure, false, true}},
        {"failed", {Word::Failure, false, false}},
        {"fails", {Word::Failure, false, false}},
        {"fail", {Word::Failure, false, false}},
        {"crashed", {Word::Failure, false, false}},
        {"broke", {Word::Failure, false, false}},
        {"unable", {Word::Failure, false, false}},
        {"cannot", {Word::Failure, false, false}},
        {"can't", {Word::Failure, false, false}},
        {"denied", {Word::Failure, false, false}},
        {"blocked", {Word::Failure, false, false}},
    };
    return t;
}

bool isNegatorFor(const std::string& w, Word target) {
    static const std::unordered_set<std::string> any = {"not", "never", "no", "without"};
    // A failure verb negates what follows it ("failed to save" is handled as
    // an infinitive; "unable ... completed" is not), but never a failure
    // word: "failed with an error" is not a success.
    static const std::unordered_set<std::string> success_only = {
        "cannot", "unable", "failed", "fails", "fail"};
    if (any.count(w)) return true;
    if (w.size() > 3 && w.compare(w.size() - 3, 3, "n't") == 0) return true;
    return target != Word::Failure && success_only.count(w) > 0;
}

bool isModal(const std::string& w) {
    static const std::unordered_set<std::string> m = {
        "should", "will", "would", "might", "may", "shall", "likely", "hopefully",
        "won't", "wouldn't", "shouldn't"};
    if (m.count(w)) return true;
    return w.size() > 3 && w.compare(w.size() - 3, 3, "'ll") == 0;
}

bool isConditional(const std::string& w) {
    return w == "if" || w == "unless" || w == "whether";
}

bool isClauseWord(const std::string& w) {
    return w == "but" || w == "however" || w == "although" || w == "though" ||
           w == "yet" || w == "while";
}

bool isWordChar(unsigned char c) { return std::isalpha(c) != 0 || c == '\''; }
bool isNameChar(unsigned char c) {
    return std::isalnum(c) != 0 || c == '_' || c == '-' || c == '.';
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool startsWith(std::string_view s, size_t i, std::string_view p) {
    return s.size() - i >= p.size() && s.compare(i, p.size(), p) == 0;
}

// Marks each byte kText / kNamesOnly / kSkip.
std::vector<char> classify(std::string_view s) {
    const size_t n = s.size();
    std::vector<char> m(n, kText);
    bool in_fence = false;
    size_t ls = 0;
    while (ls < n) {
        size_t le = s.find('\n', ls);
        if (le == std::string_view::npos) le = n;
        size_t t = ls;
        while (t < le && (s[t] == ' ' || s[t] == '\t')) ++t;
        const bool fence = startsWith(s, t, "```") || startsWith(s, t, "~~~");
        if (fence || in_fence || (t < le && s[t] == '>')) {
            for (size_t i = ls; i < le; ++i) m[i] = kSkip;
            if (fence) in_fence = !in_fence;
        } else {
            // Inline code and double-quoted text, closed on the same line.
            for (size_t i = ls; i < le;) {
                if (s[i] == '`') {
                    size_t run = i;
                    while (run < le && s[run] == '`') ++run;
                    const std::string_view tick = s.substr(i, run - i);
                    const size_t close = s.substr(0, le).find(tick, run);
                    if (close == std::string_view::npos) { i = run; continue; }
                    for (size_t k = i; k < close + tick.size(); ++k) m[k] = kNamesOnly;
                    i = close + tick.size();
                    continue;
                }
                const bool open_ascii = s[i] == '"';
                const bool open_curly = startsWith(s, i, "\xE2\x80\x9C");
                if (open_ascii || open_curly) {
                    const std::string_view closer = open_ascii ? "\"" : "\xE2\x80\x9D";
                    const size_t from = i + (open_ascii ? 1 : 3);
                    const size_t close = s.substr(0, le).find(closer, from);
                    if (close == std::string_view::npos) { i = from; continue; }
                    for (size_t k = i; k < close + closer.size(); ++k) m[k] = kNamesOnly;
                    i = close + closer.size();
                    continue;
                }
                ++i;
            }
        }
        ls = le + 1;
    }
    return m;
}

struct Token {
    std::string w;     // lowercased, ’ read as '
    size_t begin, end;
    size_t clause;
    bool text;         // kText: may be a claim
    bool hyphenated;   // directly followed by '-' ("error-free")
};

}  // namespace

ClaimReading readClaims(std::string_view s) {
    ClaimReading r;
    const size_t n = s.size();
    const std::vector<char> m = classify(s);

    // Sentence of each byte; a sentence ends at a line break, and at . ! ?
    // in plain text before whitespace or the end.
    std::vector<size_t> sent(n, 0);
    std::vector<bool> question;
    size_t cur = 0;
    question.push_back(false);
    char last_text = 0;   // the sentence's last non-space plain-text byte
    auto close_sentence = [&]() {
        question[cur] = (last_text == '?');
        ++cur;
        question.push_back(false);
        last_text = 0;
    };
    for (size_t i = 0; i < n; ++i) {
        sent[i] = cur;
        const char c = s[i];
        if (m[i] == kText && !std::isspace(static_cast<unsigned char>(c))) last_text = c;
        if (c == '\n') { close_sentence(); continue; }
        if (m[i] == kText && (c == '.' || c == '!' || c == '?') &&
            (i + 1 == n || std::isspace(static_cast<unsigned char>(s[i + 1])))) {
            close_sentence();
        }
    }
    question[cur] = (last_text == '?');
    r.sentence_names.assign(cur + 1, {});

    // Names, code and quotes included.
    for (size_t i = 0; i < n;) {
        if (m[i] == kSkip || !isNameChar(static_cast<unsigned char>(s[i]))) { ++i; continue; }
        size_t j = i;
        while (j < n && m[j] != kSkip && isNameChar(static_cast<unsigned char>(s[j]))) ++j;
        size_t a = i, b = j;
        while (a < b && (s[a] == '.' || s[a] == '-')) ++a;
        while (b > a && (s[b - 1] == '.' || s[b - 1] == '-')) --b;
        if (b > a) {
            auto& names = r.sentence_names[sent[a]];
            std::string nm = lower(s.substr(a, b - a));
            if (std::find(names.begin(), names.end(), nm) == names.end()) names.push_back(nm);
        }
        i = j;
    }

    // Words, per sentence, split into clauses.
    std::vector<std::vector<Token>> by_sentence(cur + 1);
    std::vector<size_t> clause_of_sentence(cur + 1, 0);
    for (size_t i = 0; i < n;) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (m[i] == kSkip) { ++i; continue; }
        if (m[i] == kText && (c == ',' || c == ';' || c == ':')) {
            ++clause_of_sentence[sent[i]];
            ++i;
            continue;
        }
        const bool curly = startsWith(s, i, "\xE2\x80\x99");   // ’
        if (!isWordChar(c) && !curly) { ++i; continue; }
        if (!std::isalpha(c)) { i += curly ? 3 : 1; continue; }   // no leading apostrophe
        std::string w;
        size_t j = i;
        while (j < n && m[j] != kSkip) {
            if (startsWith(s, j, "\xE2\x80\x99")) { w += '\''; j += 3; continue; }
            if (!isWordChar(static_cast<unsigned char>(s[j]))) break;
            w += static_cast<char>(std::tolower(static_cast<unsigned char>(s[j])));
            ++j;
        }
        while (!w.empty() && w.back() == '\'') w.pop_back();
        const size_t si = sent[i];
        if (isClauseWord(w)) ++clause_of_sentence[si];
        Token t{w, i, i + (j - i), clause_of_sentence[si], m[i] == kText,
                j < n && s[j] == '-'};
        // The span ends at the last letter, not a trailing apostrophe.
        while (t.end > t.begin && s[t.end - 1] == '\'') --t.end;
        if (isClauseWord(w)) ++clause_of_sentence[si];   // the word ends its clause too
        by_sentence[si].push_back(std::move(t));
        i = j;
    }

    const auto& table = wordTable();
    for (size_t si = 0; si < by_sentence.size(); ++si) {
        if (question[si]) continue;
        const auto& toks = by_sentence[si];
        for (size_t k = 0; k < toks.size(); ++k) {
            const Token& t = toks[k];
            if (!t.text || t.hyphenated) continue;
            auto it = table.find(t.w);
            if (it == table.end()) continue;
            const WordInfo info = it->second;
            // The words before it in its clause.
            bool conditional = false, negated = false, modal = false;
            size_t back = 0;
            for (size_t p = k; p-- > 0;) {
                if (toks[p].clause != t.clause) break;
                if (isConditional(toks[p].w)) conditional = true;
                if (back < 3) {
                    if (isNegatorFor(toks[p].w, info.word)) negated = true;
                    if (isModal(toks[p].w)) modal = true;
                }
                ++back;
            }
            if (conditional) continue;
            if (k > 0 && toks[k - 1].clause == t.clause && toks[k - 1].w == "to") continue;
            if (info.needs_modal_or_negator && !modal && !negated) continue;
            if (info.object_noun) {
                bool fixed_clause = false;
                for (const auto& o : toks) {
                    if (o.clause != t.clause || !o.text) continue;
                    auto f = table.find(o.w);
                    if (f != table.end() && f->second.word == Word::Fixed) { fixed_clause = true; break; }
                }
                if (fixed_clause) continue;
            }
            Claim c;
            const bool base = info.word != Word::Failure;
            c.negated = negated;
            c.positive = base != negated;
            c.kind = modal ? ClaimKind::Prediction
                   : info.word == Word::Success ? ClaimKind::Success
                   : info.word == Word::Fixed ? ClaimKind::Fixed
                   : ClaimKind::Failure;
            c.begin = t.begin;
            c.end = t.end;
            c.sentence = si;
            r.claims.push_back(c);
        }
    }
    return r;
}

ToolClaims claimsAboutTool(const ClaimReading& r, std::string_view tool,
                           const std::vector<std::string>& all_tools) {
    ToolClaims out;
    const std::string want = lower(tool);
    if (want.empty()) return out;
    auto names = [&](size_t si, const std::string& nm) {
        const auto& v = r.sentence_names[si];
        return std::find(v.begin(), v.end(), nm) != v.end();
    };
    std::vector<size_t> mention;
    for (size_t si = 0; si < r.sentence_names.size(); ++si)
        if (names(si, want)) mention.push_back(si);
    if (mention.empty()) return out;
    out.mentioned = true;
    auto take = [&](size_t si) {
        bool any = false;
        for (const auto& c : r.claims) {
            if (c.sentence != si || c.kind == ClaimKind::Prediction) continue;
            any = true;
            (c.positive ? out.positive : out.negative) = true;
        }
        return any;
    };
    bool found = false;
    for (size_t si : mention) found = take(si) || found;
    if (found) return out;
    for (size_t si : mention) {
        const size_t next = si + 1;
        if (next >= r.sentence_names.size()) continue;
        bool other = false;
        for (const auto& t : all_tools) {
            const std::string lt = lower(t);
            if (!lt.empty() && names(next, lt)) { other = true; break; }
        }
        if (!other) take(next);
    }
    return out;
}

bool onlyPositiveClaims(const ClaimReading& r) {
    bool pos = false;
    for (const auto& c : r.claims) {
        if (!c.positive) return false;
        pos = true;
    }
    return pos;
}

}  // namespace claims
}  // namespace naab
