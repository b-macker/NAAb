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

#include <sqlite3.h>

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
    std::string topic;  // database rows: the topic column, matched by name
};

struct GuidanceDoc {
    bool loaded = false;
    std::string error;                 // why it could not be read
    long long mtime_ns = -1;
    std::uintmax_t size = 0;
    std::string sig;                   // database: db + -wal change signature
    bool truncated = false;
    std::vector<GuidanceSection> sections;
};

// A name as written in a heading or a topic column, compared loosely:
// lowercase, and '_' '.' '-' ':' '/' are spaces. "Mandate alignment",
// "mandate_alignment" and "MANDATE-ALIGNMENT" are the same name.
std::string normName(const std::string& s) {
    std::string out;
    bool space = true;
    for (unsigned char c : s) {
        if (std::isalnum(c)) { out += static_cast<char>(std::tolower(c)); space = false; }
        else if (!space) { out += ' '; space = true; }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

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

    // Full chain: every delivery gets an id; the run log feeds the end-of-run
    // terminal block and is updated as the reply and validation arrive.
    unsigned long long next_id = 1;
    struct RunEntry {
        std::string id, agent, trigger, file, heading;
        int turn = -1;
        bool has_reply = false;
        int terms = 0, used = 0;
        std::string recurred;      // "true" | "false" | "not_analyzed" | ""
        std::string validation;    // "passed" | "failed" | ""
        double lookup_ms = 0.0;
    };
    std::vector<RunEntry> run_log;                            // capped
    std::vector<std::pair<std::string, std::string>> withheld_log;  // (ref, reason), capped
    std::map<int, std::vector<std::string>> awaiting_validation;    // handle -> delivery ids
    bool summary_printed = false;
};

static constexpr size_t kRunLogCap = 500;

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
        // A database is also its sidecars: a crafted -wal is read as rows.
        if (s.type == "sqlite" &&
            (canon_n == sp + "-wal" || canon_n == sp + "-shm" || canon_n == sp + "-journal")) return true;
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
// What a source may be asked about: the trigger, bound into a sqlite query as
// PARAMETERS (never spliced into the SQL text).
struct TriggerCtx {
    std::string kind;    // start | signal | validation_failed | violation | pull
    std::string arg;     // signal key / rule name / "" 
    std::string agent;   // agent name, "" when none
    std::string topic;   // pull topic; else arg (or kind)
};

namespace {

// Refuses what reaches outside the one read-only database: ATTACH/DETACH (a
// query could otherwise open any file the path policy forbids) and PRAGMA
// statements (except the read-only data_version FTS5 needs), plus every
// write. Table-valued pragma FUNCTIONS (pragma_table_info(...)) reach the
// authorizer as reads, not as SQLITE_PRAGMA: SQLite defines them only for
// pragmas that return results and have no side effects, and they read this
// same database (observed while testing S-05, not designed around). The connection is already read-only; this is the second
// wall and the one that stops ATTACH.
int guidanceAuthorizer(void*, int action, const char* a1, const char* a2, const char*, const char*) {
    // One pragma is read-only and needed: FTS5's constructor queries
    // `PRAGMA data_version` (no argument), and denying it fails every MATCH
    // with "vtable constructor failed" (observed). Only that name, only in its
    // read form; every other pragma stays refused.
    if (action == SQLITE_PRAGMA)
        return (a1 && std::string(a1) == "data_version" && a2 == nullptr) ? SQLITE_OK : SQLITE_DENY;
    switch (action) {
        case SQLITE_ATTACH: case SQLITE_DETACH:
        case SQLITE_INSERT: case SQLITE_UPDATE: case SQLITE_DELETE:
        case SQLITE_CREATE_INDEX: case SQLITE_CREATE_TABLE: case SQLITE_CREATE_TEMP_INDEX:
        case SQLITE_CREATE_TEMP_TABLE: case SQLITE_CREATE_TEMP_TRIGGER: case SQLITE_CREATE_TEMP_VIEW:
        case SQLITE_CREATE_TRIGGER: case SQLITE_CREATE_VIEW: case SQLITE_CREATE_VTABLE:
        case SQLITE_DROP_INDEX: case SQLITE_DROP_TABLE: case SQLITE_DROP_TEMP_INDEX:
        case SQLITE_DROP_TEMP_TABLE: case SQLITE_DROP_TEMP_TRIGGER: case SQLITE_DROP_TEMP_VIEW:
        case SQLITE_DROP_TRIGGER: case SQLITE_DROP_VIEW: case SQLITE_DROP_VTABLE:
        case SQLITE_ALTER_TABLE: case SQLITE_REINDEX: case SQLITE_ANALYZE:
        case SQLITE_TRANSACTION: case SQLITE_SAVEPOINT:
            return SQLITE_DENY;
        default:
            return SQLITE_OK;
    }
}

// A query that runs past its deadline is interrupted: guidance must be quick.
int guidanceProgress(void* p) {
    auto* deadline = static_cast<std::chrono::steady_clock::time_point*>(p);
    return std::chrono::steady_clock::now() > *deadline ? 1 : 0;
}

}  // namespace

// Rows (heading, text) for a sqlite source, in the query's own order -- the
// SQL decides relevance (WHERE topic = :arg, joins, FTS5 MATCH ... ORDER BY
// rank). false + why on refusal or error; every refusal warns once.
static bool guidanceQuery(GovernanceEngine& eng, GovernanceEngine::GuidanceState& st,
                          const GuidanceSourceConfig& src, const TriggerCtx& ctx,
                          const std::unordered_set<std::string>& kw,
                          std::vector<std::pair<std::string, std::string>>& rows,
                          std::string& why);

// A database source with no query is read like a document: the rows of its
// `guidance` table are its sections (topic / heading / title / name for the
// name, body / text / content / guidance for the text), re-read when the
// database or its -wal changes.
static const GuidanceDoc* guidanceLoadSqliteDoc(GovernanceEngine& eng,
                                                GovernanceEngine::GuidanceState& st,
                                                const GuidanceSourceConfig& src,
                                                std::string& why);

// The names a section must carry to be the one FOR this trigger (automatic
// routing): the check that blocked (full rule name or its last part), the
// signal that paid, the agent that is starting.
static std::vector<std::string> triggerNames(const TriggerCtx& ctx) {
    std::vector<std::string> keys;
    auto add = [&](const std::string& n) { std::string k = normName(n); if (!k.empty()) keys.push_back(k); };
    if (ctx.kind == "start") {
        add(ctx.agent);
        add("start"); add("all agents"); add("every agent");
    } else if (ctx.kind == "signal") {
        add(ctx.arg); add("signal " + ctx.arg);
    } else if (ctx.kind == "violation") {
        add(ctx.arg);
        const size_t dot = ctx.arg.rfind('.');
        if (dot != std::string::npos) add(ctx.arg.substr(dot + 1));
    } else if (ctx.kind == "validation_failed") {
        add("validation failed"); add("validation");
    }
    return keys;
}

static std::vector<Candidate> guidanceSelect(GovernanceEngine& eng,
                                             GovernanceEngine::GuidanceState& st,
                                             const GuidanceConfig& gc,
                                             const GuidanceRuleConfig& rule,
                                             const std::unordered_set<std::string>& kw,
                                             const TriggerCtx& ctx,
                                             std::vector<std::pair<std::string, std::string>>& misses) {
    std::vector<Candidate> named, anchors, queried, scored;
    const std::vector<std::string> names = rule.automatic ? triggerNames(ctx) : std::vector<std::string>{};
    for (const auto& f : rule.from) {
        const size_t hash = f.find('#');
        const std::string sname = f.substr(0, hash);
        const std::string slug = hash == std::string::npos ? "" : f.substr(hash + 1);
        const GuidanceSourceConfig* src = nullptr;
        for (const auto& s : gc.sources) if (s.name == sname) { src = &s; break; }
        if (!src) { misses.push_back({f, "undeclared"}); continue; }

        // A database with an operator query: the SQL decides relevance, so
        // its rows are taken in its own order.
        if (src->type == "sqlite" && !src->query.empty()) {
            std::vector<std::pair<std::string, std::string>> rows;
            std::string why;
            if (!guidanceQuery(eng, st, *src, ctx, kw, rows, why)) { misses.push_back({f, why}); continue; }
            std::map<std::string, int> seen_slug;
            bool found = false;
            for (const auto& [heading, text] : rows) {
                std::string rs = slugify(heading);
                int n = seen_slug[rs]++;
                if (n) rs += "-" + std::to_string(n);
                if (!slug.empty()) {
                    if (rs == slug) { anchors.push_back({sname, src->path, rs, heading, text, "anchor", 0.0}); found = true; break; }
                    continue;
                }
                queried.push_back({sname, src->path, rs, heading, text, "query", 0.0});
            }
            if (!slug.empty() && !found) misses.push_back({f, "section_not_found"});
            continue;
        }

        // A document, or a database read as one (its `guidance` table).
        std::string why;
        const GuidanceDoc* doc = src->type == "sqlite"
            ? guidanceLoadSqliteDoc(eng, st, *src, why)
            : guidanceLoadDoc(eng, st, gc, *src, why);
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
        // Automatic routing: a section NAMED for the trigger wins outright --
        // the first one in each source, so a playbook and a database that
        // both carry guidance for the same check both contribute.
        if (!names.empty()) {
            bool got = false;
            for (const auto& s : doc->sections) {
                if (got) break;
                const std::string hn = normName(s.heading), tn = normName(s.topic);
                for (const auto& k : names) {
                    if (k == hn || (!tn.empty() && k == tn)) {
                        named.push_back({sname, src->path, s.slug, s.heading,
                                         s.own.empty() ? s.body : s.own, "name", 0.0});
                        got = true;
                        break;
                    }
                }
            }
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
    if (rule.automatic) {
        // Named sections first. Otherwise only a STRONG keyword match: a
        // heading term (3) or three text terms -- weak overlap is noise, and
        // an agent's start takes named sections only.
        std::vector<Candidate> out = std::move(named);
        for (auto& c : queried) out.push_back(std::move(c));
        if (out.empty() && ctx.kind != "start") {
            for (auto& c : scored) if (c.score >= 3.0) out.push_back(std::move(c));
        }
        if (out.size() > static_cast<size_t>(rule.max_entries)) out.resize(rule.max_entries);
        return out;
    }
    std::vector<Candidate> out = std::move(anchors);
    for (auto& c : queried) out.push_back(std::move(c));
    for (auto& c : scored) out.push_back(std::move(c));
    if (out.size() > static_cast<size_t>(rule.max_entries)) out.resize(rule.max_entries);
    return out;
}

static bool guidanceQuery(GovernanceEngine& eng, GovernanceEngine::GuidanceState& st,
                          const GuidanceSourceConfig& src, const TriggerCtx& ctx,
                          const std::unordered_set<std::string>& kw,
                          std::vector<std::pair<std::string, std::string>>& rows,
                          std::string& why) {
    auto warnOnce = [&](const std::string& key, const std::string& msg) {
        if (st.warned.insert(key).second)
            fprintf(stderr, "[governance] Warning: guidance source \"%s\" %s\n",
                    src.name.c_str(), msg.c_str());
    };
    const std::string abs = eng.resolvePolicyPath(src.path);
    if (!eng.pathPolicyDenial(abs, /*write=*/false).empty()) {
        why = "policy";
        warnOnce("policy:" + abs, "is not read: the filesystem policy does not let this project read " + src.path);
        return false;
    }
    std::error_code ec;
    if (std::filesystem::is_symlink(abs, ec) || !std::filesystem::is_regular_file(abs, ec)) {
        why = "unreadable";
        warnOnce("open:" + abs, "(" + src.path + ") cannot be opened (missing, or a symbolic link)");
        return false;
    }
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(abs.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
        why = "unreadable";
        warnOnce("open:" + abs, "(" + src.path + ") cannot be opened as a database");
        if (db) sqlite3_close(db);
        return false;
    }
    sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, nullptr);
    sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
    sqlite3_limit(db, SQLITE_LIMIT_ATTACHED, 0);
    sqlite3_limit(db, SQLITE_LIMIT_LENGTH, 1024 * 1024);
    sqlite3_set_authorizer(db, guidanceAuthorizer, nullptr);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(src.timeout_ms);
    sqlite3_progress_handler(db, 1000, guidanceProgress, &deadline);

    struct Closer { sqlite3* d; sqlite3_stmt* s = nullptr; ~Closer() { if (s) sqlite3_finalize(s); sqlite3_close(d); } } closer{db};
    const char* tail = nullptr;
    if (sqlite3_prepare_v2(db, src.query.c_str(), static_cast<int>(src.query.size()), &closer.s, &tail) != SQLITE_OK
        || !closer.s) {
        why = "query_refused";
        warnOnce("prep:" + src.name, std::string("query refused: ") + sqlite3_errmsg(db));
        return false;
    }
    // ONE statement, and a read-only one: a second statement after ';' would
    // otherwise never be checked, and stmt_readonly is the engine's own verdict.
    for (const char* t = tail; t && *t; ++t) {
        if (*t != ';' && !std::isspace(static_cast<unsigned char>(*t))) {
            why = "query_refused";
            warnOnce("multi:" + src.name, "query refused: only one statement is allowed");
            return false;
        }
    }
    if (!sqlite3_stmt_readonly(closer.s)) {
        why = "query_refused";
        warnOnce("ro:" + src.name, "query refused: it is not a read-only statement");
        return false;
    }

    std::vector<std::string> words(kw.begin(), kw.end());
    std::sort(words.begin(), words.end());
    std::string kw_space, kw_or;
    for (size_t i = 0; i < words.size(); ++i) {
        if (i) { kw_space += " "; kw_or += " OR "; }
        kw_space += words[i];
        kw_or += words[i];
    }
    const int nparams = sqlite3_bind_parameter_count(closer.s);
    for (int i = 1; i <= nparams; ++i) {
        const char* raw = sqlite3_bind_parameter_name(closer.s, i);
        std::string name = raw ? std::string(raw + 1) : std::string();
        const std::string* val = nullptr;
        if (name == "kind") val = &ctx.kind;
        else if (name == "arg") val = &ctx.arg;
        else if (name == "agent") val = &ctx.agent;
        else if (name == "topic") val = &ctx.topic;
        else if (name == "keywords") val = &kw_space;
        else if (name == "keywords_or") val = &kw_or;
        if (val) {
            sqlite3_bind_text(closer.s, i, val->c_str(), static_cast<int>(val->size()), SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_null(closer.s, i);
            warnOnce("param:" + src.name + ":" + name,
                     "query parameter \"" + std::string(raw ? raw : "?") + "\" is not one of :kind :arg :agent "
                     ":topic :keywords :keywords_or; it is bound to NULL");
        }
    }
    int rc;
    int n = 0;
    while ((rc = sqlite3_step(closer.s)) == SQLITE_ROW && n < src.max_rows) {
        const int cols = sqlite3_column_count(closer.s);
        auto colText = [&](int c) -> std::string {
            const unsigned char* t = sqlite3_column_text(closer.s, c);
            return t ? std::string(reinterpret_cast<const char*>(t)) : std::string();
        };
        std::string heading = cols >= 2 ? colText(0) : "row " + std::to_string(n + 1);
        std::string text = cols >= 2 ? colText(1) : colText(0);
        if (heading.empty()) heading = "row " + std::to_string(n + 1);
        text = trimBlankLines(text);
        if (!text.empty()) rows.emplace_back(std::move(heading), std::move(text));
        ++n;
    }
    if (rc == SQLITE_INTERRUPT) {
        why = "timeout";
        warnOnce("timeout:" + src.name, "query ran past " + std::to_string(src.timeout_ms) +
                 " ms and was interrupted; nothing from it is delivered");
        rows.clear();
        return false;
    }
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        why = "query_error";
        warnOnce("step:" + src.name, std::string("query failed: ") + sqlite3_errmsg(db));
        rows.clear();
        return false;
    }
    return true;
}

static const GuidanceDoc* guidanceLoadSqliteDoc(GovernanceEngine& eng,
                                                GovernanceEngine::GuidanceState& st,
                                                const GuidanceSourceConfig& src,
                                                std::string& why) {
    auto warnOnce = [&](const std::string& key, const std::string& msg) {
        if (st.warned.insert(key).second)
            fprintf(stderr, "[governance] Warning: guidance source \"%s\" %s\n",
                    src.name.c_str(), msg.c_str());
    };
    const std::string abs = eng.resolvePolicyPath(src.path);
    if (!eng.pathPolicyDenial(abs, /*write=*/false).empty()) {
        why = "policy";
        warnOnce("policy:" + abs, "is not read: the filesystem policy does not let this project read " + src.path);
        return nullptr;
    }
    std::error_code ec;
    if (std::filesystem::is_symlink(abs, ec) || !std::filesystem::is_regular_file(abs, ec)) {
        why = "unreadable";
        warnOnce("open:" + abs, "(" + src.path + ") cannot be opened (missing, or a symbolic link)");
        st.docs.erase(abs);
        return nullptr;
    }
    // Change signature: the database and its -wal (a WAL commit changes the
    // -wal, not the main file).
    auto stamp = [&](const std::string& p) -> std::string {
        std::error_code e2;
        auto t = std::filesystem::last_write_time(p, e2);
        if (e2) return "-";
        auto sz = std::filesystem::file_size(p, e2);
        return std::to_string(static_cast<long long>(
                   std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count())) +
               ":" + std::to_string(e2 ? 0 : sz);
    };
    const std::string sig = stamp(abs) + "|" + stamp(abs + "-wal");
    auto& doc = st.docs[abs];
    if (doc.loaded && doc.sig == sig) return &doc;

    sqlite3* db = nullptr;
    if (sqlite3_open_v2(abs.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
        why = "unreadable";
        warnOnce("open:" + abs, "(" + src.path + ") cannot be opened as a database");
        if (db) sqlite3_close(db);
        st.docs.erase(abs);
        return nullptr;
    }
    sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, nullptr);
    sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
    sqlite3_limit(db, SQLITE_LIMIT_ATTACHED, 0);
    sqlite3_set_authorizer(db, guidanceAuthorizer, nullptr);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(src.timeout_ms * 4);
    sqlite3_progress_handler(db, 1000, guidanceProgress, &deadline);
    struct Closer { sqlite3* d; sqlite3_stmt* s = nullptr; ~Closer() { if (s) sqlite3_finalize(s); sqlite3_close(d); } } closer{db};
    if (sqlite3_prepare_v2(db, "SELECT * FROM guidance", -1, &closer.s, nullptr) != SQLITE_OK || !closer.s) {
        why = "no_table";
        warnOnce("table:" + abs, "(" + src.path + ") has no table named guidance -- create "
                 "guidance(topic, body), or give the source a query");
        st.docs.erase(abs);
        return nullptr;
    }
    const int cols = sqlite3_column_count(closer.s);
    auto colIndex = [&](std::initializer_list<const char*> wanted, int except) {
        for (const char* w : wanted)
            for (int c = 0; c < cols; ++c) {
                if (c == except) continue;
                std::string n = sqlite3_column_name(closer.s, c) ? sqlite3_column_name(closer.s, c) : "";
                for (auto& ch : n) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (n == w) return c;
            }
        return -1;
    };
    const int key_col = colIndex({"topic", "heading", "title", "name"}, -1);
    const int text_col = colIndex({"body", "text", "content", "guidance"}, key_col);
    const int head_col = colIndex({"heading", "title"}, -1);
    if (key_col < 0 || text_col < 0) {
        why = "no_table";
        warnOnce("cols:" + abs, "(" + src.path + "): the guidance table needs a name column (topic, heading, "
                 "title or name) and a text column (body, text, content or guidance)");
        st.docs.erase(abs);
        return nullptr;
    }
    auto colText = [&](int c) -> std::string {
        if (c < 0) return std::string();
        const unsigned char* t = sqlite3_column_text(closer.s, c);
        return t ? std::string(reinterpret_cast<const char*>(t)) : std::string();
    };
    GuidanceDoc fresh;
    fresh.loaded = true;
    fresh.sig = sig;
    std::map<std::string, int> seen;
    int rc, n = 0;
    const int kMaxRows = 5000;
    while ((rc = sqlite3_step(closer.s)) == SQLITE_ROW && n < kMaxRows) {
        ++n;
        GuidanceSection sec;
        sec.topic = colText(key_col);
        sec.heading = head_col >= 0 && head_col != key_col ? colText(head_col) : sec.topic;
        if (sec.heading.empty()) sec.heading = sec.topic;
        sec.own = sec.body = trimBlankLines(colText(text_col));
        if (sec.body.empty() || sec.heading.empty()) continue;
        std::string slug = slugify(sec.topic.empty() ? sec.heading : sec.topic);
        int k = seen[slug]++;
        sec.slug = k == 0 ? slug : slug + "-" + std::to_string(k);
        naab::keywords::extractKeywords(sec.heading + " " + sec.topic, sec.head_kw);
        naab::keywords::extractKeywords(sec.own, sec.own_kw);
        fresh.sections.push_back(std::move(sec));
    }
    if (rc == SQLITE_INTERRUPT || (rc != SQLITE_ROW && rc != SQLITE_DONE)) {
        why = rc == SQLITE_INTERRUPT ? "timeout" : "query_error";
        warnOnce("rows:" + abs, std::string("(") + src.path + ") could not be read: " + sqlite3_errmsg(db));
        st.docs.erase(abs);
        return nullptr;
    }
    if (n >= kMaxRows && rc == SQLITE_ROW) {
        fresh.truncated = true;
        warnOnce("cap:" + abs, "(" + src.path + ") has more than 5000 rows; the first 5000 are used");
    }
    doc = std::move(fresh);
    return &doc;
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
    const auto t0 = std::chrono::steady_clock::now();
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
            TriggerCtx ctx;
            ctx.kind = req.rule.kind;
            ctx.arg = req.rule.kind == "signal" && req.rule.on.size() > 7 ? req.rule.on.substr(7) : req.rule.arg;
            ctx.agent = agent_name;
            ctx.topic = ctx.arg.empty() ? ctx.kind : ctx.arg;
            auto cands = guidanceSelect(*this, st, gc, req.rule, req.keywords, ctx, misses);
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

        // Ids, timing and the run log (end-of-run block, reply linkage).
        const double lookup_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        for (auto& d : delivered) {
            d.id = "g" + std::to_string(st.next_id++);
            d.turn = turn;
            d.lookup_ms = lookup_ms;
            if (st.run_log.size() < kRunLogCap) {
                GuidanceState::RunEntry e;
                e.id = d.id; e.agent = agent_name; e.trigger = d.trigger;
                e.file = d.file; e.heading = d.heading; e.turn = turn;
                e.lookup_ms = lookup_ms;
                st.run_log.push_back(std::move(e));
            }
            auto& waiting = st.awaiting_validation[handle_id];
            if (waiting.size() < 64) waiting.push_back(d.id);
        }
        for (const auto& [trig, ref, reason] : withheld_detail) {
            (void)trig;
            if (st.withheld_log.size() < kRunLogCap) st.withheld_log.emplace_back(ref, reason);
        }
    }

    for (const auto& d : delivered) {
        char sc[32], lm[32];
        snprintf(sc, sizeof(sc), "%.1f", d.score);
        snprintf(lm, sizeof(lm), "%.3f", d.lookup_ms);
        // The delivered TEXT is part of the event: without it the chain cannot
        // show what the model was told. It has passed the secret/PII scan and
        // is bounded by max_chars_per_turn.
        writeAgentTelemetry("GUIDANCE_DELIVERED", {
            {"delivery_id", d.id},
            {"handle_id", std::to_string(handle_id)},
            {"agent", agent_name}, {"config_name", agent_name},
            {"turn", std::to_string(turn)},
            {"channel", "agent"},
            {"trigger", d.trigger},
            {"source", d.source}, {"file", d.file},
            {"section", d.section}, {"heading", d.heading},
            {"select", d.select}, {"score", sc},
            {"chars", std::to_string(d.text.size())},
            {"sha256", d.sha256},
            {"lookup_ms", lm},
            {"text", d.text},
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
            TriggerCtx ctx{"violation", rule_name, agent_ctx, rule_name};
            auto cands = guidanceSelect(*this, st, gc, rc, kw, ctx, misses);
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
                d.id = "g" + std::to_string(st.next_id++);
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
            {"delivery_id", d.id},
            {"channel", "terminal"},
            {"trigger", d.trigger},
            {"source", d.source}, {"file", d.file},
            {"section", d.section}, {"heading", d.heading}, {"select", d.select},
            {"chars", std::to_string(d.text.size())},
            {"sha256", d.sha256},
            {"text", d.text},
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
        TriggerCtx ctx{"pull", "", "", topic};
        auto cands = guidanceSelect(*this, *guidance_state_, gc, rc, kw, ctx, misses);
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
            d.id = "g" + std::to_string(guidance_state_->next_id++);
            out.push_back(std::move(d));
        }
    }
    for (const auto& d : out) {
        writeAgentTelemetry("GUIDANCE_DELIVERED", {
            {"delivery_id", d.id},
            {"channel", "script"}, {"trigger", "pull"},
            {"source", d.source}, {"file", d.file}, {"section", d.section},
            {"heading", d.heading},
            {"select", d.select}, {"chars", std::to_string(d.text.size())},
            {"sha256", d.sha256},
            {"text", d.text},
        });
    }
    for (const auto& [ref, reason] : withheld) {
        writeAgentTelemetry("GUIDANCE_WITHHELD", {
            {"channel", "script"}, {"trigger", "pull"}, {"ref", ref}, {"reason", reason},
        });
    }
    return out;
}

void GovernanceEngine::guidanceOnResponse(int handle_id, const std::string& agent_name,
                                          const std::vector<GuidanceDelivery>& delivered,
                                          const std::unordered_set<std::string>& response_keywords,
                                          bool response_ok, bool analyzed) {
    if (delivered.empty() || !guidanceEnabled()) return;
    // Which signals paid on the reply's turn (only meaningful when CDD
    // analysed it; an interval-skipped turn re-shows stale state).
    std::set<std::string> paid;
    if (analyzed) {
        if (auto ds = getDriftState(handle_id)) {
            for (int i = 0; i < NUM_CDD_SIGNALS; i++)
                if (ds->last_turn_penalties[i] > 0.0) paid.insert(kCddSignalKeys[i]);
        }
    }
    struct Row { std::string id, trigger, matched, recurred; int terms, used, turn; };
    std::vector<Row> rows;
    for (const auto& d : delivered) {
        std::unordered_set<std::string> gkw;
        naab::keywords::extractKeywords(d.text, gkw);
        std::vector<std::string> hit;
        for (const auto& k : gkw) if (response_keywords.count(k)) hit.push_back(k);
        std::sort(hit.begin(), hit.end());
        std::string matched;
        for (size_t i = 0; i < hit.size() && i < 12; ++i) { if (i) matched += ","; matched += hit[i]; }
        std::string recurred;
        if (d.trigger.rfind("signal:", 0) == 0) {
            recurred = !analyzed ? "not_analyzed"
                     : (paid.count(d.trigger.substr(7)) ? "true" : "false");
        }
        rows.push_back({d.id, d.trigger, matched, recurred,
                        static_cast<int>(gkw.size()), static_cast<int>(hit.size()), d.turn});
    }
    {
        std::lock_guard<std::mutex> lock(guidance_mutex_);
        if (guidance_state_) {
            for (const auto& r : rows) {
                for (auto& e : guidance_state_->run_log) {
                    if (e.id != r.id) continue;
                    e.has_reply = response_ok;
                    e.terms = r.terms;
                    e.used = r.used;
                    e.recurred = r.recurred;
                }
            }
        }
    }
    for (const auto& r : rows) {
        char rc[32];
        snprintf(rc, sizeof(rc), "%.2f", r.terms > 0 ? static_cast<double>(r.used) / r.terms : 0.0);
        std::unordered_map<std::string, std::string> f = {
            {"delivery_id", r.id},
            {"handle_id", std::to_string(handle_id)},
            {"agent", agent_name}, {"config_name", agent_name},
            {"turn_at_request", std::to_string(r.turn)},
            {"trigger", r.trigger},
            {"response", response_ok ? "ok" : "failed"},
            {"guidance_terms", std::to_string(r.terms)},
            {"terms_used", std::to_string(r.used)},
            {"recall", rc},
            {"matched", r.matched},
        };
        if (!r.recurred.empty()) f["trigger_recurred"] = r.recurred;
        writeAgentTelemetry("GUIDANCE_RESPONSE", f);
    }
}

void GovernanceEngine::guidanceOnValidation(int handle_id, const std::string& agent_name,
                                            bool passed) {
    if (!guidanceEnabled()) return;
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(guidance_mutex_);
        if (!guidance_state_) return;
        auto it = guidance_state_->awaiting_validation.find(handle_id);
        if (it == guidance_state_->awaiting_validation.end() || it->second.empty()) return;
        ids = std::move(it->second);
        guidance_state_->awaiting_validation.erase(it);
        for (auto& e : guidance_state_->run_log)
            if (std::find(ids.begin(), ids.end(), e.id) != ids.end() && e.validation.empty())
                e.validation = passed ? "passed" : "failed";
    }
    std::string joined;
    for (size_t i = 0; i < ids.size(); ++i) { if (i) joined += ","; joined += ids[i]; }
    writeAgentTelemetry("GUIDANCE_VALIDATION", {
        {"handle_id", std::to_string(handle_id)},
        {"agent", agent_name}, {"config_name", agent_name},
        {"delivery_ids", joined},
        {"passed", passed ? "true" : "false"},
    });
}

// The end-of-run block: what reached agents this run, as plainly as an error
// message ends with its help. Automatic -- no flag -- and silent when nothing
// was delivered or withheld, so every run without guidance is unchanged.
void GovernanceEngine::printGuidanceSummary() {
    std::vector<GuidanceState::RunEntry> log;
    std::vector<std::pair<std::string, std::string>> withheld;
    {
        std::lock_guard<std::mutex> lock(guidance_mutex_);
        if (!guidance_state_ || guidance_state_->summary_printed) return;
        if (guidance_state_->run_log.empty() && guidance_state_->withheld_log.empty()) return;
        guidance_state_->summary_printed = true;
        log = guidance_state_->run_log;
        withheld = guidance_state_->withheld_log;
    }
    double lookup_total = 0.0;
    for (const auto& e : log) lookup_total += e.lookup_ms;
    std::string out = "-- Guidance: " + std::to_string(log.size()) + " delivered to agents";
    if (!withheld.empty()) out += ", " + std::to_string(withheld.size()) + " withheld";
    if (!log.empty()) {
        char avg[48];
        snprintf(avg, sizeof(avg), " (lookup %.2f ms avg)", lookup_total / static_cast<double>(log.size()));
        out += avg;
    }
    out += " --\n";
    const size_t kShow = 12;
    for (size_t i = 0; i < log.size() && i < kShow; ++i) {
        const auto& e = log[i];
        out += "  " + e.agent + " turn " + std::to_string(e.turn) + "  " + e.trigger +
               " -> " + e.file + " > " + e.heading;
        std::string tail;
        if (e.has_reply) tail = "reply used " + std::to_string(e.used) + "/" + std::to_string(e.terms) + " terms";
        else tail = "no reply recorded";
        if (e.recurred == "true") tail += "; signal paid again";
        else if (e.recurred == "false") tail += "; signal quiet";
        if (!e.validation.empty()) tail += "; next validation " + e.validation;
        out += "  (" + tail + ")\n";
    }
    if (log.size() > kShow)
        out += "  ... and " + std::to_string(log.size() - kShow) + " more (every delivery is in telemetry)\n";
    for (size_t i = 0; i < withheld.size() && i < kShow; ++i)
        out += "  withheld: " + withheld[i].first + " (" + withheld[i].second + ")\n";
    fprintf(stderr, "%s", out.c_str());
}

}  // namespace governance
}  // namespace naab
