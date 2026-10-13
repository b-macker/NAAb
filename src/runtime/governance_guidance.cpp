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
#include "naab/host_match.h"
#include "naab/loopback_url.h"

#include <sqlite3.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <tuple>

#ifndef _WIN32
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <poll.h>
#  include <signal.h>
#  include <sys/socket.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  ifdef __linux__
#    include <sys/prctl.h>
#  endif
#  ifdef __APPLE__
#    include <crt_externs.h>
#    define NAAB_GUIDANCE_ENVIRON (*_NSGetEnviron())
#  else
     extern char** environ;
#    define NAAB_GUIDANCE_ENVIRON environ
#  endif
#else
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#endif

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

// The guidance-table row: a NAME column (the first of topic / heading / title
// / name) and a TEXT column (the first of body / text / content / guidance),
// with an optional separate display heading (heading / title). ONE definition,
// shared by a .db source read as a document and a fetched JSON document, so the
// two cannot disagree about what a row is.
const std::vector<std::string> kRowNameCols = {"topic", "heading", "title", "name"};
const std::vector<std::string> kRowTextCols = {"body", "text", "content", "guidance"};
const std::vector<std::string> kRowHeadCols = {"heading", "title"};
constexpr int kMaxRowSections = 5000;

// Append one row as a section. Empty text or name -> skipped (false).
bool appendRowSection(std::vector<GuidanceSection>& out, std::map<std::string, int>& seen,
                      const std::string& topic, const std::string& heading_in,
                      const std::string& text) {
    GuidanceSection sec;
    sec.topic = topic;
    sec.heading = heading_in.empty() ? topic : heading_in;
    sec.own = sec.body = trimBlankLines(text);
    if (sec.body.empty() || sec.heading.empty()) return false;
    std::string slug = slugify(sec.topic.empty() ? sec.heading : sec.topic);
    int k = seen[slug]++;
    sec.slug = k == 0 ? slug : slug + "-" + std::to_string(k);
    naab::keywords::extractKeywords(sec.heading + " " + sec.topic, sec.head_kw);
    naab::keywords::extractKeywords(sec.own, sec.own_kw);
    out.push_back(std::move(sec));
    return true;
}

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

    // Connectors: the engine's fetcher, set by every entry point before it
    // selects (guidanceSelect is a free function and cannot reach the engine's
    // private members). Read-only from here: lookups take the fetcher's own
    // lock just long enough to copy a cached document pointer -- never I/O.
    std::shared_ptr<GovernanceEngine::GuidanceFetcher> fetcher;
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
        // A URL or a command is not a file the program could write.
        if (s.type == "https" || s.type == "command") continue;
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

// Connectors: an https/command source's cached document, or nullptr with the
// reason (not fetched yet, refused by policy, failed). Reads the cache only.
static std::shared_ptr<const GuidanceDoc> guidanceFetchedDoc(GovernanceEngine::GuidanceState& st,
                                                             const GuidanceSourceConfig& src,
                                                             std::string& why);

static std::vector<Candidate> guidanceSelect(GovernanceEngine& eng,
                                             GovernanceEngine::GuidanceState& st,
                                             const GuidanceConfig& gc,
                                             const GuidanceRuleConfig& rule,
                                             const std::unordered_set<std::string>& kw,
                                             const TriggerCtx& ctx,
                                             std::vector<std::pair<std::string, std::string>>& misses) {
    std::vector<Candidate> named, anchors, queried, scored;
    // Keeps fetched (connector) documents alive for the whole selection; the
    // fetcher may swap its cache meanwhile without invalidating them.
    std::vector<std::shared_ptr<const GuidanceDoc>> hold;
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

        // A document, a database read as one (its `guidance` table), or a
        // connector's cached copy -- fetched whole in the background, so this
        // never waits on a network or a process.
        std::string why;
        const GuidanceDoc* doc = nullptr;
        if (src->type == "https" || src->type == "command") {
            auto held = guidanceFetchedDoc(st, *src, why);
            if (held) { doc = held.get(); hold.push_back(std::move(held)); }
        } else if (src->type == "sqlite") {
            doc = guidanceLoadSqliteDoc(eng, st, *src, why);
        } else {
            doc = guidanceLoadDoc(eng, st, gc, *src, why);
        }
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
    auto colIndex = [&](const std::vector<std::string>& wanted, int except) {
        for (const auto& w : wanted)
            for (int c = 0; c < cols; ++c) {
                if (c == except) continue;
                std::string n = sqlite3_column_name(closer.s, c) ? sqlite3_column_name(closer.s, c) : "";
                for (auto& ch : n) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (n == w) return c;
            }
        return -1;
    };
    const int key_col = colIndex(kRowNameCols, -1);
    const int text_col = colIndex(kRowTextCols, key_col);
    const int head_col = colIndex(kRowHeadCols, -1);
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
    const int kMaxRows = kMaxRowSections;
    while ((rc = sqlite3_step(closer.s)) == SQLITE_ROW && n < kMaxRows) {
        ++n;
        appendRowSection(fresh.sections, seen, colText(key_col),
                         head_col >= 0 && head_col != key_col ? colText(head_col) : std::string(),
                         colText(text_col));
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
        guidance_state_->fetcher = guidance_fetcher_;   // connectors: cache only
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
        guidance_state_->fetcher = guidance_fetcher_;   // connectors: cache only
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
        guidance_state_->fetcher = guidance_fetcher_;   // connectors: cache only
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

// ============================================================================
// Connectors, slice 3a (docs/plan-live-guidance.md, "Connectors, slice 3a")
// ============================================================================
//
// An https or command source is fetched WHOLE into a cache and selected
// locally, by the same routing as a document. Nothing from the run is ever
// sent to it: the URL and argv come only from govern.json, stdin is empty, and
// the environment is the snapshot taken when this process was allowed to fetch
// (before any program code ran) -- so neither a pull topic, a validation
// detail, a violation message nor the program's env.set can reach an
// operator-authority process or steer a fetch. Lookups read only the cache:
// the fetcher never takes guidance_mutex_ or results_mutex_, and does no I/O
// while holding its own lock.

namespace {

std::atomic<bool> g_external_guidance{false};
std::mutex g_env_mu;
std::vector<std::string> g_env_snapshot;   // "KEY=value", taken once per process
std::once_flag g_curl_once;

std::vector<std::string> currentEnvironment() {
    std::vector<std::string> out;
#ifndef _WIN32
    for (char** e = NAAB_GUIDANCE_ENVIRON; e && *e; ++e) out.emplace_back(*e);
#else
    LPCH block = ::GetEnvironmentStringsA();
    if (block) {
        for (LPCH p = block; *p;) {
            std::string entry(p);
            p += entry.size() + 1;
            if (!entry.empty() && entry[0] != '=') out.push_back(std::move(entry));
        }
        ::FreeEnvironmentStringsA(block);
    }
#endif
    return out;
}

bool asciiEqualsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

// A variable from the load-time snapshot ("" when absent). Exact name first;
// Windows names are case-insensitive, so fall back to that there.
std::string snapEnv(const std::string& key) {
    std::lock_guard<std::mutex> lk(g_env_mu);
    for (const auto& kv : g_env_snapshot)
        if (kv.size() > key.size() && kv[key.size()] == '=' && kv.compare(0, key.size(), key) == 0)
            return kv.substr(key.size() + 1);
#ifdef _WIN32
    for (const auto& kv : g_env_snapshot)
        if (kv.size() > key.size() && kv[key.size()] == '=' && asciiEqualsIgnoreCase(kv.substr(0, key.size()), key))
            return kv.substr(key.size() + 1);
#endif
    return "";
}

// scheme://host[:port]/path -- no userinfo, query or fragment, which is where a
// careless URL would carry a token. Used in warnings and telemetry.
std::string displayUrl(const std::string& url) {
    std::string out = url.substr(0, url.find_first_of("?#"));
    const size_t scheme_end = out.find("://");
    if (scheme_end != std::string::npos) {
        const size_t host_start = scheme_end + 3;
        const size_t path_start = out.find('/', host_start);
        const size_t at = out.find('@', host_start);
        if (at != std::string::npos && (path_start == std::string::npos || at < path_start))
            out.erase(host_start, at + 1 - host_start);
    }
    return out;
}

std::string sourceTarget(const GuidanceSourceConfig& src) {
    if (src.type == "https") return displayUrl(src.url);
    return src.command.empty() ? std::string() : src.command[0];   // the program, never its arguments
}

// Addresses no guidance source is fetched from, judged on the address libcurl
// actually connects to (the open-socket callback), so DNS cannot change it
// between a check and the connect. Link-local is where cloud instance
// credentials are served (169.254.169.254: AWS, GCP, Azure, Oracle); three
// metadata endpoints sit outside it and are refused by address. Private
// (RFC 1918) addresses are ALLOWED: a guidance URL is operator-declared, and an
// intranet knowledge base is the main enterprise case.
bool guidanceForbiddenV4(uint32_t ip) {
    if ((ip >> 16) == 0xA9FEu) return true;   // 169.254.0.0/16, link-local
    if (ip == 0x646464C8u) return true;       // 100.100.100.200, Alibaba Cloud metadata
    if (ip == 0xA83F8110u) return true;       // 168.63.129.16, Azure platform endpoint
    return false;
}

bool guidanceForbiddenAddress(const struct sockaddr* sa) {
    if (!sa) return true;
    if (sa->sa_family == AF_INET) {
        const auto* sin = reinterpret_cast<const struct sockaddr_in*>(sa);
        return guidanceForbiddenV4(ntohl(sin->sin_addr.s_addr));
    }
    if (sa->sa_family == AF_INET6) {
        const auto* sin6 = reinterpret_cast<const struct sockaddr_in6*>(sa);
        const auto* b = reinterpret_cast<const uint8_t*>(&sin6->sin6_addr);
        if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return true;   // fe80::/10, link-local
        static const uint8_t kAwsV6[16] = {0xfd, 0x00, 0x0e, 0xc2, 0, 0, 0, 0,
                                           0, 0, 0, 0, 0, 0, 0x02, 0x54};
        if (std::memcmp(b, kAwsV6, 16) == 0) return true;         // fd00:ec2::254, AWS IMDS
        static const uint8_t kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (std::memcmp(b, kMapped, 12) == 0) {                   // ::ffff:a.b.c.d
            const uint32_t ip = (uint32_t(b[12]) << 24) | (uint32_t(b[13]) << 16) |
                                (uint32_t(b[14]) << 8) | uint32_t(b[15]);
            return guidanceForbiddenV4(ip);
        }
        return false;
    }
    return true;   // no other family is an address a guidance fetch uses
}

struct GFetchCtx {
    std::string body;
    size_t cap = 0;
    bool over_cap = false;
    bool forbidden_addr = false;
    const std::atomic<bool>* stopping = nullptr;
};

size_t gfWrite(char* p, size_t sz, size_t n, void* ud) {
    auto* c = static_cast<GFetchCtx*>(ud);
    const size_t len = sz * n;
    if (c->body.size() + len > c->cap) { c->over_cap = true; return 0; }
    c->body.append(p, len);
    return len;
}

int gfXfer(void* ud, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* c = static_cast<GFetchCtx*>(ud);
    return (c->stopping && c->stopping->load()) ? 1 : 0;
}

curl_socket_t gfOpenSocket(void* ud, curlsocktype purpose, struct curl_sockaddr* address) {
    auto* c = static_cast<GFetchCtx*>(ud);
    if (!address) return CURL_SOCKET_BAD;
    if (purpose == CURLSOCKTYPE_IPCXN && guidanceForbiddenAddress(&address->addr)) {
        c->forbidden_addr = true;
        return CURL_SOCKET_BAD;
    }
    return ::socket(address->family, address->socktype, address->protocol);
}

// Through a proxy, the socket the request opens is the PROXY's, so the check
// above sees the proxy and never the source. The source's own address is
// therefore checked first: libcurl resolves the URL's host (its own resolver,
// bounded by the fetch timeout) and every candidate address is refused at the
// socket, so nothing but a DNS query leaves the machine and no connection is
// made. A literal address is exact. A name the proxy resolves differently from
// this machine is outside what the engine can see -- where a proxy connects is
// the proxy's decision. A name this machine cannot resolve (common behind a
// proxy) passes to the proxy unchanged.
struct GProbeCtx {
    bool forbidden = false;
    const std::atomic<bool>* stopping = nullptr;
};

curl_socket_t gfProbeSocket(void* ud, curlsocktype purpose, struct curl_sockaddr* address) {
    auto* c = static_cast<GProbeCtx*>(ud);
    if (address && purpose == CURLSOCKTYPE_IPCXN && guidanceForbiddenAddress(&address->addr))
        c->forbidden = true;
    return CURL_SOCKET_BAD;   // never connect: the probe only learns the addresses
}

int gfProbeXfer(void* ud, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* c = static_cast<GProbeCtx*>(ud);
    return (c->stopping && c->stopping->load()) ? 1 : 0;
}

bool guidanceSourceAddressForbidden(const std::string& url, bool https, int timeout_ms,
                                    const std::atomic<bool>& stopping) {
    CURL* c = curl_easy_init();
    if (!c) return false;
    GProbeCtx ctx;
    ctx.stopping = &stopping;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, https ? "https" : "http");
    curl_easy_setopt(c, CURLOPT_PROXY, "");          // resolve the SOURCE, not the proxy
    curl_easy_setopt(c, CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeout_ms));
    curl_easy_setopt(c, CURLOPT_OPENSOCKETFUNCTION, gfProbeSocket);
    curl_easy_setopt(c, CURLOPT_OPENSOCKETDATA, &ctx);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, gfProbeXfer);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &ctx);
    (void)curl_easy_perform(c);   // fails by construction; only ctx.forbidden matters
    curl_easy_cleanup(c);
    return ctx.forbidden;
}

struct FetchResult {
    std::string outcome;   // ok | refused | failed | timeout | stopped
    std::string reason;    // plain words, for warnings and telemetry
    std::string bytes;
};

// What a fetch needs from the engines it serves, asked at the moment it is
// needed (see GuidanceFetcher::engines_mu).
using GuidanceDenialFn = std::function<std::string(const std::string& host, int port, bool https)>;
using GuidancePathFn = std::function<std::string(const std::string& path)>;

FetchResult fetchHttps(const GuidanceDenialFn& policyDenial, const GuidanceSourceConfig& src,
                       int timeout_ms, size_t cap, const std::atomic<bool>& stopping) {
    std::string scheme, host, port_s;
    {
        CURLU* u = curl_url();
        if (!u || curl_url_set(u, CURLUPART_URL, src.url.c_str(), 0) != CURLUE_OK) {
            if (u) curl_url_cleanup(u);
            return {"failed", "its URL could not be parsed", ""};
        }
        char* part = nullptr;
        if (curl_url_get(u, CURLUPART_SCHEME, &part, 0) == CURLUE_OK && part) { scheme = part; curl_free(part); part = nullptr; }
        if (curl_url_get(u, CURLUPART_HOST, &part, 0) == CURLUE_OK && part) { host = part; curl_free(part); part = nullptr; }
        if (curl_url_get(u, CURLUPART_PORT, &part, CURLU_DEFAULT_PORT) == CURLUE_OK && part) { port_s = part; curl_free(part); part = nullptr; }
        curl_url_cleanup(u);
    }
    // An IPv6 literal comes back bracketed; the host policy matches the bare address.
    if (host.size() > 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    const bool https = asciiEqualsIgnoreCase(scheme, "https");
    int port = https ? 443 : 80;
    try { if (!port_s.empty()) port = std::stoi(port_s); } catch (...) {}
    if (!https && !naab::net::isLoopbackHttpUrl(src.url))
        return {"refused", "plain http is allowed only to a loopback host", ""};
    // capabilities.network, judged on every fetch from the LIVE rules: a reload
    // that tightens the policy stops the next refresh.
    const std::string deny = policyDenial(host, port, https);
    if (!deny.empty()) return {"refused", deny, ""};

    std::string token;
    if (!src.auth_env.empty()) {
        token = snapEnv(src.auth_env);
        if (token.empty()) return {"failed", "its credential variable is not set", ""};
    }

    CURL* c = curl_easy_init();
    if (!c) return {"failed", "the HTTP client could not start", ""};
    GFetchCtx ctx;
    ctx.cap = cap;
    ctx.stopping = &stopping;
    curl_easy_setopt(c, CURLOPT_URL, src.url.c_str());
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, https ? "https" : "http");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);   // #229 F43: a redirect skips the host checks
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeout_ms));
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    // Proxy and CA bundle from the LOAD-TIME snapshot, set explicitly: left to
    // itself libcurl reads them from the live environment, which the program's
    // env.set can change -- routing the fetch, credential included, through a
    // proxy or a CA the program chose. An empty CURLOPT_PROXY means no proxy.
    std::string proxy = https ? snapEnv("https_proxy") : snapEnv("http_proxy");
    if (proxy.empty()) proxy = https ? snapEnv("HTTPS_PROXY") : snapEnv("HTTP_PROXY");
    if (proxy.empty()) proxy = snapEnv("all_proxy");
    if (proxy.empty()) proxy = snapEnv("ALL_PROXY");
    curl_easy_setopt(c, CURLOPT_PROXY, proxy.c_str());
    std::string noproxy = snapEnv("no_proxy");
    if (noproxy.empty()) noproxy = snapEnv("NO_PROXY");
    if (!noproxy.empty()) curl_easy_setopt(c, CURLOPT_NOPROXY, noproxy.c_str());
    // With a proxy (even one NO_PROXY may bypass for this host -- checking
    // twice costs one DNS query), the source's address is checked before any
    // request; see guidanceSourceAddressForbidden().
    if (!proxy.empty() && guidanceSourceAddressForbidden(src.url, https, timeout_ms, stopping)) {
        curl_easy_cleanup(c);
        return {"refused", "its address is a cloud metadata or link-local address", ""};
    }
    std::string ca = snapEnv("SSL_CERT_FILE");
    if (ca.empty()) ca = snapEnv("CURL_CA_BUNDLE");
    std::error_code ec;
    if (!ca.empty() && std::filesystem::exists(ca, ec)) {
        curl_easy_setopt(c, CURLOPT_CAINFO, ca.c_str());
    } else {
        // Termux keeps its bundle off the standard path; used only where it
        // exists (the F32 lesson: never pin a path from one machine).
        static const char* kTermuxCa = "/data/data/com.termux/files/usr/etc/tls/cert.pem";
        if (std::filesystem::exists(kTermuxCa, ec)) curl_easy_setopt(c, CURLOPT_CAINFO, kTermuxCa);
    }
    curl_easy_setopt(c, CURLOPT_OPENSOCKETFUNCTION, gfOpenSocket);
    curl_easy_setopt(c, CURLOPT_OPENSOCKETDATA, &ctx);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, gfWrite);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, gfXfer);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &ctx);
    struct curl_slist* hdrs = nullptr;
    std::string auth;
    if (!token.empty()) {
        auth = "Authorization: Bearer " + token;
        hdrs = curl_slist_append(hdrs, auth.c_str());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    }
    const CURLcode rc = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    std::fill(auth.begin(), auth.end(), '\0');
    std::fill(token.begin(), token.end(), '\0');

    if (ctx.forbidden_addr) return {"refused", "its address is a cloud metadata or link-local address", ""};
    if (stopping.load()) return {"stopped", "", ""};
    if (ctx.over_cap) return {"failed", "it is larger than " + std::to_string(cap / 1024) + " KB", ""};
    if (rc == CURLE_OPERATION_TIMEDOUT)
        return {"timeout", "it took longer than " + std::to_string(timeout_ms) + " ms", ""};
    if (rc != CURLE_OK) return {"failed", curl_easy_strerror(rc), ""};
    if (status >= 300 && status < 400) return {"refused", "it answered with a redirect, which is not followed", ""};
    if (status != 200) return {"failed", "it answered HTTP " + std::to_string(status), ""};
    return {"ok", "", std::move(ctx.body)};
}

FetchResult fetchCommand(const GuidancePathFn& resolvePath, const std::string& cwd,
                         const GuidanceSourceConfig& src, int timeout_ms,
                         size_t cap, const std::atomic<bool>& stopping) {
#ifdef _WIN32
    (void)resolvePath; (void)cwd; (void)src; (void)timeout_ms; (void)cap; (void)stopping;
    return {"failed", "command sources are not supported on Windows in this build", ""};
#else
    if (src.command.empty()) return {"failed", "it has no program", ""};
    // Everything the child needs is built BEFORE fork(): allocation is not
    // async-signal-safe, and another thread may hold the allocator's lock.
    std::vector<std::string> env_strings;
    {
        std::lock_guard<std::mutex> lk(g_env_mu);
        for (const auto& kv : g_env_snapshot) {
            // NAAb's own secrets reach no operator process either (as hooks, V-SC-006).
            if (kv.rfind("NAAB_GOVERN_KEY=", 0) == 0 || kv.rfind("NAAB_LOCK_KEY=", 0) == 0 ||
                kv.rfind("NAAB_SIGNING_KEY=", 0) == 0) continue;
            env_strings.push_back(kv);
        }
    }
    std::string prog = src.command[0];
    if (prog.find('/') == std::string::npos) {
        // A bare name is looked up on the SNAPSHOT's PATH, not the live one.
        std::istringstream ps(snapEnv("PATH"));
        std::string dir, found;
        while (std::getline(ps, dir, ':')) {
            if (dir.empty()) continue;
            const std::string cand = dir + "/" + prog;
            if (::access(cand.c_str(), X_OK) == 0) { found = cand; break; }
        }
        if (found.empty()) return {"failed", "its program was not found on PATH", ""};
        prog = found;
    } else if (prog[0] != '/') {
        prog = resolvePath(prog);   // project-relative, like every policy path
    }
    std::vector<std::string> args = src.command;
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    std::vector<char*> envp;
    for (auto& e : env_strings) envp.push_back(const_cast<char*>(e.c_str()));
    envp.push_back(nullptr);
    const unsigned alarm_s = static_cast<unsigned>(timeout_ms / 1000) + 2;   // child-side backstop

    int out[2];
    if (::pipe(out) != 0) return {"failed", "a pipe could not be created", ""};
    const pid_t parent = ::getpid();
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(out[0]); ::close(out[1]);
        return {"failed", "its process could not be started", ""};
    }
    if (pid == 0) {
        // === CHILD: async-signal-safe calls only ===
        ::setpgid(0, 0);                       // its own group: a timeout kills everything it started
#ifdef __linux__
        ::prctl(PR_SET_PDEATHSIG, SIGKILL);    // dies with NAAb, even through _exit()
        if (::getppid() != parent) ::_exit(127);
#else
        (void)parent;
#endif
        const int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) { ::dup2(devnull, STDIN_FILENO); ::dup2(devnull, STDERR_FILENO); }
        ::dup2(out[1], STDOUT_FILENO);
        // Only stdin/stdout/stderr cross: no inherited descriptor (the MCP
        // event stream, a database, a socket) reaches the operator's program.
        for (int fd = 3; fd < 4096; ++fd) ::close(fd);
        if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) ::_exit(126);
        ::alarm(alarm_s);
        ::execve(prog.c_str(), argv.data(), envp.data());
        ::_exit(127);
    }
    ::setpgid(pid, pid);   // also from the parent: whichever runs first wins the race
    ::close(out[1]);
    ::fcntl(out[0], F_SETFL, ::fcntl(out[0], F_GETFL) | O_NONBLOCK);

    std::string bytes;
    bool over = false, timed_out = false, stopped = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (stopping.load()) { stopped = true; break; }
        if (std::chrono::steady_clock::now() > deadline) { timed_out = true; break; }
        struct pollfd pfd{out[0], POLLIN, 0};
        const int pr = ::poll(&pfd, 1, 50);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) continue;
        char buf[8192];
        const ssize_t n = ::read(out[0], buf, sizeof(buf));
        if (n > 0) {
            if (bytes.size() + static_cast<size_t>(n) > cap) { over = true; break; }
            bytes.append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) break;   // EOF: the program closed its output
        if (errno == EAGAIN || errno == EINTR) continue;
        break;
    }
    ::close(out[0]);
    int status = 0;
    if (over || timed_out || stopped) {
        ::kill(-pid, SIGKILL);   // the whole group, before reaping (the pid cannot be reused yet)
        ::waitpid(pid, &status, 0);
    } else {
        for (;;) {   // output closed: wait for it, still bounded
            const pid_t w = ::waitpid(pid, &status, WNOHANG);
            if (w == pid) break;
            if (w < 0 && errno != EINTR) break;
            if (stopping.load() || std::chrono::steady_clock::now() > deadline) {
                stopped = stopping.load();
                timed_out = !stopped;
                ::kill(-pid, SIGKILL);
                ::waitpid(pid, &status, 0);
                break;
            }
            ::usleep(20000);
        }
    }
    if (stopped) return {"stopped", "", ""};
    if (timed_out) return {"timeout", "it ran longer than " + std::to_string(timeout_ms) + " ms", ""};
    if (over) return {"failed", "its output is larger than " + std::to_string(cap / 1024) + " KB", ""};
    if (WIFSIGNALED(status)) return {"failed", "it was killed by signal " + std::to_string(WTERMSIG(status)), ""};
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return {"failed", "it exited " + std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1), ""};
    return {"ok", "", std::move(bytes)};
#endif
}

// A fetched document: JSON rows in the guidance-table shape -- a list of
// {topic, body} objects (or {"guidance": [...]}) with the same column names a
// .db source uses -- or, failing that, a markdown document split at headings.
bool parseFetched(const std::string& bytes, GuidanceDoc& out, std::string& why) {
    size_t i = 0;
    while (i < bytes.size() && std::isspace(static_cast<unsigned char>(bytes[i]))) ++i;
    if (i < bytes.size() && (bytes[i] == '[' || bytes[i] == '{')) {
        const auto j = nlohmann::json::parse(bytes, nullptr, /*allow_exceptions=*/false);
        if (!j.is_discarded()) {
            const nlohmann::json* rows = nullptr;
            if (j.is_array()) rows = &j;
            else if (j.is_object() && j.contains("guidance") && j["guidance"].is_array()) rows = &j["guidance"];
            if (!rows) { why = "its JSON is not a list of {topic, body} rows"; return false; }
            std::map<std::string, int> seen;
            int n = 0;
            for (const auto& row : *rows) {
                if (n >= kMaxRowSections) { out.truncated = true; break; }
                ++n;
                if (!row.is_object()) continue;
                auto pick = [&row](const std::vector<std::string>& cols, const std::string& except)
                        -> std::pair<std::string, std::string> {
                    for (const auto& col : cols) {
                        if (col == except) continue;
                        for (auto it = row.begin(); it != row.end(); ++it) {
                            if (it->is_string() && asciiEqualsIgnoreCase(it.key(), col))
                                return {col, it->get<std::string>()};
                        }
                    }
                    return {"", ""};
                };
                const auto name = pick(kRowNameCols, "");
                const auto text = pick(kRowTextCols, name.first);
                const auto head = pick(kRowHeadCols, "");
                appendRowSection(out.sections, seen, name.second,
                                 (!head.first.empty() && head.first != name.first) ? head.second : std::string(),
                                 text.second);
            }
            if (out.sections.empty()) { why = "its JSON has no row with both a topic and a body"; return false; }
            out.loaded = true;
            return true;
        }
        // Not JSON after all: read it as a markdown document below.
    }
    out.sections = parseSections(bytes);
    if (out.sections.empty()) { why = "it has no sections (no markdown headings, no JSON rows)"; return false; }
    out.loaded = true;
    return true;
}

bool sameSource(const GuidanceSourceConfig& a, const GuidanceSourceConfig& b) {
    return a.type == b.type && a.url == b.url && a.command == b.command && a.auth_env == b.auth_env;
}

}  // namespace

// ONE fetcher per config file per process. A run can hold several engines that
// loaded the same govern.json -- under --tree-walk the CLI's engine (preflight,
// intent, agent review) and the interpreter's own, plus one per tree-walker
// async interpreter -- and each used to start its own fetcher, so a command
// source ran once per engine (observed: twice per --tree-walk run). Every such
// engine now ATTACHES to the one fetcher, and a fetch is refused when ANY
// attached engine's live policy refuses it: the engine that runs the program
// is not always the first to load (under --tree-walk it is the second, and the
// CLI's engine never reloads), so judging by the first would let a tightening
// reload on the running engine go unheeded.
//
// Engine pointers are dereferenced only under engines_mu, for short calls
// (policy, paths, telemetry), never across a fetch; an engine detaches under
// engines_mu in its destructor, so a destroyed engine is never touched. Lock
// order: g_fetchers_mu before engines_mu; engines_mu is never held with mu.
struct GovernanceEngine::GuidanceFetcher {
    std::string key;                     // the config path this fetcher serves
    std::mutex engines_mu;
    std::vector<GovernanceEngine*> engines;
    std::mutex mu;                       // guards everything below; never held during I/O
    std::condition_variable cv;
    std::atomic<bool> stopping{false};
    std::thread thr;
    bool wake = false;
    struct Entry {
        GuidanceSourceConfig src;
        std::shared_ptr<const GuidanceDoc> doc;   // the cached copy (null = none)
        std::string sha;                          // of the bytes behind `doc`
        std::string reported;                     // "outcome|reason" last reported
        std::chrono::steady_clock::time_point due{};
        bool fetched_once = false;
    };
    std::map<std::string, Entry> entries;   // by source name

    void refresh(const std::string& name, int budget_ms);
    void loop();
};

std::mutex g_fetchers_mu;
std::map<std::string, std::weak_ptr<GovernanceEngine::GuidanceFetcher>> g_fetchers;

// One fetch of one source, I/O outside the lock. budget_ms < 0: the source's
// own timeout; >= 0: the start-up budget left (0 = leave it to the thread).
void GovernanceEngine::GuidanceFetcher::refresh(const std::string& name, int budget_ms) {
    GuidanceSourceConfig src;
    {
        std::lock_guard<std::mutex> lk(mu);
        auto it = entries.find(name);
        if (it == entries.end()) return;
        src = it->second.src;
    }
    int timeout = src.timeout_ms;
    if (budget_ms >= 0) timeout = std::min(timeout, budget_ms);
    if (timeout <= 0) return;   // start-up budget spent: still due, the thread fetches it at once
    size_t cap = 0;
    std::string cwd;
    {
        std::lock_guard<std::mutex> ek(engines_mu);
        if (engines.empty()) return;
        int kb = 0;   // the smallest cap any attached engine sets
        for (auto* e : engines) {
            const int k = std::max(1, e->rulesPtr()->guidance.max_source_kb);
            kb = kb == 0 ? k : std::min(kb, k);
        }
        cap = static_cast<size_t>(kb) * 1024;
        cwd = engines.front()->getGovernDir();   // one config file, one directory
    }
    const GuidanceDenialFn denial = [this](const std::string& h, int p, bool https) {
        std::lock_guard<std::mutex> ek(engines_mu);
        if (engines.empty()) return std::string("no governed program is running");
        for (auto* e : engines) {
            std::string d = e->networkPolicyDenial(h, p, https);
            if (!d.empty()) return d;
        }
        return std::string();
    };
    const GuidancePathFn resolve = [this](const std::string& p) {
        std::lock_guard<std::mutex> ek(engines_mu);
        return engines.empty() ? p : engines.front()->resolvePolicyPath(p);
    };
    const auto t0 = std::chrono::steady_clock::now();
    FetchResult r = src.type == "https" ? fetchHttps(denial, src, timeout, cap, stopping)
                                        : fetchCommand(resolve, cwd, src, timeout, cap, stopping);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (r.outcome == "stopped") return;   // shutting down: nothing to report
    std::shared_ptr<GuidanceDoc> parsed;
    std::string sha;
    const size_t nbytes = r.bytes.size();
    if (r.outcome == "ok") {
        auto d = std::make_shared<GuidanceDoc>();
        std::string why;
        if (parseFetched(r.bytes, *d, why)) {
            sha = security::CryptoUtils::sha256(r.bytes);
            parsed = std::move(d);
        } else {
            r.outcome = "failed";
            r.reason = why;
        }
    }
    bool report = false, kept_copy = false;
    size_t sections = 0;
    {
        std::lock_guard<std::mutex> lk(mu);
        auto it = entries.find(name);
        if (it == entries.end() || !sameSource(it->second.src, src)) return;   // reconfigured meanwhile
        auto& e = it->second;
        e.due = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(5, e.src.refresh_seconds));
        e.fetched_once = true;
        bool changed = false;
        if (r.outcome == "refused") {
            // Fail closed: content the policy now forbids is not delivered.
            e.doc.reset();
            e.sha.clear();
        } else if (r.outcome == "ok") {
            sections = parsed->sections.size();
            if (sha != e.sha || !e.doc) { e.doc = parsed; e.sha = sha; changed = true; }
        }
        // failed / timeout: the last good copy stays (it passed the same checks
        // when it arrived, and stale guidance beats none); the failure is reported.
        kept_copy = static_cast<bool>(e.doc);
        const std::string key = r.outcome + "|" + r.reason;
        report = changed || key != e.reported;
        e.reported = key;
    }
    if (!report) return;
    if (r.outcome != "ok") {
        fprintf(stderr, "[governance] Warning: guidance source \"%s\" was not fetched: %s%s\n",
                src.name.c_str(), r.reason.c_str(),
                r.outcome == "refused" ? "; nothing from it is delivered"
                                       : (kept_copy ? "; its last copy is still used" : ""));
    }
    char msb[32];
    snprintf(msb, sizeof(msb), "%.1f", ms);
    std::unordered_map<std::string, std::string> f = {
        {"source", src.name}, {"type", src.type}, {"target", sourceTarget(src)},
        {"outcome", r.outcome}, {"ms", msb},
    };
    if (!r.reason.empty()) f["reason"] = r.reason;
    if (r.outcome == "ok") {
        f["bytes"] = std::to_string(nbytes);
        f["sections"] = std::to_string(sections);
        f["sha256"] = sha;
    } else {
        f["kept_previous_copy"] = kept_copy ? "true" : "false";
    }
    {
        // Once, through the first attached engine: they share the config, and
        // so the telemetry file.
        std::lock_guard<std::mutex> ek(engines_mu);
        if (!engines.empty()) {
            try { engines.front()->writeAgentTelemetry("GUIDANCE_SOURCE_REFRESH", f); } catch (...) {}
        }
    }
}

void GovernanceEngine::GuidanceFetcher::loop() {
    std::unique_lock<std::mutex> lk(mu);
    while (!stopping.load()) {
        auto next = std::chrono::steady_clock::now() + std::chrono::hours(1);
        for (const auto& [n, e] : entries) next = std::min(next, e.due);
        cv.wait_until(lk, next, [this] { return stopping.load() || wake; });
        if (stopping.load()) break;
        wake = false;
        const auto now = std::chrono::steady_clock::now();
        std::vector<std::string> due;
        for (const auto& [n, e] : entries) if (e.due <= now) due.push_back(n);
        lk.unlock();
        for (const auto& n : due) {
            if (stopping.load()) break;
            refresh(n, -1);
        }
        lk.lock();
    }
}

void GovernanceEngine::allowExternalGuidance(bool on) {
    if (on) {
        std::call_once(g_curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
        std::lock_guard<std::mutex> lk(g_env_mu);
        if (g_env_snapshot.empty()) g_env_snapshot = currentEnvironment();
    }
    g_external_guidance.store(on);
}

void GovernanceEngine::startGuidanceSources(bool initial_load) {
    if (!g_external_guidance.load()) return;
    const auto rules_hold = rulesPtr();
    std::vector<GuidanceSourceConfig> ext;
    if (isActive() && rules_hold->guidance.enabled) {
        for (const auto& s : rules_hold->guidance.sources)
            if (s.type == "https" || s.type == "command") ext.push_back(s);
    }
    std::shared_ptr<GuidanceFetcher> f;
    bool created = false;
    {
        std::lock_guard<std::mutex> lk(guidance_mutex_);
        f = guidance_fetcher_;
    }
    if (!f) {
        if (ext.empty()) return;
        const std::string key = getLoadedPath();
        {
            std::lock_guard<std::mutex> rk(g_fetchers_mu);
            auto it = g_fetchers.find(key);
            if (it != g_fetchers.end()) f = it->second.lock();
            if (!f) {
                f = std::make_shared<GuidanceFetcher>();
                f->key = key;
                g_fetchers[key] = f;
                created = true;
            }
            std::lock_guard<std::mutex> ek(f->engines_mu);
            f->engines.push_back(this);
        }
        {
            std::lock_guard<std::mutex> lk(guidance_mutex_);
            guidance_fetcher_ = f;
        }
        // Another engine in this process already fetches this file's sources:
        // share its cache, fetch nothing. Its policy now also answers to this
        // engine's (the conjunction in refresh()).
        if (!created) return;
    }
    std::vector<std::string> fresh;
    {
        std::lock_guard<std::mutex> lk(f->mu);
        // A source no longer declared (or declared differently) loses its copy.
        for (auto it = f->entries.begin(); it != f->entries.end();) {
            bool keep = false;
            for (const auto& s : ext) if (s.name == it->first && sameSource(s, it->second.src)) { keep = true; break; }
            if (keep) ++it; else it = f->entries.erase(it);
        }
        for (const auto& s : ext) {
            auto it = f->entries.find(s.name);
            if (it == f->entries.end()) {
                GuidanceFetcher::Entry e;
                e.src = s;
                f->entries.emplace(s.name, std::move(e));
                fresh.push_back(s.name);
            } else {
                it->second.src.timeout_ms = s.timeout_ms;
                it->second.src.refresh_seconds = s.refresh_seconds;
                // The policy may have changed: re-check every source now.
                it->second.due = std::chrono::steady_clock::time_point{};
            }
        }
    }
    if (created) {
        bool has_command = false;
        for (const auto& s : ext) if (s.type == "command") has_command = true;
        const auto& net = rules_hold->capabilities.network;
        if (has_command && (!net.allowed_hosts.empty() || !net.blocked_hosts.empty())) {
            fprintf(stderr, "[governance] Note: a guidance command source runs with the operator's "
                            "authority; the project's allowed and blocked host lists cannot bind the "
                            "connections it makes\n");
        }
        if (initial_load) {
            // Before the program runs, bounded in total, so a violation found at
            // load already has its guidance. What the budget does not cover the
            // thread fetches at once.
            const auto t0 = std::chrono::steady_clock::now();
            const int total = std::max(0, rules_hold->guidance.initial_fetch_ms);
            for (const auto& n : fresh) {
                const int spent = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count());
                f->refresh(n, std::max(0, total - spent));
            }
        }
        f->thr = std::thread([fp = f.get()] { fp->loop(); });
    } else {
        std::lock_guard<std::mutex> lk(f->mu);
        f->wake = true;
        f->cv.notify_all();
    }
}

void GovernanceEngine::stopGuidanceSources() {
    std::shared_ptr<GuidanceFetcher> f;
    {
        std::lock_guard<std::mutex> lk(guidance_mutex_);
        f = std::move(guidance_fetcher_);
        guidance_fetcher_.reset();
    }
    if (!f) return;
    bool last = false;
    {
        // Under g_fetchers_mu, so no engine can attach between "last one out"
        // and the registry forgetting the fetcher.
        std::lock_guard<std::mutex> rk(g_fetchers_mu);
        std::lock_guard<std::mutex> ek(f->engines_mu);
        f->engines.erase(std::remove(f->engines.begin(), f->engines.end(), this), f->engines.end());
        last = f->engines.empty();
        if (last) {
            auto it = g_fetchers.find(f->key);
            if (it != g_fetchers.end() && it->second.lock() == f) g_fetchers.erase(it);
        }
    }
    if (!last) return;
    {
        std::lock_guard<std::mutex> lk(f->mu);
        f->stopping.store(true);
        f->wake = true;
    }
    f->cv.notify_all();
    if (f->thr.joinable()) f->thr.join();
}

static std::shared_ptr<const GuidanceDoc> guidanceFetchedDoc(GovernanceEngine::GuidanceState& st,
                                                             const GuidanceSourceConfig& src,
                                                             std::string& why) {
    if (!st.fetcher) {
        why = "not_fetched";
        if (!g_external_guidance.load() && st.warned.insert("nofetch:" + src.name).second) {
            fprintf(stderr, "[governance] Warning: guidance source \"%s\" is fetched only when a "
                            "program runs (naab-lang run); nothing from it is delivered here\n",
                    src.name.c_str());
        }
        return nullptr;
    }
    std::lock_guard<std::mutex> lk(st.fetcher->mu);
    auto it = st.fetcher->entries.find(src.name);
    if (it != st.fetcher->entries.end() && !sameSource(it->second.src, src)) {
        // Another engine of this process reloaded the file and the fetcher now
        // serves its version of this source; this engine follows when it reloads.
        why = "not_fetched";
        return nullptr;
    }
    if (it == st.fetcher->entries.end() || !it->second.doc) {
        why = (it != st.fetcher->entries.end() && it->second.fetched_once) ? "unavailable" : "not_fetched";
        return nullptr;
    }
    return it->second.doc;
}

}  // namespace governance
}  // namespace naab
