// governance_reports.cpp — GovernanceEngine report generation, audit, plugins
// Extracted from governance.cpp lines 5603-7484

#include "naab/governance.h"
#include "naab/telemetry_forwarder.h"
#include "naab/error_sanitizer.h"
#include "naab/crypto_utils.h"
#include "naab/language_registry.h"
#include "naab/interpreter.h"
#include "naab/analyzer/task_pattern_detector.h"
#include "naab/analyzer/syntactic_analyzer.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <regex>
#include <chrono>
#include <functional>
#include <unordered_set>
#ifndef _WIN32
#  include <unistd.h>
#  include <sys/file.h>
#  include <sys/wait.h>
#  include <signal.h>
#  include <fcntl.h>
#else
#  include <windows.h>
#endif
#include "naab/subprocess_helpers.h"
#include <fmt/core.h>

// Helper: checked fwrite with failure counter (warns once on first failure)
static bool checkedWrite(FILE* fp, const std::string& line,
                         std::atomic<int>& failure_counter) {
    size_t written = fwrite(line.c_str(), 1, line.size(), fp);
    if (written != line.size()) {
        int prev = failure_counter.fetch_add(1, std::memory_order_relaxed);
        if (prev == 0) {
            fprintf(stderr, "[governance] WARNING: telemetry write failed "
                "(wrote %zu/%zu bytes)\n", written, line.size());
        }
        return false;
    }
    return true;
}

// Forward declarations for symbols defined in other governance translation units
namespace naab {
std::pair<std::vector<std::string>, std::vector<std::string>>
lookupCweOwasp(const std::string& rule_name);  // defined in governance_engine.cpp
} // namespace naab

namespace naab {
namespace governance {
std::string normalizeLanguage(const std::string& language);  // defined in governance_checks.cpp
} // namespace governance
} // namespace naab

namespace naab {
namespace interpreter {
extern thread_local Interpreter* g_current_interpreter;  // defined in interpreter.cpp
} // namespace interpreter
} // namespace naab

namespace naab {
namespace governance {

// Defined below (after computeHash): tail-read of the last chained hash in a
// JSONL file, used to anchor hash chains to the file across runs/processes.
static std::string readLastChainedHash(const std::string& path);

// --- Audit Trail ---
void GovernanceEngine::logAuditEvent(const std::string& event_type,
                                      const std::string& rule_name,
                                      const std::string& message,
                                      const std::string& file, int line) {
    if (rules().audit.level == "none") return;
    std::lock_guard<std::mutex> lock(audit_mutex_);

    std::string output_file = rules().audit.output_file;
    if (output_file.empty()) output_file = ".governance-audit.jsonl";

    // Build entry
    nlohmann::json entry;
    entry["timestamp"] = std::chrono::system_clock::now().time_since_epoch().count();
    entry["event"] = event_type;
    entry["rule"] = rule_name;
    entry["message"] = message;
    if (!file.empty()) entry["file"] = error::ErrorSanitizer::sanitizeFilePaths(file);
    if (line > 0) entry["line"] = line;

    // Include config rationale in audit entries when available
    std::string rationale = lookupRationale(rule_name);
    if (!rationale.empty()) entry["rationale"] = rationale;

    // Tamper-evident hash chain — seed from the file's tail on this process's
    // first audit write so the chain links across runs instead of restarting
    // at genesis (which made whole-run deletion undetectable).
    if (rules().audit.tamper_evidence.enabled) {
        if (last_audit_hash_.empty()) {
            std::string tail = readLastChainedHash(output_file);
            if (!tail.empty()) last_audit_hash_ = tail;
        }
        entry["prev_hash"] = last_audit_hash_.empty()
            ? rules().audit.tamper_evidence.chain_genesis
            : last_audit_hash_;
        last_audit_hash_ = computeAuditHash(entry.dump());
        entry["hash"] = last_audit_hash_;
    }

    try {
        std::ofstream ofs(output_file, std::ios::app);
        if (ofs.is_open()) {
            ofs << entry.dump() << "\n";
            if (ofs.fail()) {
                audit_write_failures_.fetch_add(1, std::memory_order_relaxed);
                fmt::print(stderr, "[governance] AUDIT WRITE FAILURE: write to {} failed\n",
                           output_file);
            }
        } else {
            audit_write_failures_.fetch_add(1, std::memory_order_relaxed);
            fmt::print(stderr, "[governance] AUDIT WRITE FAILURE: cannot open {}\n",
                       output_file);
        }
    } catch (const std::exception& e) {
        audit_write_failures_.fetch_add(1, std::memory_order_relaxed);
        fmt::print(stderr, "[governance] AUDIT WRITE FAILURE: {}\n", e.what());
    } catch (...) {
        audit_write_failures_.fetch_add(1, std::memory_order_relaxed);
        fmt::print(stderr, "[governance] AUDIT WRITE FAILURE: unknown error\n");
    }
}

void GovernanceEngine::logPolyglotExecution(const std::string& language,
                                              const std::vector<std::string>& bound_vars,
                                              int64_t duration_us,
                                              const std::string& file, int line,
                                              const std::string& runtime_version) {
    if (!rules().audit.log_events.polyglot_executed &&
        !rules().audit.log_events.polyglot_timing) return;

    std::string vars_str;
    for (size_t i = 0; i < bound_vars.size(); ++i) {
        if (i > 0) vars_str += ", ";
        vars_str += bound_vars[i];
    }

    std::string msg = "lang=" + language;
    if (!runtime_version.empty()) msg += " runtime=" + runtime_version;
    if (!vars_str.empty()) msg += " vars=[" + vars_str + "]";
    if (rules().audit.log_events.polyglot_timing) {
        msg += " duration=" + std::to_string(duration_us) + "us";
    }

    logAuditEvent("polyglot_executed", "polyglot", msg, file, line);
}

void GovernanceEngine::logTaintDecision(const std::string& var_name,
                                         const std::string& decision,
                                         const std::string& sink,
                                         const std::string& file, int line) {
    if (!rules().audit.log_events.taint_decisions) return;

    std::string msg = "var=" + var_name + " decision=" + decision;
    if (!sink.empty()) msg += " sink=" + sink;

    logAuditEvent("taint_decision", "taint_tracking", msg, file, line);
}

void GovernanceEngine::logContractCheck(const std::string& func_name,
                                         const std::string& result,
                                         const std::string& detail,
                                         const std::string& file, int line) {
    if (!rules().audit.log_events.contract_checks) return;

    std::string msg = "func=" + func_name + " result=" + result;
    if (!detail.empty()) msg += " " + detail;

    logAuditEvent("contract_check", "contracts", msg, file, line);
}

std::string GovernanceEngine::computeAuditHash(const std::string& data) const {
    return computeHash(data, rules().audit.tamper_evidence);
}

std::string GovernanceEngine::computeHash(const std::string& data,
    const TamperEvidenceConfig& te) const {
    // HMAC-SHA256 when key configured; plain SHA-256 fallback (both cryptographic)
    if (!te.hmac_key.empty()) {
        return security::CryptoUtils::hmacSha256(data, te.hmac_key);
    }
    return security::CryptoUtils::sha256(data);
}

// ISO timestamp for chain anchor events (RunStart/RunEnd)
static std::string isoTimestampNow() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char ts_buf[32];
    std::strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);
    return std::string(ts_buf);
}

// Read the hash of the last chained event in a JSONL file. Scans backward at
// most 64KB from the end for the last line carrying a "hash" field. Returns ""
// when the file is missing, empty, or has no chained events in the window.
// Callers hold the file's flock, so the tail is stable during the read.
static std::string readLastChainedHash(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return "";
    ifs.seekg(0, std::ios::end);
    std::streamoff size = ifs.tellg();
    if (size <= 0) return "";
    const std::streamoff kWindow = 64 * 1024;
    std::streamoff start = size > kWindow ? size - kWindow : 0;
    ifs.seekg(start);
    std::string window(static_cast<size_t>(size - start), '\0');
    ifs.read(&window[0], size - start);
    window.resize(static_cast<size_t>(ifs.gcount()));

    size_t pos = window.size();
    while (pos > 0) {
        size_t nl = window.rfind('\n', pos - 1);
        size_t line_start = (nl == std::string::npos) ? 0 : nl + 1;
        std::string line = window.substr(line_start, pos - line_start);
        // Match "hash":" exactly — the leading quote excludes "prev_hash".
        if (line.find("\"hash\":\"") != std::string::npos) {
            try {
                auto ev = nlohmann::json::parse(line);
                if (ev.contains("hash") && ev["hash"].is_string()) {
                    return ev["hash"].get<std::string>();
                }
            } catch (...) {
                // Partial line at the window boundary or corrupt JSON —
                // keep scanning toward the file start.
            }
        }
        if (nl == std::string::npos || nl == 0) break;
        pos = nl;
    }
    return "";
}

// File-anchored chain: prev_hash for the next event comes from the output
// file's tail (authoritative — links across runs and flock-serialized
// concurrent writers), falling back to the in-memory hash, then genesis.
// Lazily writes the RunStart anchor before this process's first chained
// event. Requires telemetry_hash_mutex_ held and fp open + flocked.
std::string GovernanceEngine::chainPrevLocked(FILE* fp) const {
    const auto& te = rules().telemetry_output.tamper_evidence;
    // Flush buffered writes first: batched writers (writeTelemetry's check
    // loop) hold fp across many events, and the tail read below goes to the
    // file on disk — unflushed lines would make it return a stale hash.
    fflush(fp);
    std::string prev = readLastChainedHash(rules().telemetry_output.output_file);
    if (prev.empty()) {
        prev = last_telemetry_hash_.empty() ? te.chain_genesis
                                            : last_telemetry_hash_;
    }
    if (!run_start_emitted_) {
        run_start_emitted_ = true;
        nlohmann::json rs;
        rs["run_id"] = run_id_;
        rs["agent_id"] = agent_id_;
        rs["event_type"] = "RunStart";
        rs["timestamp"] = isoTimestampNow();
        // Run identity. Without these, telemetry cannot say which config
        // produced it: report.py reads src/govern.json, which need not be the
        // file that ran, and the prose-arm verification had to be settled by
        // asking a human which arm had executed.
        //
        // Added before computeHash below, so they sit INSIDE the hashed payload
        // and cannot be edited after the fact without breaking the chain.
        rs["config_fingerprint"] = config_fingerprint_.empty()
            ? std::string("unset") : config_fingerprint_;
        rs["mandate_digest"] = mandate_digest_.empty()
            ? std::string("unset") : mandate_digest_;
#ifndef _WIN32
        rs["pid"] = static_cast<long>(getpid());
#endif
        rs["prev_hash"] = prev;
        last_telemetry_hash_ = computeHash(rs.dump(), te);
        rs["hash"] = last_telemetry_hash_;
        checkedWrite(fp, rs.dump() + "\n", telemetry_write_failures_);
        chained_events_this_run_++;
        prev = last_telemetry_hash_;
    }
    return prev;
}

// RunEnd anchor: declares this run's chained event count so the verifier can
// detect trailing truncation of a run.
//
// NOT write-once. writeReports() is called from ~17 sites and a clean
// execute() is not the last of them: main.cpp's contract, quality-gate and
// baseline exits all call it again after the run has already been sealed.
// A write-once RunEnd leaves those later events undeclared, and the verifier
// reports a hard BREAK ("declares 737 but 785 observed") on a file nobody
// touched. Re-emitting reconciles the count — the verifier reads the last
// RunEnd per run_id, so the newest declaration wins — and makes the invariant
// self-healing for any future writer that appends after a RunEnd.
void GovernanceEngine::emitRunEnd(FILE* fp, const std::string& timestamp) const {
    const auto& te = rules().telemetry_output.tamper_evidence;
    if (!te.enabled) return;
    std::lock_guard<std::mutex> hlock(telemetry_hash_mutex_);
    // Nothing chained since the last RunEnd ⇒ its declaration still holds.
    if (run_end_emitted_ && chained_events_this_run_ == run_end_declared_) return;
    run_end_emitted_ = true;
    nlohmann::json re;
    re["run_id"] = run_id_;
    re["agent_id"] = agent_id_;
    re["event_type"] = "RunEnd";
    re["timestamp"] = timestamp;
    re["prev_hash"] = chainPrevLocked(fp);
    // Count includes RunStart, all chained events, and this RunEnd itself.
    re["chained_events"] = chained_events_this_run_ + 1;
    last_telemetry_hash_ = computeHash(re.dump(), te);
    re["hash"] = last_telemetry_hash_;
    checkedWrite(fp, re.dump() + "\n", telemetry_write_failures_);
    chained_events_this_run_++;
    run_end_declared_ = chained_events_this_run_;
}

int GovernanceEngine::verifyTelemetryChain(const std::string& filepath,
    const std::string& hmac_key) {
    std::ifstream ifs(filepath);
    if (!ifs.is_open()) {
        fprintf(stderr, "Error: cannot open %s\n", filepath.c_str());
        return 1;
    }

    // Per-run accounting: RunStart/RunEnd anchors + chained event counts.
    struct RunInfo {
        long long chained = 0;
        bool has_start = false;
        bool has_end = false;
        long long declared = -1;   // RunEnd's chained_events field
        int last_event_num = 0;    // file position of the run's last chained event
    };
    std::vector<std::pair<std::string, RunInfo>> runs;  // insertion-ordered
    auto runFor = [&runs](const std::string& rid) -> RunInfo& {
        for (auto& [id, info] : runs) {
            if (id == rid) return info;
        }
        runs.emplace_back(rid, RunInfo{});
        return runs.back().second;
    };

    std::string line;
    int event_num = 0;
    int chained_events = 0;
    long long unchained_lines = 0;
    std::string expected_prev_hash;
    std::string file_genesis;  // prev_hash of the file's first chained event
    int breaks = 0;
    int legacy_restarts = 0;
    int warnings = 0;

    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        event_num++;
        try {
            auto ev = nlohmann::json::parse(line);
            if (!ev.contains("hash") || !ev.contains("prev_hash")) {
                unchained_lines++;
                continue;
            }
            chained_events++;
            std::string stored_hash = ev["hash"].get<std::string>();
            std::string prev_hash = ev["prev_hash"].get<std::string>();
            if (file_genesis.empty()) file_genesis = prev_hash;
            if (!expected_prev_hash.empty() && prev_hash != expected_prev_hash) {
                // Pre-continuity writers restarted the chain at genesis every
                // run. The file's first prev_hash IS its genesis value, so a
                // mismatch that re-uses it is a legacy restart, not tampering.
                if (prev_hash == file_genesis) {
                    fprintf(stderr, "LEGACY RESTART at event %d: chain re-seeded "
                        "from genesis (pre-continuity writer)\n", event_num);
                    legacy_restarts++;
                    warnings++;
                } else {
                    fprintf(stderr, "BREAK at event %d: prev_hash mismatch\n"
                        "  expected: %s\n  got:      %s\n", event_num,
                        expected_prev_hash.c_str(), prev_hash.c_str());
                    breaks++;
                }
            }
            auto ev_copy = ev;
            ev_copy.erase("hash");
            std::string recomputed;
            if (!hmac_key.empty())
                recomputed = security::CryptoUtils::hmacSha256(ev_copy.dump(), hmac_key);
            else
                recomputed = security::CryptoUtils::sha256(ev_copy.dump());
            if (recomputed != stored_hash) {
                fprintf(stderr, "TAMPER at event %d: hash mismatch\n"
                    "  stored:     %s\n  recomputed: %s\n", event_num,
                    stored_hash.c_str(), recomputed.c_str());
                breaks++;
            }
            expected_prev_hash = stored_hash;

            // Run accounting
            std::string rid = ev.value("run_id", std::string());
            RunInfo& info = runFor(rid);
            info.chained++;
            info.last_event_num = event_num;
            std::string etype = ev.value("event_type", std::string());
            if (etype == "RunStart") info.has_start = true;
            if (etype == "RunEnd") {
                info.has_end = true;
                if (ev.contains("chained_events") &&
                    ev["chained_events"].is_number()) {
                    info.declared = ev["chained_events"].get<long long>();
                }
            }
        } catch (const nlohmann::json::parse_error&) {
            fprintf(stderr, "CORRUPT at event %d: invalid JSON\n", event_num);
            breaks++;
        }
    }

    // Per-run completeness. Interior deletions/truncations are already hard
    // BREAKs via prev_hash linkage; RunEnd accounting adds (a) count
    // cross-checks and (b) an advisory for runs that never closed — the final
    // run's missing RunEnd is indistinguishable from a crash, so it warns
    // rather than breaks.
    int last_chained_event = 0;
    for (const auto& [rid, info] : runs) {
        if (info.last_event_num > last_chained_event)
            last_chained_event = info.last_event_num;
    }
    for (const auto& [rid, info] : runs) {
        const char* rid_str = rid.empty() ? "(no run_id)" : rid.c_str();
        if (info.has_end && info.declared >= 0 && info.declared != info.chained) {
            fprintf(stderr, "BREAK in run %s: RunEnd declares %lld chained "
                "events but %lld observed\n", rid_str, info.declared, info.chained);
            breaks++;
        } else if (info.has_start && !info.has_end) {
            bool is_final = (info.last_event_num == last_chained_event);
            fprintf(stderr, "WARNING: run %s has no RunEnd (%s)\n", rid_str,
                is_final ? "final run — possibly crashed or still running"
                         : "possibly crashed; chain continuity above is authoritative");
            warnings++;
        }
    }

    if (chained_events == 0) {
        fprintf(stdout, "No chained events found in %s (%d lines read)\n",
                filepath.c_str(), event_num);
        return 0;
    }
    fprintf(stdout, "Scanned %d lines: %d chained events across %zu run(s), "
            "%lld unchained line(s)\n",
            event_num, chained_events, runs.size(), unchained_lines);
    if (breaks == 0) {
        fprintf(stdout, "Chain verified: %d chained events, no breaks"
                " (%d warning(s), %d legacy restart(s))\n",
                chained_events, warnings, legacy_restarts);
        return 0;
    } else {
        fprintf(stderr, "Chain BROKEN: %d break(s) in %d chained events (%d total lines)\n",
                breaks, chained_events, event_num);
        return 1;
    }
}

void GovernanceEngine::emitAttestation(const std::string& action_type,
    const std::string& agent_config, int turn, double pressure) {
    if (!rules().audit.provenance.enabled ||
        !rules().audit.provenance.record_attestations) return;

    nlohmann::json att;
    att["action"] = action_type;
    att["agent_config"] = agent_config;
    att["turn"] = turn;
    att["pressure"] = pressure;
    att["run_id"] = run_id_;

    // Sign the attestation if signing is enabled and key is available
    if (rules().audit.provenance.sign_records &&
        !rules().audit.provenance.signing_key.empty()) {
        try {
            std::ifstream kf(rules().audit.provenance.signing_key);
            if (kf.is_open()) {
                std::string pem((std::istreambuf_iterator<char>(kf)),
                                 std::istreambuf_iterator<char>());
                att["signature"] = security::CryptoUtils::ed25519Sign(att.dump(), pem);
                // Fingerprint the PUBLIC half. ed25519Fingerprint() reads with
                // PEM_read_bio_PUBKEY and deliberately refuses private keys, so
                // handing it the signing key silently produced "" — a valid
                // signature with no key to attribute it to.
                att["key_fingerprint"] = security::CryptoUtils::ed25519Fingerprint(
                    security::CryptoUtils::ed25519PublicFromPrivate(pem));
            }
        } catch (...) {}
    }

    logAuditEvent("execution_attestation", "agent." + action_type, att.dump(), "", 0);
}

void GovernanceEngine::emitRefusalAttestation(
    const std::string& rule_name,
    EnforcementLevel level,
    const std::string& enforcement_path,
    const std::string& violation_message) {

    // Always increment refusal counter (even without provenance — visible in dashboard/health)
    {
        std::lock_guard<std::mutex> lock(results_mutex_);
        pulse_.refusal_count++;
    }

    // Gate: telemetry write requires provenance attestations enabled
    if (!rules().telemetry_output.enabled ||
        rules().telemetry_output.output_file.empty() ||
        !rules().audit.provenance.enabled ||
        !rules().audit.provenance.record_attestations) return;

    // File open with flock (same RAII pattern as writeAgentTelemetry)
    auto fp_deleter = [](FILE* f) {
#ifndef _WIN32
        ::flock(fileno(f), LOCK_UN);
#endif
        fclose(f);
    };
    std::unique_ptr<FILE, decltype(fp_deleter)> fp(
        fopen(rules().telemetry_output.output_file.c_str(), "a"), fp_deleter);
    if (!fp) return;
#ifndef _WIN32
    if (::flock(fileno(fp.get()), LOCK_EX) != 0) {
        fprintf(stderr, "[governance] warning: telemetry file lock failed\n");
    }
#endif

    // ISO timestamp
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char ts_buf[32];
    std::strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);

    nlohmann::json ev;
    ev["run_id"] = run_id_;
    ev["agent_id"] = agent_id_;
    ev["event_type"] = "RefusalAttestation";
    ev["timestamp"] = std::string(ts_buf);
    ev["rule_name"] = rule_name;
    ev["level"] = levelToString(level);
    ev["enforcement_path"] = enforcement_path;
    ev["result"] = "refused";
    ev["binding_status"] = "non-binding";
    ev["execution_prevented"] = true;
    ev["file"] = error::ErrorSanitizer::sanitizeFilePaths(current_check_file_);
    ev["line"] = current_check_line_;
    // Cap violation message to prevent telemetry bloat
    ev["violation_message"] = violation_message.size() > 500
        ? violation_message.substr(0, 500) + "..."
        : violation_message;
    // Include rationale and decision trace from the CheckResult just pushed
    {
        std::lock_guard<std::mutex> lock(results_mutex_);
        if (!check_results_.empty()) {
            const auto& last = check_results_.back();
            if (!last.rationale.empty()) ev["rationale"] = last.rationale;
            if (!last.decision_trace.empty()) {
                ev["decision_trace"] = nlohmann::json::array();
                for (const auto& dt : last.decision_trace)
                    ev["decision_trace"].push_back(dt);
            }
            if (last.escalated) ev["escalated"] = true;
            if (!last.cwe_ids.empty()) {
                ev["cwe"] = nlohmann::json::array();
                for (const auto& c : last.cwe_ids) ev["cwe"].push_back(c);
            }
        }
    }

    // Ed25519 signature (if signing enabled + key available)
    if (rules().audit.provenance.sign_records &&
        !rules().audit.provenance.signing_key.empty()) {
        try {
            std::ifstream kf(rules().audit.provenance.signing_key);
            if (kf.is_open()) {
                std::string pem((std::istreambuf_iterator<char>(kf)),
                                 std::istreambuf_iterator<char>());
                ev["signature"] = security::CryptoUtils::ed25519Sign(ev.dump(), pem);
                // Public half — see emitAttestation() for why.
                ev["key_fingerprint"] = security::CryptoUtils::ed25519Fingerprint(
                    security::CryptoUtils::ed25519PublicFromPrivate(pem));
            }
        } catch (...) {}
    }

    // Tamper-evident hash chain (shared with writeTelemetry/writeAgentTelemetry)
    if (rules().telemetry_output.tamper_evidence.enabled) {
        std::lock_guard<std::mutex> hlock(telemetry_hash_mutex_);
        ev["prev_hash"] = chainPrevLocked(fp.get());
        last_telemetry_hash_ = computeHash(ev.dump(),
            rules().telemetry_output.tamper_evidence);
        ev["hash"] = last_telemetry_hash_;
        chained_events_this_run_++;
    }

    std::string line = ev.dump() + "\n";
    checkedWrite(fp.get(), line, telemetry_write_failures_);

    // Forward to SIEM/webhook if configured
    {
        std::shared_ptr<TelemetryForwarder> fwd;
        {
            std::lock_guard<std::mutex> lock(telemetry_fwd_mutex_);
            fwd = telemetry_forwarder_;
        }
        if (fwd) fwd->enqueue(ev.dump());
    }
}

void GovernanceEngine::emitOutputAdmissibilityAttestation(
    const std::string& agent_config, int turn,
    double coherence_score, double threshold) {

    // Gate: telemetry write requires provenance attestations enabled
    if (!rules().telemetry_output.enabled ||
        rules().telemetry_output.output_file.empty() ||
        !rules().audit.provenance.enabled ||
        !rules().audit.provenance.record_attestations) return;

    // File open with flock (same RAII pattern as emitRefusalAttestation)
    auto fp_deleter = [](FILE* f) {
#ifndef _WIN32
        ::flock(fileno(f), LOCK_UN);
#endif
        fclose(f);
    };
    std::unique_ptr<FILE, decltype(fp_deleter)> fp(
        fopen(rules().telemetry_output.output_file.c_str(), "a"), fp_deleter);
    if (!fp) return;
#ifndef _WIN32
    if (::flock(fileno(fp.get()), LOCK_EX) != 0) {
        fprintf(stderr, "[governance] warning: telemetry file lock failed\n");
    }
#endif

    // ISO timestamp
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char ts_buf[32];
    std::strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);

    nlohmann::json ev;
    ev["run_id"] = run_id_;
    ev["agent_id"] = agent_id_;
    ev["event_type"] = "OutputAdmissibilityAttestation";
    ev["timestamp"] = std::string(ts_buf);
    ev["action"] = "attest";
    ev["agent_config"] = agent_config;
    ev["turn"] = turn;
    ev["coherence_score"] = coherence_score;
    ev["threshold"] = threshold;
    ev["admissible"] = false;
    ev["binding_status"] = "attested-inadmissible";

    // Ed25519 signature (if signing enabled + key available)
    if (rules().audit.provenance.sign_records &&
        !rules().audit.provenance.signing_key.empty()) {
        try {
            std::ifstream kf(rules().audit.provenance.signing_key);
            if (kf.is_open()) {
                std::string pem((std::istreambuf_iterator<char>(kf)),
                                 std::istreambuf_iterator<char>());
                ev["signature"] = security::CryptoUtils::ed25519Sign(ev.dump(), pem);
                // Public half — see emitAttestation() for why.
                ev["key_fingerprint"] = security::CryptoUtils::ed25519Fingerprint(
                    security::CryptoUtils::ed25519PublicFromPrivate(pem));
            }
        } catch (...) {}
    }

    // Tamper-evident hash chain
    if (rules().telemetry_output.tamper_evidence.enabled) {
        std::lock_guard<std::mutex> hlock(telemetry_hash_mutex_);
        ev["prev_hash"] = chainPrevLocked(fp.get());
        last_telemetry_hash_ = computeHash(ev.dump(),
            rules().telemetry_output.tamper_evidence);
        ev["hash"] = last_telemetry_hash_;
        chained_events_this_run_++;
    }

    std::string line = ev.dump() + "\n";
    checkedWrite(fp.get(), line, telemetry_write_failures_);

    // Forward to SIEM/webhook if configured
    {
        std::shared_ptr<TelemetryForwarder> fwd;
        {
            std::lock_guard<std::mutex> lock(telemetry_fwd_mutex_);
            fwd = telemetry_forwarder_;
        }
        if (fwd) fwd->enqueue(ev.dump());
    }
}

// --- Hooks ---

void GovernanceEngine::fireHook(const HookConfig& hook,
                                 const std::unordered_map<std::string, std::string>& vars) {
    if (hook.command.empty()) return;

    try {
        // Build args with variable substitution (no shell escaping — execv is shell-free)
        std::vector<std::string> expanded_args;
        expanded_args.push_back(hook.command);  // argv[0]
        for (const auto& arg : hook.args) {
            std::string expanded = arg;
            for (const auto& [key, val] : vars) {
                std::string placeholder = "${" + key + "}";
                size_t pos = expanded.find(placeholder);
                while (pos != std::string::npos) {
                    expanded.replace(pos, placeholder.size(), val);
                    pos = expanded.find(placeholder, pos + val.size());
                }
            }
            expanded_args.push_back(std::move(expanded));
        }

#ifndef _WIN32
        // POSIX: fork + execv with per-call wall-clock timeout (thread-safe)

        // Build argv BEFORE fork — push_back may malloc, deadlocking
        // if another thread holds the allocator lock at fork time.
        std::vector<const char*> argv;
        for (const auto& a : expanded_args) argv.push_back(a.c_str());
        argv.push_back(nullptr);

        // Resolve command path before fork — execvp does PATH search which
        // may malloc (not async-signal-safe). execv is async-signal-safe.
        std::string resolved_cmd = hook.command;
        if (hook.command.find('/') == std::string::npos) {
            const char* path_env = getenv("PATH");
            if (path_env) {
                std::istringstream path_stream(path_env);
                std::string dir;
                while (std::getline(path_stream, dir, ':')) {
                    std::string candidate = dir + "/" + hook.command;
                    if (access(candidate.c_str(), X_OK) == 0) {
                        resolved_cmd = candidate;
                        break;
                    }
                }
            }
        }

        pid_t pid = fork();
        if (pid < 0) return;

        if (pid == 0) {
            // === CHILD (only async-signal-safe calls below) ===

            // V-SC-006: scrub governance keys from child environment.
            // unsetenv is technically not async-signal-safe, but these 3 calls
            // on constant strings have near-zero practical risk (no allocation in glibc).
            if (!hook.inherit_governance_keys) {
                unsetenv("NAAB_GOVERN_KEY");
                unsetenv("NAAB_LOCK_KEY");
                unsetenv("NAAB_SIGNING_KEY");
            }

            // Redirect stdout+stderr to /dev/null
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }

            // Per-call timeout via alarm (child process — no thread conflicts)
            if (hook.timeout > 0) alarm(static_cast<unsigned>(hook.timeout));

            execv(resolved_cmd.c_str(), const_cast<char* const*>(argv.data()));
            _exit(127);  // exec failed
        }

        // Parent: poll waitpid with wall-clock timeout
        auto start = std::chrono::steady_clock::now();
        int timeout_ms = (hook.timeout > 0 ? hook.timeout : 5) * 1000;
        int status = 0;
        bool timed_out = false;

        while (true) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) break;
            if (w == -1) {
                if (errno == EINTR) continue;
                // ECHILD or other fatal error — kill and reap to prevent orphan/zombie
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                status = -1;
                break;
            }

            auto elapsed = std::chrono::steady_clock::now() - start;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > timeout_ms) {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                timed_out = true;
                break;
            }
            usleep(50000);  // 50ms poll
        }

        // Diagnostic to stderr (generic — never include command or args)
        if (timed_out) {
            fprintf(stderr, "[governance] Hook killed (timeout)\n");
        } else if (WIFSIGNALED(status)) {
            // Child killed by signal (e.g. SIGALRM from per-call timeout)
            fprintf(stderr, "[governance] Hook killed (timeout)\n");
        } else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            fprintf(stderr, "[governance] Hook exited %d\n", WEXITSTATUS(status));
        }
#else
        // Windows: CreateProcessA with per-call timeout (no shell — no cmd.exe injection)
        auto quoteArg = [](const std::string& s) -> std::string {
            bool needs_quote = s.empty() ||
                               s.find(' ') != std::string::npos ||
                               s.find('"') != std::string::npos ||
                               s.find('\t') != std::string::npos;
            if (!needs_quote) return s;
            // Windows CommandLineToArgvW quoting: a run of N backslashes
            // followed by a quote needs 2N+1 backslashes; a trailing run of N
            // backslashes before the closing quote needs 2N. Escaping only the
            // quote (as the old code did) mangled args ending in '\' — the
            // trailing backslash would escape the closing quote and merge args.
            std::string result = "\"";
            size_t backslashes = 0;
            for (char c : s) {
                if (c == '\\') {
                    ++backslashes;
                } else if (c == '"') {
                    result.append(backslashes * 2 + 1, '\\');
                    result += '"';
                    backslashes = 0;
                } else {
                    result.append(backslashes, '\\');
                    result += c;
                    backslashes = 0;
                }
            }
            result.append(backslashes * 2, '\\');  // trailing run, doubled
            result += '"';
            return result;
        };

        std::string cmdline = quoteArg(hook.command);
        for (size_t i = 1; i < expanded_args.size(); ++i) {
            cmdline += ' ';
            cmdline += quoteArg(expanded_args[i]);
        }

        // V-SC-006: Build filtered environment block to scrub governance keys
        std::vector<char> env_block;
        LPVOID env_ptr = nullptr;
        if (!hook.inherit_governance_keys) {
            LPCH cur_env = ::GetEnvironmentStringsA();
            if (cur_env) {
                for (LPCH p = cur_env; *p; ) {
                    std::string entry(p);
                    p += entry.size() + 1;
                    size_t eq = entry.find('=');
                    if (eq != std::string::npos) {
                        std::string key = entry.substr(0, eq);
                        if (naab::runtime::shouldScrubEnvVar(key)) continue;
                    }
                    for (char c : entry) env_block.push_back(c);
                    env_block.push_back('\0');
                }
                ::FreeEnvironmentStringsA(cur_env);
            }
            env_block.push_back('\0');  // double-null terminator
            env_ptr = env_block.data();
        }

        SECURITY_ATTRIBUTES sa = {};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        HANDLE hNul = ::CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE,
                                    &sa, OPEN_EXISTING, 0, nullptr);

        STARTUPINFOA si = {};
        si.cb = sizeof(si);
        if (hNul != INVALID_HANDLE_VALUE) {
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
            si.hStdOutput = hNul;
            si.hStdError = hNul;
        }

        PROCESS_INFORMATION pi = {};
        std::vector<char> cmdline_buf(cmdline.begin(), cmdline.end());
        cmdline_buf.push_back('\0');

        BOOL ok = ::CreateProcessA(nullptr, cmdline_buf.data(),
                                   nullptr, nullptr,
                                   hNul != INVALID_HANDLE_VALUE,
                                   0, env_ptr, nullptr, &si, &pi);
        if (hNul != INVALID_HANDLE_VALUE) ::CloseHandle(hNul);

        if (ok) {
            DWORD timeout_ms = static_cast<DWORD>(
                (hook.timeout > 0 ? hook.timeout : 5) * 1000);
            DWORD waitRc = ::WaitForSingleObject(pi.hProcess, timeout_ms);
            if (waitRc == WAIT_TIMEOUT) {
                ::TerminateProcess(pi.hProcess, 1);
                ::WaitForSingleObject(pi.hProcess, 1000);
                fprintf(stderr, "[governance] Hook killed (timeout)\n");
            } else {
                DWORD exit_code = 0;
                if (::GetExitCodeProcess(pi.hProcess, &exit_code) && exit_code != 0) {
                    fprintf(stderr, "[governance] Hook exited %lu\n", exit_code);
                }
            }
            ::CloseHandle(pi.hThread);
            ::CloseHandle(pi.hProcess);
        }
#endif
    } catch (...) {
        // Hook failures must NEVER mask governance enforcement
    }
}

// --- Report Helpers ---

static std::string xmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out += c;
        }
    }
    return out;
}

static std::string csvEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

static std::string htmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            default:   out += c;
        }
    }
    return out;
}

// --- Report Generation ---

std::string GovernanceEngine::generateJsonReport() const {
    nlohmann::json report;
    report["version"] = "4.0";
    report["mode"] = rules().mode == GovernanceMode::ENFORCE ? "enforce" : (rules().mode == GovernanceMode::AUDIT ? "audit" : "off");

    int total = 0, passed = 0, failed_hard = 0, failed_soft = 0, advisories = 0;
    for (const auto& r : check_results_) {
        total++;
        if (r.passed) { passed++; }
        else if (r.level == EnforcementLevel::HARD) { failed_hard++; }
        else if (r.level == EnforcementLevel::SOFT) { failed_soft++; }
        else { advisories++; }
    }
    report["summary"]["total"] = total;
    report["summary"]["passed"] = passed;
    report["summary"]["failed_hard"] = failed_hard;
    report["summary"]["failed_soft"] = failed_soft;
    report["summary"]["advisories"] = advisories;

    if (rules().scoring.enabled) {
        report["summary"]["cumulative_risk_score"] = cumulative_score_;
        report["summary"]["risk_zone"] = cumulative_score_ >= rules().scoring.red_threshold ? "red" :
                                         cumulative_score_ >= rules().scoring.yellow_threshold ? "yellow" : "green";
        nlohmann::json breakdown = nlohmann::json::object();
        for (const auto& [rule, score] : score_contributions_) {
            breakdown[rule] = score;
        }
        report["summary"]["score_breakdown"] = breakdown;
    }

    report["results"] = nlohmann::json::array();
    for (const auto& r : check_results_) {
        nlohmann::json entry;
        entry["rule"] = r.rule_name;
        entry["level"] = levelToString(r.level);
        entry["passed"] = r.passed;
        if (!r.message.empty()) entry["message"] = r.message;
        entry["category"] = r.category;
        entry["severity"] = r.severity;
        entry["file"] = r.file;
        entry["line"] = r.line;
        if (!r.cwe_ids.empty()) {
            entry["cwe"] = nlohmann::json::array();
            for (const auto& c : r.cwe_ids) entry["cwe"].push_back(c);
        }
        if (!r.owasp_ids.empty()) {
            entry["owasp"] = nlohmann::json::array();
            for (const auto& o : r.owasp_ids) entry["owasp"].push_back(o);
        }
        if (!r.rationale.empty()) entry["rationale"] = r.rationale;
        if (!r.decision_trace.empty()) {
            entry["decision_trace"] = nlohmann::json::array();
            for (const auto& step : r.decision_trace) entry["decision_trace"].push_back(step);
        }
        if (!r.explanation.empty()) entry["explanation"] = r.explanation;
        report["results"].push_back(entry);
    }
    return report.dump(2);
}

std::string GovernanceEngine::generateSarifReport() const {
    nlohmann::json sarif;
    sarif["version"] = "2.1.0";
    sarif["$schema"] = "https://raw.githubusercontent.com/oasis-tcs/sarif-spec/master/Schemata/sarif-schema-2.1.0.json";

    nlohmann::json run;

    // Tool metadata
    run["tool"]["driver"]["name"] = "NAAb Governance Engine";
    run["tool"]["driver"]["version"] = "4.0";
    run["tool"]["driver"]["semanticVersion"] = "4.0.0";
    run["tool"]["driver"]["informationUri"] = "https://github.com/nickvdyck/naab-lang";
    run["tool"]["driver"]["organization"] = "NAAb";

    // Build unique rules array from all check results
    std::map<std::string, size_t> rule_index_map;
    auto& rules_arr = run["tool"]["driver"]["rules"] = nlohmann::json::array();

    for (const auto& r : check_results_) {
        if (rule_index_map.count(r.rule_name)) continue;
        size_t idx = rules_arr.size();
        rule_index_map[r.rule_name] = idx;

        nlohmann::json rule;
        rule["id"] = r.rule_name;
        rule["name"] = r.rule_name;

        // Human-readable description from rule name
        std::string short_desc = r.rule_name;
        for (auto& ch : short_desc) {
            if (ch == '_') ch = ' ';
            else if (ch == '.') ch = ' ';
        }
        rule["shortDescription"]["text"] = short_desc;

        // Default configuration level
        if (r.level == EnforcementLevel::ADVISORY)
            rule["defaultConfiguration"]["level"] = "warning";
        else
            rule["defaultConfiguration"]["level"] = "error";

        if (!r.category.empty()) {
            rule["properties"]["category"] = r.category;
        }

        // Feature 3: CWE/OWASP tags in SARIF
        auto [cwes, owasps] = lookupCweOwasp(r.rule_name);
        if (!cwes.empty()) {
            rule["properties"]["cwe"] = nlohmann::json::array();
            for (const auto& cwe : cwes) rule["properties"]["cwe"].push_back(cwe);
            std::string cwe_num = cwes[0].substr(4); // "CWE-89" -> "89"
            rule["helpUri"] = "https://cwe.mitre.org/data/definitions/" + cwe_num + ".html";
        }
        if (!owasps.empty()) {
            rule["properties"]["owasp"] = nlohmann::json::array();
            for (const auto& o : owasps) rule["properties"]["owasp"].push_back(o);
        }

        rules_arr.push_back(rule);
    }

    // Results (failures only)
    auto& results_arr = run["results"] = nlohmann::json::array();

    for (const auto& r : check_results_) {
        if (r.passed) continue;

        nlohmann::json result;
        result["ruleId"] = r.rule_name;
        result["kind"] = "fail";  // SARIF §3.27.9 — required by GitHub code scanning

        auto it = rule_index_map.find(r.rule_name);
        if (it != rule_index_map.end()) {
            result["ruleIndex"] = static_cast<int>(it->second);
        }

        // SARIF level mapping
        if (r.level == EnforcementLevel::ADVISORY)
            result["level"] = "warning";
        else
            result["level"] = "error";

        // Full message — prefer explanation (human-friendly) when available
        if (!r.explanation.empty()) {
            result["message"]["text"] = r.explanation;
        } else {
            result["message"]["text"] = r.message.empty() ? r.rule_name : r.message;
        }

        // Physical location with file and line.
        //
        // GitHub code scanning treats results[].locations[] as required — a
        // result with no location cannot be placed on the code. Config-level
        // findings (contradictions, capability and restriction rules) carry
        // neither a file nor a line, so they used to emit no `locations` key
        // at all and the whole document went up locationless.
        //
        // Fall back to govern.json, which is the artifact such a finding is
        // actually about: a contradiction between two governance keys belongs
        // on the config that declares them, not on an arbitrary source file.
        {
            nlohmann::json location;
            nlohmann::json physical;

            if (!r.file.empty()) {
                std::string uri = r.file;
                if (uri.size() >= 2 && uri[0] == '.' && uri[1] == '/') uri = uri.substr(2);
                physical["artifactLocation"]["uri"] = uri;
                physical["artifactLocation"]["uriBaseId"] = "%SRCROOT%";
            } else {
                physical["artifactLocation"]["uri"] = "govern.json";
                physical["artifactLocation"]["uriBaseId"] = "%SRCROOT%";
            }

            if (r.line > 0) {
                physical["region"]["startLine"] = r.line;
                physical["region"]["startColumn"] = 1;
            }

            location["physicalLocation"] = physical;
            result["locations"] = nlohmann::json::array({location});
        }

        // Stable fingerprint for cross-scan deduplication (GitHub code scanning)
        {
            std::string fp_input = r.rule_name + "|" + r.file + "|"
                + std::to_string(r.line);
            result["fingerprints"]["primaryLocationLineHash"]
                = security::CryptoUtils::sha256(fp_input);
        }

        // Properties
        nlohmann::json props;
        if (!r.category.empty()) props["category"] = r.category;
        if (!r.severity.empty()) props["severity"] = r.severity;
        props["enforcement"] = levelToString(r.level);
        if (!r.rationale.empty()) props["rationale"] = r.rationale;
        if (!r.decision_trace.empty()) {
            props["decisionTrace"] = nlohmann::json::array();
            for (const auto& step : r.decision_trace) props["decisionTrace"].push_back(step);
        }
        result["properties"] = props;

        // SARIF codeFlows for taint lineage visualization
        if (r.rule_name.find("taint_tracking") != std::string::npos) {
            for (const auto& trace : r.decision_trace) {
                if (trace.find("taint origin:") != std::string::npos) {
                    nlohmann::json codeFlow, threadFlow;
                    nlohmann::json source_loc, sink_loc;

                    // Extract source description (always present after "taint origin: ")
                    std::string source_desc = trace.substr(14);
                    auto at_pos = trace.find(" at ");
                    if (at_pos != std::string::npos) {
                        source_desc = trace.substr(14, at_pos - 14);
                        std::string loc_str = trace.substr(at_pos + 4);
                        auto colon = loc_str.rfind(':');
                        if (colon != std::string::npos) {
                            source_loc["location"]["physicalLocation"]["artifactLocation"]["uri"]
                                = loc_str.substr(0, colon);
                            try {
                                source_loc["location"]["physicalLocation"]["region"]["startLine"]
                                    = std::stoi(loc_str.substr(colon + 1));
                            } catch (...) {}
                        }
                    }
                    // Always set message — SARIF schema requires location.message
                    source_loc["location"]["message"]["text"] = "Taint source: " + source_desc;

                    if (!r.file.empty()) {
                        sink_loc["location"]["physicalLocation"]["artifactLocation"]["uri"] = r.file;
                        sink_loc["location"]["physicalLocation"]["region"]["startLine"] = r.line;
                    }
                    sink_loc["location"]["message"]["text"] = "Taint sink";

                    threadFlow["locations"] = nlohmann::json::array({source_loc, sink_loc});
                    codeFlow["threadFlows"] = nlohmann::json::array({threadFlow});
                    result["codeFlows"] = nlohmann::json::array({codeFlow});
                    break;
                }
            }
        }

        results_arr.push_back(result);
    }

    // Invocations (required by GitHub code scanning)
    bool any_hard_failures = false;
    for (const auto& r : check_results_) {
        if (!r.passed && r.level != EnforcementLevel::ADVISORY) {
            any_hard_failures = true;
            break;
        }
    }
    run["invocations"] = nlohmann::json::array({
        {{"executionSuccessful", !any_hard_failures}}
    });

    sarif["runs"] = nlohmann::json::array({run});
    return sarif.dump(2);
}

std::string GovernanceEngine::generateJunitReport() const {
    std::ostringstream oss;
    int total = static_cast<int>(check_results_.size());
    int failures = 0, warnings = 0;
    for (const auto& r : check_results_) {
        if (!r.passed) {
            if (r.level == EnforcementLevel::ADVISORY) warnings++;
            else failures++;
        }
    }

    // Get current timestamp
    auto now = std::time(nullptr);
    char ts_buf[64];
    struct tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    std::strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);

    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    oss << "<testsuite name=\"NAAb Governance\" tests=\"" << total
        << "\" failures=\"" << failures
        << "\" errors=\"0\" skipped=\"0\""
        << " warnings=\"" << warnings << "\""
        << " timestamp=\"" << ts_buf << "\""
        << " time=\"0\">\n";

    for (const auto& r : check_results_) {
        std::string classname = r.category.empty()
            ? "governance" : "governance." + r.category;

        oss << "  <testcase name=\"" << xmlEscape(r.rule_name)
            << "\" classname=\"" << xmlEscape(classname) << "\" time=\"0\"";

        // Add file/line as attributes (widely supported by CI tools)
        if (!r.file.empty()) oss << " file=\"" << xmlEscape(r.file) << "\"";
        if (r.line > 0) oss << " line=\"" << r.line << "\"";

        if (r.passed) {
            oss << "/>\n";
        } else {
            oss << ">\n";
            std::string first_line = r.message.empty() ? r.rule_name
                : r.message.substr(0, r.message.find('\n'));
            oss << "    <failure message=\"" << xmlEscape(first_line)
                << "\" type=\"" << xmlEscape(levelToString(r.level)) << "\"";
            if (!r.rationale.empty()) oss << " rationale=\"" << xmlEscape(r.rationale) << "\"";
            oss << ">"
                << xmlEscape(r.message.empty() ? r.rule_name : r.message);
            if (!r.explanation.empty()) {
                oss << "\n--- Explanation ---\n" << xmlEscape(r.explanation) << "\n";
            }
            if (!r.decision_trace.empty()) {
                oss << "\n--- Decision Trace ---\n";
                for (const auto& step : r.decision_trace) oss << xmlEscape(step) << "\n";
            }
            oss << "</failure>\n";
            oss << "  </testcase>\n";
        }
    }

    oss << "</testsuite>\n";
    return oss.str();
}

std::string GovernanceEngine::generateCsvReport() const {
    std::ostringstream oss;
    oss << "rule,level,passed,message,category,severity,file,line,cwe,owasp,rationale\n";
    for (const auto& r : check_results_) {
        // Build semicolon-joined CWE and OWASP strings
        std::string cwe_str, owasp_str;
        for (size_t i = 0; i < r.cwe_ids.size(); i++) {
            if (i > 0) cwe_str += ";";
            cwe_str += r.cwe_ids[i];
        }
        for (size_t i = 0; i < r.owasp_ids.size(); i++) {
            if (i > 0) owasp_str += ";";
            owasp_str += r.owasp_ids[i];
        }
        oss << "\"" << csvEscape(r.rule_name) << "\","
            << levelToString(r.level) << ","
            << (r.passed ? "true" : "false") << ","
            << "\"" << csvEscape(r.message) << "\","
            << "\"" << csvEscape(r.category) << "\","
            << "\"" << csvEscape(r.severity) << "\","
            << "\"" << csvEscape(r.file) << "\","
            << r.line << ","
            << "\"" << csvEscape(cwe_str) << "\","
            << "\"" << csvEscape(owasp_str) << "\","
            << "\"" << csvEscape(r.rationale) << "\"\n";
    }
    return oss.str();
}

std::string GovernanceEngine::generateHtmlReport() const {
    std::ostringstream oss;
    int total = 0, passed = 0, failed = 0, warnings = 0;
    for (const auto& r : check_results_) {
        total++;
        if (r.passed) passed++;
        else if (r.level == EnforcementLevel::ADVISORY) warnings++;
        else failed++;
    }

    oss << "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">\n";
    oss << "<title>NAAb Governance Report</title>\n";
    oss << "<style>\n"
        << "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; margin: 2em; }\n"
        << "table { border-collapse: collapse; width: 100%; }\n"
        << "th, td { border: 1px solid #ddd; padding: 8px; text-align: left; }\n"
        << "th { background: #f5f5f5; }\n"
        << "tr:nth-child(even) { background: #fafafa; }\n"
        << ".pass { color: #2e7d32; } .fail-hard { color: #c62828; }\n"
        << ".fail-soft { color: #e65100; } .warning { color: #f9a825; }\n"
        << ".summary { display: flex; gap: 2em; margin: 1em 0; }\n"
        << ".stat { padding: 1em; border-radius: 8px; background: #f5f5f5; }\n"
        << ".msg { max-width: 500px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }\n"
        << ".msg:hover { white-space: normal; }\n"
        << "</style></head><body>\n";

    oss << "<h1>NAAb Governance Report</h1>\n";
    oss << "<div class=\"summary\">\n"
        << "  <div class=\"stat\"><strong>" << total << "</strong> checks</div>\n"
        << "  <div class=\"stat pass\"><strong>" << passed << "</strong> passed</div>\n"
        << "  <div class=\"stat fail-hard\"><strong>" << failed << "</strong> failed</div>\n"
        << "  <div class=\"stat warning\"><strong>" << warnings << "</strong> warnings</div>\n"
        << "</div>\n";

    if (failed > 0 || warnings > 0) {
        oss << "<h2>Issues</h2>\n<table>\n";
        oss << "<tr><th>Rule</th><th>Level</th><th>Category</th><th>File</th><th>Line</th><th>Message</th><th>Rationale</th><th>Trace</th></tr>\n";
        for (const auto& r : check_results_) {
            if (r.passed) continue;
            std::string css_class = (r.level == EnforcementLevel::HARD) ? "fail-hard" :
                                    (r.level == EnforcementLevel::SOFT) ? "fail-soft" : "warning";
            std::string first_line = r.message.empty() ? r.rule_name
                : r.message.substr(0, r.message.find('\n'));
            oss << "<tr class=\"" << css_class << "\">"
                << "<td>" << htmlEscape(r.rule_name) << "</td>"
                << "<td>" << htmlEscape(levelToString(r.level)) << "</td>"
                << "<td>" << htmlEscape(r.category) << "</td>"
                << "<td>" << htmlEscape(r.file) << "</td>"
                << "<td>" << r.line << "</td>"
                << "<td class=\"msg\" title=\"" << htmlEscape(r.message) << "\">"
                << htmlEscape(first_line) << "</td>"
                << "<td>" << htmlEscape(r.rationale) << "</td>"
                << "<td>";
            if (!r.decision_trace.empty()) {
                oss << "<details><summary>" << r.decision_trace.size() << " steps</summary><ol>";
                for (const auto& step : r.decision_trace) oss << "<li>" << htmlEscape(step) << "</li>";
                oss << "</ol></details>";
            }
            oss << "</td></tr>\n";
        }
        oss << "</table>\n";
    }

    oss << "<h2>All Checks (" << total << ")</h2>\n<table>\n";
    oss << "<tr><th>Rule</th><th>Level</th><th>Status</th><th>Category</th></tr>\n";
    for (const auto& r : check_results_) {
        std::string status_class = r.passed ? "pass" :
            (r.level == EnforcementLevel::ADVISORY ? "warning" : "fail-hard");
        oss << "<tr><td>" << htmlEscape(r.rule_name) << "</td>"
            << "<td>" << htmlEscape(levelToString(r.level)) << "</td>"
            << "<td class=\"" << status_class << "\">" << (r.passed ? "PASS" : "FAIL") << "</td>"
            << "<td>" << htmlEscape(r.category) << "</td></tr>\n";
    }
    oss << "</table>\n</body></html>\n";
    return oss.str();
}

void GovernanceEngine::writeReports() const {
    auto writeFile = [](const std::string& path, const std::string& content) {
        if (path.empty()) return;
        std::ofstream ofs(path);
        if (ofs.is_open()) ofs << content;
    };
    writeFile(rules().output.file_output.report_json, generateJsonReport());
    writeFile(rules().output.file_output.report_sarif, generateSarifReport());
    writeFile(rules().output.file_output.report_junit, generateJunitReport());
    writeFile(rules().output.file_output.report_csv, generateCsvReport());
    writeFile(rules().output.file_output.report_html, generateHtmlReport());
    writeTelemetry();
}

// --- Telemetry JSONL Output ---
void GovernanceEngine::writeTelemetry() const {
    if (!rules().telemetry_output.enabled || rules().telemetry_output.output_file.empty()) return;

    // Use C FILE* + flock for atomic multi-process writes.
    // RAII wrapper ensures fclose+unlock even if json serialization throws.
    auto fp_deleter = [](FILE* f) {
#ifndef _WIN32
        ::flock(fileno(f), LOCK_UN);
#endif
        fclose(f);
    };
    std::unique_ptr<FILE, decltype(fp_deleter)> fp(
        fopen(rules().telemetry_output.output_file.c_str(), "a"), fp_deleter);
    if (!fp) {
        fprintf(stderr, "[governance] Warning: Could not open telemetry file: %s\n",
                rules().telemetry_output.output_file.c_str());
        return;
    }
#ifndef _WIN32
    if (::flock(fileno(fp.get()), LOCK_EX) != 0) {
        fprintf(stderr, "[governance] warning: telemetry file lock failed\n");
    }
#endif

    // ISO timestamp
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char ts_buf[32];
    std::strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);
    std::string timestamp(ts_buf);

    // Fix 4B: opt-in deduplication of governance check entries. The key set is
    // a member, not a local, so dedup still holds across the multiple
    // writeTelemetry() calls a single run makes.
    size_t dedup_count = 0;
    size_t events_written = 0;

    // Resume where the previous dump stopped. check_results_ is never cleared,
    // so iterating from the start re-emits every earlier result — a clean
    // execute() followed by main.cpp's quality-gate exit duplicated the whole
    // set (45-48 records on a live run), inflating every count derived from
    // this file. Results only ever accumulate, so an index is sufficient.
    const size_t results_total = check_results_.size();
    for (size_t ri = telemetry_results_dumped_; ri < results_total; ++ri) {
        const auto& r = check_results_[ri];
        // Fix 4B: skip duplicate (rule_name, file, line) entries
        if (rules().telemetry_output.deduplicate_checks) {
            std::string key = r.rule_name + "|" + r.file + "|" + std::to_string(r.line);
            if (!telemetry_dedup_seen_.insert(key).second) { dedup_count++; continue; }
        }

        nlohmann::json ev;
        ev["run_id"] = run_id_;
        ev["agent_id"] = agent_id_;
        ev["event_type"] = r.passed ? "GovernanceCheck" : "RuleViolation";
        ev["rule_name"] = r.rule_name;
        ev["result"] = r.passed ? "pass" : "block";
        ev["message"] = r.message.empty()
            ? (r.passed ? "Check passed: " + r.rule_name : "Check failed: " + r.rule_name)
            : r.message;
        ev["timestamp"] = timestamp;
        ev["file"] = error::ErrorSanitizer::sanitizeFilePaths(r.file);
        ev["line"] = r.line;
        ev["category"] = r.category;
        ev["severity"] = r.severity;
        ev["level"] = levelToString(r.level);
        // Scoring enrichment: include weight and escalation data for advisory violations
        if (rules().scoring.enabled && !r.passed && r.level == EnforcementLevel::ADVISORY) {
            auto sit = score_contributions_.find(r.rule_name);
            if (sit != score_contributions_.end()) {
                ev["score_contribution"] = sit->second;
            }
            ev["escalated"] = r.escalated;
        }
        if (!r.cwe_ids.empty()) {
            ev["cwe"] = nlohmann::json::array();
            for (const auto& c : r.cwe_ids) ev["cwe"].push_back(c);
        }
        if (!r.owasp_ids.empty()) {
            ev["owasp"] = nlohmann::json::array();
            for (const auto& o : r.owasp_ids) ev["owasp"].push_back(o);
        }
        // Tamper-evident hash chain for telemetry entries
        if (rules().telemetry_output.tamper_evidence.enabled) {
            std::lock_guard<std::mutex> hlock(telemetry_hash_mutex_);
            ev["prev_hash"] = chainPrevLocked(fp.get());
            last_telemetry_hash_ = computeHash(ev.dump(),
                rules().telemetry_output.tamper_evidence);
            ev["hash"] = last_telemetry_hash_;
            chained_events_this_run_++;
        }

        std::string line = ev.dump() + "\n";
        if (checkedWrite(fp.get(), line, telemetry_write_failures_))
            events_written++;

        // C2: local shared_ptr copy prevents use-after-free during reload/destruction
        {
            std::shared_ptr<TelemetryForwarder> fwd;
            {
                std::lock_guard<std::mutex> lock(telemetry_fwd_mutex_);
                fwd = telemetry_forwarder_;
            }
            if (fwd) fwd->enqueue(ev.dump());
        }
    }

    // Fix 4B: emit summary event recording dedup stats
    if (rules().telemetry_output.deduplicate_checks && dedup_count > 0) {
        nlohmann::json summary;
        summary["run_id"] = run_id_;
        summary["event_type"] = "GovernanceCheckSummary";
        summary["timestamp"] = timestamp;
        summary["total_checks"] = check_results_.size();
        summary["unique_sites"] = telemetry_dedup_seen_.size();
        summary["deduplicated"] = dedup_count;
        if (rules().telemetry_output.tamper_evidence.enabled) {
            std::lock_guard<std::mutex> hlock(telemetry_hash_mutex_);
            summary["prev_hash"] = chainPrevLocked(fp.get());
            last_telemetry_hash_ = computeHash(summary.dump(),
                rules().telemetry_output.tamper_evidence);
            summary["hash"] = last_telemetry_hash_;
            chained_events_this_run_++;
        }
        std::string sline = summary.dump() + "\n";
        if (checkedWrite(fp.get(), sline, telemetry_write_failures_))
            events_written++;
    }

    // Everything above this point has now been committed to the file.
    telemetry_results_dumped_ = results_total;

    // Fix 5B: emit end-of-run health warnings (catches instrumentation failures
    // that per-turn checkGovernanceHealth() missed due to check_after_turns gate)
    emitEndOfRunHealthWarnings(fp.get(), timestamp);

    // End-of-run scoring snapshot for cross-run analysis. Re-emitted only when
    // the score actually moved since the last one: a run that writes reports
    // more than once (clean execute() then a quality-gate exit) otherwise files
    // an identical snapshot each time.
    if (rules().scoring.enabled && cumulative_score_ > 0 &&
        cumulative_score_ != scoring_snapshot_last_score_) {
        scoring_snapshot_last_score_ = cumulative_score_;
        nlohmann::json snap;
        snap["run_id"] = run_id_;
        snap["event_type"] = "ScoringSnapshot";
        snap["timestamp"] = timestamp;
        snap["cumulative_score"] = cumulative_score_;
        snap["risk_zone"] = cumulative_score_ >= rules().scoring.red_threshold ? "RED" :
                            cumulative_score_ >= rules().scoring.yellow_threshold ? "YELLOW" : "GREEN";
        nlohmann::json breakdown = nlohmann::json::object();
        for (const auto& [rule, total] : score_contributions_) {
            breakdown[rule] = total;
        }
        snap["score_breakdown"] = breakdown;
        if (rules().telemetry_output.tamper_evidence.enabled) {
            std::lock_guard<std::mutex> hlock(telemetry_hash_mutex_);
            snap["prev_hash"] = chainPrevLocked(fp.get());
            last_telemetry_hash_ = computeHash(snap.dump(),
                rules().telemetry_output.tamper_evidence);
            snap["hash"] = last_telemetry_hash_;
            chained_events_this_run_++;
        }
        std::string sline = snap.dump() + "\n";
        if (checkedWrite(fp.get(), sline, telemetry_write_failures_))
            events_written++;
        {
            std::shared_ptr<TelemetryForwarder> fwd;
            { std::lock_guard<std::mutex> lock(telemetry_fwd_mutex_); fwd = telemetry_forwarder_; }
            if (fwd) fwd->enqueue(snap.dump());
        }
    }

    // RunEnd anchor: declares this run's chained event count so the verifier
    // can detect trailing truncation. Emitted once, after all other events.
    emitRunEnd(fp.get(), timestamp);

    // fp_deleter handles flock(LOCK_UN) + fclose automatically.

    if (events_written > 0) {
        int failures = telemetry_write_failures_.load(std::memory_order_relaxed);
        if (failures > 0)
            fprintf(stderr, "[governance] Telemetry: %zu events written to %s (%d write failures)\n",
                    events_written, rules().telemetry_output.output_file.c_str(), failures);
        else
            fprintf(stderr, "[governance] Telemetry: %zu events written to %s\n",
                    events_written, rules().telemetry_output.output_file.c_str());
    }
}

void GovernanceEngine::writeAgentTelemetry(
    const std::string& event_type,
    const std::unordered_map<std::string, std::string>& fields) {

    if (!rules().telemetry_output.enabled || rules().telemetry_output.output_file.empty()) return;

    auto fp_deleter = [](FILE* f) {
#ifndef _WIN32
        ::flock(fileno(f), LOCK_UN);
#endif
        fclose(f);
    };
    std::unique_ptr<FILE, decltype(fp_deleter)> fp(
        fopen(rules().telemetry_output.output_file.c_str(), "a"), fp_deleter);
    if (!fp) return;
#ifndef _WIN32
    if (::flock(fileno(fp.get()), LOCK_EX) != 0) {
        fprintf(stderr, "[governance] warning: telemetry file lock failed\n");
    }
#endif

    // ISO timestamp
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char ts_buf[32];
    std::strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);

    nlohmann::json ev;
    ev["run_id"] = run_id_;
    ev["agent_id"] = agent_id_;
    ev["event_type"] = event_type;
    ev["timestamp"] = std::string(ts_buf);
    for (const auto& [k, v] : fields) {
        // cdd_snapshot carries a pre-serialized JSON object (decision-state
        // snapshot) — embed it structurally so it is queryable and covered
        // by the event hash as one object.
        if (k == "cdd_snapshot") {
            try {
                ev[k] = nlohmann::json::parse(v);
                continue;
            } catch (...) { /* fall through to string */ }
        }
        ev[k] = error::ErrorSanitizer::sanitizeFilePaths(v);
    }

    // Tamper-evident hash chain
    if (rules().telemetry_output.tamper_evidence.enabled) {
        std::lock_guard<std::mutex> hlock(telemetry_hash_mutex_);
        ev["prev_hash"] = chainPrevLocked(fp.get());
        last_telemetry_hash_ = computeHash(ev.dump(),
            rules().telemetry_output.tamper_evidence);
        ev["hash"] = last_telemetry_hash_;
        chained_events_this_run_++;
    }

    std::string line = ev.dump() + "\n";
    checkedWrite(fp.get(), line, telemetry_write_failures_);

    // C2: local shared_ptr copy prevents use-after-free during reload/destruction
    {
        std::shared_ptr<TelemetryForwarder> fwd;
        {
            std::lock_guard<std::mutex> lock(telemetry_fwd_mutex_);
            fwd = telemetry_forwarder_;
        }
        if (fwd) fwd->enqueue(ev.dump());
    }
}

// --- Agent Interaction Transcript ---

void GovernanceEngine::writeAgentTranscript(const std::string& json_line) {
    if (!rules().transcript.enabled || rules().transcript.output_file.empty()) return;

    // entry_hash is computed over the entry as the caller built it (without
    // the entry_hash field), then embedded in the written line and — when the
    // telemetry hash chain is active — committed into the chain via a
    // TRANSCRIPT_REF event. The transcript itself stays outside the chain
    // (audit/debug log by design), but tampering with an entry becomes
    // detectable against the chained reference.
    std::string entry_hash = security::CryptoUtils::sha256(json_line);
    std::string out_line = json_line;
    std::unordered_map<std::string, std::string> ref_fields;
    ref_fields["entry_hash"] = entry_hash;
    try {
        auto j = nlohmann::json::parse(json_line);
        j["entry_hash"] = entry_hash;
        out_line = j.dump();
        // Correlation fields for matching the reference to the entry
        if (j.contains("type") && j["type"].is_string())
            ref_fields["entry_type"] = j["type"].get<std::string>();
        if (j.contains("handle_id"))
            ref_fields["handle_id"] = j["handle_id"].dump();
        if (j.contains("turn"))
            ref_fields["turn"] = j["turn"].dump();
        if (j.contains("agent") && j["agent"].is_string())
            ref_fields["config_name"] = j["agent"].get<std::string>();
    } catch (...) {
        // Not valid JSON — write as-is; the reference still commits to it.
    }

    auto fp_deleter = [](FILE* f) {
#ifndef _WIN32
        ::flock(fileno(f), LOCK_UN);
#endif
        fclose(f);
    };
    std::unique_ptr<FILE, decltype(fp_deleter)> fp(
        fopen(rules().transcript.output_file.c_str(), "a"), fp_deleter);
    if (!fp) return;
#ifndef _WIN32
    if (::flock(fileno(fp.get()), LOCK_EX) != 0) {
        fprintf(stderr, "[governance] warning: transcript file lock failed\n");
    }
#endif

    std::string line = out_line + "\n";
    checkedWrite(fp.get(), line, telemetry_write_failures_);

    // Cross-reference into the tamper-evident telemetry chain
    if (rules().telemetry_output.enabled &&
        rules().telemetry_output.tamper_evidence.enabled) {
        writeAgentTelemetry("TRANSCRIPT_REF", ref_fields);
    }
}

bool GovernanceEngine::isTranscriptAgent(const std::string& agent_name) const {
    if (!rules().transcript.enabled || rules().transcript.output_file.empty()) return false;
    if (rules().transcript.agents.empty()) return true;  // empty = all agents
    for (const auto& a : rules().transcript.agents) {
        if (a == agent_name) return true;
    }
    return false;
}

// --- Agent Role Application ---
// C1: init-only — copy-mutate-swap to avoid writing shared rules_ptr_
void GovernanceEngine::applyAgentRole() {
    auto new_rules = std::make_shared<GovernanceRules>(rules());  // copy current
    for (const auto& role : new_rules->agents) {
        if (role.name == agent_id_) {
            // Restrict allowed languages: intersect with base allowed_languages
            if (!role.allowed_languages.empty()) {
                if (new_rules->allowed_languages.empty()) {
                    // No base restriction — apply role's languages as the restriction
                    for (const auto& l : role.allowed_languages)
                        new_rules->allowed_languages.insert(l);
                } else {
                    // Intersect: keep only languages in both base AND role
                    std::unordered_set<std::string> intersection;
                    for (const auto& l : role.allowed_languages) {
                        if (new_rules->allowed_languages.count(l))
                            intersection.insert(l);
                    }
                    new_rules->allowed_languages = intersection;
                }
            }

            // V-GOV-018: Apply per-role shell restriction.
            // Roles can only restrict (deny shell); they cannot grant shell if
            // the global policy already denies it.
            if (role.shell_allowed_set && !role.shell_allowed) {
                new_rules->shell_allowed = false;
            }

            // Publish updated rules
            std::atomic_store(&rules_ptr_,
                std::const_pointer_cast<const GovernanceRules>(new_rules));

            // Path restrictions enforced at runtime via checkPathAccess()
            fprintf(stderr, "[governance] Agent role applied: %s (languages: ",
                    agent_id_.c_str());
            bool first = true;
            for (const auto& l : new_rules->allowed_languages) {
                if (!first) fprintf(stderr, ", ");
                fprintf(stderr, "%s", l.c_str());
                first = false;
            }
            fprintf(stderr, "), shell: %s\n",
                    new_rules->shell_allowed ? "allowed" : "blocked");
            return;
        }
    }
    // No matching role found — base rules apply unchanged
}

// --- Environment Variable Substitution ---
std::string GovernanceEngine::substituteEnvVars(const std::string& value) const {
    if (!rules().meta.environment.allow_env_var_substitution) return value;
    std::string result = value;
    try {
        std::regex re("\\$\\{([^}:]+)(?::-([^}]*))?\\}");
        std::smatch match;
        std::string::const_iterator search_start = result.cbegin();
        std::string output;
        while (std::regex_search(search_start, result.cend(), match, re)) {
            output.append(search_start, search_start + match.position());
            std::string var_name = match[1].str();
            std::string default_val = match.size() > 2 ? match[2].str() : "";
            const char* env_val = std::getenv(var_name.c_str());
            output.append(env_val ? env_val : default_val);
            search_start += match.position() + match.length();
        }
        output.append(search_start, result.cend());
        return output;
    } catch (...) { return value; }
}


// ============================================================================
// Polyglot Optimization Checks
// ============================================================================

std::string GovernanceEngine::checkPolyglotOptimization(
    const std::string& language,
    const std::string& code,
    int line
) {
    if (!active_) return "";
    if (!rules().polyglot_optimization.enabled) return "";

    // Create detector with task→language matrix from config
    std::map<std::string, std::map<std::string, int>> matrix;
    for (const auto& [task, lang_scores] : rules().polyglot_optimization.task_language_matrix) {
        for (const auto& [lang, score_data] : lang_scores) {
            matrix[task][lang] = score_data.score;
        }
    }

    // Phase 2: Fuse calibration data — measured scores override hardcoded defaults
    // Priority: calibration > govern.json matrix > hardcoded defaults
    if (rules().polyglot_optimization.calibration.enabled) {
        const_cast<GovernanceEngine*>(this)->loadCalibration();
        for (const auto& [task, lang_entries] : calibration_data_) {
            for (const auto& [lang, entry] : lang_entries) {
                if (entry.score > 0) {
                    matrix[task][lang] = entry.score;
                }
            }
        }
    }

    analyzer::ComprehensiveTaskDetector detector(matrix);

    // Analyze code
    auto result = detector.analyze(code, language);

    // Check enforcement level
    std::string level = rules().polyglot_optimization.enforcement_level;

    // Helper errors config
    bool show_suggestions = rules().polyglot_optimization.helper_errors.enabled;

    // Determine if we should suggest different language
    bool should_suggest = false;
    std::string message;

    // Never suggest switching to the same language or when there's no real improvement
    if (result.optimal_language == language ||
        result.improvement_percent <= 0 ||
        result.optimal_language_score <= result.current_language_score) {
        should_suggest = false;
    }
    // Only suggest when improvement is substantial (>50%) to avoid
    // driving LLMs into infinite language-switching loops over marginal gains.
    // A 25% improvement (40→50) is not worth the code rewrite.
    else if (result.improvement_percent > 50) {
        should_suggest = true;
    } else if (result.current_language_score < 40 && result.optimal_language_score > 80) {
        // Only flag truly bad choices (score < 40 vs optimal > 80)
        should_suggest = true;
    }

    // Never suggest a language that is blocked by governance
    if (should_suggest && rules().blocked_languages.count(result.optimal_language)) {
        should_suggest = false;
    }

    if (should_suggest && show_suggestions) {
        // Build concise note for all levels
        std::string note = fmt::format(
            "Consider using {} instead of {} (score: {}/100 vs {}/100, +{}% improvement)",
            result.optimal_language, language,
            result.optimal_language_score, result.current_language_score,
            result.improvement_percent);

        if (level == "hard") {
            // Verbose hint block only for hard enforcement
            suggestBetterLanguage(
                language, code,
                analyzer::taskIntentToString(result.primary_task),
                {result.optimal_language},
                result.improvement_percent,
                result.reasons
            );
            message = fmt::format(
                "HARD violation: Suboptimal language choice\n"
                "  Current: {} (score: {}/100)\n"
                "  Optimal: {} (score: {}/100)\n"
                "  Improvement: +{}%\n\n"
                "  This code MUST use a more appropriate language.",
                language, result.current_language_score,
                result.optimal_language, result.optimal_language_score,
                result.improvement_percent
            );
        } else if (level == "soft") {
            // 1-line advisory via centralized emitAdvisory
            emitAdvisory(fmt::format("[governance] {}", note));
            message = note;
        } else if (level == "advisory") {
            // Record only — no output (check_results_ below handles it)
            message = note;
        }

        // Record check result
        CheckResult check;
        check.rule_name = "polyglot_optimization";
        check.level = level == "hard" ? EnforcementLevel::HARD :
                     level == "soft" ? EnforcementLevel::SOFT :
                                      EnforcementLevel::ADVISORY;
        check.passed = (level == "advisory");  // advisory suggestions aren't failures
        check.message = message;
        check.category = "polyglot";
        check.severity = result.improvement_percent > 50 ? "high" :
                        result.improvement_percent > 30 ? "medium" : "low";
        check.line = line;
        check.file = current_check_file_;
        {
            std::lock_guard<std::mutex> lock(results_mutex_);
            check_results_.push_back(check);
        }

        // Only HARD blocks execution
        if (level == "hard") {
            return message;
        }
    }

    return "";
}

void GovernanceEngine::suggestBetterLanguage(
    const std::string& current_lang,
    const std::string& /* code */,
    const std::string& task_type,
    const std::vector<std::string>& optimal_langs,
    int improvement_percent,
    const std::vector<std::string>& reasons
) {
    if (!rules().polyglot_optimization.helper_errors.enabled) return;

    bool show_example = rules().polyglot_optimization.helper_errors.show_example_code;

    // Phase 3: Determine confidence level
    std::string confidence = "ESTIMATED";
    std::string confidence_detail;

    // Check calibration data for this task type
    if (!calibration_data_.empty()) {
        // Map semantic task types to calibration categories
        // Calibration uses directory names (numerical, string, etc.)
        // Analyzer uses intent strings (numerical_computation, string_manipulation, etc.)
        std::string cal_task;
        if (task_type.find("numerical") != std::string::npos ||
            task_type.find("linear") != std::string::npos ||
            task_type.find("statistical") != std::string::npos)
            cal_task = "numerical";
        else if (task_type.find("string") != std::string::npos)
            cal_task = "string";
        else if (task_type.find("file") != std::string::npos)
            cal_task = "file_io";
        else if (task_type.find("json") != std::string::npos ||
                 task_type.find("data_serialization") != std::string::npos ||
                 task_type.find("data_parsing") != std::string::npos)
            cal_task = "json";
        else if (task_type.find("concurrent") != std::string::npos ||
                 task_type.find("async") != std::string::npos ||
                 task_type.find("parallel") != std::string::npos)
            cal_task = "concurrency";
        else if (task_type.find("cli") != std::string::npos ||
                 task_type.find("batch") != std::string::npos)
            cal_task = "cli";
        else if (task_type.find("web") != std::string::npos ||
                 task_type.find("network") != std::string::npos)
            cal_task = "web_apis";
        else if (task_type.find("system") != std::string::npos ||
                 task_type.find("memory") != std::string::npos ||
                 task_type.find("process") != std::string::npos)
            cal_task = "systems";

        if (!cal_task.empty() && calibration_data_.count(cal_task)) {
            auto& cal_cat = calibration_data_.at(cal_task);
            bool have_current = cal_cat.count(current_lang) > 0;
            bool have_optimal = !optimal_langs.empty() && cal_cat.count(optimal_langs[0]) > 0;

            if (have_current && have_optimal) {
                confidence = "CALIBRATED";
                auto& cur = cal_cat.at(current_lang);
                auto& opt = cal_cat.at(optimal_langs[0]);
                if (cur.us > 0 && opt.us > 0) {
                    double speedup = (double)cur.us / (double)opt.us;
                    confidence_detail = fmt::format("{} {:.1f}x faster (calibrated on this machine)",
                        optimal_langs[0], speedup);
                }
            }
        }
    }

    // Check confidence display level
    const auto& conf_cfg = rules().polyglot_optimization.confidence;
    if (conf_cfg.min_display_level == "measured" && confidence != "MEASURED") return;
    if (conf_cfg.min_display_level == "calibrated" &&
        confidence != "MEASURED" && confidence != "CALIBRATED") return;
    if (conf_cfg.suppress_unknown && confidence == "UNKNOWN") return;

    // Format helper error similar to stdlib helper errors
    fmt::print("\n  Hint: Language optimization opportunity detected.\n\n");
    fmt::print("  Current language: {} (for {} task)\n", current_lang, task_type);

    if (!optimal_langs.empty()) {
        if (optimal_langs.size() == 1) {
            fmt::print("  Optimal language: {}\n", optimal_langs[0]);
        } else {
            std::string langs_str;
            for (size_t i = 0; i < optimal_langs.size(); ++i) {
                if (i > 0) langs_str += ", ";
                langs_str += optimal_langs[i];
            }
            fmt::print("  Optimal languages: {}\n", langs_str);
        }
    }

    // Phase 3: Show confidence level
    fmt::print("  Confidence: {}\n", confidence);
    if (conf_cfg.show_measurement_details && !confidence_detail.empty()) {
        fmt::print("  Detail: {}\n", confidence_detail);
    }

    if (improvement_percent > 0) {
        fmt::print("  Potential improvement: +{}%\n\n", improvement_percent);
    }

    // Show reasons
    if (!reasons.empty()) {
        fmt::print("  Reasons:\n");
        int count = 0;
        for (const auto& reason : reasons) {
            if (count++ >= 3) break;  // Show top 3 reasons
            fmt::print("    • {}\n", reason);
        }
        fmt::print("\n");
    }

    // Show example if enabled (only if optimal differs from current)
    if (show_example && !optimal_langs.empty() && optimal_langs[0] != current_lang) {
        fmt::print("  Example refactoring:\n");
        fmt::print("    ✗ Current: <<{}  [code] >>\n", current_lang);
        fmt::print("    ✓ Better:  <<{}  [code] >>\n\n", optimal_langs[0]);
    }

    fmt::print("  For more: docs/polyglot/optimization_guide.md\n\n");
}

// ============================================================================
// Empirical Profiling
// ============================================================================

bool GovernanceEngine::isProfilingEnabled() const {
    return active_ && rules().polyglot_optimization.enabled &&
           rules().polyglot_optimization.profiling.enabled;
}

void GovernanceEngine::writeProfileEntry(const std::string& language,
                                         const std::string& task_category,
                                         const std::string& code_hash,
                                         int64_t duration_us) {
    if (!isProfilingEnabled()) return;

    auto& cfg = rules().polyglot_optimization.profiling;

    // Expand ~ in path
    std::string path = cfg.profile_path;
    if (path.size() >= 2 && path[0] == '~' && path[1] == '/') {
        const char* home = std::getenv("HOME");
        if (home) path = std::string(home) + path.substr(1);
    }

    // Ensure parent directory exists
    auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    // Read existing entries (ring buffer)
    nlohmann::json entries = nlohmann::json::array();
    {
        std::ifstream in(path);
        if (in.is_open()) {
            try {
                nlohmann::json existing;
                in >> existing;
                if (existing.is_array()) entries = existing;
            } catch (...) {
                // Corrupted file — start fresh
            }
        }
    }

    // Build new entry
    auto now = std::chrono::system_clock::now();
    auto ts = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();

    nlohmann::json entry;
    entry["lang"] = language;
    entry["task"] = task_category;
    if (cfg.include_code_hash && !code_hash.empty()) {
        entry["hash"] = code_hash;
    }
    entry["us"] = duration_us;
    entry["ts"] = ts;

    entries.push_back(entry);

    // Ring buffer: trim to max_entries
    if (cfg.max_entries > 0 && (int)entries.size() > cfg.max_entries) {
        int excess = (int)entries.size() - cfg.max_entries;
        entries.erase(entries.begin(), entries.begin() + excess);
    }

    // Write back
    std::ofstream out(path);
    if (out.is_open()) {
        out << entries.dump(2);
    }
}

bool GovernanceEngine::loadCalibration() {
    if (calibration_loaded_) return !calibration_data_.empty();
    calibration_loaded_ = true;

    auto& cfg = rules().polyglot_optimization.calibration;
    if (!cfg.enabled) return false;

    // Expand ~ in path
    std::string path = cfg.calibration_path;
    if (path.size() >= 2 && path[0] == '~' && path[1] == '/') {
        const char* home = std::getenv("HOME");
        if (home) path = std::string(home) + path.substr(1);
    }

    std::ifstream in(path);
    if (!in.is_open()) return false;

    try {
        nlohmann::json j;
        in >> j;

        if (!j.contains("results") || !j["results"].is_object()) return false;

        // Check age
        if (cfg.max_age_days > 0 && j.contains("timestamp") && j["timestamp"].is_string()) {
            // Simple age check: compare epoch-based if available
            // For now, just load the data regardless of age
        }

        for (auto& [task, lang_scores] : j["results"].items()) {
            if (!lang_scores.is_object()) continue;
            for (auto& [lang, data] : lang_scores.items()) {
                CalibrationEntry entry;
                if (data.is_object()) {
                    if (data.contains("us")) entry.us = data["us"].get<int64_t>();
                    if (data.contains("score")) entry.score = data["score"].get<int>();
                }
                calibration_data_[task][lang] = entry;
            }
        }

        return !calibration_data_.empty();
    } catch (...) {
        return false;
    }
}

// ============================================================================
// Polyglot Consensus Verification
// ============================================================================

bool GovernanceEngine::isVerificationEnabled() const {
    return active_ &&
           rules().polyglot_optimization.enabled &&
           rules().polyglot_optimization.verification.enabled &&
           !rules().polyglot_optimization.verification.consensus_languages.empty();
}

bool GovernanceEngine::isNumericString(const std::string& s) {
    if (s.empty()) return false;
    char* end = nullptr;
    std::strtod(s.c_str(), &end);
    return end != s.c_str() && *end == '\0';
}

std::string GovernanceEngine::escapeStringForVerification(const std::string& s) {
    std::string result;
    for (char c : s) {
        if (c == '\'') result += "\\'";
        else if (c == '\"') result += "\\\"";
        else if (c == '\\') result += "\\\\";
        else if (c == '\n') result += "\\n";
        else if (c == '\r') continue;
        else result += c;
    }
    return result;
}

bool GovernanceEngine::compareResults(const std::string& a, const std::string& b, double tolerance) {
    // Exact string match first (fast path)
    if (a == b) return true;

    // Whitespace-normalized string match
    auto normalize = [](const std::string& s) -> std::string {
        std::string result;
        for (char c : s) {
            if (c == '\r') continue;
            result += c;
        }
        // Trim trailing whitespace/newlines
        while (!result.empty() && (result.back() == '\n' || result.back() == ' ' || result.back() == '\t'))
            result.pop_back();
        // Trim leading whitespace
        size_t start = result.find_first_not_of(" \t\n");
        if (start != std::string::npos) result = result.substr(start);
        return result;
    };

    std::string na = normalize(a);
    std::string nb = normalize(b);
    if (na == nb) return true;

    // Numeric comparison with tolerance
    if (isNumericString(na) && isNumericString(nb)) {
        double da = std::stod(na);
        double db = std::stod(nb);
        return std::abs(da - db) <= tolerance;
    }

    return false;
}

std::string GovernanceEngine::classifyTaskForVerification(
    const std::string& code, const std::string& language) {

    std::map<std::string, std::map<std::string, int>> matrix;
    for (const auto& [task, lang_scores] : rules().polyglot_optimization.task_language_matrix) {
        for (const auto& [lang, score_data] : lang_scores) {
            matrix[task][lang] = score_data.score;
        }
    }

    analyzer::ComprehensiveTaskDetector detector(matrix);
    auto result = detector.analyze(code, language);
    return analyzer::taskIntentToString(result.primary_task);
}

std::string GovernanceEngine::extractMathExpression(
    const std::string& code, const std::string& lang) {

    std::string trimmed = code;
    auto start = trimmed.find_first_not_of(" \t\n\r");
    auto end = trimmed.find_last_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    trimmed = trimmed.substr(start, end - start + 1);

    // Single line? Likely a pure expression
    if (trimmed.find('\n') == std::string::npos) {
        // Strip common wrappers
        if (lang == "python" && trimmed.substr(0, 6) == "print(" && trimmed.back() == ')')
            return trimmed.substr(6, trimmed.size() - 7);
        if ((lang == "javascript" || lang == "js") && trimmed.substr(0, 12) == "console.log(" && trimmed.back() == ')')
            return trimmed.substr(12, trimmed.size() - 13);
        if (lang == "ruby" && trimmed.size() > 5 && trimmed.substr(0, 5) == "puts ")
            return trimmed.substr(5);

        // Check if it looks like a math expression
        bool looks_numeric = true;
        for (char c : trimmed) {
            if (!std::isdigit(c) && c != '.' && c != '+' && c != '-' &&
                c != '*' && c != '/' && c != '(' && c != ')' && c != ' ' &&
                c != '%' && c != 'e' && c != 'E') {
                looks_numeric = false;
                break;
            }
        }
        if (looks_numeric) return trimmed;
    }

    // Multi-line: look for last line as the result expression
    std::istringstream stream(trimmed);
    std::string line, last_line;
    while (std::getline(stream, line)) {
        auto ls = line.find_first_not_of(" \t");
        if (ls != std::string::npos) last_line = line.substr(ls);
    }

    // Check if last line is a simple expression (no assignment, no import)
    if (!last_line.empty() && last_line.find('=') == std::string::npos &&
        last_line.find("import") == std::string::npos) {
        // Strip print wrappers from last line too
        if (last_line.substr(0, 6) == "print(" && last_line.back() == ')')
            return last_line.substr(6, last_line.size() - 7);
        if (last_line.substr(0, 12) == "console.log(" && last_line.back() == ')')
            return last_line.substr(12, last_line.size() - 13);
        return last_line;
    }

    return "";  // Can't extract — fallback to echo strategy
}

std::string GovernanceEngine::generateEchoCode(
    const std::string& target_lang, const std::string& value) {
    std::string esc = escapeStringForVerification(value);
    if (target_lang == "python") return "print('" + esc + "')";
    if (target_lang == "javascript" || target_lang == "js")
        return "console.log('" + esc + "')";
    if (target_lang == "go")
        return "package main\nimport \"fmt\"\nfunc main(){fmt.Print(\"" + esc + "\")}";
    if (target_lang == "ruby") return "print '" + esc + "'";
    if (target_lang == "nim") return "import std/strutils\nstdout.write(\"" + esc + "\")";
    if (target_lang == "julia") return "print(\"" + esc + "\")";
    if (target_lang == "rust")
        return "fn main(){print!(\"" + esc + "\");}";
    if (target_lang == "shell" || target_lang == "sh" || target_lang == "bash")
        return "printf '%s' '" + esc + "'";
    // Default fallback
    return "print('" + esc + "')";
}

std::string GovernanceEngine::generateVerificationCode(
    const std::string& task_type,
    const std::string& original_code,
    const std::string& original_result,
    const std::string& source_lang,
    const std::string& target_lang)
{
    // ================================================================
    // NUMERICAL VERIFICATION
    // ================================================================
    if (task_type.find("numerical") != std::string::npos ||
        task_type.find("statistical") != std::string::npos ||
        task_type.find("linear") != std::string::npos) {

        std::string expr = extractMathExpression(original_code, source_lang);

        if (target_lang == "python") {
            if (!expr.empty())
                return "result = " + expr + "\nprint(result)";
            return "print(" + original_result + ")";
        }
        else if (target_lang == "javascript" || target_lang == "js") {
            if (!expr.empty())
                return "console.log(" + expr + ")";
            return "console.log(" + original_result + ")";
        }
        else if (target_lang == "go") {
            std::string e = expr.empty() ? original_result : expr;
            return "package main\nimport \"fmt\"\nfunc main() {\n\tfmt.Print(" + e + ")\n}";
        }
        else if (target_lang == "ruby") {
            return "print " + (expr.empty() ? original_result : expr);
        }
        else if (target_lang == "nim") {
            std::string e = expr.empty() ? original_result : expr;
            return "import std/strutils\nstdout.write($(" + e + "))";
        }
        else if (target_lang == "julia") {
            return "print(" + (expr.empty() ? original_result : expr) + ")";
        }
        else if (target_lang == "rust") {
            std::string e = expr.empty() ? original_result : expr;
            return "fn main() { print!(\"{}\", " + e + "); }";
        }
        else if (target_lang == "shell" || target_lang == "sh" || target_lang == "bash") {
            std::string e = expr.empty() ? original_result : expr;
            return "echo $(( " + e + " ))";
        }
    }

    // ================================================================
    // STRING VERIFICATION
    // ================================================================
    if (task_type.find("string") != std::string::npos) {
        return generateEchoCode(target_lang, original_result);
    }

    // ================================================================
    // JSON / DATA VERIFICATION
    // ================================================================
    if (task_type.find("json") != std::string::npos ||
        task_type.find("data_parsing") != std::string::npos ||
        task_type.find("data_serialization") != std::string::npos) {

        std::string esc = escapeStringForVerification(original_result);
        if (target_lang == "python") {
            return "import json\ndata = json.loads('" + esc + "')\nprint(json.dumps(data, sort_keys=True))";
        }
        else if (target_lang == "javascript" || target_lang == "js") {
            return "const d = JSON.parse('" + esc + "');\n"
                   "const keys = Object.keys(d).sort();\n"
                   "const sorted = {}; keys.forEach(k => sorted[k] = d[k]);\n"
                   "console.log(JSON.stringify(sorted))";
        }
        else if (target_lang == "go") {
            return "package main\nimport(\"encoding/json\"\n\"fmt\")\n"
                   "func main() {\n\tvar d map[string]interface{}\n"
                   "\tjson.Unmarshal([]byte(`" + original_result + "`), &d)\n"
                   "\tb, _ := json.Marshal(d)\n\tfmt.Print(string(b))\n}";
        }
        // Other languages: echo the result
        return generateEchoCode(target_lang, original_result);
    }

    // ================================================================
    // FILE / CLI / WEB / CONCURRENCY / SYSTEMS — echo only
    // (Can't safely re-run side effects)
    // ================================================================
    return generateEchoCode(target_lang, original_result);
}

// --- Output Baselines ---

void GovernanceEngine::loadBaselines() {
    if (baselines_loaded_) return;
    baselines_loaded_ = true;

    // Resolve path relative to govern.json directory
    std::string path = rules().baselines.path;
    if (!path.empty() && path[0] != '/') {
        auto gov_dir = std::filesystem::path(loaded_path_).parent_path();
        path = (gov_dir / path).string();
    }
    baselines_path_ = path;

    // Allocate JSON object on heap (opaque via void*)
    auto* data = new nlohmann::json();
    baselines_data_ = data;

    std::ifstream in(path);
    if (in.is_open()) {
        try {
            in >> *data;
        } catch (...) {
            // Corrupt file — start fresh
            *data = nlohmann::json::object();
        }
    }

    if (!data->contains("version")) (*data)["version"] = "1.0";
    if (!data->contains("entries")) (*data)["entries"] = nlohmann::json::object();
}

void GovernanceEngine::saveBaselines() {
    if (!baselines_data_) return;
    auto* data = static_cast<nlohmann::json*>(baselines_data_);

    auto parent = std::filesystem::path(baselines_path_).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    std::ofstream out(baselines_path_);
    if (out.is_open()) {
        out << data->dump(2) << "\n";
    }
}

void GovernanceEngine::recordBaseline(const std::string& key,
                                       const std::string& output,
                                       const std::string& type) {
    loadBaselines();
    if (!baselines_data_) return;
    auto* data = static_cast<nlohmann::json*>(baselines_data_);

    auto now = std::chrono::system_clock::now();
    auto ts = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();

    nlohmann::json entry;
    entry["output"] = output;
    entry["type"] = type;
    entry["recorded_at"] = ts;
    entry["runs"] = 1;
    entry["last_seen"] = ts;

    (*data)["entries"][key] = entry;
    baselines_dirty_ = true;
}

std::string GovernanceEngine::checkBaseline(const std::string& key,
                                             const std::string& output,
                                             const std::string& type,
                                             int line) {
    if (!rules().baselines.enabled) return "";

    loadBaselines();
    if (!baselines_data_) return "";
    auto* data = static_cast<nlohmann::json*>(baselines_data_);

    auto& entries = (*data)["entries"];
    if (!entries.contains(key)) {
        // No baseline exists
        if (rules().baselines.auto_record) {
            recordBaseline(key, output, type);
        }
        return "";
    }

    auto& entry = entries[key];
    std::string expected = entry.contains("output") && entry["output"].is_string() ? entry["output"].get<std::string>() : "";
    std::string expected_type = entry.contains("type") && entry["type"].is_string() ? entry["type"].get<std::string>() : "";

    // Compare using tolerance for numeric types
    bool matches = false;
    if ((type == "float" || type == "int") &&
        (expected_type == "float" || expected_type == "int")) {
        matches = compareResults(output, expected, rules().baselines.tolerance);
    } else {
        matches = (output == expected);
    }

    if (matches) {
        // Update runs counter and last_seen
        int runs = (entry.contains("runs") && entry["runs"].is_number_integer()) ? entry["runs"].get<int>() : 0;
        entry["runs"] = runs + 1;
        auto now = std::chrono::system_clock::now();
        auto ts = std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch()).count();
        entry["last_seen"] = ts;
        baselines_dirty_ = true;
        return "";
    }

    // Mismatch
    return enforce("baselines", rules().baselines.level,
        formatError(rules().baselines.level,
            fmt::format("Baseline mismatch for '{}': expected '{}', got '{}'",
                key, expected, output),
            line > 0 ? fmt::format("line {}", line) : "",
            "baselines",
            "Output has changed from previously recorded baseline",
            "", ""));
}

// --- Drift Tracking ---

void GovernanceEngine::writeDriftEvent(
    const std::string& language, const std::string& task_type,
    const std::string& code_hash, const std::string& expected,
    const std::string& got, int line, int consensus, int total,
    const std::string& file) {

    auto& dtc = rules().polyglot_optimization.verification.drift_tracking;
    if (!dtc.enabled) return;

    // Expand ~ in path
    std::string path = dtc.path;
    if (path.size() >= 2 && path[0] == '~' && path[1] == '/') {
        const char* home = std::getenv("HOME");
        if (home) path = std::string(home) + path.substr(1);
    }

    // Ensure parent directory exists
    auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    // Build JSONL entry
    auto now = std::chrono::system_clock::now();
    auto ts = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();

    nlohmann::json entry;
    entry["ts"] = ts;
    entry["lang"] = language;
    entry["task"] = task_type;
    if (dtc.include_code_hash && !code_hash.empty()) entry["hash"] = code_hash;
    entry["expected"] = expected;
    entry["got"] = got;
    entry["line"] = line;
    entry["consensus"] = consensus;
    entry["total"] = total;
    if (!file.empty()) entry["file"] = file;

    // Append-only write (O(1) per event instead of O(n) read+write)
    {
        std::ofstream out(path, std::ios::app);
        if (out.is_open()) {
            out << entry.dump() << "\n";
        }
    }

    // Periodic ring buffer trim — only when writes exceed max_entries
    drift_write_count_++;
    if (dtc.max_entries > 0 && drift_write_count_ >= dtc.max_entries) {
        drift_write_count_ = 0;
        std::vector<std::string> lines;
        {
            std::ifstream in(path);
            if (in.is_open()) {
                std::string l;
                while (std::getline(in, l)) {
                    if (!l.empty()) lines.push_back(l);
                }
            }
        }
        if (static_cast<int>(lines.size()) > dtc.max_entries) {
            int excess = static_cast<int>(lines.size()) - dtc.max_entries;
            lines.erase(lines.begin(), lines.begin() + excess);
            std::ofstream out(path);
            if (out.is_open()) {
                for (const auto& l : lines) {
                    out << l << "\n";
                }
            }
        }
    }
}

void GovernanceEngine::analyzeDriftTrend(const std::string& language) {
    auto& dtc = rules().polyglot_optimization.verification.drift_tracking;
    if (!dtc.enabled) return;

    // Expand ~ in path
    std::string path = dtc.path;
    if (path.size() >= 2 && path[0] == '~' && path[1] == '/') {
        const char* home = std::getenv("HOME");
        if (home) path = std::string(home) + path.substr(1);
    }

    // Read JSONL and filter by language
    std::vector<nlohmann::json> events;
    {
        std::ifstream in(path);
        if (!in.is_open()) return;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            try {
                auto j = nlohmann::json::parse(line);
                if (j.contains("lang") && j["lang"].is_string() && j["lang"].get<std::string>() == language) {
                    events.push_back(std::move(j));
                }
            } catch (...) {
                // Skip corrupt lines
            }
        }
    }

    // Look at last trend_window events
    int window = dtc.trend_window;
    int start = static_cast<int>(events.size()) > window
        ? static_cast<int>(events.size()) - window : 0;

    // Count events in the window — every logged event IS a drift event
    int events_in_window = 0;
    for (int i = start; i < static_cast<int>(events.size()); i++) {
        events_in_window++;
    }

    if (events_in_window == 0) return;

    // Rate = drift_events / window_size
    // e.g., 3 drift events in a window of 10 → 30% → triggers at threshold 0.3
    double drift_rate = static_cast<double>(events_in_window) / static_cast<double>(window);

    if (drift_rate >= dtc.escalation_threshold) {
        fprintf(stderr,
            "\n  [governance] DRIFT TREND WARNING: %s has %.0f%% drift rate "
            "(%d events in last %d window)\n"
            "    Threshold: %.0f%% — consider investigating %s block consistency\n\n",
            language.c_str(), drift_rate * 100.0,
            events_in_window, window,
            dtc.escalation_threshold * 100.0, language.c_str());
    }
}

std::string GovernanceEngine::verifyPolyglotResult(
    const std::string& language,
    const std::string& code,
    const std::string& result_str,
    int line)
{
    if (!isVerificationEnabled()) return "";

    auto& cfg = rules().polyglot_optimization.verification;

    // 1. Classify the task
    std::string task_type = classifyTaskForVerification(code, language);

    // 2. Check if this task type should be verified
    if (!cfg.verify_task_types.empty()) {
        bool found = false;
        for (const auto& vt : cfg.verify_task_types) {
            if (task_type.find(vt) != std::string::npos || vt.find(task_type) != std::string::npos) {
                found = true;
                break;
            }
        }
        if (!found) return "";
    }

    // 3. Filter consensus languages to installed only, skip original language
    auto& registry = runtime::LanguageRegistry::instance();
    std::vector<std::string> available_langs;
    std::string norm_lang = normalizeLanguage(language);
    for (const auto& lang : cfg.consensus_languages) {
        std::string norm = normalizeLanguage(lang);
        if (norm == norm_lang) continue;
        if (registry.getExecutor(norm) != nullptr) {
            available_langs.push_back(norm);
        }
    }

    if (available_langs.empty()) return "";

    // 4. Run verification in each language
    std::vector<VerificationResult> results;
    results.push_back({norm_lang, result_str, 0, true, ""});  // Original

    for (const auto& target_lang : available_langs) {
        VerificationResult vr;
        vr.language = target_lang;

        std::string verif_code = generateVerificationCode(
            task_type, code, result_str, norm_lang, target_lang);

        if (verif_code.empty()) {
            vr.success = false;
            vr.error = "No template available";
            results.push_back(vr);
            continue;
        }

        auto start_time = std::chrono::steady_clock::now();
        try {
            auto* verif_executor = registry.getExecutor(target_lang);
            if (!verif_executor) {
                vr.success = false;
                vr.error = "Executor not found";
                results.push_back(vr);
                continue;
            }

            auto verif_value = verif_executor->executeWithReturn(verif_code);
            vr.result = verif_value.isNull() ? "" : verif_value.toString();
            vr.success = true;
            // Drain captured output to prevent leaking into subsequent real executions
            verif_executor->getCapturedOutput();
        } catch (const governance::GovernanceHardError&) {
            throw;
        } catch (const std::exception& e) {
            vr.success = false;
            vr.error = e.what();
        }

        auto end_time = std::chrono::steady_clock::now();
        vr.duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
        results.push_back(vr);
    }

    // 5. Compare all results against original
    int agree_count = 0;
    int total_count = 0;
    std::vector<std::string> drift_details;

    for (const auto& vr : results) {
        if (!vr.success) continue;
        total_count++;
        if (compareResults(result_str, vr.result, cfg.tolerance)) {
            agree_count++;
        } else {
            drift_details.push_back(fmt::format("{}={}", vr.language, vr.result));
        }
    }

    // 6. Format and output governance message
    bool consensus = (agree_count >= cfg.min_consensus) && drift_details.empty();

    if (consensus) {
        std::string lang_vals;
        for (const auto& vr : results) {
            if (!vr.success) continue;
            if (!lang_vals.empty()) lang_vals += "  ";
            lang_vals += fmt::format("{}={}", vr.language, vr.result);
        }
        fmt::print("\n  [governance] Verification: {} block (line {})\n", language, line);
        if (cfg.show_drift_details) {
            fmt::print("    \xe2\x9c\x93 {}  ({}/{} agree)\n\n", lang_vals, agree_count, total_count);
        } else {
            fmt::print("    \xe2\x9c\x93 {}/{} agree\n\n", agree_count, total_count);
        }
        return "";
    }

    // Drift detected
    std::string level_str = cfg.enforcement_level;
    std::string level_upper = level_str;
    std::transform(level_upper.begin(), level_upper.end(), level_upper.begin(), ::toupper);

    fmt::print("\n  [governance] Verification MISMATCH: {} block (line {})  [{}]\n",
        language, line, level_upper);

    if (cfg.show_drift_details) {
        for (const auto& vr : results) {
            if (!vr.success) {
                fmt::print("    {} = ERROR: {}\n", vr.language, vr.error);
                continue;
            }
            bool matches = compareResults(result_str, vr.result, cfg.tolerance);
            fmt::print("    {}{}={}\n", matches ? "\xe2\x9c\x93 " : "\xe2\x9c\x97 ", vr.language, vr.result);
        }

        if (isNumericString(result_str) && !drift_details.empty()) {
            for (const auto& vr : results) {
                if (vr.success && !compareResults(result_str, vr.result, cfg.tolerance) &&
                    isNumericString(vr.result)) {
                    double diff = std::abs(std::stod(result_str) - std::stod(vr.result));
                    fmt::print("    Drift: {:.2e} (tolerance: {:.2e})\n", diff, cfg.tolerance);
                    break;
                }
            }
        }
    }

    fmt::print("    Task: {} | Consensus: {}/{}\n\n", task_type, agree_count, total_count);

    // Drift tracking: write event and analyze trend
    {
        std::size_t hash_val = std::hash<std::string>{}(code);
        char hash_buf[16];
        snprintf(hash_buf, sizeof(hash_buf), "%06zx", hash_val & 0xFFFFFF);
        writeDriftEvent(language, task_type, hash_buf, result_str,
            drift_details.empty() ? "" : drift_details[0],
            line, agree_count, total_count, "");
        analyzeDriftTrend(language);
    }

    // Audit logging for soft/hard enforcement
    if (level_str == "soft" || level_str == "hard") {
        fprintf(stderr, "[governance] AUDIT DRIFT: %s block at line %d — %d/%d consensus (%s)\n",
            language.c_str(), line, agree_count, total_count, task_type.c_str());
    }

    // Hard enforcement: block execution
    if (level_str == "hard") {
        std::string details;
        for (const auto& vr : results) {
            if (vr.success) details += fmt::format("  {}={}\n", vr.language, vr.result);
        }
        return fmt::format(
            "Verification HARD violation: Cross-language drift detected at line {}\n"
            "  Task: {}\n"
            "  Results:\n{}"
            "  Consensus: {}/{} (minimum: {})\n\n"
            "  If this threshold seems incorrect, contact the project owner.",
            line, task_type, details, agree_count, total_count, cfg.min_consensus);
    }

    return "";
}

// ============================================================================
// Governance Plugin API (#23): NAAb-based custom governance rules
// ============================================================================

void GovernanceEngine::loadPlugins() {
    if (plugins_loaded_) return;
    plugins_loaded_ = true;

    auto* interp = interpreter::g_current_interpreter;
    if (!interp) {
        fprintf(stderr, "[governance] Warning: No interpreter available for plugin loading\n");
        return;
    }

    namespace fs = std::filesystem;
    for (auto& plugin : getMutableRules().governance_plugins) {
        if (plugin.rules.empty()) continue;

        // Resolve file path relative to govern.json directory
        fs::path plugin_path;
        if (fs::path(plugin.file_path).is_absolute()) {
            plugin_path = plugin.file_path;
        } else {
            plugin_path = fs::path(govern_json_dir_) / plugin.file_path;
        }

        if (!fs::exists(plugin_path)) {
            fprintf(stderr, "[governance] Warning: Plugin file not found: %s\n",
                    plugin_path.string().c_str());
            continue;
        }

        try {
            interp->loadPluginFile(plugin_path.string());
            plugin.loaded = true;
        } catch (const governance::GovernanceHardError&) {
            throw;
        } catch (const std::exception& e) {
            fprintf(stderr, "[governance] Warning: Failed to load plugin %s: %s\n",
                    plugin_path.string().c_str(), e.what());
        }
    }
}

std::string GovernanceEngine::checkPluginRules(
    const std::string& trigger,
    const std::unordered_map<std::string, interpreter::NaabVal>& context,
    int line) {

    // Re-entrancy guard: prevent infinite recursion if plugin code triggers governance
    if (in_plugin_check_) return "";

    // No plugins configured — fast path
    if (rules().governance_plugins.empty()) return "";

    // Lazy-load plugins on first call
    if (!plugins_loaded_) loadPlugins();

    auto* interp = interpreter::g_current_interpreter;
    if (!interp) return "";

    // Scope guard for re-entrancy flag
    struct PluginGuard {
        bool& flag;
        PluginGuard(bool& f) : flag(f) { flag = true; }
        ~PluginGuard() { flag = false; }
    } guard(in_plugin_check_);

    for (const auto& plugin : rules().governance_plugins) {
        if (!plugin.loaded) continue;

        for (const auto& rule : plugin.rules) {
            if (!rule.enabled) continue;
            if (rule.trigger != trigger) continue;

            // Language filter
            if (!rule.languages.empty()) {
                auto lang_it = context.find("language");
                if (lang_it != context.end()) {
                    std::string lang;
                    auto lang_val = lang_it->second;
                    if (lang_val.isString()) lang = lang_val.asString();
                    bool matches = false;
                    for (const auto& l : rule.languages) {
                        if (l == lang) { matches = true; break; }
                    }
                    if (!matches) continue;
                }
            }

            // Look up function in global env
            interpreter::NaabVal fn;
            try {
                fn = interp->getGlobalEnv()->get(rule.function_name);
                if (fn.isNull()) {
                    if (warned_plugin_rules_.find(rule.function_name) == warned_plugin_rules_.end()) {
                        fprintf(stderr, "[governance] Warning: Plugin function '%s' not found (rule %s)\n",
                                rule.function_name.c_str(), rule.id.c_str());
                        warned_plugin_rules_.insert(rule.function_name);
                    }
                    continue;
                }
            } catch (...) {
                if (warned_plugin_rules_.find(rule.function_name) == warned_plugin_rules_.end()) {
                    fprintf(stderr, "[governance] Warning: Plugin function '%s' not found (rule %s)\n",
                            rule.function_name.c_str(), rule.id.c_str());
                    warned_plugin_rules_.insert(rule.function_name);
                }
                continue;
            }

            // Build context dict as NaabVal
            std::unordered_map<std::string, interpreter::NaabVal> ctx_map;
            for (const auto& [key, val] : context) {
                ctx_map[key] = val;
            }
            auto ctx_val = interpreter::NaabVal::makeDict(std::move(ctx_map));

            // Call the plugin function
            interpreter::NaabVal result;
            try {
                result = interp->callFunction(fn, {ctx_val});
            } catch (const governance::GovernanceHardError&) {
                throw;
            } catch (const std::exception& e) {
                fprintf(stderr, "[governance] Plugin rule '%s' threw: %s\n",
                        rule.id.c_str(), e.what());
                continue;
            }

            // Parse result — must be a dict with "pass" key
            if (!result.isDict()) {
                if (warned_plugin_rules_.find(rule.id + ".result") == warned_plugin_rules_.end()) {
                    fprintf(stderr, "[governance] Warning: Plugin rule '%s' returned %s (expected dict with 'pass' key)\n",
                            rule.id.c_str(), result.getTypeName().c_str());
                    warned_plugin_rules_.insert(rule.id + ".result");
                }
                continue;
            }

            // Extract "pass" field
            auto& result_dict = result.asDict();
            bool passed = true;
            bool has_pass = false;
            for (const auto& [k, v] : result_dict) {
                if (k == "pass") {
                    has_pass = true;
                    if (v.isBool()) passed = v.asBool();
                    else if (v.isNull()) passed = false;
                    break;
                }
            }

            if (!has_pass) {
                if (warned_plugin_rules_.find(rule.id + ".pass") == warned_plugin_rules_.end()) {
                    fprintf(stderr, "[governance] Warning: Plugin rule '%s' returned dict without 'pass' key\n",
                            rule.id.c_str());
                    warned_plugin_rules_.insert(rule.id + ".pass");
                }
                continue;
            }

            std::string rule_path = "governance_plugins." + rule.id;

            if (passed) {
                recordPass(rule_path, rule.level);
                continue;
            }

            // Failed — build error message from plugin result + rule defaults
            std::string msg = rule.message;
            std::string help = rule.help;
            std::string good_ex = rule.good_example;
            std::string bad_ex = rule.bad_example;

            // Plugin can override any of these
            for (const auto& [k, v] : result_dict) {
                if (k == "message" && v.isString()) msg = v.asString();
                else if (k == "help" && v.isString()) help = v.asString();
                else if (k == "good_example" && v.isString()) good_ex = v.asString();
                else if (k == "bad_example" && v.isString()) bad_ex = v.asString();
            }

            if (msg.empty()) {
                msg = fmt::format("Plugin rule '{}' violated", rule.id);
            }

            std::string err = enforce(rule_path, rule.level,
                formatError(rule.level, msg,
                    line > 0 ? fmt::format("line {}", line) : "",
                    "governance_plugins[\"" + rule.id + "\"]",
                    help, bad_ex, good_ex));
            if (!err.empty()) return err;
        }
    }

    return "";
}

} // namespace governance
} // namespace naab
