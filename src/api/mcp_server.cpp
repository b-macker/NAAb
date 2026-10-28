// `naab-lang mcp` -- governed execution over the Model Context Protocol (stdio).
//
// Design, measurements and the premises that did not survive tracing:
// docs/plan-mcp-service.md. The short form:
//
//   * Every propose and commit runs in a NEW `naab-lang` process (the CLI run
//     path, one copy of every governance check). No data, interpreter state or
//     crash carries from one call to the next, and no user code ever runs in
//     this process.
//   * Oversight is continuous anyway: each worker streams the behavioural-
//     sequence events it records to a pipe this server reads; the server keeps
//     them in a per-caller ledger and replays them, with their ages, into the
//     next worker's detector. A sequence split across calls is still caught.
//   * Commits from one caller run one at a time, in arrival order, while
//     sequence detection is enabled: a worker is seeded when it starts, so two
//     commits in flight together could not see each other's events.
//   * Leases narrow (request ∩ policy, with a time limit), propose returns a
//     single-use token bound to the program and policy hashes, commit runs
//     exactly that program, and every execution returns an attestation.
//
// Transport: one JSON-RPC 2.0 message per line on stdin/stdout. Protocol
// revisions 2025-11-25 (initialize handshake) and 2026-07-28 (stateless,
// `server/discover`, per-request `_meta`) are both served.

#include "naab/mcp_server.h"
#include "naab/governance.h"
#include "naab/crypto_utils.h"
#include "naab/trust_store.h"
#include "naab/json_depth.h"
#include "naab/config.h"

#include <nlohmann/json.hpp>
#include <fmt/core.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#  include "naab/scoped_temp_dir.h"   // POSIX: mkdtemp
#  include "worker.h"
#  include <cerrno>
#  include <csignal>
#  include <fcntl.h>
#  include <poll.h>
#  include <sys/stat.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace naab {
namespace api {

#ifdef _WIN32

int runMcpServer(const std::string&) {
    fprintf(stderr,
        "Error: `naab-lang mcp` is not available on this platform yet.\n\n"
        "  Help:\n"
        "  - The first version starts workers with POSIX process groups and pipes.\n"
        "  - Run it on Linux or macOS.\n");
    return 1;
}

#else

namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

const char* const kV2026 = "2026-07-28";
const std::vector<std::string> kInitVersions = {"2025-11-25", "2025-06-18", "2025-03-26"};
const std::vector<std::string> kAllVersions = {"2026-07-28", "2025-11-25", "2025-06-18", "2025-03-26"};
const std::vector<std::string> kActions = {
    "FS_READ", "FS_WRITE", "NET_CONNECT", "SHELL_EXEC", "ENV_READ", "ENV_WRITE", "AGENT_SEND"};

constexpr size_t kMaxMessage = 16u * 1024u * 1024u;   // one JSON-RPC line
constexpr int kDefaultTimeoutSeconds = 30;            // the CLI's own default
constexpr int kDefaultLeaseSeconds = 600;
constexpr int kMaxLeaseSeconds = 86400;

const char* const kInstructions =
    "Runs NAAb programs under the project's govern.json. Every call runs in a fresh "
    "process; behavioural-sequence history carries across calls, so splitting a "
    "dangerous sequence over several calls does not evade it. Use naab_lease to "
    "narrow what a call may do, naab_propose to check a program without running it, "
    "and naab_commit to run exactly the proposed program. naab_run does both in one "
    "call. Every execution returns a signed attestation when the project configures "
    "a signing key.";

std::string dumpJson(const json& j) {
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string isoUtc(std::chrono::system_clock::time_point tp) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_buf{};
    gmtime_r(&t, &tm_buf);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return buf;
}

std::string sha256(const std::string& s) { return security::CryptoUtils::sha256(s); }

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// The policy a token is bound to is the whole `extends` chain plus each
// signature: a parent can change while the child's bytes do not.
std::string policyChainHash(const std::string& govern_path) {
    std::string acc;
    std::set<std::string> seen;
    std::string p = govern_path;
    for (int depth = 0; depth < 8 && !p.empty(); ++depth) {
        std::error_code ec;
        std::string canon = fs::weakly_canonical(p, ec).string();
        if (ec) canon = p;
        if (!seen.insert(canon).second) break;
        std::string bytes;
        if (!readFile(canon, bytes)) { acc += canon + "\nMISSING\n"; break; }
        acc += canon + "\n" + sha256(bytes) + "\n";
        std::string sig;
        if (readFile(canon + ".sig", sig)) acc += "sig " + sha256(sig) + "\n";
        json j;
        try { j = json::parse(bytes); } catch (...) { break; }
        if (!j.is_object() || !j.contains("extends") || !j["extends"].is_string()) break;
        fs::path parent = j["extends"].get<std::string>();
        if (parent.is_relative()) parent = fs::path(canon).parent_path() / parent;
        p = parent.string();
    }
    return sha256(acc);
}

// What the protocol layer reports back as a JSON-RPC error.
struct RpcError {
    int code;
    std::string message;
    json data;
};

// ---------------------------------------------------------------------------
// The policy, loaded fresh for every call so a changed govern.json is seen.
// ---------------------------------------------------------------------------
struct Policy {
    std::shared_ptr<governance::GovernanceEngine> engine;
    std::string govern_path;
    std::string sha;
    bool signed_config = false;
    bool bsd = false;
    int timeout_seconds = kDefaultTimeoutSeconds;
    size_t ledger_cap = 200;
    bool sign = false;
    std::string signing_key;
    unsigned max_concurrent = 1;
};

// ---------------------------------------------------------------------------
// Worker process: runWorker() and InFlight are in worker.h (shared with REST
// /execute). The event stream's lines are parsed here.
// ---------------------------------------------------------------------------
std::function<bool(const std::string&)> eventSink(std::function<void(const json&)> f) {
    return [f = std::move(f)](const std::string& line) {
        json j;
        try { j = json::parse(line); } catch (...) { return false; }
        if (!j.is_object() || !j.contains("t") || !j["t"].is_string()) return false;
        f(j);
        return true;
    };
}

std::string verdictFor(const WorkerResult& w) {
    if (!w.started) return "error";
    if (w.cancelled) return "cancelled";
    if (w.timed_out) return "timeout";
    if (w.term_signal != 0) return "crashed";
    switch (w.exit_code) {
        case 0: return "pass";
        case 2: return "quality_gate";
        case 3: return "blocked";
        default: return "error";
    }
}

// The lines of a worker's output a caller needs: what governance said.
// Bounded to its first kFindingsHead and last kFindingsTail lines, with the
// gap counted between them: it kept the FIRST 60, so a program that wrote to
// stderr pushed out everything governance printed after it, the end-of-run
// groups (printed last) first of all (A43(c)).
constexpr size_t kFindingsHead = 20;
constexpr size_t kFindingsTail = 60;
json findingsFrom(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.empty()) continue;
        if (line.rfind("[governance] Loaded:", 0) == 0) continue;
        if (line.rfind("-- Governance:", 0) == 0) continue;
        lines.push_back(line);
    }
    json f = json::array();
    if (lines.size() <= kFindingsHead + kFindingsTail) {
        for (auto& l : lines) f.push_back(l);
        return f;
    }
    for (size_t i = 0; i < kFindingsHead; ++i) f.push_back(lines[i]);
    f.push_back("[" + std::to_string(lines.size() - kFindingsHead - kFindingsTail) +
                " lines omitted]");
    for (size_t i = lines.size() - kFindingsTail; i < lines.size(); ++i) f.push_back(lines[i]);
    return f;
}

// Reads newline-delimited messages from a descriptor without ever holding more
// than kMaxMessage bytes of one line: std::getline would buffer a line of any
// size, so one endless line from a client exhausted memory before the size
// check could run. An over-long line is discarded through its newline and
// reported, and the reader carries on.
class LineReader {
public:
    explicit LineReader(int fd) : fd_(fd) {}

    // false at EOF. `too_long` is set for a line that exceeded the cap.
    bool next(std::string& line, bool& too_long) {
        too_long = false;
        for (;;) {
            size_t nl = buf_.find('\n');
            if (nl != std::string::npos) {
                if (skipping_) {
                    buf_.erase(0, nl + 1);
                    skipping_ = false;
                    too_long = true;
                    line.clear();
                    return true;
                }
                line.assign(buf_, 0, nl);
                buf_.erase(0, nl + 1);
                return true;
            }
            if (skipping_) buf_.clear();
            else if (buf_.size() > kMaxMessage) { buf_.clear(); skipping_ = true; }
            char tmp[65536];
            ssize_t n = read(fd_, tmp, sizeof(tmp));
            if (n < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (n == 0) {
                if (skipping_) { skipping_ = false; too_long = true; line.clear(); return true; }
                if (buf_.empty()) return false;
                line.swap(buf_);
                buf_.clear();
                return true;
            }
            buf_.append(tmp, static_cast<size_t>(n));
        }
    }

private:
    int fd_;
    std::string buf_;
    bool skipping_ = false;
};

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------
class Server {
public:
    Server(std::string self, std::string cwd, int out_fd)
        : self_(std::move(self)), cwd_(std::move(cwd)), out_fd_(out_fd) {}

    int run();

private:
    struct Lease {
        std::string id;
        std::vector<std::string> granted;
        Clock::time_point expires;
        std::string expires_iso;
    };
    struct Pending {
        std::string program;
        std::string program_sha;
        std::string policy_sha;
        std::string lease_id;
        Clock::time_point expires;
    };
    struct LedgerEvent {
        std::string t, d, a;
        Clock::time_point at;
    };

    // --- I/O ---
    void send(const json& msg);
    void sendResult(const json& id, json result, bool v2026);
    void sendError(const json& id, int code, const std::string& message, const json& data = nullptr);

    // --- dispatch ---
    void handleLine(const std::string& line);
    void handleRequest(const json& msg);
    void handleNotification(const json& msg);
    json toolList() const;
    json callTool(const std::string& name, const json& args, const json& progress_token,
                  const std::string& id_key, uint64_t ticket, bool v2026);

    // --- tools ---
    json toolLease(const json& args);
    json toolPropose(const json& args);
    json toolExecute(const std::string& tool, const json& args, const json& progress_token,
                     const std::string& id_key, uint64_t ticket);
    json toolVerify(const json& args);

    // --- helpers ---
    bool loadPolicy(Policy& p, std::string& error);
    bool leaseFor(const std::string& lease_id, Lease& out, std::string& why);
    std::vector<std::string> workerLeaseArgs(const Lease* lease) const;
    std::string writeProgram(const std::string& code);
    json execute(const std::string& tool, const std::string& program, const Policy& policy,
                 const Lease* lease, const json& progress_token, const std::string& id_key,
                 uint64_t ticket);
    json makeAttestation(const std::string& tool, const std::string& program_sha,
                         const Policy& policy, const Lease* lease, const std::string& verdict,
                         int exit_code, const std::string& output, int events,
                         const std::string& started, const std::string& finished);
    bool scanOutput(const Policy& policy, const std::string& output, std::string& why);
    void withholdFindings(const Policy& policy, json& findings, std::string& verdict);
    json toolResult(const json& structured, bool is_error);

    // ordering of one caller's commits (see the file comment)
    uint64_t takeTicket();
    void waitTurn(uint64_t ticket);
    void releaseTicket(uint64_t ticket);

    void acquireSlot(unsigned limit);
    void releaseSlot();

    void cleanupStalePrograms();

    std::string self_;
    std::string cwd_;
    int out_fd_;

    std::mutex out_mu_;

    std::mutex mu_;                     // leases_, tokens_, inflight_
    std::map<std::string, Lease> leases_;
    std::map<std::string, Pending> tokens_;
    std::map<std::string, std::shared_ptr<InFlight>> inflight_;

    std::mutex ledger_mu_;
    std::deque<LedgerEvent> ledger_;

    std::mutex ticket_mu_;
    std::condition_variable ticket_cv_;
    uint64_t next_ticket_ = 1;
    std::set<uint64_t> open_tickets_;

    std::mutex slot_mu_;
    std::condition_variable slot_cv_;
    unsigned slots_in_use_ = 0;

    std::mutex attest_mu_;
    std::string prev_attestation_sha_;

    std::mutex calls_mu_;
    std::condition_variable calls_cv_;
    int calls_running_ = 0;
};

void Server::send(const json& msg) {
    std::string line = dumpJson(msg) + "\n";
    std::lock_guard<std::mutex> lk(out_mu_);
    const char* p = line.data();
    size_t left = line.size();
    while (left > 0) {
        ssize_t n = write(out_fd_, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;  // the client has gone; nothing left to tell it
        }
        p += n;
        left -= static_cast<size_t>(n);
    }
}

void Server::sendResult(const json& id, json result, bool v2026) {
    if (v2026) {
        if (!result.contains("resultType")) result["resultType"] = "complete";
        result["_meta"]["io.modelcontextprotocol/serverInfo"] =
            json{{"name", "naab-lang"}, {"version", NAAB_VERSION_STRING}};
    }
    send(json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
}

void Server::sendError(const json& id, int code, const std::string& message, const json& data) {
    json err{{"code", code}, {"message", message}};
    if (!data.is_null()) err["data"] = data;
    send(json{{"jsonrpc", "2.0"}, {"id", id}, {"error", err}});
}

uint64_t Server::takeTicket() {
    std::lock_guard<std::mutex> lk(ticket_mu_);
    uint64_t t = next_ticket_++;
    open_tickets_.insert(t);
    return t;
}

void Server::waitTurn(uint64_t ticket) {
    std::unique_lock<std::mutex> lk(ticket_mu_);
    ticket_cv_.wait(lk, [&] { return !open_tickets_.empty() && *open_tickets_.begin() == ticket; });
}

void Server::releaseTicket(uint64_t ticket) {
    {
        std::lock_guard<std::mutex> lk(ticket_mu_);
        open_tickets_.erase(ticket);
    }
    ticket_cv_.notify_all();
}

void Server::acquireSlot(unsigned limit) {
    std::unique_lock<std::mutex> lk(slot_mu_);
    slot_cv_.wait(lk, [&] { return slots_in_use_ < std::max(1u, limit); });
    slots_in_use_++;
}

void Server::releaseSlot() {
    {
        std::lock_guard<std::mutex> lk(slot_mu_);
        slots_in_use_--;
    }
    slot_cv_.notify_all();
}

bool Server::loadPolicy(Policy& p, std::string& error) {
    p.engine = std::make_shared<governance::GovernanceEngine>();
    if (!p.engine->discoverAndLoad(cwd_)) {
        error = "no usable govern.json was found for this project";
        return false;
    }
    const auto& rules = p.engine->getRules();
    p.govern_path = p.engine->getLoadedPath();
    p.sha = policyChainHash(p.govern_path);
    std::error_code ec;
    p.signed_config = fs::exists(p.govern_path + ".sig", ec);
    p.bsd = rules.behavioral_sequences.enabled;
    if (rules.limits.timeout.global > 0) p.timeout_seconds = rules.limits.timeout.global;
    size_t cap = std::max<size_t>(rules.behavioral_sequences.window_size, 1);
    for (const auto& pat : rules.behavioral_sequences.patterns) {
        size_t reach = static_cast<size_t>(std::max(pat.max_gap, 0) + 1) * std::max<size_t>(pat.steps.size(), 1);
        cap = std::max(cap, reach);
    }
    p.ledger_cap = std::min<size_t>(cap, 100000);
    p.sign = rules.audit.provenance.sign_records && !rules.audit.provenance.signing_key.empty();
    if (p.sign) p.signing_key = p.engine->resolvePolicyPath(rules.audit.provenance.signing_key);
    unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    p.max_concurrent = rules.api.max_concurrent > 0
        ? static_cast<unsigned>(rules.api.max_concurrent) : hw;
    return true;
}

bool Server::leaseFor(const std::string& lease_id, Lease& out, std::string& why) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = leases_.find(lease_id);
    if (it == leases_.end()) { why = "unknown lease"; return false; }
    if (Clock::now() > it->second.expires) { why = "the lease has expired"; return false; }
    out = it->second;
    return true;
}

std::vector<std::string> Server::workerLeaseArgs(const Lease* lease) const {
    if (!lease) return {};
    std::string g;
    for (const auto& a : lease->granted) {
        if (!g.empty()) g += ",";
        g += a;
    }
    return {"--lease-grant", g};
}

std::string Server::writeProgram(const std::string& code) {
    return writeWorkerProgram(cwd_, ".naab-mcp-", code);
}

void Server::cleanupStalePrograms() {
    removeStaleWorkerPrograms(cwd_, ".naab-mcp-");
}

json Server::toolResult(const json& structured, bool is_error) {
    json r;
    r["content"] = json::array({json{{"type", "text"}, {"text", dumpJson(structured)}}});
    r["structuredContent"] = structured;
    r["isError"] = is_error;
    return r;
}

// --- naab_lease ------------------------------------------------------------
json Server::toolLease(const json& args) {
    if (!args.contains("capabilities") || !args["capabilities"].is_array())
        throw RpcError{-32602, "naab_lease: 'capabilities' must be an array of action names", nullptr};
    std::vector<std::string> requested;
    for (const auto& c : args["capabilities"]) {
        if (!c.is_string() || std::find(kActions.begin(), kActions.end(), c.get<std::string>()) == kActions.end())
            throw RpcError{-32602, "naab_lease: unknown capability", json{{"known", kActions}}};
        std::string s = c.get<std::string>();
        if (std::find(requested.begin(), requested.end(), s) == requested.end()) requested.push_back(s);
    }
    int ttl = kDefaultLeaseSeconds;
    if (args.contains("ttl_seconds")) {
        if (!args["ttl_seconds"].is_number_integer())
            throw RpcError{-32602, "naab_lease: 'ttl_seconds' must be an integer", nullptr};
        ttl = std::clamp(args["ttl_seconds"].get<int>(), 1, kMaxLeaseSeconds);
    }
    Policy policy;
    std::string err;
    if (!loadPolicy(policy, err))
        return toolResult(json{{"verdict", "error"}, {"reason", err}}, true);
    const auto& r = policy.engine->getRules();
    auto allowed = [&](const std::string& a, std::string& why) -> bool {
        if (a == "SHELL_EXEC" && !(r.shell_allowed && r.capabilities.shell.enabled)) { why = "the project disables shell"; return false; }
        if (a == "NET_CONNECT" && !(r.network_allowed && r.capabilities.network.enabled)) { why = "the project disables network"; return false; }
        if (a == "FS_READ" && r.capabilities.filesystem.mode == "none") { why = "the project disables the filesystem"; return false; }
        if (a == "FS_WRITE" && r.capabilities.filesystem.mode != "write") { why = "the project's filesystem is not writable"; return false; }
        if (a == "ENV_READ" && !r.capabilities.env_vars.read) { why = "the project disables environment reads"; return false; }
        if (a == "ENV_WRITE" && !r.capabilities.env_vars.write) { why = "the project disables environment writes"; return false; }
        if (a == "AGENT_SEND" && r.agents.empty()) { why = "the project configures no agents"; return false; }
        return true;
    };
    Lease lease;
    lease.id = randomHex(16);
    json denied = json::array();
    for (const auto& a : requested) {
        std::string why;
        if (allowed(a, why)) lease.granted.push_back(a);
        else denied.push_back(json{{"capability", a}, {"reason", why}});
    }
    lease.expires = Clock::now() + std::chrono::seconds(ttl);
    lease.expires_iso = isoUtc(std::chrono::system_clock::now() + std::chrono::seconds(ttl));
    json warnings = json::array();
    if (std::find(lease.granted.begin(), lease.granted.end(), "SHELL_EXEC") != lease.granted.end())
        warnings.push_back("SHELL_EXEC grants what a shell can reach: a program with it can start "
                           "another process this lease does not bind. Narrow leases are meaningful "
                           "only without SHELL_EXEC.");
    {
        std::lock_guard<std::mutex> lk(mu_);
        // Drop expired leases so the table cannot grow without bound.
        for (auto it = leases_.begin(); it != leases_.end();) {
            if (Clock::now() > it->second.expires) it = leases_.erase(it);
            else ++it;
        }
        leases_[lease.id] = lease;
    }
    json out{{"lease_id", lease.id}, {"granted", lease.granted}, {"denied", denied},
             {"expires_at", lease.expires_iso}, {"ttl_seconds", ttl}, {"warnings", warnings}};
    return toolResult(out, false);
}

// --- naab_propose -----------------------------------------------------------
json Server::toolPropose(const json& args) {
    if (!args.contains("code") || !args["code"].is_string())
        throw RpcError{-32602, "naab_propose: 'code' must be a string", nullptr};
    std::string code = args["code"].get<std::string>();
    Lease lease;
    const Lease* lease_ptr = nullptr;
    std::string lease_id;
    if (args.contains("lease_id") && !args["lease_id"].is_null()) {
        if (!args["lease_id"].is_string())
            throw RpcError{-32602, "naab_propose: 'lease_id' must be a string", nullptr};
        lease_id = args["lease_id"].get<std::string>();
        std::string why;
        if (!leaseFor(lease_id, lease, why))
            return toolResult(json{{"verdict", "refused"}, {"reason", why}}, true);
        lease_ptr = &lease;
    }
    Policy policy;
    std::string err;
    if (!loadPolicy(policy, err))
        return toolResult(json{{"verdict", "error"}, {"reason", err}}, true);
    if (code.size() > policy.engine->getRules().api.max_body)
        return toolResult(json{{"verdict", "refused"}, {"reason", "the program is larger than api.max_body"}}, true);

    std::string path = writeProgram(code);
    if (path.empty())
        return toolResult(json{{"verdict", "error"}, {"reason", "could not write the program file"}}, true);
    std::vector<std::string> wargs{"--lint-only"};
    for (auto& a : workerLeaseArgs(lease_ptr)) wargs.push_back(a);
    wargs.push_back("--");
    wargs.push_back(path);
    auto fl = std::make_shared<InFlight>();
    acquireSlot(policy.max_concurrent);
    WorkerResult w = runWorker(self_, cwd_, wargs, policy.timeout_seconds, fl,
                               eventSink([](const json&) {}));
    releaseSlot();
    unlink(path.c_str());

    std::string verdict = verdictFor(w);
    // The CLI prints a HARD block on STDOUT, and findings came from stderr
    // alone: a blocked propose answered findings [] (A43(b)). No program runs
    // under --lint-only, so stdout carries only what the CLI says; it goes
    // through the same secret/PII scan as naab_run's stderr.
    json findings = findingsFrom(w.out + "\n" + w.err);
    withholdFindings(policy, findings, verdict);
    json out{{"verdict", verdict}, {"exit_code", w.exit_code},
             {"findings", findings},
             {"program_sha256", sha256(code)}, {"policy_sha256", policy.sha}};
    if (verdict == "pass") {
        Pending pending{code, sha256(code), policy.sha, lease_id,
                        Clock::now() + std::chrono::seconds(kDefaultLeaseSeconds)};
        if (lease_ptr) pending.expires = std::min(pending.expires, lease.expires);
        std::string token = randomHex(24);
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto it = tokens_.begin(); it != tokens_.end();) {
                if (Clock::now() > it->second.expires) it = tokens_.erase(it);
                else ++it;
            }
            tokens_[token] = std::move(pending);
        }
        out["token"] = token;
    }
    return toolResult(out, verdict != "pass");
}

// --- naab_commit / naab_run -------------------------------------------------
json Server::toolExecute(const std::string& tool, const json& args, const json& progress_token,
                         const std::string& id_key, uint64_t ticket) {
    std::string program;
    std::string lease_id;
    std::string expected_policy;
    if (tool == "naab_commit") {
        if (!args.contains("token") || !args["token"].is_string())
            throw RpcError{-32602, "naab_commit: 'token' must be a string", nullptr};
        Pending pending;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = tokens_.find(args["token"].get<std::string>());
            if (it == tokens_.end())
                return toolResult(json{{"verdict", "refused"},
                                       {"reason", "unknown or already used token: propose again"}}, true);
            pending = it->second;
            tokens_.erase(it);  // single-use, whatever happens next
        }
        if (Clock::now() > pending.expires)
            return toolResult(json{{"verdict", "refused"}, {"reason", "the token has expired: propose again"}}, true);
        program = pending.program;
        lease_id = pending.lease_id;
        expected_policy = pending.policy_sha;
    } else {
        if (!args.contains("code") || !args["code"].is_string())
            throw RpcError{-32602, "naab_run: 'code' must be a string", nullptr};
        program = args["code"].get<std::string>();
        if (args.contains("lease_id") && !args["lease_id"].is_null()) {
            if (!args["lease_id"].is_string())
                throw RpcError{-32602, "naab_run: 'lease_id' must be a string", nullptr};
            lease_id = args["lease_id"].get<std::string>();
        }
    }
    Lease lease;
    const Lease* lease_ptr = nullptr;
    if (!lease_id.empty()) {
        std::string why;
        if (!leaseFor(lease_id, lease, why))
            return toolResult(json{{"verdict", "refused"}, {"reason", why}}, true);
        lease_ptr = &lease;
    }
    Policy policy;
    std::string err;
    if (!loadPolicy(policy, err))
        return toolResult(json{{"verdict", "error"}, {"reason", err}}, true);
    if (!expected_policy.empty() && expected_policy != policy.sha)
        return toolResult(json{{"verdict", "refused"},
                               {"reason", "the policy changed since propose: propose again"}}, true);
    if (program.size() > policy.engine->getRules().api.max_body)
        return toolResult(json{{"verdict", "refused"}, {"reason", "the program is larger than api.max_body"}}, true);
    return execute(tool, program, policy, lease_ptr, progress_token, id_key, ticket);
}

json Server::execute(const std::string& tool, const std::string& program, const Policy& policy,
                     const Lease* lease, const json& progress_token, const std::string& id_key,
                     uint64_t ticket) {
    // With sequence detection on, this caller's commits run one at a time in
    // arrival order; without it there is nothing to protect.
    if (policy.bsd) waitTurn(ticket);
    else releaseTicket(ticket);
    struct TicketGuard {
        Server* s; uint64_t t; bool held;
        ~TicketGuard() { if (held) s->releaseTicket(t); }
    } ticket_guard{this, ticket, policy.bsd};

    std::shared_ptr<InFlight> fl;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = inflight_.find(id_key);
        if (it != inflight_.end()) fl = it->second;
    }
    if (!fl) fl = std::make_shared<InFlight>();
    {
        std::lock_guard<std::mutex> lk(fl->mu);
        if (fl->cancelled) return json{{"__cancelled__", true}};
    }

    const std::string started = isoUtc(std::chrono::system_clock::now());
    const std::string program_sha = sha256(program);

    // Per-call directory for the seed file: 0700, removed on every exit path.
    std::error_code tmp_ec;
    runtime::ScopedTempDir call_tmp(fs::temp_directory_path(tmp_ec), "naab-mcp-");
    if (!call_tmp.valid())
        return toolResult(json{{"verdict", "error"}, {"reason", "could not create a working directory"}}, true);
    const std::string call_dir = call_tmp.path().string();

    std::vector<std::string> wargs = workerLeaseArgs(lease);
    if (policy.bsd) {
        std::string seed = call_dir + "/seed.jsonl";
        std::ofstream so(seed, std::ios::binary);
        auto now = Clock::now();
        std::lock_guard<std::mutex> lk(ledger_mu_);
        for (const auto& e : ledger_) {
            json j{{"t", e.t}, {"d", e.d},
                   {"age_ms", std::chrono::duration_cast<std::chrono::milliseconds>(now - e.at).count()}};
            if (!e.a.empty()) j["a"] = e.a;
            so << dumpJson(j) << "\n";
        }
        so.close();
        wargs.push_back("--event-seed");
        wargs.push_back(seed);
    }
    wargs.push_back("--event-stream-fd");
    wargs.push_back("3");

    std::string path = writeProgram(program);
    if (path.empty())
        return toolResult(json{{"verdict", "error"}, {"reason", "could not write the program file"}}, true);
    wargs.push_back("--");
    wargs.push_back(path);

    int progress = 0;
    const size_t cap = policy.ledger_cap;
    auto on_event = [&](const json& ev) {
        LedgerEvent le;
        le.t = ev["t"].get<std::string>();
        if (ev.contains("d") && ev["d"].is_string()) le.d = ev["d"].get<std::string>();
        if (ev.contains("a") && ev["a"].is_string()) le.a = ev["a"].get<std::string>();
        le.at = Clock::now();
        {
            std::lock_guard<std::mutex> lk(ledger_mu_);
            ledger_.push_back(le);
            while (ledger_.size() > cap) ledger_.pop_front();
        }
        if (!progress_token.is_null()) {
            ++progress;
            send(json{{"jsonrpc", "2.0"}, {"method", "notifications/progress"},
                      {"params", {{"progressToken", progress_token}, {"progress", progress},
                                  {"message", le.t}}}});
        }
    };

    acquireSlot(policy.max_concurrent);
    WorkerResult w = runWorker(self_, cwd_, wargs, policy.timeout_seconds, fl, eventSink(on_event));
    releaseSlot();
    unlink(path.c_str());

    if (w.cancelled) return json{{"__cancelled__", true}};

    std::string verdict = verdictFor(w);
    std::string output = w.out;
    json findings = findingsFrom(w.err);
    std::string why;
    if (!output.empty() && scanOutput(policy, output, why)) {
        verdict = "blocked";
        output.clear();
        findings.push_back("output withheld: " + why);
    }
    withholdFindings(policy, findings, verdict);
    const std::string finished = isoUtc(std::chrono::system_clock::now());
    json att = makeAttestation(tool, program_sha, policy, lease, verdict, w.exit_code,
                               output, w.events, started, finished);
    json out{{"verdict", verdict}, {"exit_code", w.exit_code}, {"output", output},
             {"output_truncated", w.out_truncated}, {"findings", findings},
             {"events", w.events}, {"program_sha256", program_sha},
             {"policy_sha256", policy.sha}, {"attestation", att}};
    if (w.term_signal) out["signal"] = w.term_signal;
    return toolResult(out, verdict != "pass");
}

// The program's output goes to an AI agent whose context reaches a model
// provider: the same scan agent.send() applies to model output, under the same
// keys. The server's own engine runs no user code; it only scans text.
// stderr is an egress too. `findings` carries the worker's stderr (and, for
// naab_propose, its stdout) back to the caller (an AI agent whose context
// reaches a model provider), and the output scan covers stdout only -- so a
// secret written to stderr (io.write_error, a polyglot exception) reached the
// caller verbatim while the same value on stdout was withheld. Scan the joined
// findings under the same keys; on a hit, withhold them and block, as stdout
// does. Governance "why blocked" lines already mask secrets (`<redacted>`), so
// a normal block keeps its findings; only genuine secret/PII text trips this.
void Server::withholdFindings(const Policy& policy, json& findings, std::string& verdict) {
    if (findings.empty()) return;
    std::string joined;
    for (const auto& f : findings) {
        if (f.is_string()) { joined += f.get<std::string>(); joined += "\n"; }
    }
    std::string why;
    if (!joined.empty() && scanOutput(policy, joined, why)) {
        verdict = "blocked";
        findings = json::array();
        findings.push_back("diagnostics withheld: " + why + " (in the worker's messages)");
    }
}

bool Server::scanOutput(const Policy& policy, const std::string& output, std::string& why) {
    auto& eng = *policy.engine;
    auto check = [&](const std::function<std::string()>& fn, const char* label) -> bool {
        try {
            if (!fn().empty()) { why = label; return true; }
        } catch (const governance::GovernanceHardError&) {
            why = label;
            return true;
        } catch (const std::exception&) {
            why = label;
            return true;
        }
        return false;
    };
    if (check([&] { return eng.checkSecrets(output); }, "a secret was detected in the output")) return true;
    if (check([&] { return eng.checkPii(output); }, "personal data was detected in the output")) return true;
    return false;
}

json Server::makeAttestation(const std::string& tool, const std::string& program_sha,
                             const Policy& policy, const Lease* lease, const std::string& verdict,
                             int exit_code, const std::string& output, int events,
                             const std::string& started, const std::string& finished) {
    std::lock_guard<std::mutex> lk(attest_mu_);
    json att;
    att["naab_mcp_attestation"] = 1;
    att["call_id"] = randomHex(12);
    att["tool"] = tool;
    att["program_sha256"] = program_sha;
    att["policy_sha256"] = policy.sha;
    att["policy_signed"] = policy.signed_config;
    att["lease"] = lease ? json{{"lease_id", lease->id}, {"granted", lease->granted}} : json(nullptr);
    att["verdict"] = verdict;
    att["exit_code"] = exit_code;
    att["output_sha256"] = sha256(output);
    att["events"] = events;
    att["started_at"] = started;
    att["finished_at"] = finished;
    att["prev_attestation_sha256"] = prev_attestation_sha_;
    att["signed"] = false;
    if (policy.sign) {
        std::string pem;
        if (readFile(policy.signing_key, pem)) {
            try {
                att["signed"] = true;
                att["key_fingerprint"] = security::CryptoUtils::ed25519Fingerprint(
                    security::CryptoUtils::ed25519PublicFromPrivate(pem));
                std::string sig = security::CryptoUtils::ed25519Sign(att.dump(), pem);
                if (sig.empty()) {
                    att["signed"] = false;
                    att.erase("key_fingerprint");
                } else {
                    att["signature"] = sig;
                }
            } catch (...) {
                att["signed"] = false;
                att.erase("key_fingerprint");
                att.erase("signature");
            }
        }
    }
    prev_attestation_sha_ = sha256(att.dump());
    return att;
}

// --- naab_verify_attestation ------------------------------------------------
json Server::toolVerify(const json& args) {
    if (!args.contains("attestation") || !args["attestation"].is_object())
        throw RpcError{-32602, "naab_verify_attestation: 'attestation' must be an object", nullptr};
    const json& att = args["attestation"];
    bool is_signed = att.value("signed", false);
    if (!is_signed || !att.contains("signature") || !att["signature"].is_string())
        return toolResult(json{{"valid", false}, {"signed", false}, {"reason", "not signed"}}, false);
    json payload = att;
    payload.erase("signature");
    const std::string data = payload.dump();
    const std::string sig = att["signature"].get<std::string>();
    const std::string fp = att.value("key_fingerprint", "");

    std::vector<std::pair<std::string, std::string>> candidates;  // {source, public pem}
    Policy policy;
    std::string err;
    if (loadPolicy(policy, err) && policy.sign) {
        std::string pem;
        if (readFile(policy.signing_key, pem)) {
            std::string pub = security::CryptoUtils::ed25519PublicFromPrivate(pem);
            if (!pub.empty() && security::CryptoUtils::ed25519Fingerprint(pub) == fp)
                candidates.push_back({"this project's signing key", pub});
        }
    }
    for (const auto& [kfp, pem] : security::TrustStore::loadKeys())
        if (kfp == fp) candidates.push_back({"a trusted key", pem});
    if (candidates.empty())
        return toolResult(json{{"valid", false}, {"signed", true}, {"key_fingerprint", fp},
                               {"reason", "no known key has this fingerprint"}}, false);
    for (const auto& [source, pem] : candidates) {
        if (security::CryptoUtils::ed25519Verify(data, sig, pem))
            return toolResult(json{{"valid", true}, {"signed", true}, {"key_fingerprint", fp},
                                   {"reason", "signature verified with " + source}}, false);
    }
    return toolResult(json{{"valid", false}, {"signed", true}, {"key_fingerprint", fp},
                           {"reason", "the signature does not match the content"}}, false);
}

// --- protocol ---------------------------------------------------------------
json Server::toolList() const {
    auto schema = [](json props, std::vector<std::string> required) {
        json s{{"type", "object"}, {"properties", std::move(props)}, {"additionalProperties", false}};
        if (!required.empty()) s["required"] = required;
        return s;
    };
    json caps{{"type", "array"}, {"items", {{"type", "string"}, {"enum", kActions}}}};
    json tools = json::array();
    tools.push_back({{"name", "naab_lease"},
        {"title", "Lease capabilities"},
        {"description", "Declare the capability actions the next calls need. Returns a lease "
                        "granting the request intersected with the project policy, with a time "
                        "limit. A lease only narrows; it never grants what the policy denies."},
        {"inputSchema", schema({{"capabilities", caps},
                                {"ttl_seconds", {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaxLeaseSeconds}}}},
                               {"capabilities"})},
        {"annotations", {{"readOnlyHint", true}, {"openWorldHint", false}}}});
    tools.push_back({{"name", "naab_propose"},
        {"title", "Check a program without running it"},
        {"description", "Run every governance check on a NAAb program without executing it. "
                        "On a pass, returns a single-use token that naab_commit runs."},
        {"inputSchema", schema({{"code", {{"type", "string"}}}, {"lease_id", {{"type", "string"}}}}, {"code"})},
        {"annotations", {{"readOnlyHint", true}, {"openWorldHint", false}}}});
    tools.push_back({{"name", "naab_commit"},
        {"title", "Run a proposed program"},
        {"description", "Run exactly the program a naab_propose token was issued for. Refused "
                        "if the token was used, expired, or the policy changed since."},
        {"inputSchema", schema({{"token", {{"type", "string"}}}}, {"token"})},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"openWorldHint", true}}}});
    tools.push_back({{"name", "naab_run"},
        {"title", "Check and run a program"},
        {"description", "Check and run a NAAb program in one call, under the project policy and "
                        "the lease if one is given. Every check runs before any code executes."},
        {"inputSchema", schema({{"code", {{"type", "string"}}}, {"lease_id", {{"type", "string"}}}}, {"code"})},
        {"annotations", {{"readOnlyHint", false}, {"destructiveHint", true}, {"openWorldHint", true}}}});
    tools.push_back({{"name", "naab_verify_attestation"},
        {"title", "Verify an execution attestation"},
        {"description", "Check an attestation's signature against this project's signing key "
                        "and the trusted keys."},
        {"inputSchema", schema({{"attestation", {{"type", "object"}}}}, {"attestation"})},
        {"annotations", {{"readOnlyHint", true}, {"openWorldHint", false}}}});
    return json{{"tools", tools}};
}

json Server::callTool(const std::string& name, const json& args, const json& progress_token,
                      const std::string& id_key, uint64_t ticket, bool /*v2026*/) {
    if (name == "naab_lease") return toolLease(args);
    if (name == "naab_propose") return toolPropose(args);
    if (name == "naab_commit" || name == "naab_run")
        return toolExecute(name, args, progress_token, id_key, ticket);
    if (name == "naab_verify_attestation") return toolVerify(args);
    throw RpcError{-32602, "unknown tool: " + name, nullptr};
}

void Server::handleNotification(const json& msg) {
    const std::string method = msg["method"].get<std::string>();
    if (method == "notifications/cancelled") {
        if (!msg.contains("params") || !msg["params"].is_object() || !msg["params"].contains("requestId"))
            return;
        std::string key = dumpJson(msg["params"]["requestId"]);
        std::shared_ptr<InFlight> fl;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = inflight_.find(key);
            if (it != inflight_.end()) fl = it->second;
        }
        if (!fl) return;  // finished already: a cancel can always arrive late
        std::lock_guard<std::mutex> lk(fl->mu);
        fl->cancelled = true;
        // The whole group: Python stuck inside a C call stops too.
        if (fl->pid > 0) kill(-fl->pid, SIGKILL);
    }
    // notifications/initialized and anything else: nothing to do.
}

void Server::handleRequest(const json& msg) {
    const json id = msg["id"];
    const std::string method = msg["method"].get<std::string>();
    json params = msg.contains("params") ? msg["params"] : json::object();
    if (!params.is_object()) { sendError(id, -32602, "params must be an object"); return; }

    // 2026-07-28: the version rides on every request.
    bool v2026 = false;
    if (params.contains("_meta") && params["_meta"].is_object() &&
        params["_meta"].contains("io.modelcontextprotocol/protocolVersion")) {
        const json& v = params["_meta"]["io.modelcontextprotocol/protocolVersion"];
        std::string ver = v.is_string() ? v.get<std::string>() : "";
        if (std::find(kAllVersions.begin(), kAllVersions.end(), ver) == kAllVersions.end()) {
            sendError(id, -32022, "Unsupported protocol version",
                      json{{"supported", kAllVersions}, {"requested", ver}});
            return;
        }
        v2026 = (ver == kV2026);
    }

    if (method == "initialize") {
        std::string requested = params.value("protocolVersion", "");
        std::string chosen = std::find(kInitVersions.begin(), kInitVersions.end(), requested) != kInitVersions.end()
            ? requested : kInitVersions.front();
        sendResult(id, json{{"protocolVersion", chosen},
                            {"capabilities", {{"tools", {{"listChanged", false}}}}},
                            {"serverInfo", {{"name", "naab-lang"}, {"version", NAAB_VERSION_STRING}}},
                            {"instructions", kInstructions}}, false);
        return;
    }
    if (method == "ping") { sendResult(id, json::object(), v2026); return; }
    if (method == "server/discover") {
        sendResult(id, json{{"supportedVersions", kAllVersions},
                            {"capabilities", {{"tools", {{"listChanged", false}}}}},
                            {"instructions", kInstructions},
                            {"ttlMs", 0}, {"cacheScope", "private"}}, true);
        return;
    }
    if (method == "tools/list") {
        json r = toolList();
        if (v2026) { r["ttlMs"] = 0; r["cacheScope"] = "private"; }
        sendResult(id, r, v2026);
        return;
    }
    if (method == "tools/call") {
        if (!params.contains("name") || !params["name"].is_string()) {
            sendError(id, -32602, "tools/call: 'name' must be a string");
            return;
        }
        std::string name = params["name"].get<std::string>();
        json args = params.contains("arguments") ? params["arguments"] : json::object();
        if (!args.is_object()) { sendError(id, -32602, "tools/call: 'arguments' must be an object"); return; }
        json progress_token = nullptr;
        if (params.contains("_meta") && params["_meta"].is_object() && params["_meta"].contains("progressToken")) {
            const json& pt = params["_meta"]["progressToken"];
            if (pt.is_string() || pt.is_number_integer()) progress_token = pt;
        }
        // Arrival order is decided here, on the reading thread.
        const bool executes = (name == "naab_commit" || name == "naab_run");
        uint64_t ticket = executes ? takeTicket() : 0;
        std::string id_key = dumpJson(id);
        {
            std::lock_guard<std::mutex> lk(mu_);
            inflight_[id_key] = std::make_shared<InFlight>();
        }
        {
            std::lock_guard<std::mutex> lk(calls_mu_);
            calls_running_++;
        }
        std::thread([this, id, name, args, progress_token, id_key, ticket, executes, v2026]() {
            bool cancelled = false;
            try {
                json r = callTool(name, args, progress_token, id_key, ticket, v2026);
                if (r.contains("__cancelled__")) cancelled = true;
                else sendResult(id, r, v2026);
            } catch (const RpcError& e) {
                sendError(id, e.code, e.message, e.data);
            } catch (const std::exception& e) {
                sendError(id, -32603, std::string("internal error: ") + e.what());
            }
            (void)cancelled;  // a cancelled request gets no response (MCP cancellation)
            if (executes) releaseTicket(ticket);  // idempotent
            {
                std::lock_guard<std::mutex> lk(mu_);
                inflight_.erase(id_key);
            }
            // Last touch of `this`: notify under the lock, so run() cannot
            // return and destroy the server before this thread lets go of it.
            std::lock_guard<std::mutex> lk(calls_mu_);
            calls_running_--;
            calls_cv_.notify_all();
        }).detach();
        return;
    }
    sendError(id, -32601, "Method not found: " + method);
}

void Server::handleLine(const std::string& line) {
    if (!util::jsonDepthWithin(line)) {
        sendError(nullptr, -32600, "message nested too deeply");
        return;
    }
    json msg;
    try {
        msg = json::parse(line);
    } catch (...) {
        sendError(nullptr, -32700, "Parse error");
        return;
    }
    if (!msg.is_object()) {
        sendError(nullptr, -32600, "Invalid Request: batches are not supported");
        return;
    }
    json id = msg.contains("id") ? msg["id"] : json(nullptr);
    bool id_ok = id.is_null() || id.is_string() || id.is_number_integer();
    if (msg.value("jsonrpc", "") != "2.0" || !id_ok) {
        sendError(id_ok ? id : json(nullptr), -32600, "Invalid Request");
        return;
    }
    if (msg.contains("method")) {
        if (!msg["method"].is_string()) { sendError(id, -32600, "Invalid Request"); return; }
        if (msg.contains("id") && !msg["id"].is_null()) handleRequest(msg);
        else handleNotification(msg);
        return;
    }
    // A response from the client: this server sends no requests, so ignore it.
}

int Server::run() {
    cleanupStalePrograms();
    {
        Policy p;
        std::string err;
        if (!loadPolicy(p, err)) {
            fprintf(stderr,
                "Error: naab-lang mcp found no usable govern.json from %s\n\n"
                "  Help:\n"
                "  - Start the server in a project directory (or below one) that has a govern.json.\n"
                "  - Create one with: naab-lang init --governance\n", cwd_.c_str());
            return 4;
        }
        if (!p.bsd) {
            fprintf(stderr,
                "[mcp] Behavioural-sequence detection is off in this project, so no sequence is "
                "tracked across calls (behavioral_sequences.enabled).\n");
        }
    }
    LineReader reader(0);
    std::string line;
    bool too_long = false;
    while (reader.next(line, too_long)) {
        if (too_long) { sendError(nullptr, -32600, "message too large"); continue; }
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        handleLine(line);
    }
    // stdin closed: finish what is running, then exit.
    std::unique_lock<std::mutex> lk(calls_mu_);
    calls_cv_.wait(lk, [&] { return calls_running_ == 0; });
    return 0;
}

}  // namespace

int runMcpServer(const std::string& self_exe) {
    // The protocol owns stdout. Anything else that prints (a library warning,
    // a stray fmt::print) would corrupt it, so the real stdout moves to a
    // private descriptor and descriptor 1 becomes stderr.
    int proto_fd = dup(1);
    if (proto_fd < 0) {
        fprintf(stderr, "Error: naab-lang mcp could not set up its output\n");
        return 1;
    }
    {
        int f = fcntl(proto_fd, F_GETFD);
        if (f >= 0) fcntl(proto_fd, F_SETFD, f | FD_CLOEXEC);
    }
    fflush(stdout);
    dup2(2, 1);
    // A client that goes away must not kill the server mid-write.
    signal(SIGPIPE, SIG_IGN);
    std::error_code ec;
    std::string cwd = fs::current_path(ec).string();
    Server server(self_exe, cwd, proto_fd);
    return server.run();
}

#endif  // _WIN32

}  // namespace api
}  // namespace naab
