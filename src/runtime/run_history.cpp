// Run history (Bundle 20): see include/naab/run_history.h.
#include "naab/run_history.h"

#include "naab/bounded_jsonl.h"
#include "naab/crypto_utils.h"
#include "naab/lexer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

namespace naab {
namespace history {

using json = nlohmann::json;
using naab::lexer::TokenType;

Fingerprint fingerprintNaab(const std::string& text, const std::string& own_name,
                            std::string* stream_out) {
    Fingerprint out;
    std::vector<naab::lexer::Token> toks;
    try {
        naab::lexer::Lexer lx(text);
        toks = lx.tokenize();
    } catch (...) {
        return out;   // does not lex: no fingerprint, so nothing can match it
    }
    std::map<std::string, std::string> renamed;
    std::string stream;
    for (size_t i = 0; i < toks.size(); ++i) {
        const auto& t = toks[i];
        if (t.type == TokenType::NEWLINE || t.type == TokenType::COMMENT ||
            t.type == TokenType::END_OF_FILE)
            continue;
        // A leading `export` says where the function is visible, not what it
        // does: a module's text starts there (declarationText() takes the
        // declaration's line), and the same function in the entry file must
        // read the same, or code moved into a module is never recognised.
        if (out.tokens == 0 && t.type == TokenType::EXPORT) continue;
        std::string piece;
        if (t.type == TokenType::IDENTIFIER) {
            const bool after_dot = i > 0 && (toks[i - 1].type == TokenType::DOT ||
                                             toks[i - 1].type == TokenType::QUESTION_DOT);
            const bool before_dot = i + 1 < toks.size() && toks[i + 1].type == TokenType::DOT;
            if (!own_name.empty() && t.value == own_name) {
                piece = "I:$self";
            } else if (after_dot || before_dot) {
                piece = "I:" + t.value;   // a member, a module or a receiver keeps its name
            } else {
                auto it = renamed.find(t.value);
                if (it == renamed.end())
                    it = renamed.emplace(t.value, "v" + std::to_string(renamed.size())).first;
                piece = "I:" + it->second;
            }
        } else if (t.type == TokenType::INLINE_CODE) {
            std::string collapsed;
            bool space = false;
            for (char c : t.value) {
                if (std::isspace(static_cast<unsigned char>(c))) { space = true; continue; }
                if (space && !collapsed.empty()) collapsed += ' ';
                space = false;
                collapsed += c;
            }
            piece = "C:" + collapsed;
        } else if (t.type == TokenType::STRING) {
            piece = "S";   // a literal's value is not structure, and may be a secret
        } else if (t.type == TokenType::NUMBER) {
            piece = "N";
        } else {
            piece = std::to_string(static_cast<int>(t.type)) + ":" + t.value;
        }
        stream += piece;
        stream += '\x1f';
        ++out.tokens;
    }
    out.fp = naab::security::CryptoUtils::sha256(
        "fpv" + std::to_string(kFingerprintVersion) + "\x1e" + stream);
    if (stream_out) *stream_out = std::move(stream);
    return out;
}

std::string lfSha256(const std::string& text) {
    std::string lf;
    lf.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;
        lf += text[i];
    }
    return naab::security::CryptoUtils::sha256(lf);
}

bool rememberedRule(const std::string& rule) {
    return rule == "code_quality.no_placeholders" ||
           rule == "code_quality.no_oversimplification" ||
           rule == "code_quality.no_incomplete_logic" ||
           rule == "code_quality.complexity_floor";
}

bool secretLikeRule(const std::string& rule) {
    std::string r = rule;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r.find("secret") != std::string::npos || r.find("pii") != std::string::npos;
}

int levelRank(const std::string& level) {
    std::string l = level;
    std::transform(l.begin(), l.end(), l.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (l == "advisory") return 0;
    if (l == "detect") return 1;
    if (l == "soft") return 2;
    if (l == "approval_required") return 3;
    if (l == "hard") return 4;
    return -1;
}

const Remembered* matchMoved(const RunHistory& h, const Fingerprint& fp,
                             const std::string& function) {
    if (fp.fp.empty()) return nullptr;
    for (const auto& r : h.remembered) {
        if (r.fpv != kFingerprintVersion || r.fp != fp.fp) continue;
        if (fp.tokens >= kMinTokensWithoutName || r.function == function) return &r;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Reading

namespace {

// Why a run's history was reset (HISTORY_RESET's reason, and the one line the
// run prints). knownResetReason() is the list `naab-gov history` shows.
const char* const kResetAnchorUnreadable = "the history anchor is unreadable";
const char* const kResetFileGone = "the telemetry file the history was kept in is gone";
const char* const kResetFileShorter =
    "the telemetry file is shorter than when the history was written";
const char* const kResetCarryGone =
    "the telemetry file no longer holds the history it was anchored to";

std::string str(const json& o, const char* k) {
    auto it = o.find(k);
    if (it == o.end() || !it->is_string()) return "";
    std::string v = it->get<std::string>();
    return v.size() > 512 ? std::string() : v;   // a field that long is not ours
}
int num(const json& o, const char* k) {
    auto it = o.find(k);
    if (it == o.end()) return 0;
    if (it->is_number_integer()) return it->get<int>();
    if (it->is_string()) {
        try { return std::stoi(it->get<std::string>()); } catch (...) { return 0; }
    }
    return 0;
}

bool hexish(const std::string& s) {
    if (s.empty() || s.size() > 128) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

// A remembered refusal read back: every field shape-checked, or dropped.
bool readEntry(const json& e, Remembered& r) {
    if (!e.is_object()) return false;
    r.fp = str(e, "fp"); r.fpv = num(e, "fpv"); r.tokens = num(e, "tokens");
    r.rule = str(e, "rule"); r.level = str(e, "level");
    r.file = str(e, "file"); r.function = str(e, "function");
    r.sha256 = str(e, "sha256"); r.line = num(e, "line");
    r.origin_run = str(e, "origin_run"); r.origin_hash = str(e, "origin_hash");
    r.unseen = num(e, "unseen");
    return hexish(r.fp) && rememberedRule(r.rule) && levelRank(r.level) >= 0 && !r.file.empty();
}

bool readRepair(const json& e, Repair& r) {
    if (!e.is_object()) return false;
    r.rule = str(e, "rule"); r.level = str(e, "level");
    r.file = str(e, "file"); r.function = str(e, "function");
    r.refused_run = str(e, "refused_run"); r.repaired_run = str(e, "repaired_run");
    r.refused_sha256 = str(e, "refused_sha256"); r.repaired_sha256 = str(e, "repaired_sha256");
    return rememberedRule(r.rule) && !r.file.empty();
}

struct Line {
    long long offset = 0;
    std::string text;
};

// Lines of `buf` (which starts at file offset `base`). The first partial line
// is skipped when `skip_first_partial`.
std::vector<Line> splitLines(const std::string& buf, long long base, bool skip_first_partial) {
    std::vector<Line> out;
    size_t pos = 0;
    if (skip_first_partial) {
        size_t nl = buf.find('\n');
        if (nl == std::string::npos) return out;
        pos = nl + 1;
    }
    while (pos < buf.size()) {
        size_t nl = buf.find('\n', pos);
        if (nl == std::string::npos) break;   // a line still being written: not ours yet
        out.push_back({base + static_cast<long long>(pos), buf.substr(pos, nl - pos)});
        pos = nl + 1;
    }
    return out;
}

bool readRegion(const std::string& path, long long offset, size_t len, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(offset);
    if (!in) return false;
    out.assign(len, '\0');
    in.read(&out[0], static_cast<std::streamsize>(len));
    out.resize(static_cast<size_t>(in.gcount()));
    return true;
}

// The parsed object when the line is a chained event whose hash recomputes.
bool verifiedEvent(const std::string& text,
                   const std::function<std::string(const std::string&)>& hash, json& ev) {
    if (text.empty() || text.size() > naab::evidence::kMaxTelemetryLineBytes) return false;
    if (!naab::evidence::jsonNestingWithin(text, naab::evidence::kMaxJsonDepth)) return false;
    ev = json::parse(text, nullptr, false);
    if (!ev.is_object()) return false;
    auto h = ev.find("hash");
    if (h == ev.end() || !h->is_string()) return false;
    const std::string claimed = h->get<std::string>();
    json body = ev;
    body.erase("hash");
    return hash(body.dump()) == claimed;
}

void addOrUpdate(std::vector<Remembered>& v, const Remembered& r) {
    for (auto& e : v) {
        if (e.rule == r.rule && e.file == r.file && e.function == r.function) { e = r; return; }
    }
    v.push_back(r);
}

}  // namespace

RunHistory readRunHistory(const std::string& telemetry_path,
                          const std::string& anchor_path,
                          const std::function<std::string(const std::string&)>& hash) {
    RunHistory h;
    std::error_code ec;
    const auto fsize = std::filesystem::file_size(telemetry_path, ec);
    const long long size = ec ? -1 : static_cast<long long>(fsize);

    // 1. The anchor, when it still names a carry this file holds.
    long long carry_offset = -1;
    std::string carry_hash;
    bool anchor_present = false;
    if (!anchor_path.empty() && std::filesystem::exists(anchor_path, ec)) {
        anchor_present = true;
        std::ifstream in(anchor_path, std::ios::binary);
        std::stringstream ss; ss << in.rdbuf();
        const std::string a = ss.str();
        json aj = a.size() < 4096 ? json::parse(a, nullptr, false) : json();
        if (aj.is_object() && aj.contains("offset") && aj["offset"].is_number_integer() &&
            aj.contains("hash") && aj["hash"].is_string()) {
            carry_offset = aj["offset"].get<long long>();
            carry_hash = aj["hash"].get<std::string>();
        }
        if (carry_offset < 0) h.reset_reason = kResetAnchorUnreadable;
    }
    if (size < 0) {
        if (anchor_present) h.reset_reason = kResetFileGone;
        return h;
    }

    std::vector<Line> lines;   // the carry line first, then the window after it
    bool have_carry = false;
    if (carry_offset >= 0) {
        if (carry_offset >= size) {
            h.reset_reason = kResetFileShorter;
        } else {
            const size_t len = static_cast<size_t>(std::min<long long>(
                size - carry_offset, static_cast<long long>(kForwardWindowBytes) +
                                         static_cast<long long>(naab::evidence::kMaxTelemetryLineBytes)));
            std::string buf;
            if (readRegion(telemetry_path, carry_offset, len, buf)) {
                h.bytes_read += static_cast<long long>(buf.size());
                lines = splitLines(buf, carry_offset, false);
            }
            json ev;
            if (!lines.empty() && verifiedEvent(lines[0].text, hash, ev) &&
                str(ev, "event_type") == "HISTORY_CARRY" && str(ev, "hash") == carry_hash) {
                have_carry = true;
                h.from_anchor = true;
            } else {
                h.reset_reason = kResetCarryGone;
                lines.clear();
            }
        }
    }
    if (!have_carry) {
        // 2. No usable anchor: the last carry in the last kBackwardScanBytes.
        const long long start = std::max<long long>(0, size - static_cast<long long>(kBackwardScanBytes));
        std::string buf;
        if (readRegion(telemetry_path, start, static_cast<size_t>(size - start), buf)) {
            h.bytes_read += static_cast<long long>(buf.size());
            auto all = splitLines(buf, start, start > 0);
            size_t from = 0;
            for (size_t i = all.size(); i-- > 0;) {
                if (all[i].text.find("\"event_type\":\"HISTORY_CARRY\"") == std::string::npos) continue;
                json ev;
                if (verifiedEvent(all[i].text, hash, ev) && str(ev, "event_type") == "HISTORY_CARRY") {
                    from = i; have_carry = true;
                }
                break;
            }
            lines.assign(all.begin() + static_cast<std::ptrdiff_t>(from), all.end());
        }
    }

    // 3. The carry, then the rows after it. Only lines that can matter are
    // parsed (a carry, or a refusal that names its subject), and each is used
    // only when its own hash recomputes. Links are not required: on an unkeyed
    // chain anyone can recompute them, and a keyed chain's HMAC already binds
    // each line. A line that does not verify is skipped, never trusted.
    size_t i = 0;
    auto loadCarry = [&](const json& ev) {
        h.remembered.clear(); h.repairs.clear();
        h.carry_run = str(ev, "run_id");
        if (ev.contains("entries") && ev["entries"].is_array())
            for (const auto& e : ev["entries"]) { Remembered r; if (readEntry(e, r)) addOrUpdate(h.remembered, r); }
        if (ev.contains("repairs") && ev["repairs"].is_array())
            for (const auto& e : ev["repairs"]) { Repair r; if (readRepair(e, r)) h.repairs.push_back(r); }
    };
    if (have_carry && !lines.empty()) {
        json ev;
        if (verifiedEvent(lines[0].text, hash, ev)) loadCarry(ev);
        i = 1;
    }
    long long window = 0;
    for (; i < lines.size(); ++i) {
        window += static_cast<long long>(lines[i].text.size()) + 1;
        if (window > static_cast<long long>(kForwardWindowBytes)) break;
        const std::string& t = lines[i].text;
        const bool carry = t.find("\"event_type\":\"HISTORY_CARRY\"") != std::string::npos;
        const bool refusal = t.find("\"event_type\":\"RuleViolation\"") != std::string::npos &&
                             t.find("\"subject_fp\"") != std::string::npos;
        if (!carry && !refusal) continue;
        json ev;
        if (!verifiedEvent(t, hash, ev)) continue;
        if (carry) { loadCarry(ev); h.window_rows = 0; continue; }
        Remembered r;
        r.rule = str(ev, "rule_name");
        r.fp = str(ev, "subject_fp");
        r.fpv = num(ev, "subject_fpv");
        r.tokens = num(ev, "subject_tokens");
        r.level = str(ev, "level");
        r.file = str(ev, "subject_file");
        r.function = str(ev, "subject_function");
        r.sha256 = str(ev, "subject_sha256");
        r.line = num(ev, "line");
        r.origin_run = str(ev, "run_id");
        r.origin_hash = str(ev, "hash");
        if (!hexish(r.fp) || !rememberedRule(r.rule) || levelRank(r.level) < 0 || r.file.empty()) continue;
        addOrUpdate(h.remembered, r);
        ++h.window_rows;
    }
    if (h.remembered.size() > kMaxRemembered) h.remembered.resize(kMaxRemembered);
    if (h.repairs.size() > kMaxRepairs) h.repairs.resize(kMaxRepairs);
    return h;
}

bool knownResetReason(const std::string& reason) {
    return reason == kResetAnchorUnreadable || reason == kResetFileGone ||
           reason == kResetFileShorter || reason == kResetCarryGone;
}

HistoryReport readHistoryReport(const std::string& telemetry_path,
                                const std::function<std::string(const std::string&)>& hash) {
    HistoryReport rep;
    std::error_code ec;
    const auto fsize = std::filesystem::file_size(telemetry_path, ec);
    if (ec) return rep;
    {
        std::ifstream probe(telemetry_path, std::ios::binary);
        if (!probe) return rep;
    }
    rep.opened = true;
    // The history a run with no anchor would start from: the same reader.
    rep.history = readRunHistory(telemetry_path, "", hash);

    const long long size = static_cast<long long>(fsize);
    const long long start = std::max<long long>(0, size - static_cast<long long>(kBackwardScanBytes));
    rep.cut = start > 0;
    std::string buf;
    if (!readRegion(telemetry_path, start, static_cast<size_t>(size - start), buf)) return rep;
    for (const auto& ln : splitLines(buf, start, start > 0)) {
        const std::string& t = ln.text;
        const bool subject = t.find("\"event_type\":\"HISTORY_SUBJECT\"") != std::string::npos;
        const bool reset = t.find("\"event_type\":\"HISTORY_RESET\"") != std::string::npos;
        const bool other = t.find("\"event_type\":\"HISTORY_CARRY\"") != std::string::npos ||
                           t.find("\"event_type\":\"HISTORY_LOADED\"") != std::string::npos ||
                           (t.find("\"event_type\":\"RuleViolation\"") != std::string::npos &&
                            t.find("\"subject_fp\"") != std::string::npos);
        if (!subject && !reset && !other) continue;
        ++rep.history_lines;
        json ev;
        if (!verifiedEvent(t, hash, ev)) continue;
        ++rep.verified;
        const std::string type = str(ev, "event_type");
        if (type == "HISTORY_RESET") {
            rep.resets.push_back(str(ev, "reason"));
        } else if (type == "HISTORY_SUBJECT") {
            SubjectEvent se;
            se.run = str(ev, "run_id"); se.kind = str(ev, "kind");
            se.rule = str(ev, "rule"); se.level = str(ev, "level");
            se.file = str(ev, "file"); se.function = str(ev, "function");
            se.origin_run = str(ev, "origin_run");
            se.origin_file = str(ev, "origin_file");
            se.origin_function = str(ev, "origin_function");
            if (!rememberedRule(se.rule)) continue;
            rep.sightings.push_back(se);
            if (rep.sightings.size() > kMaxReportSightings) rep.sightings.erase(rep.sightings.begin());
        }
    }
    return rep;
}

long long runTime(const std::string& run_id) {
    const size_t dash = run_id.find('-');
    if (dash == 0 || dash == std::string::npos || dash > 19) return -1;
    long long t = 0;
    for (size_t i = 0; i < dash; ++i) {
        if (run_id[i] < '0' || run_id[i] > '9') return -1;
        t = t * 10 + (run_id[i] - '0');
    }
    return t;
}

void mergeConcurrent(const RunHistory& other, const std::string& this_run,
                     std::vector<Remembered>& carry, std::vector<Repair>& repairs) {
    const auto key = [](const std::string& rule, const std::string& file, const std::string& fn) {
        return rule + '\x1f' + file + '\x1f' + fn;
    };
    std::set<std::string> judged;   // what this run refused or repaired
    for (const auto& e : carry)
        if (e.origin_run == this_run) judged.insert(key(e.rule, e.file, e.function));
    for (const auto& r : repairs)
        if (r.repaired_run == this_run) judged.insert(key(r.rule, r.file, r.function));

    for (const auto& e : other.remembered) {
        const std::string k = key(e.rule, e.file, e.function);
        if (judged.count(k)) continue;
        auto it = std::find_if(carry.begin(), carry.end(), [&](const Remembered& c) {
            return key(c.rule, c.file, c.function) == k;
        });
        if (it == carry.end()) carry.push_back(e);                  // only the other run holds it
        else if (runTime(e.origin_run) > runTime(it->origin_run)) *it = e;   // refused later there
    }
    for (const auto& r : other.repairs) {
        const std::string k = key(r.rule, r.file, r.function);
        const bool dup = std::any_of(repairs.begin(), repairs.end(), [&](const Repair& x) {
            return key(x.rule, x.file, x.function) == k && x.repaired_run == r.repaired_run;
        });
        if (!dup) repairs.push_back(r);
        if (judged.count(k)) continue;
        // The other run repaired it after it was refused: it is no longer remembered.
        carry.erase(std::remove_if(carry.begin(), carry.end(), [&](const Remembered& c) {
            return key(c.rule, c.file, c.function) == k &&
                   runTime(c.origin_run) <= runTime(r.repaired_run);
        }), carry.end());
    }
    std::stable_sort(repairs.begin(), repairs.end(), [](const Repair& a, const Repair& b) {
        return runTime(a.repaired_run) > runTime(b.repaired_run);
    });
    if (repairs.size() > kMaxRepairs) repairs.resize(kMaxRepairs);
    if (carry.size() > kMaxRemembered) {
        std::stable_sort(carry.begin(), carry.end(),
                         [](const Remembered& a, const Remembered& b) { return a.unseen < b.unseen; });
        carry.resize(kMaxRemembered);
    }
}

// ---------------------------------------------------------------------------
// Writing

std::string carryEntriesJson(const std::vector<Remembered>& entries) {
    json a = json::array();
    for (const auto& r : entries) {
        a.push_back({{"fp", r.fp}, {"fpv", r.fpv}, {"tokens", r.tokens}, {"rule", r.rule},
                     {"level", r.level}, {"file", r.file}, {"function", r.function},
                     {"sha256", r.sha256}, {"line", r.line}, {"origin_run", r.origin_run},
                     {"origin_hash", r.origin_hash}, {"unseen", r.unseen}});
    }
    return a.dump();
}

std::string carryRepairsJson(const std::vector<Repair>& repairs) {
    json a = json::array();
    for (const auto& r : repairs) {
        a.push_back({{"rule", r.rule}, {"level", r.level}, {"file", r.file},
                     {"function", r.function}, {"refused_run", r.refused_run},
                     {"repaired_run", r.repaired_run}, {"refused_sha256", r.refused_sha256},
                     {"repaired_sha256", r.repaired_sha256}});
    }
    return a.dump();
}

std::string anchorJson(const std::string& telemetry_path, long long offset,
                       const std::string& hash, const std::string& run_id) {
    json a = {{"v", 1}, {"telemetry", telemetry_path}, {"offset", offset},
              {"hash", hash}, {"run_id", run_id}};
    return a.dump() + "\n";
}

}  // namespace history
}  // namespace naab
