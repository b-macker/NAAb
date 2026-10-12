// Live governance guidance — docs/plan-live-guidance.md.
//
// Operator-declared sources (slice 1: markdown files) are split into sections
// at their headings, re-read whenever their size or modification time changes,
// and selected by what governance just observed: an agent's first send, a drift
// signal that paid a penalty, a failed validation, or a rule violation. The
// delivered text is exactly what the source says -- nothing here writes,
// rewrites or summarises it.
//
// The channel is governed like any other route into a model:
//   * a source is read only if the project's filesystem policy lets the
//     project read it (pathPolicyDenial, decide-only), so guidance cannot carry
//     a blocked file -- govern.json, its signature, the trusted keys -- to a
//     model provider;
//   * the governed program may not WRITE a source (isGuidanceSourcePath, wired
//     into checkPathAccessImpl): no self-instruction loop;
//   * text passes the secret/PII patterns (secretFinding/piiFinding, the
//     decide-only twins of checkSecrets/checkPii) before any channel; a hit
//     withholds that entry rather than blocking the run;
//   * every delivery is logged (GUIDANCE_DELIVERED, sha256 of the text).

#include "naab/governance.h"
#include "naab/keyword_extract.h"
#include "naab/bounded_read.h"
#include "naab/crypto_utils.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

namespace naab {
namespace governance {

namespace {

struct GuidanceSection {
    std::string slug;
    std::string heading;
    std::string body;   // to the next heading of the same or a higher level
                        // (includes subsections): what an #anchor delivers
    std::string own;    // to the next heading of ANY level: what keyword
                        // selection scores and delivers
    std::unordered_set<std::string> head_kw;
    std::unordered_set<std::string> own_kw;
};

struct GuidanceDoc {
    bool loaded = false;
    std::string error;                 // why it could not be read
    long long mtime_ns = -1;
    std::uintmax_t size = 0;
    bool truncated = false;
    std::vector<GuidanceSection> sections;
};

// GitHub-style heading slug: lowercase ASCII letters and digits, spaces to '-',
// '-' and '_' kept, other ASCII punctuation dropped, non-ASCII bytes kept.
std::string slugify(const std::string& heading) {
    std::string out;
    for (unsigned char c : heading) {
        if (std::isalnum(c)) out += static_cast<char>(std::tolower(c));
        else if (c == ' ') out += '-';
        else if (c == '-' || c == '_') out += static_cast<char>(c);
        else if (c >= 0x80) out += static_cast<char>(c);
    }
    return out;
}

std::string trimBlankLines(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == '\n' || s[b] == '\r' || s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == '\n' || s[e - 1] == '\r' || s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

// Sections at ATX headings (# .. ######) outside fenced code blocks. A
// section's body runs to the next heading of the same or a higher level, so a
// parent section includes its subsections.
std::vector<GuidanceSection> parseSections(const std::string& text) {
    std::vector<std::string> lines;
    {
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
    }
    struct H { size_t line; int level; std::string text; };
    std::vector<H> heads;
    bool in_fence = false;
    std::string fence;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& l = lines[i];
        size_t p = 0;
        while (p < l.size() && p < 3 && l[p] == ' ') ++p;
        if (l.compare(p, 3, "```") == 0 || l.compare(p, 3, "~~~") == 0) {
            const std::string marker = l.substr(p, 3);
            if (!in_fence) { in_fence = true; fence = marker; }
            else if (marker == fence) { in_fence = false; }
            continue;
        }
        if (in_fence || p >= l.size() || l[p] != '#') continue;
        int level = 0;
        while (p + level < l.size() && l[p + level] == '#') ++level;
        if (level > 6) continue;
        size_t q = p + level;
        if (q < l.size() && l[q] != ' ' && l[q] != '\t') continue;  // "#tag", not a heading
        std::string h = trimBlankLines(q < l.size() ? l.substr(q) : std::string());
        // Optional closing sequence: "## Title ##"
        size_t e = h.size();
        while (e > 0 && h[e - 1] == '#') --e;
        if (e < h.size() && (e == 0 || h[e - 1] == ' ')) h = trimBlankLines(h.substr(0, e));
        if (h.empty()) continue;
        heads.push_back({i, level, h});
    }
    std::vector<GuidanceSection> out;
    std::map<std::string, int> seen;
    for (size_t k = 0; k < heads.size(); ++k) {
        size_t end = lines.size();
        for (size_t m = k + 1; m < heads.size(); ++m) {
            if (heads[m].level <= heads[k].level) { end = heads[m].line; break; }
        }
        const size_t own_end = k + 1 < heads.size() ? std::min(end, heads[k + 1].line) : end;
        std::string body, own;
        for (size_t i = heads[k].line + 1; i < end; ++i) {
            body += lines[i]; body += '\n';
            if (i < own_end) { own += lines[i]; own += '\n'; }
        }
        GuidanceSection s;
        s.heading = heads[k].text;
        std::string slug = slugify(s.heading);
        int n = seen[slug]++;
        s.slug = n == 0 ? slug : slug + "-" + std::to_string(n);
        s.body = trimBlankLines(body);
        s.own = trimBlankLines(own);
        naab::keywords::extractKeywords(s.heading, s.head_kw);
        naab::keywords::extractKeywords(s.own, s.own_kw);
        out.push_back(std::move(s));
    }
    return out;
}

bool triggerArgMatches(const std::string& pattern, const std::string& value) {
    if (pattern == "*") return true;
    if (!pattern.empty() && pattern.back() == '*')
        return value.compare(0, pattern.size() - 1, pattern, 0, pattern.size() - 1) == 0;
    return pattern == value;
}

bool agentMatches(const GuidanceRuleConfig& rc, const std::string& agent) {
    if (rc.agents.empty()) return true;
    return std::find(rc.agents.begin(), rc.agents.end(), agent) != rc.agents.end();
}

// Truncate at a UTF-8 boundary so a cut never splits a multi-byte character.
std::string truncateUtf8(const std::string& s, size_t max) {
    if (s.size() <= max) return s;
    size_t cut = max;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut);
}

const char* kTruncMark = "\n[... guidance truncated]";

}  // namespace

struct GovernanceEngine::GuidanceState {
    std::map<std::string, GuidanceDoc> docs;   // by resolved path
    struct Pending { GuidanceRuleConfig rule; std::unordered_set<std::string> keywords; };
    std::map<int, std::vector<Pending>> pending;              // by handle
    std::map<int, std::map<std::string, int>> last_sent;      // handle -> "src#slug" -> turn
    std::set<std::string> warned;                             // one-time warnings
};

// --- helpers that need the engine (file-local, take the state by reference) --

namespace {

struct Candidate {
    std::string source, file, slug, heading, body, select;
    double score = 0.0;
};

}  // namespace

static const GuidanceDoc* guidanceLoadDoc(GovernanceEngine& eng,
                                          GovernanceEngine::GuidanceState& st,
                                          const GuidanceConfig& gc,
                                          const GuidanceSourceConfig& src,
                                          std::string& why_not);

bool GovernanceEngine::guidanceEnabled() const {
    return isActive() && rules().guidance.enabled;
}

std::string GovernanceEngine::formatGuidance(const std::vector<GuidanceDelivery>& items) {
    std::string out;
    for (const auto& d : items) {
        if (!out.empty()) out += "\n\n";
        out += "[Project guidance: " + d.file + " > " + d.heading + "]\n" + d.text;
    }
    return out;
}

bool GovernanceEngine::isGuidanceSourcePath(const std::string& canon_n) const {
    // Hold the rules for the whole call: a mid-run reload swaps rules_ptr_
    // and could free the object a bare rules() reference points into.
    const auto rules_hold = rulesPtr();
    const auto& gc = rules_hold->guidance;
    if (!gc.enabled || gc.sources.empty()) return false;
    std::error_code ec;
    const bool target_exists = std::filesystem::exists(canon_n, ec);
    for (const auto& s : gc.sources) {
        const std::string sp = resolvePolicyPath(s.path);
        if (sp == canon_n) return true;
        // A second name for the same file (a hard link) is the same source.
        if (target_exists) {
            std::error_code ec2;
            if (std::filesystem::exists(sp, ec2) &&
                std::filesystem::equivalent(sp, canon_n, ec2) && !ec2) return true;
        }
    }
    return false;
}

// The document for a source, re-read when its size or mtime moved. Returns
// nullptr (with why_not) when policy forbids the read or the file is unusable.
static const GuidanceDoc* guidanceLoadDoc(GovernanceEngine& eng,
                                          GovernanceEngine::GuidanceState& st,
                                          const GuidanceConfig& gc,
                                          const GuidanceSourceConfig& src,
                                          std::string& why_not) {
    const std::string abs = eng.resolvePolicyPath(src.path);
    // Read only what the project may read: a guidance source must not be the
    // route by which a blocked file reaches a model provider.
    const std::string denial = eng.pathPolicyDenial(abs, /*write=*/false);
    if (!denial.empty()) {
        why_not = "policy";
        if (st.warned.insert("policy:" + abs).second) {
            fprintf(stderr, "[governance] Warning: guidance source \"%s\" is not read: "
                            "the filesystem policy does not let this project read %s\n",
                    src.name.c_str(), src.path.c_str());
        }
        return nullptr;
    }
    std::error_code ec;
    auto ftime = std::filesystem::last_write_time(abs, ec);
    if (ec) {
        why_not = "unreadable";
        if (st.warned.insert("missing:" + abs).second) {
            fprintf(stderr, "[governance] Warning: guidance source \"%s\" (%s) cannot be read\n",
                    src.name.c_str(), src.path.c_str());
        }
        auto it = st.docs.find(abs);
        if (it != st.docs.end()) st.docs.erase(it);
        return nullptr;
    }
    const long long mt = static_cast<long long>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(ftime.time_since_epoch()).count());
    const std::uintmax_t sz = std::filesystem::file_size(abs, ec);
    auto& doc = st.docs[abs];
    if (doc.loaded && doc.mtime_ns == mt && doc.size == sz) return &doc;

    const std::size_t cap = static_cast<std::size_t>(gc.max_source_kb) * 1024;
    auto content = naab::readFileBounded(abs, cap + 1);
    if (!content) {
        why_not = "unreadable";
        doc = GuidanceDoc{};
        if (st.warned.insert("open:" + abs).second) {
            fprintf(stderr, "[governance] Warning: guidance source \"%s\" (%s) cannot be opened "
                            "(missing, or a symbolic link)\n", src.name.c_str(), src.path.c_str());
        }
        st.docs.erase(abs);
        return nullptr;
    }
    GuidanceDoc fresh;
    fresh.loaded = true;
    fresh.mtime_ns = mt;
    fresh.size = sz;
    if (content->size() > cap) {
        content->resize(cap);
        fresh.truncated = true;
        if (st.warned.insert("cap:" + abs).second) {
            fprintf(stderr, "[governance] Warning: guidance source \"%s\" is larger than %d KB; "
                            "only the first %d KB is used\n",
                    src.name.c_str(), gc.max_source_kb, gc.max_source_kb);
        }
    }
    fresh.sections = parseSections(*content);
    doc = std::move(fresh);
    return &doc;
}

// Candidates for one rule, best first: anchors in the order listed, then
// keyword matches by score (heading overlap weighs 3, body overlap 1; ties go to
// document order). At most rule.max_entries.
static std::vector<Candidate> guidanceSelect(GovernanceEngine& eng,
                                             GovernanceEngine::GuidanceState& st,
                                             const GuidanceConfig& gc,
                                             const GuidanceRuleConfig& rule,
                                             const std::unordered_set<std::string>& kw,
                                             std::vector<std::pair<std::string, std::string>>& misses) {
    std::vector<Candidate> anchors, scored;
    for (const auto& f : rule.from) {
        const size_t hash = f.find('#');
        const std::string sname = f.substr(0, hash);
        const std::string slug = hash == std::string::npos ? "" : f.substr(hash + 1);
        const GuidanceSourceConfig* src = nullptr;
        for (const auto& s : gc.sources) if (s.name == sname) { src = &s; break; }
        if (!src) { misses.push_back({f, "undeclared"}); continue; }
        std::string why;
        const GuidanceDoc* doc = guidanceLoadDoc(eng, st, gc, *src, why);
        if (!doc) { misses.push_back({f, why}); continue; }
        if (!slug.empty()) {
            bool found = false;
            for (const auto& s : doc->sections) {
                if (s.slug == slug) {
                    anchors.push_back({sname, src->path, s.slug, s.heading, s.body, "anchor", 0.0});
                    found = true;
                    break;
                }
            }
            if (!found) misses.push_back({f, "section_not_found"});
            continue;
        }
        // Keyword selection scores and delivers a section's OWN text (up to
        // its first subheading). Scoring the full body let a container -- a
        // title over the whole document -- match every topic through its
        // children and win every selection (caught by G-04/G-12a).
        for (const auto& s : doc->sections) {
            if (s.own.empty()) continue;   // a pure container has nothing of its own
            double score = 0.0;
            for (const auto& k : kw) {
                if (s.head_kw.count(k)) score += 3.0;
                else if (s.own_kw.count(k)) score += 1.0;
            }
            if (score > 0.0) {
                Candidate c{sname, src->path, s.slug, s.heading, s.own, "keywords", score};
                scored.push_back(std::move(c));
            }
        }
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    std::vector<Candidate> out = std::move(anchors);
    for (auto& c : scored) out.push_back(std::move(c));
    if (out.size() > static_cast<size_t>(rule.max_entries)) out.resize(rule.max_entries);
    return out;
}

// "" when the text may be delivered; otherwise the withheld reason.
// Decide-only: checkSecrets()/checkPii() would ENFORCE on a hit (a HARD
// secret ends the run) and record a pass otherwise. Guidance is withheld, not
// blocked, so it uses the twins that share their patterns and switches.
static std::string guidanceScan(GovernanceEngine& eng, const std::string& text) {
    if (!eng.secretFinding(text).empty()) return "secret";
    if (!eng.piiFinding(text).empty()) return "pii";
    return "";
}

void GovernanceEngine::guidanceOnAgentTrigger(int handle_id, const std::string& agent_name,
                                              const std::string& kind, const std::string& arg,
                                              const std::unordered_set<std::string>& keywords) {
    if (!guidanceEnabled()) return;
    // Hold the rules for the whole call: a mid-run reload swaps rules_ptr_
    // and could free the object a bare rules() reference points into.
    const auto rules_hold = rulesPtr();
    const auto& gc = rules_hold->guidance;
    std::lock_guard<std::mutex> lock(guidance_mutex_);
    if (!guidance_state_) guidance_state_ = std::make_shared<GuidanceState>();
    auto& queue = guidance_state_->pending[handle_id];
    for (const auto& rc : gc.rules) {
        if (!rc.to_agent || rc.kind != kind || !agentMatches(rc, agent_name)) continue;
        if (kind == "signal" && !triggerArgMatches(rc.arg, arg)) continue;
        GuidanceState::Pending p{rc, keywords};
        if (kind == "signal") {
            // Selection for a signal works from its name ("mandate_alignment"
            // -> mandate, alignment) plus whatever the caller supplied.
            naab::keywords::extractKeywords(arg, p.keywords);
        }
        p.rule.on = kind == "signal" ? "signal:" + arg : rc.on;
        if (queue.size() < 32) queue.push_back(std::move(p));
    }
}

std::vector<GuidanceDelivery> GovernanceEngine::guidanceForSend(
    int handle_id, const std::string& agent_name, int turn,
    const std::unordered_set<std::string>& start_keywords) {
    std::vector<GuidanceDelivery> delivered;
    if (!guidanceEnabled()) return delivered;
    // Hold the rules for the whole call: a mid-run reload swaps rules_ptr_
    // and could free the object a bare rules() reference points into.
    const auto rules_hold = rulesPtr();
    const auto& gc = rules_hold->guidance;

    std::vector<std::tuple<std::string, std::string, std::string>> withheld_detail;  // trigger, ref, reason
    {
        std::lock_guard<std::mutex> lock(guidance_mutex_);
        if (!guidance_state_) guidance_state_ = std::make_shared<GuidanceState>();
        auto& st = *guidance_state_;

        std::vector<GuidanceState::Pending> requests;
        if (turn == 0) {
            for (const auto& rc : gc.rules) {
                if (rc.kind == "start" && rc.to_agent && agentMatches(rc, agent_name))
                    requests.push_back({rc, start_keywords});
            }
        }
        auto pit = st.pending.find(handle_id);
        if (pit != st.pending.end()) {
            for (auto& p : pit->second) requests.push_back(std::move(p));
            st.pending.erase(pit);
        }
        if (requests.empty()) return delivered;

        auto& sent = st.last_sent[handle_id];
        std::set<std::string> this_send;
        size_t used = 0;
        const size_t budget = static_cast<size_t>(std::max(0, gc.max_chars_per_turn));
        for (const auto& req : requests) {
            std::vector<std::pair<std::string, std::string>> misses;
            auto cands = guidanceSelect(*this, st, gc, req.rule, req.keywords, misses);
            for (const auto& m : misses)
                withheld_detail.emplace_back(req.rule.on, m.first, m.second);
            for (auto& c : cands) {
                const std::string key = c.source + "#" + c.slug;
                if (this_send.count(key)) continue;
                auto sit = sent.find(key);
                if (sit != sent.end() && turn - sit->second < gc.cooldown_turns) continue;
                const std::string reason = guidanceScan(*this, c.body);
                if (!reason.empty()) {
                    withheld_detail.emplace_back(req.rule.on, key, reason);
                    continue;
                }
                const std::string header = "[Project guidance: " + c.file + " > " + c.heading + "]\n";
                const size_t sep = delivered.empty() ? 0 : 2;
                if (used + sep + header.size() >= budget) {
                    withheld_detail.emplace_back(req.rule.on, key, "budget");
                    continue;
                }
                const size_t room = budget - used - sep - header.size();
                std::string text = c.body;
                if (text.size() > room) {
                    const size_t mark = std::string(kTruncMark).size();
                    if (room <= mark + 16) {
                        withheld_detail.emplace_back(req.rule.on, key, "budget");
                        continue;
                    }
                    text = truncateUtf8(text, room - mark) + kTruncMark;
                }
                GuidanceDelivery d;
                d.trigger = req.rule.on;
                d.source = c.source;
                d.file = c.file;
                d.section = c.slug;
                d.heading = c.heading;
                d.text = std::move(text);
                d.select = c.select;
                d.score = c.score;
                d.sha256 = security::CryptoUtils::sha256(d.text);
                used += sep + header.size() + d.text.size();
                this_send.insert(key);
                sent[key] = turn;
                delivered.push_back(std::move(d));
            }
        }
    }

    for (const auto& d : delivered) {
        char sc[32];
        snprintf(sc, sizeof(sc), "%.1f", d.score);
        writeAgentTelemetry("GUIDANCE_DELIVERED", {
            {"handle_id", std::to_string(handle_id)},
            {"agent", agent_name}, {"config_name", agent_name},
            {"turn", std::to_string(turn)},
            {"channel", "agent"},
            {"trigger", d.trigger},
            {"source", d.source}, {"file", d.file},
            {"section", d.section},
            {"select", d.select}, {"score", sc},
            {"chars", std::to_string(d.text.size())},
            {"sha256", d.sha256},
        });
    }
    for (const auto& [trig, ref, reason] : withheld_detail) {
        writeAgentTelemetry("GUIDANCE_WITHHELD", {
            {"handle_id", std::to_string(handle_id)},
            {"agent", agent_name}, {"config_name", agent_name},
            {"turn", std::to_string(turn)},
            {"channel", "agent"},
            {"trigger", trig}, {"ref", ref}, {"reason", reason},
        });
    }
    return delivered;
}

GovernanceEngine::GuidanceTerminal GovernanceEngine::guidanceForTerminal(
    const std::string& rule_name, const std::string& violation_message) {
    GuidanceTerminal g;
    if (!guidanceEnabled()) return g;
    // Hold the rules for the whole call: a mid-run reload swaps rules_ptr_
    // and could free the object a bare rules() reference points into.
    const auto rules_hold = rulesPtr();
    const auto& gc = rules_hold->guidance;
    bool any = false;
    for (const auto& rc : gc.rules)
        if (rc.kind == "violation" && rc.to_terminal && triggerArgMatches(rc.arg, rule_name)) { any = true; break; }
    if (!any) return g;

    // Agent-path checks carry "<agent:NAME>" as their location; a rule scoped
    // to agents applies only there.
    std::string agent_ctx;
    {
        const std::string f = checkFile();
        if (f.rfind("<agent:", 0) == 0 && f.size() > 8 && f.back() == '>')
            agent_ctx = f.substr(7, f.size() - 8);
    }

    std::unordered_set<std::string> kw;
    naab::keywords::extractKeywords(rule_name, kw);
    naab::keywords::extractKeywords(violation_message.substr(0, violation_message.find('\n')), kw);

    {
        std::lock_guard<std::mutex> lock(guidance_mutex_);
        if (!guidance_state_) guidance_state_ = std::make_shared<GuidanceState>();
        auto& st = *guidance_state_;
        std::set<std::string> seen;
        size_t used = 0;
        const size_t budget = static_cast<size_t>(std::max(0, gc.max_chars_terminal));
        for (const auto& rc : gc.rules) {
            if (rc.kind != "violation" || !rc.to_terminal || !triggerArgMatches(rc.arg, rule_name)) continue;
            if (!rc.agents.empty() && (agent_ctx.empty() || !agentMatches(rc, agent_ctx))) continue;
            std::vector<std::pair<std::string, std::string>> misses;
            auto cands = guidanceSelect(*this, st, gc, rc, kw, misses);
            for (const auto& m : misses) g.withheld.push_back({rc.on, m.first, m.second});
            for (auto& c : cands) {
                const std::string key = c.source + "#" + c.slug;
                if (!seen.insert(key).second) continue;
                const std::string reason = guidanceScan(*this, c.body);
                if (!reason.empty()) { g.withheld.push_back({rc.on, key, reason}); continue; }
                if (used >= budget) { g.withheld.push_back({rc.on, key, "budget"}); continue; }
                std::string text = c.body;
                const size_t room = budget - used;
                if (text.size() > room) text = truncateUtf8(text, room) + kTruncMark;
                GuidanceDelivery d;
                d.trigger = "violation:" + rule_name;
                d.source = c.source; d.file = c.file; d.section = c.slug; d.heading = c.heading;
                d.text = std::move(text); d.select = c.select; d.score = c.score;
                d.sha256 = security::CryptoUtils::sha256(d.text);
                used += d.text.size();
                g.delivered.push_back(std::move(d));
            }
        }
    }

    for (const auto& d : g.delivered) {
        g.suffix += "\n\n  Project guidance (" + d.file + " > " + d.heading + "):\n";
        std::istringstream in(d.text);
        std::string line;
        bool first = true;
        while (std::getline(in, line)) {
            if (!first) g.suffix += "\n";
            g.suffix += "    " + line;
            first = false;
        }
    }
    if (!g.suffix.empty()) g.suffix += "\n";
    return g;
}

void GovernanceEngine::emitGuidanceTerminal(const GuidanceTerminal& g) {
    for (const auto& d : g.delivered) {
        writeAgentTelemetry("GUIDANCE_DELIVERED", {
            {"channel", "terminal"},
            {"trigger", d.trigger},
            {"source", d.source}, {"file", d.file},
            {"section", d.section}, {"select", d.select},
            {"chars", std::to_string(d.text.size())},
            {"sha256", d.sha256},
        });
    }
    for (const auto& w : g.withheld) {
        if (w.size() != 3) continue;
        writeAgentTelemetry("GUIDANCE_WITHHELD", {
            {"channel", "terminal"}, {"trigger", w[0]}, {"ref", w[1]}, {"reason", w[2]},
        });
    }
}

std::vector<GuidanceDelivery> GovernanceEngine::guidancePull(const std::string& topic,
                                                             const std::string& source_filter,
                                                             int max_entries) {
    std::vector<GuidanceDelivery> out;
    if (!guidanceEnabled() || topic.empty()) return out;
    // Hold the rules for the whole call: a mid-run reload swaps rules_ptr_
    // and could free the object a bare rules() reference points into.
    const auto rules_hold = rulesPtr();
    const auto& gc = rules_hold->guidance;
    max_entries = std::max(1, std::min(10, max_entries));

    // A pull is an ad-hoc rule over every source (or the one named).
    GuidanceRuleConfig rc;
    rc.on = "pull";
    rc.kind = "pull";
    rc.max_entries = max_entries;
    std::unordered_set<std::string> kw;
    const size_t hash = topic.find('#');
    if (hash != std::string::npos) {
        const std::string sname = topic.substr(0, hash);
        const std::string slug = topic.substr(hash + 1);
        for (const auto& s : gc.sources) {
            if ((!sname.empty() && s.name != sname) || (!source_filter.empty() && s.name != source_filter)) continue;
            rc.from.push_back(s.name + "#" + slug);
        }
    } else {
        for (const auto& s : gc.sources)
            if (source_filter.empty() || s.name == source_filter) rc.from.push_back(s.name);
        naab::keywords::extractKeywords(topic, kw);
    }
    if (rc.from.empty()) return out;

    std::vector<std::tuple<std::string, std::string>> withheld;
    {
        std::lock_guard<std::mutex> lock(guidance_mutex_);
        if (!guidance_state_) guidance_state_ = std::make_shared<GuidanceState>();
        std::vector<std::pair<std::string, std::string>> misses;
        auto cands = guidanceSelect(*this, *guidance_state_, gc, rc, kw, misses);
        const size_t cap = static_cast<size_t>(std::max(0, gc.max_chars_per_turn));
        for (auto& c : cands) {
            const std::string reason = guidanceScan(*this, c.body);
            if (!reason.empty()) { withheld.emplace_back(c.source + "#" + c.slug, reason); continue; }
            GuidanceDelivery d;
            d.trigger = "pull";
            d.source = c.source; d.file = c.file; d.section = c.slug; d.heading = c.heading;
            d.text = c.body.size() > cap ? truncateUtf8(c.body, cap) + kTruncMark : c.body;
            d.select = c.select; d.score = c.score;
            d.sha256 = security::CryptoUtils::sha256(d.text);
            out.push_back(std::move(d));
        }
    }
    for (const auto& d : out) {
        writeAgentTelemetry("GUIDANCE_DELIVERED", {
            {"channel", "script"}, {"trigger", "pull"},
            {"source", d.source}, {"file", d.file}, {"section", d.section},
            {"select", d.select}, {"chars", std::to_string(d.text.size())},
            {"sha256", d.sha256},
        });
    }
    for (const auto& [ref, reason] : withheld) {
        writeAgentTelemetry("GUIDANCE_WITHHELD", {
            {"channel", "script"}, {"trigger", "pull"}, {"ref", ref}, {"reason", reason},
        });
    }
    return out;
}

}  // namespace governance
}  // namespace naab
