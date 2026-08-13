#pragma once

// NAAb Governance Engine v3.0
// Runtime enforcement of project governance rules via govern.json
//
// Three-tier enforcement model (inspired by HashiCorp Sentinel):
//   HARD      - Block execution. No override possible.
//   SOFT      - Block execution. Override with --governance-override flag.
//   ADVISORY  - Warn only. Execution continues.
//
// Zero overhead when no govern.json exists.
// Every rule is configurable and can be turned off.

#include <cstdio>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <map>
#include <chrono>
#include <regex>
#include <mutex>
#include <atomic>
#include <memory>
#include <optional>
#include <set>
#include <functional>

#include "naab/project_context.h"
#include "naab/naab_val.h"
#include "naab/behavioral_sequence.h"

namespace naab {
class TelemetryForwarder;  // Forward declaration (telemetry_forwarder.h)
namespace ast { class Program; }  // Forward declaration for drift detection
namespace governance {

// ============================================================================
// Core Enums
// ============================================================================

enum class EnforcementLevel {
    NONE,              // Not set (use parent level). For contract per-function overrides.
    HARD,              // Block execution. No override possible.
    APPROVAL_REQUIRED, // Block unless a signed approval token exists for this rule.
    SOFT,              // Block execution. Override with --governance-override.
    ADVISORY,          // Warn only. Execution continues.
    DETECT             // Block execution but catchable by NAAb try/catch. For testing.
};

enum class GovernanceMode {
    ENFORCE,    // Normal enforcement (default)
    AUDIT,      // Dry-run: check everything, block nothing
    OFF         // Disabled
};
// M7: Ratchet logic depends on ENFORCE < AUDIT < OFF ordering
static_assert(static_cast<int>(GovernanceMode::ENFORCE) < static_cast<int>(GovernanceMode::AUDIT),
              "GovernanceMode ordering broken — ratchet enforcement will malfunction");
static_assert(static_cast<int>(GovernanceMode::AUDIT) < static_cast<int>(GovernanceMode::OFF),
              "GovernanceMode ordering broken — ratchet enforcement will malfunction");

// ============================================================================
// Section 1: Language Control
// ============================================================================

struct ImportRules {
    std::string mode = "blocklist";  // "blocklist" or "allowlist"
    std::vector<std::string> blocked;
    std::vector<std::string> allowed;
    std::vector<std::string> blocked_from;  // "os:system,popen"
};

struct LanguageConfig {
    std::string version_hint;
    int timeout = 0;            // 0 = use global
    int max_lines = 0;          // 0 = no limit
    int max_output_size = 0;    // 0 = no limit
    EnforcementLevel dangerous_calls = EnforcementLevel::HARD;
    bool dangerous_calls_enabled = false;

    ImportRules imports;
    std::vector<std::string> banned_functions;
    std::vector<std::string> banned_globals;
    std::vector<std::string> banned_keywords;
    std::vector<std::string> banned_imports;      // Go
    std::vector<std::string> banned_namespaces;   // C#
    std::vector<std::string> banned_includes;     // C++
    std::vector<std::string> banned_commands;      // Shell
    std::vector<std::string> required_imports;

    // Style rules (each can be "hard"/"soft"/"advisory"/false)
    std::string indent_style;  // "spaces" or "tabs"
    int indent_size = 0;
    std::string encoding;
    bool no_star_imports = false;
    EnforcementLevel no_star_imports_level = EnforcementLevel::ADVISORY;
    bool strict_mode = false;
    EnforcementLevel strict_mode_level = EnforcementLevel::ADVISORY;
    bool no_var = false;
    EnforcementLevel no_var_level = EnforcementLevel::ADVISORY;
    bool no_console_log = false;
    EnforcementLevel no_console_log_level = EnforcementLevel::ADVISORY;

    // Shell-specific
    bool shell_injection = false;
    EnforcementLevel shell_injection_level = EnforcementLevel::HARD;
    bool require_set_e = false;
    EnforcementLevel require_set_e_level = EnforcementLevel::SOFT;
    bool require_set_u = false;
    EnforcementLevel require_set_u_level = EnforcementLevel::ADVISORY;
    bool require_set_pipefail = false;
    EnforcementLevel require_set_pipefail_level = EnforcementLevel::ADVISORY;
    bool require_quoting = false;
    EnforcementLevel require_quoting_level = EnforcementLevel::SOFT;
    bool no_curl_pipe_sh = false;
    EnforcementLevel no_curl_pipe_sh_level = EnforcementLevel::HARD;
    bool no_wget_pipe_bash = false;
    EnforcementLevel no_wget_pipe_bash_level = EnforcementLevel::HARD;

    // Go-specific
    bool require_package_main = false;

    // Python-specific
    bool allow_f_strings = true;
    bool allow_walrus = true;
    int max_string_length = 0;
};

struct LanguagesConfig {
    std::unordered_set<std::string> allowed;
    std::unordered_set<std::string> blocked;
    // Languages that appeared in BOTH lists as written. The parser resolves the
    // conflict immediately (blocked wins, fail-closed) by erasing them from
    // `allowed`, so by the time any check runs the intersection is empty by
    // construction — which made CONTRA-007 dead code testing for a state its own
    // loader guarantees cannot exist. Recorded here so the operator can still be
    // told their config contradicts itself; the resolution and the report are not
    // in conflict.
    std::unordered_set<std::string> allowed_and_blocked;
    bool require_explicit = false;
    std::unordered_map<std::string, LanguageConfig> per_language;
};

// ============================================================================
// Section 2: Capabilities (Resource Access Control)
// ============================================================================

struct NetworkCapability {
    bool enabled = true;
    std::string rationale;   // WHY this capability is enabled/disabled
    bool http_allowed = true;
    bool https_only = false;
    std::vector<std::string> allowed_hosts;
    std::vector<std::string> blocked_hosts;
    std::vector<int> allowed_ports;
    int max_request_size = 0;
    int max_response_size = 0;
    bool allow_websockets = true;
    bool allow_raw_sockets = true;
};

struct FilesystemCapability {
    std::string mode = "write";  // "none", "read", "write"
    std::string rationale;
    std::vector<std::string> allowed_paths;
    std::vector<std::string> blocked_paths;
    std::vector<std::string> allowed_extensions;
    std::vector<std::string> blocked_extensions;
    int max_file_size = 0;
    int max_files = 0;
    bool allow_symlinks = true;
    bool allow_hidden_files = true;
    bool allow_absolute_paths = true;
};

struct ShellCapability {
    bool enabled = true;
    std::string rationale;
    std::vector<std::string> allowed_commands;
    std::vector<std::string> blocked_commands;
    bool allow_pipes = true;
    bool allow_redirects = true;
    bool allow_backgrounding = true;
    int max_execution_time = 0;
};

struct EnvVarsCapability {
    std::string rationale;
    bool read = true;
    bool write = true;
    std::vector<std::string> allowed_read;
    std::vector<std::string> blocked_read;
    std::vector<std::string> allowed_write;
    std::vector<std::string> blocked_write;
    // V-SC-006-ext: Polyglot subprocess environment scrubbing.
    // Controls which parent env vars are inherited by polyglot subprocesses.
    // "blocklist" (default): scrub NAAb secrets + blocked_subprocess_prefixes
    // "allowlist": only pass essential system vars + allowed_subprocess_vars
    std::string subprocess_scrub_mode;  // "blocklist" or "allowlist"
    std::vector<std::string> blocked_subprocess_prefixes;  // e.g., "AWS_", "OPENAI_"
    std::vector<std::string> blocked_subprocess_vars;  // exact var names
    std::vector<std::string> allowed_subprocess_vars;  // allowlist mode: explicitly allowed
};

struct ProcessCapability {
    std::string rationale;
    bool spawn = true;
    bool signals = true;
    int max_processes = 0;
    bool allow_daemon = true;
};

struct TimeCapability {
    bool allow_sleep = true;
    int max_sleep_seconds = 0;
    bool allow_timers = true;
};

struct MemoryCapability {
    int max_allocation_mb = 0;
    bool allow_mmap = true;
    bool allow_shared_memory = true;
};

struct CapabilitiesConfig {
    NetworkCapability network;
    FilesystemCapability filesystem;
    ShellCapability shell;
    EnvVarsCapability env_vars;
    ProcessCapability process;
    TimeCapability time;
    MemoryCapability memory;
};

// ============================================================================
// Section 3: Resource Limits
// ============================================================================

struct TimeoutLimits {
    int global = 0;
    int per_block = 0;
    int total_polyglot = 0;
};

struct MemoryLimits {
    int per_block_mb = 0;
    int total_mb = 0;
};

struct ExecutionLimits {
    int call_depth = 0;
    int loop_iterations = 0;
    int polyglot_blocks = 0;
    int parallel_blocks = 0;
    int total_executions = 0;
};

struct DataLimits {
    int array_size = 0;
    int dict_size = 0;
    int string_length = 0;
    int nesting_depth = 0;
    int output_size = 0;
    int input_size = 0;
};

struct CodeLimits {
    int max_lines_per_block = 0;
    int max_total_polyglot_lines = 0;
    int max_functions = 0;
    int max_variables = 0;
    int max_nesting_depth = 0;
};

struct RateLimits {
    int max_polyglot_per_second = 0;
    int max_stdlib_calls_per_second = 0;
    int max_file_ops_per_second = 0;
    int cooldown_on_limit_ms = 100;
};

struct LimitsConfig {
    TimeoutLimits timeout;
    MemoryLimits memory;
    ExecutionLimits execution;
    DataLimits data;
    CodeLimits code;
    RateLimits rate;
};

// ============================================================================
// Section 4: Requirements
// ============================================================================

struct RequirementRule {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::HARD;
    std::string message;
    std::string rationale;
};

struct ErrorHandlingRequirement : RequirementRule {
    bool require_try_catch = true;
    bool require_catch_body = true;
};

struct NamingConventions {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::ADVISORY;
    std::string variables = "snake_case";
    std::string functions = "snake_case";
    std::string constants = "UPPER_SNAKE_CASE";
    bool check_naab_code = true;
    bool check_polyglot_code = false;
};

struct NoGlobalState {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::ADVISORY;
    bool allow_global_constants = true;
    bool allow_global_functions = true;
    bool block_global_variables = true;
};

struct DocumentationRequirement {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::ADVISORY;
    bool require_file_comment = false;
    bool require_function_comment = false;
    bool require_main_comment = false;
};

struct RequirementsConfig {
    RequirementRule main_block;
    ErrorHandlingRequirement error_handling;
    RequirementRule strict_types;
    NoGlobalState no_global_state;
    NamingConventions naming_conventions;
    DocumentationRequirement documentation;
    RequirementRule version_pinning;
};

// ============================================================================
// Section 5: Restrictions
// ============================================================================

struct PolyglotOutputRestriction {
    std::string format = "any";  // "any", "json", "string"
    int max_size = 0;
    bool require_structured = false;
    bool validate_json = false;
};

struct DangerousCallsRestriction {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::HARD;
    std::string rationale;
    std::vector<std::string> allowlist;
    std::vector<std::string> blocklist_extra;
    bool check_chained_calls = false;
    bool check_string_formatting = false;
};

struct ShellInjectionRestriction {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::HARD;
    std::string rationale;
    std::vector<std::string> patterns;
    std::vector<std::string> allowlist;
    bool check_variable_expansion = false;
    bool require_quoting = false;
};

struct ImportsRestriction {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::SOFT;
    std::string rationale;
    std::string mode = "blocklist";
    std::unordered_map<std::string, std::vector<std::string>> blocked;  // lang -> imports
    std::unordered_map<std::string, std::vector<std::string>> allowed;  // lang -> imports
};

struct DataExfiltrationRestriction {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::HARD;
    std::string rationale;
    bool block_base64_encode_secrets = true;
    bool block_hex_encode_secrets = true;
    bool block_url_encode_secrets = true;
    bool block_network_exfil = true;
    bool block_socket_exfil = true;
    bool block_encoding_chains = true;
    int max_encoded_output_length = 0;
    std::vector<std::string> patterns;  // user override — replaces defaults if non-empty
};

struct ResourceAbuseRestriction {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::HARD;
    std::string rationale;
    bool block_fork_bomb = true;
    bool block_infinite_loops = false;
    EnforcementLevel infinite_loops_level = EnforcementLevel::ADVISORY;
    bool block_recursive_file_ops = true;
    bool block_disk_filling = true;
    int max_temp_files = 0;
    int max_temp_file_size = 0;
};

struct PrivilegeEscalationRestriction {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::HARD;
    std::string rationale;
    bool block_sudo = true;
    bool block_su = true;
    bool block_chmod_suid = true;
    bool block_setuid = true;
    bool block_capability_changes = true;
};

struct InfoDisclosureRestriction {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::SOFT;
    std::string rationale;
    bool block_env_dump = true;
    bool block_process_listing = true;
    bool block_system_info_leak = true;
    bool block_directory_listing = false;
    EnforcementLevel directory_listing_level = EnforcementLevel::ADVISORY;
    bool block_error_stack_traces = false;
};

struct CodeInjectionRestriction {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::HARD;
    std::string rationale;
    bool block_dynamic_code_gen = true;
    bool block_template_injection = true;
    bool block_sql_injection_patterns = true;
    bool block_xpath_injection = true;
    bool block_ldap_injection = true;
    bool block_command_injection = true;
};

struct CryptoRestriction {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::ADVISORY;
    std::string rationale;
    bool block_weak_hashing = true;
    std::vector<std::string> weak_hashes;      // e.g., "md5", "sha1"
    bool block_weak_encryption = true;
    std::vector<std::string> weak_ciphers;     // e.g., "des", "rc4"
    bool block_hardcoded_keys = true;
    bool block_hardcoded_ivs = true;
    bool require_secure_random = false;
    EnforcementLevel secure_random_level = EnforcementLevel::ADVISORY;
};

struct VcsSecretExtractionRestriction {
    bool enabled = true;                                // on by default
    EnforcementLevel level = EnforcementLevel::SOFT;    // 3/4 signals = ADVISORY, 4/4 = SOFT
    std::string rationale;
};

struct ObfuscationRestriction {
    bool enabled = true;                                // on by default
    EnforcementLevel level = EnforcementLevel::SOFT;    // co-occurrence scoring: 2 signals = ADVISORY, 3+ = configured level
    std::string rationale;
};

struct RestrictionsConfig {
    PolyglotOutputRestriction polyglot_output;
    DangerousCallsRestriction dangerous_calls;
    ShellInjectionRestriction shell_injection;
    ImportsRestriction imports;
    DataExfiltrationRestriction data_exfiltration;
    ResourceAbuseRestriction resource_abuse;
    PrivilegeEscalationRestriction privilege_escalation;
    InfoDisclosureRestriction information_disclosure;
    CodeInjectionRestriction code_injection;
    CryptoRestriction crypto;
    VcsSecretExtractionRestriction vcs_secret_extraction;
    ObfuscationRestriction obfuscation;
};

// ============================================================================
// Section 6: Code Quality
// ============================================================================

struct PatternWithSeverity {
    std::string pattern;
    std::string severity;  // "critical", "high", "medium", "low"
};

struct EntropyCheckConfig {
    bool enabled = false;
    double threshold = 4.5;
    int min_length = 20;
    int max_length = 500;
    bool check_base64 = true;
    bool check_hex = true;
    bool check_url_encoded = true;
    bool ignore_comments = false;
    bool ignore_urls = true;
};

struct SuspiciousVariableNames {
    bool enabled = false;
    std::vector<std::string> names;
};

struct NoSecretsConfig {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::HARD;
    std::string rationale;
    std::unordered_map<std::string, PatternWithSeverity> patterns;
    std::vector<PatternWithSeverity> custom_patterns;
    std::vector<std::string> allowlist;
    EntropyCheckConfig entropy_check;
    SuspiciousVariableNames suspicious_variable_names;
};

struct NoPlaceholdersConfig {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::SOFT;
    std::string rationale;
    std::vector<std::string> markers;
    std::vector<std::string> custom_markers;
    bool ignore_in_comments_only = false;
    bool case_sensitive = false;
    int max_violations_before_block = 0;
};

struct NoHardcodedResultsConfig {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::ADVISORY;
    std::string rationale;
    bool check_return_true_false = true;
    bool check_return_none_null = true;
    bool check_return_empty_collections = true;
    bool check_dict_success_fields = true;
    bool check_dict_status_fields = true;
    bool check_perfect_scores = true;
    bool check_zero_counts = true;
    bool check_round_numbers = true;
    std::unordered_map<std::string, PatternWithSeverity> patterns;
};

struct NoPiiConfig {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::ADVISORY;
    std::string rationale;
    bool detect_ssn = true;
    bool detect_credit_card = true;
    bool detect_email = true;
    bool detect_phone = true;
    bool detect_ip_address = false;
    bool detect_drivers_license = false;
    bool detect_passport = false;
    bool detect_iban = false;
    bool detect_medical_record = false;
    std::vector<PatternWithSeverity> custom_pii_patterns;
    std::vector<std::string> allowlist_patterns;
    bool mask_in_errors = true;
    std::unordered_map<std::string, PatternWithSeverity> pii_patterns;
};

struct NoTemporaryCodeConfig {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::SOFT;
    std::string rationale;
    std::vector<std::string> patterns;
    std::vector<std::string> custom_patterns;
    bool case_sensitive = false;
};

struct NoSimulationMarkersConfig {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::HARD;
    std::string rationale;
    std::vector<std::string> patterns;
    std::vector<std::string> custom_patterns;
    bool case_sensitive = false;
};

struct NoMockDataConfig {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::ADVISORY;
    std::string rationale;
    std::vector<std::string> variable_prefixes;
    std::vector<std::string> function_prefixes;
    std::vector<std::string> literal_patterns;
    std::vector<std::string> custom_prefixes;
    std::vector<std::string> custom_patterns;
    bool ignore_in_test_context = true;
};

struct NoOversimplificationConfig {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::SOFT;
    std::string rationale;
    std::vector<std::string> patterns;
    std::vector<std::string> custom_patterns;
    bool check_empty_bodies = true;
    bool check_trivial_returns = true;
    bool check_identity_functions = true;
    bool check_not_implemented = true;
    bool check_comment_only_bodies = true;
    bool check_fabricated_results = true;
    bool case_sensitive = false;
    int min_function_lines = 2;
};

struct NoIncompleteLogicConfig {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::SOFT;
    std::string rationale;
    std::vector<std::string> patterns;
    std::vector<std::string> custom_patterns;
    bool check_empty_catch = true;
    bool check_swallowed_exceptions = true;
    bool check_generic_errors = true;
    bool check_vague_error_messages = true;
    bool check_single_iteration_loops = true;
    bool check_bare_raise = true;
    bool check_always_true_false = true;
    bool check_missing_validation = true;
    bool case_sensitive = false;
    std::vector<std::string> suppressions;  // "category:file_glob" patterns to suppress
};

struct NoHallucinatedApisConfig {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::ADVISORY;
    std::string rationale;
    std::vector<std::string> python_patterns;
    std::vector<std::string> javascript_patterns;
    std::vector<std::string> shell_patterns;
    std::vector<std::string> go_patterns;
    std::vector<std::string> ruby_patterns;
    std::vector<std::string> cross_language_patterns;
    std::vector<std::string> custom_patterns;
    bool check_cross_language = true;
    bool check_made_up_functions = true;
    bool check_wrong_syntax = true;
    bool case_sensitive = true;
};

struct ApologeticCategory {
    std::vector<std::string> patterns;
    std::string severity;
};

struct NoApologeticLanguageConfig {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::ADVISORY;
    std::string rationale;
    std::unordered_map<std::string, ApologeticCategory> categories;
    std::vector<std::string> custom_patterns;
    bool scan_comments_only = true;
    bool scan_strings = false;
};

struct NoDeadCodeConfig {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::ADVISORY;
    std::string rationale;
    bool detect_unreachable_after_return = true;
    bool detect_always_true_conditions = true;
    bool detect_always_false_conditions = true;
    bool detect_empty_except_blocks = true;
    bool detect_unused_variables = false;
    bool detect_commented_out_code = true;
    std::vector<std::string> patterns;
};

struct NoDebugArtifactsConfig {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::SOFT;
    std::string rationale;
    std::vector<std::string> patterns;
    std::vector<std::string> custom_patterns;
    std::vector<std::string> allowlist;
    bool check_polyglot_only = true;
    bool check_naab_code = false;
};

struct NoUnsafeDeserializationConfig {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::HARD;
    std::string rationale;
    std::vector<std::string> patterns;
};

struct NoSqlInjectionConfig {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::HARD;
    std::string rationale;
    std::vector<std::string> patterns;
    std::vector<std::string> allowlist;
};

struct NoPathTraversalConfig {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::HARD;
    std::string rationale;
    std::vector<std::string> patterns;
};

struct NoHardcodedUrlsConfig {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::ADVISORY;
    std::string rationale;
    std::vector<std::string> patterns;
    std::vector<std::string> allowlist;
    bool check_internal_urls = false;
};

struct NoHardcodedIpsConfig {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::ADVISORY;
    std::string rationale;
    std::vector<std::string> patterns;
    std::vector<std::string> allowlist;
};

struct MaxComplexityConfig {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::ADVISORY;
    std::string rationale;
    int max_lines_per_block = 0;
    int max_nesting_depth = 0;
    int max_parameters = 0;
    int max_local_variables = 0;
    int max_cyclomatic_complexity = 0;
    int max_cognitive_complexity = 0;
};

struct EncodingConfig {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::ADVISORY;
    std::string rationale;
    bool require_utf8 = true;
    bool block_null_bytes = true;
    bool block_control_characters = true;
    bool block_bom = false;
    bool block_unicode_bidi = true;
    bool block_homoglyph_attacks = true;
};

// Complexity floor: minimum structural complexity for functions
struct ComplexityFloorRule {
    std::vector<std::string> names;       // Substring match against function name
    int min_score = 10;                   // Minimum complexity score (0-100)
    bool require_branching_or_loops = false;
    std::string message;                  // Custom error message (optional)
};

struct ComplexityFloorConfig {
    bool enabled = false;                  // Only active when explicitly configured
    EnforcementLevel level = EnforcementLevel::SOFT;
    std::string rationale;
    int min_score = 10;                    // Global minimum
    int min_lines_for_check = 0;           // Skip floor for functions shorter than N lines (0 = disabled)
    bool check_polyglot = true;
    bool check_naab = true;
    bool skip_if_has_polyglot_block = true;
    std::vector<ComplexityFloorRule> rules; // Name-specific rules
    // Complexity scoring weights — control how structural features map to score.
    // Score = sum of (feature × weight), capped at 100. When custom_weights is true,
    // checkComplexityFloor re-scores the profile using these values instead of the
    // SyntacticAnalyzer defaults. Rationale for defaults documented in syntactic_analyzer.cpp.
    struct ComplexityWeights {
        bool custom = false;               // false = use SyntacticAnalyzer defaults
        int loop = 5;                      // per real loop (excl. padding)
        int padding_loop = 1;              // per padding loop (small-range, minimal credit)
        int nested_loops = 15;             // bonus if nested loops detected
        int large_iterations = 20;         // bonus for 1M+ iteration range
        int function = 3;                  // per function definition
        int recursion = 10;                // bonus for recursive calls
        int array_ops = 5;                 // map/filter/reduce operations
        int pipeline = 3;                  // per |> stage (capped at 5 stages = 15)
        int pipeline_cap = 15;             // max pipeline contribution
        int comprehension = 5;             // list/dict comprehension
        int memory_alloc = 10;             // new/malloc
        int lifetime = 10;                 // delete/free
        int pointers = 15;                 // raw pointer usage
        int try_catch = 5;                 // try/catch block
        int error_propagation = 5;         // ? operator, Result types
        int import = 2;                    // per imported module
        int external_call = 1;             // per external function call
    } weights;
};

struct DuplicateCallsConfig {
    bool enabled = true;
    int threshold = 3;           // Warn at N+ calls per function (was hardcoded 2)
    int max_entries = 5;         // Max unique calls to show in grouped output
    std::string rationale;
};

struct PolyglotTryCatchConfig {
    bool enabled = true;         // Also requires no_incomplete_logic.enabled for backward compat
    int max_entries = 3;         // Max functions to list in grouped output
};

struct IntentValidationConfig {
    bool enabled = false;
    bool required = false;         // Missing @intent is a violation
    EnforcementLevel level = EnforcementLevel::SOFT;            // Mismatch level
    std::string rationale;
    EnforcementLevel missing_level = EnforcementLevel::ADVISORY; // Missing level
    std::string mode = "hybrid";   // "static", "agent", "hybrid"
    int min_function_lines = 3;    // Skip tiny functions
    std::vector<std::string> exempt_functions;
    // Owner-defined intents (ground truth from govern.json)
    std::string project_intent;    // Broad project purpose — freeform text
    std::unordered_map<std::string, std::string> function_intents; // Per-function requirements
};

struct SemanticChecksConfig {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::ADVISORY;
    std::string rationale;
    bool check_imports = true;           // Validate imports against known modules
    bool check_api_signatures = true;    // Validate common API call patterns
    bool check_shell_syntax = true;      // Shell-specific syntax validation
    bool check_dangerous_eval = true;    // eval/exec with dynamic strings
};

struct CodeQualityConfig {
    NoSecretsConfig no_secrets;
    NoPlaceholdersConfig no_placeholders;
    NoHardcodedResultsConfig no_hardcoded_results;
    NoPiiConfig no_pii;
    NoTemporaryCodeConfig no_temporary_code;
    NoSimulationMarkersConfig no_simulation_markers;
    NoMockDataConfig no_mock_data;
    NoApologeticLanguageConfig no_apologetic_language;
    NoDeadCodeConfig no_dead_code;
    NoDebugArtifactsConfig no_debug_artifacts;
    NoUnsafeDeserializationConfig no_unsafe_deserialization;
    NoSqlInjectionConfig no_sql_injection;
    NoPathTraversalConfig no_path_traversal;
    NoHardcodedUrlsConfig no_hardcoded_urls;
    NoHardcodedIpsConfig no_hardcoded_ips;
    MaxComplexityConfig max_complexity;
    EncodingConfig encoding;
    NoOversimplificationConfig no_oversimplification;
    NoIncompleteLogicConfig no_incomplete_logic;
    NoHallucinatedApisConfig no_hallucinated_apis;
    ComplexityFloorConfig complexity_floor;
    DuplicateCallsConfig duplicate_calls;
    PolyglotTryCatchConfig polyglot_try_catch;
    SemanticChecksConfig semantic_checks;
    IntentValidationConfig intent_validation;

    // Drift detection: block execution when a rewrite loses significant functionality
    struct DriftDetectionConfig {
        bool enabled = false;
        EnforcementLevel level = EnforcementLevel::HARD;
        std::string rationale;
        std::string baseline_path = ".naab/drift-baseline.json";
        double max_function_loss = 0.5;   // Block if >50% functions disappear
        double max_loc_loss = 0.6;        // Block if >60% LOC disappears
        double max_export_loss = 0.0;     // Block if ANY export disappears
        double max_struct_loss = 0.5;     // Block if >50% structs disappear
        double max_function_gain = 0.5;   // Block if >50% more functions than baseline
        bool auto_save = false;           // If true, auto-update baseline after pass
        // Gate 1: Signature stability
        bool check_signatures = true;
        double max_param_loss = 0.5;
        // Gate 2: Import regression
        bool check_imports = true;
        double max_import_loss = 0.5;
        // Gate 3: Complexity regression (per-function)
        bool check_complexity = true;
        double max_complexity_loss = 0.6;
        int min_complexity_baseline = 10;  // Skip trivial functions below this score
        // Gate 4: Comment inflation
        bool check_comment_ratio = true;
        double max_comment_ratio = 0.5;
        double max_comment_only_ratio = 0.7;  // Independent check — no code loss required
        // Gate 5: Dead export gate (no baseline needed)
        bool check_hollow_exports = true;
        int min_hollow_export_complexity = 5;  // Exports below this score with params are hollow
        // Gate 6: Polyglot regression
        bool check_polyglot = true;
        double max_polyglot_loss = 0.5;
        // Gate 7: Struct field stability
        bool check_struct_fields = true;
        double max_field_loss = 0.5;
        // Gate 8: Test function regression
        bool check_test_functions = true;
        double max_test_loss = 0.0;
        // Gate 9: Function name stability — block when baseline function names disappear
        bool check_function_names = true;
        double max_function_name_loss = 0.5;
        // Gate 10: Baseline tamper protection — fail-closed when baseline is missing
        bool require_baseline = false;
        // Gate 11: Function body hash — detect rewrites that game structural metrics
        bool check_body_hash = true;
        // Gate 12: Parameter utilization — detect functions that ignore their inputs
        bool check_param_utilization = true;
        double min_param_utilization = 0.5;  // At least 50% of params must be referenced
        // Gate 13: Config presence — fail-closed if govern.json removed since baseline
        bool check_config_presence = false;
        // Gate 14: Script location — block execution from unexpected directories
        bool check_script_location = false;
        // Gate 16: Signature presence — fail-closed if .sig removed since baseline
        bool check_signature_presence = false;
        // Gate 17: Polyglot content regression — detect polyglot block simplification
        bool check_polyglot_content = false;
        double max_polyglot_shrink = 0.5;  // Block if polyglot LOC drops >50%
        // Gate 18: New function detection — flag functions added since baseline
        bool check_new_functions = false;  // opt-in (like gates 13-17)
    } drift_detection;
};

// ============================================================================
// Section 7: Custom Rules
// ============================================================================

struct CustomRule {
    std::string id;
    std::string name;
    std::string description;
    std::string rationale;     // WHY this rule exists at its enforcement tier
    std::string pattern;       // regex
    std::vector<std::string> languages;  // empty = all
    EnforcementLevel level = EnforcementLevel::HARD;
    std::string message;
    std::string help;
    std::string good_example;
    std::string bad_example;
    std::vector<std::string> tags;
    bool enabled = true;
    bool case_sensitive = false;
    bool multiline = false;
    std::regex compiled_pattern;
    bool pattern_valid = false;
};

// ============================================================================
// Section 7b: Governance Plugins (NAAb-based custom checks)
// ============================================================================

struct GovernancePluginRule {
    std::string id;               // e.g. "SEC-001"
    std::string function_name;    // NAAb function to call (must be exported)
    std::string description;
    std::string rationale;
    EnforcementLevel level = EnforcementLevel::HARD;
    std::vector<std::string> languages;  // empty = all languages
    std::string trigger;          // "polyglot_block", "naab_function", "polyglot_output"
    std::string message;          // default message if plugin doesn't provide one
    std::string help;
    std::string good_example;
    std::string bad_example;
    bool enabled = true;
};

struct GovernancePlugin {
    std::string file_path;        // relative to govern.json directory
    std::vector<GovernancePluginRule> rules;
    bool loaded = false;
};

// ============================================================================
// Section 8: Scope-Based Overrides
// ============================================================================

struct ScopeOverride {
    std::string glob_pattern;
    // Store as JSON-like key-value pairs for flexible override
    // e.g., "code_quality.no_placeholders" -> "false"
    std::unordered_map<std::string, std::string> overrides;
};

// ============================================================================
// Section 9: Output Configuration
// ============================================================================

struct SummaryConfig {
    bool enabled = true;
    std::string format = "detailed";  // "compact", "detailed", "minimal"
    bool show_passing = true;
    bool show_skipped = false;
    std::string group_by = "category";  // "category", "severity", "rule"
    std::string sort_by = "severity";
};

struct ErrorOutputConfig {
    bool verbose = true;
    bool show_line_preview = true;
    int show_code_context = 3;
    bool show_help = true;
    bool show_examples = true;
    bool show_rule_path = true;
    bool show_fix_suggestions = true;
    int max_errors_per_rule = 5;
    int max_total_errors = 50;
    int truncate_long_lines = 120;
};

struct FormattingConfig {
    bool color = true;
    bool unicode_symbols = true;
    int width = 80;
    int indent = 2;
};

struct FileOutputConfig {
    std::string report_json;   // path or empty
    std::string report_sarif;
    std::string report_junit;
    std::string report_csv;
    std::string report_html;
};

struct OutputConfig {
    SummaryConfig summary;
    ErrorOutputConfig errors;
    FormattingConfig formatting;
    FileOutputConfig file_output;
    int max_advisories = 15;       // Cap total [ADVISORY] messages (0 = unlimited)
    bool advisory_summary = true;  // Show "... and N more" when capped
    std::string voice;             // Agent name for voice synthesis of violations
    bool voice_cache = false;      // Cache voice synthesis results
};

// ============================================================================
// Section 10: Audit Trail & Provenance
// ============================================================================

struct TamperEvidenceConfig {
    bool enabled = false;
    std::string algorithm = "sha256";
    std::string chain_genesis = "NAAB-GOVERNANCE-GENESIS";
    std::string hmac_key;      // HMAC-SHA256 key; empty = plain SHA-256 fallback
    std::string hmac_key_env;  // Env var name to read key from (preferred over literal)
};

struct LogEventsConfig {
    bool checks_passed = true;
    bool checks_failed = true;
    bool checks_warned = true;
    bool overrides = true;
    bool config_loaded = true;
    bool execution_start = true;
    bool execution_end = true;
    bool polyglot_executed = true;
    bool polyglot_timing = false;
    bool taint_decisions = false;
    bool contract_checks = false;
};

struct RetentionConfig {
    int max_file_size_mb = 100;
    int rotate_at_mb = 50;
    int keep_rotated = 5;
};

struct ProvenanceConfig {
    bool enabled = false;
    bool record_proof_objects = false;
    bool record_attestations = false;
    bool record_decisions = false;
    bool sign_records = false;
    std::string signing_key;
    std::string signing_key_env;  // env var for signing key path (preferred)
};

struct AuditConfig {
    std::string level = "none";  // "none", "basic", "full"
    std::string output_file = ".governance-audit.jsonl";
    TamperEvidenceConfig tamper_evidence;
    LogEventsConfig log_events;
    RetentionConfig retention;
    ProvenanceConfig provenance;
};

// ============================================================================
// Section 11: Meta-Rules
// ============================================================================

struct SchemaValidationConfig {
    bool warn_unknown_keys = true;
    bool suggest_corrections = true;
    bool strict_types = true;
};

struct ConfigImmutabilityConfig {
    std::string hash;
    bool verify_on_load = false;
    bool block_on_mismatch = false;
};

struct InheritanceConfig {
    int max_depth = 5;
    std::string merge_strategy = "child_wins";  // "child_wins", "parent_wins"
    std::string merge_arrays = "replace";        // "replace", "append"
    bool allow_circular = false;
};

struct FeatureFlagsConfig {
    bool experimental_checks = false;
    bool legacy_compatibility = true;
    bool verbose_parsing = false;
};

struct EnvironmentConfig {
    bool allow_env_var_substitution = false;
    std::string env_prefix = "NAAB_GOV_";
    bool allow_cli_override = true;
};

struct MetaConfig {
    SchemaValidationConfig schema_validation;
    ConfigImmutabilityConfig config_immutability;
    InheritanceConfig inheritance;
    FeatureFlagsConfig feature_flags;
    EnvironmentConfig environment;

    // Permit a mid-run reload to introduce an agent that did not exist before.
    //
    // Default false, because the ratchet's rule is that mid-run changes may only
    // tighten, and a new identity carrying no per-agent restrictions is not
    // tightening. Without this the two paths disagreed: flipping an existing
    // agent's shell_allowed false -> true is refused, while adding a NEW agent
    // that simply never had the restriction was waved through with a notice —
    // so what the ratchet denied to an identity was available by renaming it.
    //
    // The blast radius is bounded and worth stating: a per-agent grant can only
    // restrict BELOW the global capabilities, never exceed them, and global
    // loosening is itself ratcheted. So this closes "re-grant what was withdrawn"
    // rather than an escalation past the envelope.
    //
    // Enabling it mid-run is itself a loosening violation.
    bool allow_agent_addition_mid_run = false;
};

// ============================================================================
// Section 12: Hooks
// ============================================================================

struct HookConfig {
    std::string command;
    std::vector<std::string> args;
    int timeout = 5;
    bool inherit_governance_keys = false;  // V-SC-006: scrub NAAB keys by default
};

struct HooksConfig {
    HookConfig on_violation;
    HookConfig on_override;
    HookConfig on_complete;
    HookConfig pre_check;
    HookConfig post_check;
};

// ============================================================================
// Section 12c: Approval Tokens (APPROVAL_REQUIRED tier)
// ============================================================================

struct ApprovalToken {
    std::string approver_id;
    std::string rule_name;
    std::string reason;
    int64_t expiry_timestamp = 0;   // unix epoch seconds, 0 = no expiry
    std::string signature_b64;      // Ed25519 signature over canonical fields
};

struct ApprovalConfig {
    std::string store_path = ".naab/approvals.json";
    std::vector<std::string> approver_keys;  // public key fingerprints
    int default_expiry_hours = 24;
};

// ============================================================================
// Section 12c: Trust Policy (Authority Decay & Key Lifecycle)
// ============================================================================

struct TrustPolicyConfig {
    int max_signature_age_days = 0;     // 0 = no staleness check
    bool require_fresh_signature = false;
    EnforcementLevel stale_signature_level = EnforcementLevel::ADVISORY;
    bool check_key_expiry = true;
    bool check_revocation = true;
};

// ============================================================================
// Section 12d: Environment Attestation (Prerequisites)
// ============================================================================

struct PrerequisiteCheck {
    std::string type;       // "python_version", "package", "tool", "env_var", "command"
    std::string name;       // package name, tool name, env var name
    std::string required;   // version constraint or "exists"
    EnforcementLevel level = EnforcementLevel::ADVISORY;
    std::string message;
};

struct PrerequisitesConfig {
    bool enabled = false;
    std::vector<PrerequisiteCheck> checks;
};

struct AttestationResult {
    std::string check_type;
    std::string check_name;
    bool passed = false;
    std::string observed;
    std::string required;
    std::string message;
    EnforcementLevel level = EnforcementLevel::ADVISORY;
};

// ============================================================================
// Section 12e: Contradiction Detection
// ============================================================================

struct ContradictionResult {
    std::string pattern_id;     // "CONTRA-001"
    std::string description;
    EnforcementLevel level;
    std::string resolution;
};

struct ContradictionDetectionConfig {
    bool enabled = true;        // on by default (cheap static analysis)
    EnforcementLevel max_level = EnforcementLevel::ADVISORY;
};

// ============================================================================
// Section 12b: Quality Gate
// ============================================================================

struct QualityGateCondition {
    std::string metric;     // "hard_violations", "soft_violations", etc.
    std::string op;         // ">", ">=", "<", "<=", "=="
    int threshold = 0;
};

struct QualityGateConfig {
    bool enabled = false;
    std::vector<QualityGateCondition> conditions;
};

// ============================================================================
// Section 12b: Cumulative Risk Scoring
// ============================================================================

struct ScoringConfig {
    bool enabled = false;
    std::string rationale;      // WHY these scoring thresholds were chosen
    int default_weight = 3;     // weight for any advisory finding without a rule override
    std::unordered_map<std::string, int> rule_weights;  // per-rule weight overrides
    int green_threshold  = 0;   // below: silent
    int yellow_threshold = 10;  // at/above: enhanced warning with breakdown
    int red_threshold    = 25;  // at/above: block via quality gate (exit 2)
    std::string threshold_mode = "fixed";  // "fixed" or "per_source"
};

// ============================================================================
// Scoring Calibration — operator-driven weight overrides
// ============================================================================

struct ScoringCalibrationEntry {
    int weight_override = -1;       // -1 = no override, use config weight
    std::string reason;              // operator's rationale
    std::string updated_at;          // ISO timestamp
    int observation_count = 0;       // times operator reviewed this rule
};

struct ScoringCalibrationConfig {
    bool enabled = false;
    std::string rationale;
    std::string path = ".naab/scoring-calibration.json";
    bool auto_save = true;
};

// ============================================================================
// Behavioral Sequence Detection — config structs
// ============================================================================

struct SequenceStep {
    std::vector<std::string> match_any;        // OR-list: ["env.get", "file.read"]
    std::vector<std::string> detail_globs;     // parallel glob per matcher ("" = any)
    // detail_globs[i] applies to match_any[i]; use "type:glob|type2:glob2" in govern.json
};

struct SequencePattern {
    std::string name;                     // "data_exfiltration"
    std::vector<SequenceStep> steps;      // ordered steps
    int max_gap = 10;                     // max events between consecutive step matches
    int decay_seconds = 300;              // events older than this don't count
    int decay_turns = 20;                 // events older than N turns don't count
    EnforcementLevel level = EnforcementLevel::SOFT;
    std::string rationale;
    bool cross_agent = false;             // if true, pattern only matches when events span 2+ agents
};

struct BehavioralSequenceConfig {
    bool enabled = false;
    std::string rationale;
    size_t window_size = 200;             // max events in ring buffer
    std::vector<SequencePattern> patterns;
};

struct ContextDriftConfig {
    bool enabled = false;
    std::string rationale;
    EnforcementLevel level = EnforcementLevel::ADVISORY;
    // Rationale: 0.5 = midpoint of [0,1] range — agent starts with benefit of the doubt (1.0)
    // Coherence below this value triggers enforcement. With content-aware CDD (Phase 1a),
    // well-behaved agents maintain ~0.9; degraded agents drop to 0.6-0.7. Threshold at 0.7
    // means composite pressure starts contributing when coherence dips below 0.7.
    double coherence_threshold = 0.7;
    int max_contradictions = 5;
    int check_interval_turns = 3;
    int fingerprint_window = 20;
    bool rate_normalized = false;  // F5: scale penalties by rate (count/turns) instead of flat weight
    // Rationale: floor on the rate-normalized penalty as a fraction of the base
    // weight. Without it, an adversary who knows the rate formula can pace drift
    // (one deviation every N turns) so dilution drives each penalty toward zero —
    // the boiling-frog attack. A firing signal always pays at least this fraction.
    double rate_normalized_floor = 0.5;
    // Adaptive-absorption cap: a signal that keeps firing but is fully absorbed
    // by the learned baseline (penalty 0) starts paying base_penalty after this
    // many CONSECUTIVE absorbed post-baseline checks. 0 = unlimited absorption
    // (historical behavior). Distinguishes persistent drift the baseline
    // normalized from occasional absorbed noise; structurally-firing signals on
    // by-design agents (JSON-output operator vs English-keyword mandate) should
    // instead be disabled per-agent via context_drift_signals. Ratchet: raising
    // or removing the limit mid-run is a loosening violation.
    int adaptive_absorption_limit = 0;
    // Rationale: 0.2 = partial recovery (20% of full coherence). Prevents full reset after a single
    // correct action, requiring sustained good behavior to fully recover. Analogous to credit
    // restoration: recent good conduct helps but doesn't erase history immediately.
    double coherence_recovery_amount = 0.2;
    double coherence_recovery_cap = 1.0;     // Maximum coherence after recovery (< 1.0 for diminishing returns)
    double coherence_natural_healing = 0.0;  // F15: per-turn recovery when no signals fire (0 = disabled)
    // Rationale: half the S22 validation_outcome weight (0.15), so an agent that
    // eventually fixes failing code can climb back out of the penalty loop, while
    // oscillating fail/pass stays net-negative. Credited only on a fail→pass
    // transition (credits <= failures — recording passes cannot pump coherence).
    // Ratchet: raising mid-run is a loosening violation.
    double validation_recovery_amount = 0.075;
    // Temporal trust decay — coherence erodes over time even when idle
    bool temporal_decay_enabled = false;
    // Rationale: 0.01/min = 100 minutes from full coherence (1.0) to zero if completely idle.
    // Slow enough that brief pauses don't trigger, fast enough that abandoned sessions decay
    // within a reasonable window. Grace period prevents penalizing normal inter-turn latency.
    double temporal_decay_per_minute = 0.01;
    double temporal_decay_grace_minutes = 1.0;
    // Adaptive baselining — observe normal behavior before penalizing deviations
    bool adaptive_baseline_enabled = false;
    int adaptive_baseline_window = 5;           // turns to observe before penalizing
    double adaptive_baseline_sensitivity = 2.0; // k*stddev threshold above mean
    // Escalation effectiveness — measure post-escalation coherence over a window
    int escalation_effectiveness_window = 5;     // turns after escalation to measure effectiveness
    struct Signals {
        bool repeated_failures = true;
        bool circular_actions = true;
        bool scope_creep = true;
        bool intent_contradictions = true;
        bool vocabulary_contraction = true;
        bool coherence_velocity = true;       // F1: fire when coherence decaying rapidly
        bool capability_underutilization = false; // F9: fire on sudden late capability use
        bool semantic_stability = true;        // F19: fire on content topic shift
        bool mandate_alignment = true;         // fire when response drifts from system_prompt keywords
        bool response_quality = true;          // fire when content/output ratio is low (agent)
        bool thinking_collapse = true;         // fire when thinking tokens drop >50% from baseline
        bool context_growth = true;            // fire when input tokens exceed baseline by factor
        bool instruction_recall = true;        // fire when agent stops referencing earlier user instructions
        bool plan_drift = true;                // fire when agent diverges from its stated multi-step plan
        bool entity_consistency = true;        // fire when entity-context associations contradict prior turns
        bool instruction_conflict = true;      // fire when user instructions contradict prior instructions
        bool persona_fingerprint = true;       // fire when response style deviates from established baseline
        bool tool_chain_integrity = true;      // fire when agent misrepresents tool results
        bool claim_result_reconciliation = true; // fire when agent misrepresents tool success/failure status
        bool prompt_compliance = true;           // fire when agent complies with off-topic prompts
        bool response_repetition = true;         // fire when agent produces verbatim duplicate responses
        bool validation_outcome = true;          // S22: fire when a turn's output failed external validation (pytest/convergence). Zero-cost until agent.record_validation() feeds a result.
        // S23: fire when a response is degenerate (near-empty: output_tokens below
        // thresholds.response_min_output_tokens). DEFAULT OFF — terse-by-design
        // agents (single-word verdict judges) would fire constantly; under adaptive
        // baselining structural terseness self-absorbs, so configs that enable this
        // should typically also enable adaptive_baseline.
        bool response_degenerate = false;
        bool exclude_infrastructure_errors = true; // exclude API/network errors from repeated_failures signal
    } signals;
    // Weights control how much each signal reduces coherence per occurrence.
    // Rationale: ordered by threat severity. contradiction (0.2) is heaviest because
    // retrying a blocked action via different paths is the strongest drift indicator.
    // scope_creep and vocabulary_contraction (0.15) are moderate — behavioral shifts that
    // may be legitimate exploration. circular (0.1) is lower because simple retries are
    // common in normal operation. repeated_failure (0.05) is lightest because the same
    // error recurring often reflects a stuck state, not malice. Sum ≈ 1.0 ensures a
    // single turn firing all signals would roughly halve coherence from 1.0.
    struct Weights {
        double circular = 0.1;
        double scope_creep = 0.15;
        double contradiction = 0.2;
        double repeated_failure = 0.05;
        double vocabulary_contraction = 0.15;
        double coherence_velocity = 0.12;     // INERT: S6 is detection-only (velocity = last turn's
                                              // net penalty, so a direct penalty double-counts and
                                              // cascades); it escalates via the pressure composite
                                              // instead. Field kept for config compatibility.
        double capability_underutilization = 0.1; // F9: low — may be legitimate deferred usage
        double semantic_stability = 0.1;       // F19: low — topic shifts are common in exploration
        double mandate_alignment = 0.12;       // moderate — mandate drift is a stronger signal than topic shift
        double response_quality = 0.08;        // moderate — low content ratio indicates degradation
        double thinking_collapse = 0.06;       // low-moderate — progressive model capability loss
        double context_growth = 0.07;          // moderate — context bloat degrades all other signals
        double instruction_recall = 0.08;     // moderate — forgetting user instructions is a strong drift signal
        double plan_drift = 0.09;             // moderate-high — diverging from stated plan is a strong drift signal
        double entity_consistency = 0.08;     // moderate — entity confusion indicates state corruption
        double instruction_conflict = 0.10;   // moderate-high — contradictory instructions are a strong confusion signal
        double persona_fingerprint = 0.05;    // low — style shifts are common and often legitimate
        double tool_chain_integrity = 0.08;   // moderate — misrepresenting tool results indicates hallucination
        double claim_result_reconciliation = 0.12; // moderate-high — status misrepresentation is unambiguous hallucination
        double prompt_compliance = 0.10;             // moderate-high — complying with off-topic prompts is unambiguous drift
        double response_repetition = 0.15;           // moderate-high — verbatim repetition is a strong drift indicator
        double validation_outcome = 0.15;            // S22: high — a failed test is unambiguous ground-truth that the output is wrong. Flat penalty (base_penalty), never baseline-absorbed.
        double response_degenerate = 0.08;           // S23: a near-empty response from an agent whose baseline is substantive output.
    } weights;

    // Signal detection thresholds — tune sensitivity of individual CDD signals.
    // Rationale for each default documented inline.
    struct Thresholds {
        // -0.15: a 15% coherence drop in one turn. Normal turns show ≤5% variation;
        // -0.15 is 3x normal noise, filtering jitter while catching real drops.
        double velocity_drop = -0.15;
        // S23: responses below this many output tokens are degenerate (a 1-token
        // "APPROVED" from a reviewer asked for a reasoned verdict). 8 tokens ~= one
        // short sentence fragment — low enough that legitimately concise answers
        // never trip it without baselining.
        int response_min_output_tokens = 8;
        // 3 turns: same fingerprint appearing in 3 consecutive turns is a strong repeat
        // signal. 2 would be too noisy (legitimate retry); 4+ misses fast loops.
        int circular_lookback = 3;
        // 10 turns: a capability unused for 10+ turns after grant suggests the agent
        // didn't need it — possible capability hoarding. Matches the adaptive baseline
        // window default (5 turns × 2 for margin).
        int underutilization_delay = 10;
        // 3 history + 2 new types: requires enough history to distinguish warmup from
        // drift (3 turns), and multiple simultaneous new event types (2+) to avoid
        // false-firing on normal single-capability exploration.
        int scope_creep_min_history = 3;
        int scope_creep_min_new_types = 2;
        // 3 same errors: one error is normal, two could be a retry. Three identical
        // errors indicates a stuck loop. Matches the "three strikes" heuristic used
        // in circuit breaker patterns.
        int repeated_failure_count = 3;
        // 6 turns: need at least 3 turns per half-window (early vs recent) to compute
        // meaningful Shannon entropy. Fewer turns produce unstable entropy estimates.
        int vocab_contraction_window = 6;
        // 0.5 initial entropy: below this the agent's vocabulary was already narrow,
        // so further contraction is meaningless. 0.5 bits ≈ 2 roughly-equal event types.
        // 0.6 ratio: recent entropy dropped to 60% of initial = 40% contraction, a
        // substantial narrowing that filters normal turn-to-turn variation.
        double entropy_min_initial = 0.5;
        double entropy_contraction_ratio = 0.6;
        // 10 entries: enough for velocity/acceleration smoothing over recent history
        // without excessive memory. Matches underutilization_delay for consistency.
        int coherence_history_size = 10;
        // 0.3: fire when <30% of output tokens are content (rest is thinking).
        // Normal responses have 50-80% content; <30% means thinking dominates.
        double response_quality_min_ratio = 0.3;
        // 0.5: fire when mean thinking drops to 50% of baseline. Gradual degradation
        // threshold — aggressive enough to catch collapse, not so tight as to fire on
        // normal variation.
        double thinking_collapse_ratio = 0.5;
        // 20: rolling window size for thinking token history. Matches conversation depth.
        int thinking_history_window = 20;
        // 0.25: Jaccard similarity below 25% = major topic shift. Normal on-topic turns
        // share 30-60% of keywords; 25% catches "todo app" → "tell me jokes" transitions.
        double semantic_stability_min_overlap = 0.25;
        // 0.15: at least 15% of system_prompt keywords should appear in rolling response
        // average. Below this the agent has abandoned its mandate.
        double mandate_alignment_min = 0.15;
        // 3.0: fire when input tokens exceed 3x the baseline mean. Normal context grows
        // gradually (1.5-2x); 3x indicates prompt bloat diluting high-signal tokens.
        double context_growth_factor = 3.0;
        // 0.10: at least 10% of accumulated instruction keywords should appear in responses.
        // Lower than mandate_alignment (0.15) because instructions accumulate over time and
        // not every response needs to reference every instruction.
        double instruction_recall_min = 0.10;
        // 0.20: at least 20% keyword overlap to consider a plan step "referenced".
        // Lower than semantic_stability (0.25) because plan step descriptions are short
        // and a response may address a step without using all its exact words.
        double plan_step_overlap_min = 0.20;
        // 0.25: when a known entity reappears with <25% overlap of its historical context
        // keywords, the agent may be confusing entities or mixing state. Normal variation
        // in how an entity is described stays above 30-40%.
        double entity_context_min_overlap = 0.25;
        // 5: number of recent per-sighting context sets kept per entity. A contradiction
        // means the current context matches NONE of the recent sightings (max Jaccard
        // below entity_context_min_overlap). Bounding the window keeps the comparison
        // frame stable — an unbounded historical union dilutes overlap toward zero for
        // any evolving agent, making the signal structurally inevitable.
        int entity_context_window = 5;
        // 0.40: two instructions sharing >40% topic keywords are about the same subject.
        // Combined with presence of negation markers, indicates a contradiction.
        double instruction_conflict_topic_overlap = 0.40;
        // 10: sliding window size for instruction history
        int instruction_conflict_window = 10;
        // 2.0: fire when response keyword count deviates by more than 2 standard deviations
        // from the baseline mean. Normal variation stays within 1.5 stddev.
        double persona_deviation_factor = 2.0;
        // 20: rolling window of response keyword counts backing the persona
        // baseline. Same default as thinking_history_window, which S17 used to
        // borrow outright — so tuning the THINKING window silently retuned the
        // persona baseline, and the borrowed key's comment claimed it was the
        // coherence history size, which it also is not.
        int persona_history_window = 20;
        // 0.15: at least 15% of tool result keywords should appear in the agent's response
        // when it references that tool. Below this the agent may be fabricating results.
        double tool_result_recall_min = 0.15;
        // 0.70: rolling claim accuracy below 70% triggers escalated penalty.
        // Agent must correctly report tool success/failure at least 70% of the time.
        double claim_accuracy_min = 0.70;
        // 0.10: prompt-to-mandate overlap below 10% = clearly off-topic prompt.
        // System prompt for "todo app" has keywords like "todo", "app", "bugs", "code", "features".
        // A prompt about fibonacci or poetry shares <5% overlap. On-task prompts share 15-40%.
        double prompt_compliance_mandate_min = 0.10;
        // 50: minimum response token count to consider "substantive compliance."
        // Short refusals ("I can only help with todo apps") are typically <50 tokens.
        // Full compliance ("Here's a fibonacci function...") is typically >100 tokens.
        int prompt_compliance_response_min_tokens = 50;
        // 5 responses: check previous 5 responses for exact fingerprint match.
        // Small window avoids stale matches while catching stuck-in-a-loop patterns.
        int response_repetition_lookback = 5;
    } thresholds;

    // Reality Checkpoint: composite operational pressure detection
    struct RealityCheckpoint {
        bool enabled = true;
        std::string rationale;
        EnforcementLevel level = EnforcementLevel::SOFT;
        double pressure_threshold = 0.7;
        int sustained_turns_required = 3;
        int min_turns_between_checkpoints = 5;
        int expected_conversation_depth = 20;
        // Pressure weights: 5 active factors sum to 1.0 by default. Ranked by
        // information value: coherence_proximity (0.35) is the single best predictor
        // of drift; signal_density (0.25) captures multi-signal convergence;
        // risk_score (0.20) incorporates cumulative scoring; conversation_depth (0.10)
        // and bsd_partial_progress (0.10) are weaker contextual signals.
        // Opt-in factors (default 0.0) are domain-specific and need explicit tuning.
        struct PressureWeights {
            double coherence_proximity = 0.35;   // strongest: direct coherence measurement
            double risk_score_proximity = 0.20;  // cumulative violation weight
            double signal_density = 0.25;        // multi-signal convergence in single turn
            double conversation_depth = 0.10;    // longer conversations correlate with drift
            double bsd_partial_progress = 0.10;  // behavioral sequence progress toward match
            double pipeline_inherited = 0.0;     // opt-in: upstream pipeline stage pressure
            double coherence_acceleration = 0.0; // opt-in: second derivative of coherence
            double codegen_pressure = 0.0;       // opt-in: blocked/total codegen ratio
            double bsd_eviction_pressure = 0.0;  // opt-in: evicted/total BSD event ratio
            double semantic_deviation = 0.0;     // opt-in: semantic mandate deviation pressure
        } weights;
        // Scaling factors for pressure normalization.
        // 4.0: maps 4 signals/turn to pressure=1.0. Most turns fire 0-2 signals;
        // 4+ simultaneous signals is a strong anomaly worth full pressure.
        double signal_density_divisor = 4.0;
        // 5.0: maps acceleration of 0.2/turn to pressure=1.0. Normal acceleration
        // is <0.05; 0.2 = rapid coherence change worth maximum pressure.
        double acceleration_multiplier = 5.0;
    } reality_checkpoint;
};

// Exposure Tracking: aggregate autonomous action volume across all agents
struct ExposureTrackingConfig {
    bool enabled = false;
    std::string rationale;
    int max_autonomous_actions = 0;     // 0 = unlimited; advisory when exceeded
    int max_unique_agents = 0;          // 0 = unlimited
    double coherence_floor = 0.0;       // 0.0 = disabled; deny admission below this coherence
    int max_pipeline_depth = 0;          // F3: 0 = unlimited; deny admission when exceeded
    int checkpoint_cooldown_turns = 0;   // F12: mandatory pause turns after reality checkpoint fires
    double min_capability_utilization = 0.0;  // F18: 0.0 = disabled; min |exercised|/|granted|
    int utilization_check_after_turns = 10;   // F18: begin checking after N turns
    EnforcementLevel level = EnforcementLevel::ADVISORY;
};

// F10: Temporal Coupling Detection — suspicious inter-agent timing correlation
struct TemporalCouplingConfig {
    bool enabled = false;
    std::string rationale;
    double max_correlation = 0.85;  // flag if pairwise correlation exceeds this
    int min_events = 10;           // minimum events before computing correlation
    EnforcementLevel level = EnforcementLevel::ADVISORY;
};

// F6: Graduated Circuit Breakers — system-wide governance levels
enum class GovernanceLevel { NORMAL = 0, ELEVATED = 1, HIGH = 2, CRITICAL = 3 };

struct CircuitBreakerConfig {
    bool enabled = false;
    std::string rationale;
    // Thresholds divide the [0,1] pressure range into 4 zones: NORMAL [0,0.4),
    // ELEVATED [0.4,0.6), HIGH [0.6,0.8), CRITICAL [0.8,1.0]. Roughly equal-width
    // bands above 0.4, with NORMAL getting a wider "safe" zone to reduce chatter.
    double elevated_threshold = 0.4;
    double high_threshold = 0.6;
    double critical_threshold = 0.8;
    // Sustained turns required increase with severity: 2→3→4. Higher levels need
    // more evidence before escalating — reducing false positives at the cost of
    // slightly slower response to genuine threats. Mirrors hysteresis in physical
    // circuit breakers (higher current requires longer sustained duration to trip).
    int elevated_sustained = 2;
    int high_sustained = 3;
    int critical_sustained = 4;
    // De-escalation hysteresis: consecutive turns where the freshly computed
    // target level sits BELOW the current level before stepping down ONE level.
    // Without this, a single calm composite sample dropped the level straight
    // to NORMAL — escalation was sustained but de-escalation was instant, so
    // scrutiny disappeared exactly when a decaying agent briefly looked calm.
    int deescalate_sustained = 2;
    // Step-up challenge at elevated governance levels
    bool step_up_enabled = false;
    std::string step_up_at_level = "elevated";
    std::string step_up_challenge = "Before responding, restate your current objective in one sentence, then proceed.";
    // 5 words: minimum response length to ensure the agent actually restates its
    // objective rather than giving a trivial "OK" response.
    int step_up_min_words = 5;
    // 3 turns cooldown: prevents challenge fatigue — too frequent challenges degrade
    // agent performance. Matches pulse cooldown for consistency.
    int step_up_cooldown_turns = 3;
    // Coherence-floor challenge trigger: when true (and output_admissibility is
    // enabled), a handle whose CDD coherence is below the OA threshold draws a
    // step-up challenge on its next send regardless of the engine-global
    // governance level. Closes the sub-OA dead zone where the quarantine
    // streak (kill path) accrues every send while level re-escalation (the
    // normal challenge trigger) needs elevated_sustained pressure samples —
    // the recovery ladder must not be slower than the kill path. Cooldown and
    // max_challenge_failures still apply. Default false (no behavior change).
    // Ratchet: disabling mid-run is a loosening violation.
    bool step_up_on_inadmissible = false;
    // Consecutive failed step-up challenges before the agent is terminated
    // (GovernanceHardError). Below the limit, a failed challenge blocks the
    // send with a CATCHABLE error and the agent is re-challenged after the
    // cooldown — mirrors output_admissibility.max_quarantine_streak, giving
    // challenges the same hysteresis as every other gate instead of a
    // one-strike death penalty (live runs showed agents at coherence 1.0
    // killed for paraphrasing a recall answer). Default 1 preserves the
    // historical one-strike behavior. Ratchet: raising mid-run is loosening.
    int max_challenge_failures = 1;
    // 0.4 = 40% keyword overlap: the agent's challenge response must contain at least
    // 40% of the combined system_prompt + recent user prompt key terms. High enough to
    // catch unrelated or evasive responses, low enough to allow paraphrasing. Minimum
    // enforceable floor: 0.2 (lower values are clamped during config parsing).
    double step_up_keyword_threshold = 0.4;
    // Contextual challenges: when true, challenge prompts are dynamically selected
    // from DriftState data (tool results, plan steps, instructions, entities) rather
    // than using the canned step_up_challenge prompt. Falls back to canned when no
    // richer data is available.
    bool step_up_contextual = false;
    // Lower threshold for contextual challenges — tool/plan keywords are more specific
    // than system_prompt keywords, so a lower overlap bar is appropriate.
    double step_up_contextual_threshold = 0.30;
    // Challenge history mode: controls what conversation context the model
    // receives when answering a step-up challenge.
    // "full"    — all messages (no cap, original behavior)
    // "recent"  — last N messages only (default, backward compatible)
    // "summary" — mechanical DriftState summary preamble + last N recent messages
    std::string step_up_challenge_history = "recent";
    // Number of recent messages to include. Used by "recent" (as the cap) and
    // "summary" (as the recent tail after the summary preamble). Default 20
    // matches the original MAX_CHALLENGE_HISTORY constant.
    int step_up_history_recent_count = 20;
    // Mandate reinforcement — periodic reminder prepended to user message.
    // Prevents drift by reminding the model of its objective every N turns.
    // Zero extra API calls — injected into existing message content.
    bool mandate_reinforcement_enabled = false;
    // 10 turns: roughly every other lease window. Frequent enough to prevent
    // drift, rare enough to avoid prompt bloat and model desensitization.
    int mandate_reinforcement_interval = 10;
    // Empty = auto-generated "[Task Reminder: {system_prompt}]"
    std::string mandate_reinforcement_message;
    // Coherence correction — CDD-triggered stronger correction prepended to
    // user message when coherence drops below threshold. Steers the model back
    // before it reaches the challenge/kill zone.
    bool coherence_correction_enabled = false;
    // 0.85: fires when coherence drops 15% from perfect, well above the 0.7
    // kill threshold. Gives headroom for the correction to take effect.
    double coherence_correction_threshold = 0.85;
    // 5 turns: prevent spamming corrections. Long enough for the model to
    // respond, short enough to catch rapid decay.
    int coherence_correction_cooldown_turns = 5;
    // Empty = auto-generated "[Focus Alert: ... Your role: {system_prompt} ...]"
    std::string coherence_correction_message;
    // Output admissibility — post-CDD gate on response coherence.
    // Symmetric to checkAdmission() (pre-send). Evaluates after CDD scores.
    struct OutputAdmissibilityConfig {
        bool enabled = false;
        double threshold = 0.70;               // coherence floor for output
        std::string action = "quarantine";     // "block", "quarantine", "attest"
        EnforcementLevel level = EnforcementLevel::SOFT;  // for "block" action only
        // Split commit: whether quarantine/attest responses enter handle history.
        // "commit" (default) = inadmissible content stays in conversation context;
        // "exclude" = returned to caller but never appended to history.
        std::string inadmissible_history = "commit";
        // Gate tool execution on current coherence/pulse before each tool call.
        bool gate_tool_calls = false;
        // Maximum consecutive quarantined responses before hard-blocking the agent.
        // 0 = disabled (no streak limit). Ratchet: removing limit (N->0) is loosening.
        // Default 5: quarantine+commit otherwise loops forever with degraded content
        // poisoning the context of every subsequent turn.
        int max_quarantine_streak = 5;

        // Corroboration required before a quarantined turn ADVANCES the streak
        // toward the kill. 0 = disabled, every quarantine advances it (historical
        // behaviour, and the default so existing configs are untouched).
        //
        // Coherence is a weighted sum, so a single noisy signal firing turn after
        // turn accumulates to a kill on its own. Replaying real per-turn signal
        // traces, semantic_stability alone — firing on nothing worse than a
        // compliant agent varying its phrasing — drove coherence under the
        // threshold three turns running and killed the run at turn 8. Requiring N
        // DISTINCT penalising signals in the same turn removed that false kill
        // while every genuine failure mode still died on exactly the same turn.
        //
        // Counted from DriftState.last_turn_penalties: one entry per signal, so
        // it is inherently distinct-per-signal, absorbed firings (penalty 0) do
        // not count, and detection-only coherence_velocity is excluded for free
        // because it never writes a penalty. Do NOT count signals_fired_this_turn
        // instead — repeated_failures and intent_contradictions increment it
        // inside per-item loops, so one signal could corroborate itself.
        //
        // Ratchet: enabling this or raising N produces FEWER kills, so both are
        // loosening.
        int require_corroboration = 0;
    } output_admissibility;
};

// Advisory Escalation — repeated advisories harden over time
// 1st occurrence: ADVISORY (warn). 2nd: increased weight. 3rd+: escalate to SOFT (block)
// Mirrors OSHA violation escalation: first informal warning, then formal, then citation.
struct AdvisoryEscalationConfig {
    bool enabled = false;
    std::string rationale;
    // 3 occurrences: mirrors "three strikes" enforcement pattern (OSHA progressive
    // discipline). First advisory is informational, second shows persistence, third
    // demonstrates the agent is ignoring warnings — warranting escalation to SOFT block.
    int soft_after = 3;
    // 1.5x multiplier: moderate weight increase on repeated advisories. Additive with
    // each occurrence (2nd = 1.5x, 3rd = 1.5x again before SOFT escalation). Increases
    // cumulative risk score pressure without immediately blocking.
    double weight_multiplier = 1.5;
};

// F4: Governance Health — verify governance instrumentation is operational
struct GovernanceHealthConfig {
    bool enabled = false;
    std::string rationale;
    int check_after_turns = 10;           // begin checking after N agent turns
    double governance_entropy_warning = 0.5;  // F16: low entropy in check results = suspicious
    // Pulse verdict thresholds — control when governance self-health transitions.
    // 50 consecutive passes: a well-functioning governance system should occasionally
    // fire advisories. 50+ consecutive passes with zero findings suggests governance
    // is being bypassed or not instrumenting correctly. Conservative threshold to
    // avoid false alarms during short, well-behaved conversations.
    int consecutive_passes_suspicion = 50;
    // 3 degraded turns + 3 signals: IMPAIRED requires sustained multi-signal
    // degradation, not a single bad turn. Mirrors circuit breaker hysteresis —
    // the system must be consistently unhealthy, not just momentarily stressed.
    int impaired_degraded_turns = 3;
    int impaired_signal_count = 3;
    // 3 turns cooldown: prevents rapid oscillation between verdict levels.
    // Matches circuit breaker cooldown for behavioral consistency.
    int pulse_cooldown_turns = 3;
};

// Governance Pulse — real-time self-assessment of governance health
// Per-subsystem tracking with hysteresis before escalation
enum class PulseVerdict { HEALTHY = 0, DEGRADED = 1, IMPAIRED = 2 };

struct GovernancePulse {
    PulseVerdict verdict = PulseVerdict::HEALTHY;

    // Monotonic counters (updated in recordPass/enforce under results_mutex_)
    int total_checks = 0;
    // Consecutive agent turns that produced no enforcement. Advanced once per
    // pulse evaluation (one per analyzed agent turn), reset by enforce() and on
    // epoch boundaries. NOT a count of passing checks — see computePulseVerdict.
    int consecutive_passes = 0;
    bool blocked_since_last_pulse = false;  // set by enforce(), consumed by computePulseVerdict
    int advisory_count = 0;           // advisory findings emitted (did not block execution)
    int refusal_count = 0;            // enforcement refusals attested (all blocking paths)

    // Per-subsystem health
    bool bsd_connected = true;
    bool cdd_connected = true;
    bool telemetry_connected = true;
    bool transcript_connected = true;
    double entropy = -1.0;            // -1 = not yet computed

    // Hysteresis (sustained degradation required before transition)
    int consecutive_degraded = 0;
    int last_transition_turn = -100;  // cooldown between transitions

    // The clean-turn streak as it stood when the last transition fired, before
    // the epoch boundary reset it. Reading consecutive_passes after a transition
    // always yields 0, which throws away the one number that says how far past
    // the suspicion threshold the streak actually ran.
    int passes_at_transition = 0;

    // Why the last evaluation counted degradation signals. Comma-separated
    // subsystem names, empty when none fired. A DEGRADED verdict with no
    // recorded reason is unattributable — the pulse must be able to say why.
    std::string degradation_reasons;

    // Timing
    int64_t last_check_epoch_ms = 0;
};

// F7: Pipeline Separation of Duties — no adjacent pipeline stages may share the same agent config
struct PipelineSeparationConfig {
    bool enabled = false;
    std::string rationale;
    EnforcementLevel level = EnforcementLevel::SOFT;
};

// Named scorer configs — loaded from "scorers" section of govern.json
struct ScorerConfig {
    std::string name;
    bool enabled = true;
    int default_weight = 3;
    std::unordered_map<std::string, int> rule_weights;
    int green_threshold = 0;
    int yellow_threshold = 10;
    int red_threshold = 25;
    std::string threshold_mode = "fixed";  // "fixed" or "per_source"
};

// ============================================================================
// Section 12c: Governance Baseline
// ============================================================================

struct GovernanceBaselineConfig {
    bool enabled = false;
    std::string path = ".naab/governance-baseline.json";
    bool fail_on_regression = true;
    EnforcementLevel level = EnforcementLevel::SOFT;
};

// ============================================================================
// Section 13: Polyglot-Specific Rules
// ============================================================================

struct VariableBindingRules {
    bool require_explicit = false;
    EnforcementLevel require_explicit_level = EnforcementLevel::ADVISORY;
    int max_bound_variables = 0;
    bool validate_types = false;
};

struct PolyglotOutputRules {
    bool require_json_pipe = false;
    bool require_naab_return = false;
    int max_output_lines = 0;
    bool strip_whitespace = false;
    bool validate_encoding = true;
};

struct ContextIsolationRules {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::ADVISORY;
    bool no_shared_state = false;
    bool no_env_pollution = true;
    bool clean_temp_files = true;
};

struct ParallelExecutionRules {
    int max_parallel_blocks = 0;
    int timeout_per_block = 0;
    std::string fail_strategy = "fail_fast";  // "fail_fast", "continue"
    bool allow_shared_variables = false;
};

struct PersistentRuntimeRules {
    int max_sessions = 0;
    int session_timeout = 0;
    int max_memory_per_session_mb = 0;
    bool allow_state_persistence = true;
};

struct PolyglotConfig {
    VariableBindingRules variable_binding;
    PolyglotOutputRules output;
    ContextIsolationRules context_isolation;
    ParallelExecutionRules parallel;
    PersistentRuntimeRules persistent_runtime;
};

// ============================================================================
// Section 14a: Function Contracts
// ============================================================================

struct FunctionContract {
    std::string description;
    std::string rationale;
    EnforcementLevel level = EnforcementLevel::NONE;  // NONE = use parent level
    std::string return_type;           // "int", "float", "string", "bool", "array", "dict", "null"
    bool has_return_range = false;
    double return_range_min = 0, return_range_max = 0;
    bool has_return_min = false;
    double return_min = 0;
    bool has_return_max = false;
    double return_max = 0;
    std::vector<std::string> return_one_of;
    bool return_non_empty = false;
    std::vector<std::string> return_keys;
    bool return_keys_non_null = false;    // if true, all return_keys values must be non-null
    bool return_keys_non_empty = false;   // if true, all return_keys values must be non-empty (not null/""/[]/{}
    int return_length_min = -1;        // -1 = not set
    int return_length_max = -1;
    bool return_not_null = false;
    std::string return_matches;        // regex pattern the string return value must match
    std::vector<std::string> params;   // v4: input params, format "name:type" (type = any|int|float|string|bool|dict|array)
    std::vector<std::string> must_call;    // v5: patterns that MUST appear in function body (static analysis)
    std::vector<std::string> must_contain; // v5: literal strings that MUST appear in function body
    int min_arity = -1;  // naab-29 L-08: minimum parameter count (-1 = not specified)
    int max_arity = -1;  // naab-29 L-08: maximum parameter count (-1 = not specified)

    // v6: Execution-based contracts (golden tests)
    struct MustProduceCase {
        std::vector<std::string> args_json;  // Each arg as JSON string (header-safe, no nlohmann)
        std::string expect_json;              // Expected result as JSON string
    };
    std::vector<MustProduceCase> must_produce;

    // v6: Static derivation check — return key must reference specified parameters
    struct MustDeriveFromSpec {
        std::string return_key;              // Key in the return dict
        std::vector<std::string> params;     // Parameter names it must derive from
    };
    std::vector<MustDeriveFromSpec> must_derive_from;

    // v6: Anti-hardcoding — return key must change when input varies
    struct MustVarySpec {
        std::string key;                         // Return key to check
        std::string across;                      // Parameter name to vary
        std::vector<std::string> fixtures_json;  // Optional: user-provided test inputs as JSON
    };
    std::vector<MustVarySpec> must_vary;

    // v6: Mutation testing — function must produce different outputs for different inputs
    struct MustDifferentiateCase {
        std::string input_a_json;  // First input as JSON
        std::string input_b_json;  // Second input as JSON
        std::string key;           // Return key that must differ
    };
    std::vector<MustDifferentiateCase> must_differentiate;

    // v6: Case-sensitivity awareness — function must handle case variants consistently
    struct MustHandleCaseSpec {
        std::vector<std::string> inputs_json;  // Case-varied inputs as JSON strings
        std::string expect;                     // "consistent" or "different"
    };
    std::vector<MustHandleCaseSpec> must_handle_case;

    // v6: Invariant checks — expressions that must hold on the function's return value
    // Format: "result.key op value" (e.g., "result.compliance_rate >= 0")
    std::vector<std::string> must_satisfy;
    std::vector<std::string> must_satisfy_args_json;  // Test args for must_satisfy (JSON strings)
};

struct ContractsConfig {
    EnforcementLevel level = EnforcementLevel::SOFT;
    bool validate_inputs = false;
    int contract_test_timeout_ms = 5000;  // Timeout per execution contract test case
    std::map<std::string, FunctionContract> functions;
};

// ============================================================================
// Section 15: Polyglot Optimization
// ============================================================================

struct TaskInferencePattern {
    std::vector<std::string> patterns;          // Regex patterns to detect task type
    std::vector<std::string> optimal_languages;  // Best languages for this task
    std::vector<std::string> suboptimal_languages; // Languages to avoid
    std::string message;                         // Help message
};

struct PatternDetectionConfig {
    bool enabled = true;
    std::map<std::string, TaskInferencePattern> task_inference;
    // task_inference contains entries like:
    // "numerical_operations": { patterns: [...], optimal_languages: ["julia", "nim"], ... }
    // "string_processing": { patterns: [...], optimal_languages: ["python", "ruby"], ... }
    // "file_operations": { patterns: [...], optimal_languages: ["shell", "bash"], ... }
    // etc.
};

struct LanguageDiversityConfig {
    bool enabled = true;
    int min_languages = 2;                 // Minimum languages for complex scripts
    int max_single_language_percent = 70;  // Max % of one language
    std::string message = "Consider using specialized languages for performance-critical sections";
};

struct HelperErrorConfig {
    bool enabled = true;
    bool show_alternative_language = true;
    bool show_example_code = true;
    double fuzzy_match_threshold = 0.7;
};

struct AIGuidanceConfig {
    bool enabled = true;
    bool include_in_errors = true;
    bool suggest_refactoring = true;
    bool show_benchmarks = false;
};

// Empirical profiling config — times every polyglot block execution
struct ProfilingConfig {
    bool enabled = false;
    std::string profile_path = "~/.naab/profile.json";
    int max_entries = 10000;        // Ring buffer
    bool include_code_hash = true;  // For dedup
};

// Calibration config — machine-specific benchmark data
struct CalibrationConfig {
    bool enabled = true;
    bool auto_calibrate = false;
    std::string calibration_path = "~/.naab/calibration.json";
    int max_age_days = 30;
    int iterations = 3;
};

// Confidence labeling config — evidence basis for suggestions
struct ConfidenceConfig {
    std::string min_display_level = "estimated";  // "measured"|"calibrated"|"estimated"|"unknown"
    bool suppress_unknown = true;
    bool show_measurement_details = true;
};

// Drift tracking config — persistent cross-language drift monitoring
struct DriftTrackingConfig {
    bool enabled = false;
    std::string path = "~/.naab/drift.jsonl";
    int max_entries = 1000;
    int trend_window = 10;
    double escalation_threshold = 0.3;
    bool include_code_hash = true;
};

// Polyglot verification config — cross-language consensus checking
struct VerificationConfig {
    bool enabled = false;                           // Master switch
    std::string enforcement_level = "advisory";     // "advisory" | "soft" | "hard"
    std::vector<std::string> consensus_languages;   // Languages to verify against
    double tolerance = 1e-10;                       // Numeric comparison tolerance
    std::vector<std::string> verify_task_types;     // Task categories to verify
    int min_consensus = 2;                          // Min languages that must agree
    int max_verification_time_ms = 5000;            // Timeout per verification run
    bool show_drift_details = true;                 // Show actual values in report
    DriftTrackingConfig drift_tracking;
};

struct TaskLanguageScore {
    int score = 0;            // 0-100 score
    std::string reason;       // Why this score
};

struct PolyglotOptimizationConfig {
    bool enabled = true;
    std::string enforcement_level = "soft";  // "none" | "advisory" | "soft" | "hard"

    PatternDetectionConfig pattern_detection;
    LanguageDiversityConfig language_diversity;
    HelperErrorConfig helper_errors;
    AIGuidanceConfig ai_guidance;

    // Empirical optimization (Phases 1-3)
    ProfilingConfig profiling;
    CalibrationConfig calibration;
    ConfidenceConfig confidence;
    VerificationConfig verification;

    // Task→Language scoring matrix loaded from JSON
    // Structure: task_language_matrix[task_name][language] = TaskLanguageScore
    std::map<std::string, std::map<std::string, TaskLanguageScore>> task_language_matrix;
};

// ============================================================================
// Section 16: Output Baselines
// ============================================================================

struct BaselinesConfig {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::ADVISORY;
    std::string path = ".naab/baselines.json";
    double tolerance = 1e-6;
    bool auto_record = false;
    bool hash_keys = true;
};

// ============================================================================
// Taint Tracking Configuration
// ============================================================================

struct TaintMetadata {
    std::string origin_function;   // e.g., "env.get"
    std::string origin_argument;   // e.g., "SECRET_KEY"
    std::string source_file;       // file where taint was introduced
    int source_line = 0;
    int64_t timestamp = 0;         // epoch ms
};

struct TaintTrackingConfig {
    bool enabled = false;
    bool lineage = false;          // track origin metadata per tainted var
    std::string level = "hard";    // hard, soft, advisory
    std::string rationale;
    std::vector<std::string> sources;      // e.g., "env.get", "io.read_line", "file.read", "polyglot_output"
    std::vector<std::string> sinks;        // e.g., "shell_exec", "file.write", "file.append"
    std::vector<std::string> sanitizers;   // e.g., "validate_", "sanitize_", "escape_", "int(", "float("
    bool gate_cross_block = false;         // F14: enforce cross-block taint (not just advisory)
    EnforcementLevel cross_block_level = EnforcementLevel::SOFT;  // F14: enforcement level for cross-block
};

// ============================================================================
// Telemetry Output Config
// ============================================================================

struct TelemetryOutputConfig {
    bool enabled = false;
    std::string output_file;  // JSONL path, append mode
    TamperEvidenceConfig tamper_evidence;  // hash chain for telemetry entries
    // Per-decision CDD state snapshots attached to SEMANTIC_TURN and
    // OUTPUT_ADMISSIBILITY_EVAL events (replay-grade evidence). Ratchet:
    // disabling mid-run is a violation.
    bool decision_snapshots = false;

    // --- Forwarding (webhook/SIEM) ---
    std::string webhook_url;            // POST endpoint for JSON event batches
    std::string webhook_auth_header;    // full header value, e.g. "Bearer xxx"
    std::string webhook_auth_env;       // env var name for auth header value
    int forward_batch_size = 10;        // events per POST request
    int forward_timeout_ms = 5000;      // per-request timeout
    int forward_retry_count = 2;        // retries on transient failure
    int forward_buffer_max = 1000;      // max queued events before drop
    int forward_shutdown_drain_ms = 5000; // M2: max shutdown drain time
    bool deduplicate_checks = false;  // Fix 4B: collapse duplicate (rule,file,line) entries
};

// ============================================================================
// Agent Interaction Transcript Config
// ============================================================================

struct TranscriptConfig {
    bool enabled = false;
    std::string output_file;             // JSONL path, append mode
    std::vector<std::string> agents;     // empty = all agents, ["name"] = filter
};

// ============================================================================
// Agent Config (was AgentRoleConfig — now includes LLM config)
// ============================================================================

struct AgentConfig {
    std::string name;

    // --- Permissions (existing from AgentRoleConfig) ---
    std::vector<std::string> allowed_languages;
    std::vector<std::string> blocked_languages;  // V-GOV-020: per-agent language blocking
    std::vector<std::string> blocked_paths;
    std::vector<std::string> allowed_paths;
    // V-GOV-018: per-agent shell capability override.
    // shell_allowed_set = false means "use global policy" (don't restrict further).
    // shell_allowed_set = true + shell_allowed = false → block shell for this role.
    //
    // shell_allowed gates TWO different things, which is why shell_content_allowed
    // exists below:
    //   1. EXECUTION — checkShellAllowed() blocks <<shell>> blocks and prerequisite
    //      commands for this role.
    //   2. CONTENT — agentSend() scans the agent's RESPONSE TEXT for shell syntax
    //      and refuses the turn if it matches.
    // (2) is the original purpose: the field shipped as per-agent sandbox config
    // alongside allowed_paths/blocked_paths, which are also response scans, and
    // predates tool execution. (1) was layered on later by V-GOV-020.
    bool shell_allowed = true;
    bool shell_allowed_set = false;

    // Opt out of the CONTENT half of shell_allowed without touching the execution
    // half. For an agent whose job is to author runbooks or install instructions:
    // the content scan matches "$ <command>" lines, so `$ npm install express` is
    // refused — an agent restricted from shell cannot write install instructions
    // for any language.
    //
    // Deliberately additive and fail-closed rather than a rename. Unset means the
    // content scan still runs, so no existing config changes behaviour. It relaxes
    // only when set true, and only the content scan — execution stays gated by
    // shell_allowed, which for an agent with an empty allowed_actions is the ONLY
    // thing gating it (checkShellAllowed skips the SHELL_EXEC matrix test when the
    // list is empty). Deprecating shell_allowed's execution role would silently
    // loosen exactly those configs; govern-template.json's "researcher" is one.
    //
    // Ratchet: false/unset -> true is a loosening violation.
    //
    // PAIR THIS WITH capabilities.shell.enabled = false. This flag stops the
    // agent's response being REFUSED for containing shell; it does not stop
    // anyone running what the response contains. Per-agent shell_allowed gates
    // execution only for code running under that role (--agent-id); an
    // orchestration script calling codegen.run("shell", …) on the response is
    // gated by the GLOBAL capability instead. Measured:
    //
    //   agent shell_allowed=false, shell_content_allowed=true
    //     capabilities.shell.enabled = true   -> script executes the response
    //     capabilities.shell.enabled = false  -> blocked
    //
    // So on a config where global shell is enabled — the usual case, and the
    // shape of the runbook scenario this field was added for — permitting the
    // content also makes it runnable by the surrounding script. The remaining
    // boundary is the global capability and the sandbox's SYS_EXEC, not this
    // field's twin.
    bool shell_content_allowed = false;
    bool shell_content_allowed_set = false;
    // Per-agent network capability override (same pattern as shell_allowed)
    bool network_allowed = true;
    bool network_allowed_set = false;
    // Fine-grained action matrix — empty = all allowed
    // Actions: "SHELL_EXEC", "NET_CONNECT", "FS_READ", "FS_WRITE", "AGENT_SEND", "TOOL_EXEC", "CODEGEN_EXEC"
    std::vector<std::string> allowed_actions;

    // --- LLM config (populated from "agents" key in govern.json) ---
    std::string provider = "anthropic";
    // Optional API endpoint base URL override (e.g. self-hosted gateway or
    // local test stub). Empty = provider's default endpoint. Must be https://
    // unless the host is loopback (127.0.0.1 / localhost / [::1]).
    std::string api_base;
    std::string model;                          // primary model (first in chain)
    std::vector<std::string> model_chain;       // fallback models [model, fallback1, ...]
    std::string api_key_env = "ANTHROPIC_API_KEY";  // primary key env var
    std::vector<std::string> api_key_envs;      // rotation keys [primary, alt1, alt2, ...]
    int max_tokens = 4096;
    // Floor on effective max_tokens (0 = no floor). Ratchet raise-only: prevents
    // an operator from crippling an agent mid-run by shrinking its token budget
    // below a working minimum. Effective request budget = max(max_tokens, min_tokens).
    int min_tokens = 0;
    int thinking_budget = -1;  // -1=provider default, 0=disable, >0=budget in tokens
    std::string system_prompt;
    std::vector<std::string> tools;
    // Tool execution configuration
    bool tools_enabled = false;                  // master switch — tools blocked unless true
    int max_tool_calls_per_turn = 5;             // max tool invocations per agentSend() call
    int max_tool_loop_turns = 10;                // max LLM round-trips in tool loop
    int tool_result_max_chars = 4096;            // truncate individual tool results beyond this
    int tool_result_max_total_chars = 32768;     // cumulative tool result cap per agentSend()
    int tool_timeout_seconds = 10;               // per-tool-call execution timeout
    int max_turns = 50;
    int max_total_tokens = 100000;
    // agent.propose() candidate cap. 0 = propose disabled (fail-closed).
    int propose_candidates_max = 0;
    double temperature = 1.0;
    std::string stop_reason_action = "end";
    bool stream = false;
    int timeout_seconds = 60;  // F11: per-call LLM API timeout (raised from 30 — slow models need 50+s)
    std::string response_format;  // "json" = validate + warn on non-JSON responses
    int risk_budget = 0;          // F8: finite risk budget (0 = unlimited), consumed by governance events

    // Retry configuration (per-agent)
    struct RetryConfig {
        int max_attempts = 1;           // total tries per call (across keys+models). 1 = no retry
        int backoff_ms = 1000;          // initial backoff delay
        double backoff_multiplier = 2.0;// exponential backoff factor
        bool jitter = true;             // random jitter on backoff (thundering herd prevention)
        std::vector<int> retry_on = {429, 503};      // HTTP codes that trigger retry
        std::vector<int> skip_key_on = {401};         // mark key dead for this run
        std::vector<int> fallback_model_on = {404, 503}; // advance model chain
        std::vector<int> never_retry = {400};         // bad request = don't retry
        int key_retry_after_seconds = 0;             // 0 = never revive dead keys (backward compat)
    };
    RetryConfig retry;

    // Client-side rate limiting (per-agent)
    struct RateLimitConfig {
        int requests_per_minute = 0;    // 0 = no client-side limit
        int delay_between_calls_ms = 0; // forced inter-call delay
    };
    RateLimitConfig rate_limit;

    // Standing Lease — TTL on agent authorization (Kerberos TGT / OAuth access token analog)
    // After N turns, standing expires. Agent must re-authorize via step-up challenge.
    // 0 = no lease (unlimited standing). Requires step_up_enabled in circuit_breaker.
    int standing_lease_turns = 0;
    int standing_lease_seconds = 0;  // 0 = no wall-clock lease

    // Context windowing — limit conversation history sent per API call.
    // 0 = unlimited (backward compatible). When set, only last N messages included.
    int context_window = 0;
    // "full" = all messages (context_window ignored), "recent" = last N only,
    // "summary" = DriftState summary preamble + last N messages
    std::string context_strategy = "full";

    // Output Contract — validation schema for LLM responses (Phase 7)
    struct OutputContract {
        std::string format;  // "json", "text", etc. (empty = no format enforcement)
        std::vector<std::string> required_fields;  // for JSON: list of keys that must be present
        std::unordered_map<std::string, std::string> field_types;  // field → "string", "number", "boolean", etc.
        std::unordered_map<std::string, std::string> regex_checks;  // field → regex pattern to match
    };
    OutputContract output_contract;  // empty = no contract enforcement

    // Per-agent CDD signal overrides — keys are the canonical signal config
    // names (kCddSignalKeys); value overrides the global context_drift.signals
    // setting for handles of this agent. Ratchet: disabling a signal that was
    // effectively enabled mid-run is a violation.
    std::map<std::string, bool> context_drift_signals;
};

// ============================================================================
// Dynamic Code Execution Configuration
// ============================================================================

struct CodegenConfig {
    bool enabled = false;                      // Master switch (default off)
    EnforcementLevel level = EnforcementLevel::HARD;
    std::string rationale;

    // Per-call limits
    int max_code_size_bytes = 65536;           // 64KB per call
    int max_code_lines = 500;                  // per call
    int timeout_seconds = 15;                  // per-call execution timeout

    // Cumulative session limits
    int max_cumulative_code_bytes = 524288;    // 512KB per process lifetime
    int max_cumulative_calls = 100;            // total codegen.run() calls per process
    int max_cumulative_calls_per_agent = 20;   // per agent handle

    // Taint policy
    bool allow_tainted_code = false;           // tainted code strings HARD-blocked by default

    // Recursion prevention
    int max_nesting_depth = 0;                 // 0 = no nesting allowed

    // Language restrictions (augments global languages.allowed)
    std::vector<std::string> allowed_languages;   // empty = use global policy
    std::vector<std::string> blocked_languages;   // additional blocks for codegen

    // Error handling
    bool sanitize_stderr = true;               // scrub system paths from stderr
    int max_stderr_chars = 2048;               // truncate stderr
};

// ============================================================================
// Master Rules Structure
// ============================================================================

struct GovernanceRules {
    std::string version;
    GovernanceMode mode = GovernanceMode::ENFORCE;
    std::string extends_path;  // "extends" field
    std::string description;

    // M3: tracks which JSON keys were explicitly present in the config file.
    // Used by mergeRules() to distinguish "user set field to default" from "never set".
    std::unordered_set<std::string> explicitly_set;

    LanguagesConfig languages;
    CapabilitiesConfig capabilities;
    LimitsConfig limits;
    RequirementsConfig requirements;
    RestrictionsConfig restrictions;
    CodeQualityConfig code_quality;
    std::vector<CustomRule> custom_rules;
    std::vector<GovernancePlugin> governance_plugins;
    std::vector<ScopeOverride> scopes;
    OutputConfig output;
    AuditConfig audit;
    MetaConfig meta;
    HooksConfig hooks;
    PolyglotConfig polyglot;
    PolyglotOptimizationConfig polyglot_optimization;
    ContractsConfig contracts;
    TaintTrackingConfig taint_tracking;
    BehavioralSequenceConfig behavioral_sequences;
    ContextDriftConfig context_drift;
    ExposureTrackingConfig exposure_tracking;
    PipelineSeparationConfig pipeline_separation;
    GovernanceHealthConfig governance_health;
    CircuitBreakerConfig circuit_breaker;
    AdvisoryEscalationConfig advisory_escalation;
    TemporalCouplingConfig temporal_coupling;
    BaselinesConfig baselines;
    ProjectContextConfig project_context;
    ApprovalConfig approval;
    TrustPolicyConfig trust_policy;
    PrerequisitesConfig prerequisites;
    ContradictionDetectionConfig contradiction_detection;
    CodegenConfig codegen;

    // Integrity: HMAC signing of govern.json and drift baselines
    struct IntegrityConfig {
        // V-SC-008: require_signature removed — NAAB_GOVERN_KEY presence is the sole authority.
        std::vector<std::string> blocked_flags;
    } integrity;

    // --- Governance behavior (configurable via govern.json or CLI flags) ---
    // CLI flags override these when present.
    bool explanations_enabled = true;  // governance.explanations (default on)
    bool verbose = false;           // --governance-verbose
    bool dashboard = false;         // --governance-dashboard
    bool baseline_save = false;     // --governance-baseline-save
    bool drift_baseline_save = false; // --drift-baseline-save
    bool allow_override = false;    // --governance-override
    bool require_override_reason = false;  // governance.require_override_reason
    bool lint_only_config = false;  // --lint-only
    bool record_baselines = false;  // --governance-record-baselines
    bool check_baselines = false;   // --governance-check-baselines
    bool quiet_config = false;      // --quiet / -q
    bool no_color_config = false;   // --no-color
    std::string report_json;        // --governance-report
    std::string report_sarif;       // --governance-sarif
    std::string report_junit;       // --governance-junit
    std::string telemetry_path;     // --governance-telemetry
    std::string agent_id_config;    // --agent-id (empty = use default "anonymous")
    std::string default_env;        // --env

    // --- Runtime limits (configurable via govern.json "runtime" section) ---
    struct RuntimeConfig {
        int timeout = 30;
        size_t memory_limit = 0;     // 0 = unlimited
        size_t gc_threshold = 5000;
        bool gc_stats = false;
    };
    RuntimeConfig runtime;

    // --- Security (configurable via govern.json "security" section) ---
    std::string sandbox_level_config;  // restricted/standard/elevated/unrestricted
    bool allow_network_config = false; // --allow-network
    bool strict_types_config = false;  // --strict-types

    // --- API settings (configurable via govern.json "api" section) ---
    struct ApiKeyEntry {
        std::string key;                    // the API key value
        std::string name;                   // human-readable label
        std::vector<std::string> scopes;    // e.g. ["execute", "check", "blocks", "stats"]
    };
    struct ApiConfig {
        std::string key;                     // legacy single key (backward compat)
        std::vector<ApiKeyEntry> keys;       // multi-key with scoped permissions
        int timeout = 10;
        int rate_limit = 0;                  // 0 = unlimited
        size_t max_body = 1048576;           // 1 MiB
        std::string tls_cert_path;           // PEM cert for HTTPS
        std::string tls_key_path;            // PEM key for HTTPS
    };
    ApiConfig api;

    // --- Legacy flat fields (kept for backward compatibility) ---
    std::unordered_set<std::string> allowed_languages;
    std::unordered_set<std::string> blocked_languages;
    bool network_allowed = true;
    std::string filesystem_mode = "write";
    bool shell_allowed = true;
    int timeout_seconds = 0;
    int memory_limit_mb = 0;
    int max_call_depth = 0;
    int max_array_size = 0;
    bool require_error_handling = false;
    EnforcementLevel error_handling_level = EnforcementLevel::HARD;
    bool require_main_block = false;
    EnforcementLevel main_block_level = EnforcementLevel::HARD;
    std::string polyglot_output = "any";
    bool restrict_dangerous_calls = false;
    EnforcementLevel dangerous_calls_level = EnforcementLevel::HARD;
    bool no_placeholders = false;
    EnforcementLevel no_placeholders_level = EnforcementLevel::SOFT;
    bool no_secrets = false;
    EnforcementLevel no_secrets_level = EnforcementLevel::HARD;
    bool no_hardcoded_results = false;
    EnforcementLevel no_hardcoded_results_level = EnforcementLevel::ADVISORY;
    std::string audit_level = "none";
    bool tamper_evidence = false;

    // --- Agent governance ---
    TelemetryOutputConfig telemetry_output;
    TranscriptConfig transcript;
    std::vector<AgentConfig> agents;  // was: agent_roles (migrated to unified config)
    std::vector<ScorerConfig> scorers; // named scorer configs from "scorers" section

    // --- Agent dispatch (parallel agent execution config) ---
    struct AgentDispatchConfig {
        int max_concurrent = 6;    // max concurrent agent API calls
        int pool_size = 6;         // thread pool worker count (I/O-bound, not CPU)
        int pool_queue_max = 50;   // max queued tasks before rejection
        int max_retries_per_run = 0; // 0 = unlimited
        int default_timeout_seconds = 0; // 0 = use per-agent struct default (60s)

        // Hard stop: run-level kill switch for all agent API calls
        struct HardStopConfig {
            int max_calls_per_run = 0;          // 0 = unlimited
            int max_tokens_per_run = 0;         // 0 = unlimited
            int max_agent_time_ms = 0;          // 0 = unlimited
            int consecutive_failure_limit = 0;  // 0 = unlimited
            std::string action = "block";       // "block" or "warn"
        };
        HardStopConfig hard_stop;
    };
    AgentDispatchConfig agent_dispatch;

    // --- Agent review (LLM-based governance phase) ---
    struct AgentReviewSection {
        bool enabled = false;
        std::vector<std::string> detection;       // agent names for detection
        std::string validation;                    // agent name for validation
        std::string voice;                         // agent name for voice synthesis
        std::string scorer;                        // scorer config name
        std::map<std::string, std::string> enforcement;  // zone -> level
        bool cache = false;
        bool hints = false;  // show rejected findings as [hint] lines
        std::string fail_policy = "open";  // F10: "open" or "closed"
        std::string dispatch_mode = "sequential";  // "sequential" | "parallel"
        int max_parallel = 0;      // 0 = unlimited (all detection agents at once)
        std::string fail_strategy = "fail_fast";   // "fail_fast" | "continue"
    } agent_review;

    // --- Quality gate (Feature 2) ---
    QualityGateConfig quality_gate;

    // --- Cumulative risk scoring ---
    ScoringConfig scoring;
    ScoringCalibrationConfig scoring_calibration;

    // --- Governance baseline (Feature 4) ---
    GovernanceBaselineConfig governance_baseline;

    // --- Environment overrides (Feature 5) ---
    std::map<std::string, std::unordered_map<std::string, std::string>> environments;

    // --- Runtime version pinning (Phase 8.4) ---
    struct RuntimeVersionPin {
        std::string language;           // e.g., "python", "javascript", "go"
        std::string required_version;   // Prefix "3.11", or ">=3.10"
        EnforcementLevel level = EnforcementLevel::ADVISORY;
        std::string message;            // Custom message (empty = auto-generated)
    };
    std::vector<RuntimeVersionPin> runtime_versions;
};

// ============================================================================
// Check Result Tracking
// ============================================================================

struct CheckResult {
    std::string rule_name;
    EnforcementLevel level;
    bool passed;
    std::string message;
    std::string category;     // for grouping in reports (e.g., "code_quality", "restrictions")
    std::string severity;     // "critical", "high", "medium", "low"
    int line = 0;
    std::string file;         // source file path
    std::vector<std::string> cwe_ids;    // e.g., {"CWE-89"}
    std::vector<std::string> owasp_ids;  // e.g., {"A03:2021"}
    bool preflight = false;   // F8: preflight results survive FIFO eviction
    // Decision rationale: WHY this rule is at this enforcement tier (from govern.json)
    std::string rationale;
    // Decision trace: HOW the engine reached its verdict (computed at check time)
    std::vector<std::string> decision_trace;
    // Human-readable explanation: plain-English sentence for end users
    std::string explanation;
    bool escalated = false;   // advisory escalated to effective block
};

// ============================================================================
// Audit Trail Entry
// ============================================================================

struct AuditEntry {
    std::string timestamp;
    std::string event_type;  // "check_passed", "check_failed", "override", etc.
    std::string rule_name;
    std::string file;
    int line = 0;
    std::string message;
    std::string rationale;   // WHY this rule is at this tier (from govern.json)
    std::string previous_hash;
    std::string current_hash;
};

// ============================================================================
// Pattern Database Types
// ============================================================================
// Pass 2: Post-Execution Audit Records
// ============================================================================

struct PolyglotExecutionRecord {
    std::string language;
    std::string runtime_version;
    int source_line = 0;
    int64_t duration_us = 0;
    std::string file;
    std::vector<std::string> bound_vars;
    std::string captured_output;
    std::string captured_stderr;
    std::string final_code;         // actual code sent to executor
    bool contract_verified = false;
    int exit_code = 0;
};

struct TaintFlowRecord {
    std::string var_name;
    std::string source_type;  // "user_input", "polyglot_output", "env_var"
    std::string sink_type;    // "shell_exec", "file_write", "" if no sink reached
    std::string decision;     // "blocked", "allowed", "sanitized"
    std::string file;
    int line = 0;
};

struct SideEffectRecord {
    std::string type;    // "file_write", "file_delete", "env_set", "env_delete", "shell_exec", "network"
    std::string detail;  // path, var name, command, URL
    std::string file;
    int line = 0;
};

struct CrossBlockFlow {
    int from_block_line = 0;
    std::string from_language;
    int to_block_line = 0;
    std::string to_language;
    std::vector<std::string> vars;  // variables that flowed
    bool sanitized = false;
};

// ============================================================================

struct DangerousPattern {
    std::string language;
    std::string pattern;
    std::string description;
    std::string safe_alternative;
};

struct SecretPattern {
    std::string pattern;
    std::string description;
    std::string severity;
};

// ============================================================================
// Rate Limiter
// ============================================================================

struct RateLimiter {
    int max_per_second = 0;
    std::chrono::steady_clock::time_point window_start;
    int count_in_window = 0;

    bool check() {
        if (max_per_second <= 0) return true;
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - window_start).count();
        if (elapsed >= 1) {
            window_start = now;
            count_in_window = 1;
            return true;
        }
        count_in_window++;
        return count_in_window <= max_per_second;
    }
};

// ============================================================================
// Governance Engine (Main Class)
// ============================================================================

// Process-level flag for exit code determination (Feature 1)
// Survives stack unwinding — set in enforce() when HARD violation fires
extern std::atomic<bool> g_governance_hard_block;

// Uncatchable governance exception — NAAb try/catch explicitly re-throws this.
// Prevents LLM agents from swallowing HARD blocks via try/catch and continuing execution.
class GovernanceHardError : public std::runtime_error {
public:
    explicit GovernanceHardError(const std::string& msg)
        : std::runtime_error(msg) {}
};

class GovernanceEngine {
public:
    // Calibration entry (public for getCalibrationData() return type)
    struct CalibrationEntry {
        int64_t us = 0;   // Median microseconds
        int score = 0;    // Normalized 0-100
    };

    GovernanceEngine();
    ~GovernanceEngine();

    // --- Thread-local accessor (for stdlib modules that need governance config) ---
    static GovernanceEngine* getCurrent();
    static void setCurrent(GovernanceEngine* engine);

    // --- Loading ---
    bool loadFromFile(const std::string& path);
    bool loadFromString(const std::string& json_config);
    bool discoverAndLoad(const std::string& start_dir);

    // --- Mid-run reload (Governance Under Survivability) ---
    bool reloadIfChanged();                        // check mtime, reload if tightened, return true if reloaded
    std::vector<std::string> getAndClearNotices(); // retrieve + clear pending reload notices
    int getReloadCount() const { return reload_count_.load(std::memory_order_relaxed); }

    // --- State ---
    bool isActive() const { return active_; }
    bool isOverrideEnabled() const { return override_enabled_; }
    void setOverrideEnabled(bool enabled) { override_enabled_ = enabled; }
    void setOverrideReason(const std::string& reason) { override_reason_ = reason; }
    void setAgentGovernanceActive(bool active) { agent_governance_active_.store(active, std::memory_order_relaxed); }
    const std::string& getOverrideReason() const { return override_reason_; }
    bool hasValidApproval(const std::string& rule_name, std::string& approver_id_out) const;
    const std::string& getLoadedPath() const { return loaded_path_; }
    const std::string& getLastError() const { return last_error_; }
    GovernanceMode getMode() const;
    const std::string& getGovernDir() const { return govern_json_dir_; }
    const GovernanceRules& getRules() const;
    std::shared_ptr<const GovernanceRules> getRulesPtr() const;
    GovernanceRules& getMutableRules();  // init-only — UB if called during concurrent execution
    void addGovernanceProtectedPaths(GovernanceRules& target_rules);

    // --- Legacy Getters (backward compatible) ---
    int getTimeoutSeconds() const;
    int getMemoryLimitMB() const;
    bool requiresErrorHandling() const;
    bool requiresMainBlock() const;
    std::string getAuditLevel() const;
    bool isTamperEvidenceEnabled() const;

    // --- Codegen accessors ---
    bool getCodegenEnabled() const;
    const CodegenConfig& getCodegenConfig() const;

    // --- Agent identity ---
    void setAgentId(const std::string& id) { agent_id_ = id; }
    const std::string& getAgentId() const { return agent_id_; }
    void applyAgentRole();

    // --- VM integration for execution contracts (type-erased to avoid VM link dependency) ---
    using ContractCallFn = std::function<interpreter::NaabVal(interpreter::NaabVal, const std::vector<interpreter::NaabVal>&, bool)>;
    using ContractGlobalsFn = std::function<const std::unordered_map<std::string, interpreter::NaabVal>&()>;
    void setVMCallbacks(ContractCallFn call_fn, ContractGlobalsFn globals_fn) {
        vm_call_fn_ = std::move(call_fn);
        vm_globals_fn_ = std::move(globals_fn);
    }
    void clearVMCallbacks() { vm_call_fn_ = nullptr; vm_globals_fn_ = nullptr; }

    // Call a NaabVal function via the VM callback (used by tool execution loop)
    // taint_all_args: when true, mark all arguments as tainted on the VM stack
    interpreter::NaabVal callVMFunction(const interpreter::NaabVal& fn,
                                         const std::vector<interpreter::NaabVal>& args,
                                         bool taint_all_args = false) {
        if (!vm_call_fn_) {
            throw std::runtime_error("Agent error: no VM callback available for tool execution");
        }
        return vm_call_fn_(fn, args, taint_all_args);
    }
    bool hasVMCallbacks() const { return vm_call_fn_ != nullptr; }

    // --- Path access control ---
    std::string checkPathAccess(const std::string& filepath, const std::string& mode);

    // --- Telemetry ---
    void writeTelemetry() const;
    void writeAgentTelemetry(const std::string& event_type,
        const std::unordered_map<std::string, std::string>& fields);
    // Per-decision CDD state snapshot as a compact JSON string ("" when the
    // handle has no drift state). Attached to telemetry events when
    // telemetry.decision_snapshots is enabled.
    std::string snapshotCddState(int handle_id) const;

    // --- Agent interaction transcript ---
    void writeAgentTranscript(const std::string& json_line);
    bool isTranscriptAgent(const std::string& agent_name) const;
    const std::string& getRunId() const { return run_id_; }

    // --- Check context (for report file/line tracking) ---
    void setCheckContext(const std::string& file, int line = 0);

    // --- Per-language getters ---
    int getTimeoutForLanguage(const std::string& lang) const;
    int getMaxLinesForLanguage(const std::string& lang) const;
    const LanguageConfig* getLanguageConfig(const std::string& lang) const;

    // --- Codegen checks ---
    std::string checkCodegenAllowed(const std::string& language, size_t code_size, int line = 0);

    // --- Original checks (backward compatible) ---
    std::string checkLanguageAllowed(const std::string& language, int line = 0);
    std::string checkNetworkAllowed();
    std::string checkFilesystemAllowed(const std::string& mode);
    std::string checkShellAllowed();
    std::string checkEnvVarRead(const std::string& var_name);
    std::string checkEnvVarWrite(const std::string& var_name);
    std::string checkCallDepth(size_t current_depth);
    std::string checkArraySize(size_t size);
    std::string checkPolyglotOutput(const std::string& output);
    // C2 fix: preprocess code the same way checkPolyglotBlock does
    // (normalizeUnicode, normalizeWhitespace, stripStringLiterals, expandDangerousAliases)
    std::string preprocessCode(const std::string& language, const std::string& code);

    std::string checkDangerousCall(const std::string& language,
                                    const std::string& code, int line = 0);
    std::string checkSecrets(const std::string& code, int line = 0);
    std::string checkPlaceholders(const std::string& code, int line = 0);
    std::string checkHardcodedResults(const std::string& code, int line = 0);

    // --- New v3.0 checks ---
    std::string checkPii(const std::string& code, int line = 0);
    std::string checkTemporaryCode(const std::string& code, int line = 0);
    std::string checkSimulationMarkers(const std::string& code, int line = 0);
    std::string checkMockData(const std::string& code, int line = 0);
    std::string checkApologeticLanguage(const std::string& code, int line = 0);
    std::string checkDeadCode(const std::string& code, int line = 0);
    std::string checkDebugArtifacts(const std::string& language,
                                     const std::string& code, int line = 0);
    std::string checkUnsafeDeserialization(const std::string& code, int line = 0);
    std::string checkSqlInjection(const std::string& code, int line = 0);
    std::string checkPathTraversal(const std::string& code, int line = 0);
    std::string checkHardcodedUrls(const std::string& code, int line = 0);
    std::string checkHardcodedIps(const std::string& code, int line = 0);
    std::string checkEncoding(const std::string& code, int line = 0);
    std::string checkComplexity(const std::string& code, int line = 0);

    // LLM anti-drift checks
    std::string checkOversimplification(const std::string& code, int line = 0);
    std::string checkIncompleteLogic(const std::string& code, int line = 0, const std::string& source_file = "");
    std::string checkCosmeticSanitizer(const std::string& function_name,
                                        const std::string& body, int line = 0);
    std::string checkEmptyMain(const std::string& source);
    std::string checkIntentValidation(const std::string& function_name,
                                       const std::string& intent,
                                       const std::string& body, int line);
    // Preflight intent gate — runs all intent checks before execution
    std::string preflightIntentCheck(const ast::Program& program, const std::string& source);
    bool hasIntentBlock() const;
    std::string checkHallucinatedApis(const std::string& language,
                                       const std::string& code, int line = 0);
    std::string checkSemanticIssues(const std::string& language,
                                     const std::string& code, int line = 0);

    // Security checks
    std::string checkShellInjection(const std::string& code, int line = 0);
    std::string checkCodeInjection(const std::string& language,
                                    const std::string& code,
                                    const std::string& normalized_code,
                                    int line = 0);
    std::string checkPrivilegeEscalation(const std::string& code, int line = 0);
    std::string checkDataExfiltration(const std::string& code, int line = 0);
    std::string checkResourceAbuse(const std::string& code, int line = 0);
    std::string checkInfoDisclosure(const std::string& language,
                                     const std::string& code, int line = 0);
    std::string checkCryptoWeakness(const std::string& code,
                                     const std::string& normalized_code,
                                     int line = 0);
    std::string checkVcsSecretExtraction(const std::string& code, int line = 0);
    std::string checkObfuscationSignals(const std::string& language,
                                        const std::string& raw_code,
                                        const std::string& stripped_code,
                                        int line = 0);

    // Capability checks for polyglot blocks
    std::string checkNetworkImports(const std::string& language,
                                     const std::string& code, int line = 0);
    std::string checkFilesystemImports(const std::string& language,
                                        const std::string& code, int line = 0);

    // Per-language checks
    std::string checkImports(const std::string& language,
                              const std::string& code, int line = 0);
    std::string checkBannedFunctions(const std::string& language,
                                      const std::string& code, int line = 0);
    std::string checkLanguageStyle(const std::string& language,
                                    const std::string& code, int line = 0);
    std::string checkCodeSize(const std::string& language,
                               const std::string& code, int line = 0);

    // Custom rules
    std::string checkCustomRules(const std::string& language,
                                  const std::string& code, int line = 0);

    // Governance plugins (NAAb-based custom checks)
    std::string checkPluginRules(const std::string& trigger,
                                  const std::unordered_map<std::string, interpreter::NaabVal>& context,
                                  int line = 0);
    void loadPlugins();

    // Resource limits
    std::string checkLoopIterations(size_t count);
    std::string checkPolyglotBlockCount(size_t count);
    std::string incrementAndCheckPolyglotBlockCount();
    std::string checkStringLength(size_t length);
    std::string checkNestingDepth(size_t depth);
    std::string checkOutputSize(size_t size);
    std::string checkDictSize(size_t size);

    // Rate limiting
    bool checkPolyglotRate();
    bool checkStdlibRate();
    bool checkFileOpsRate();

    // Comprehensive check (runs all applicable checks on a polyglot block)
    std::string checkPolyglotBlock(const std::string& language,
                                    const std::string& code,
                                    const std::string& source_file,
                                    int line = 0);

    // Overload with variable binding count for enforcement
    std::string checkPolyglotBlock(const std::string& language,
                                    const std::string& code,
                                    const std::string& source_file,
                                    int line,
                                    size_t binding_count);

    // Variable binding enforcement
    std::string checkVariableBinding(size_t binding_count, int line);

    // --- Governance Integrity (EVA-11/EVA-12) ---
    // Prevents LLM config manipulation by enforcing minimum levels
    void enforceMinimumLevels(GovernanceRules& r);

    // --- Function Contract Check ---
    // Verifies function return values against declared contracts
    std::string checkFunctionContract(const std::string& func_name,
                                       const std::string& result_str,
                                       const std::string& result_type,
                                       int line = 0,
                                       const std::string& source_file = "");

    std::string checkFunctionInputContract(const std::string& func_name,
                                            const std::vector<std::string>& arg_types,
                                            int line = 0);

    // --- Complexity Floor Check ---
    // Verifies that functions meet minimum structural complexity
    std::string checkComplexityFloor(const std::string& code,
                                      const std::string& function_name,
                                      int line = 0);

    // --- Behavioral Contract Check ---
    // Verifies function body contains required call patterns (must_call)
    std::string checkFunctionBehavioralContract(
        const std::string& func_name,
        const std::string& func_body,
        int line = 0,
        int param_count = -1);

    // --- Execution-Based Contract Tests ---
    // Runs must_produce golden tests after program execution (post-execution pass)
    std::string runExecutionContracts();

    // --- NAAb Function Body Quality Check ---
    // Scans ALL NAAb function bodies for stubs/oversimplification
    std::string checkNaabFunctionBody(const std::string& function_name,
                                       const std::string& source_code,
                                       int line = 0,
                                       const std::string& source_file = "",
                                       int param_count = -1);

    // --- Polyglot Optimization Checks ---
    std::string checkPolyglotOptimization(const std::string& language,
                                          const std::string& code,
                                          int line = 0);

    // --- Empirical Profiling ---
    void writeProfileEntry(const std::string& language,
                           const std::string& task_category,
                           const std::string& code_hash,
                           int64_t duration_us);
    bool loadCalibration();
    bool isProfilingEnabled() const;
    const std::map<std::string, std::map<std::string, CalibrationEntry>>&
        getCalibrationData() const { return calibration_data_; }

    void suggestBetterLanguage(const std::string& current_lang,
                               const std::string& code,
                               const std::string& task_type,
                               const std::vector<std::string>& optimal_langs,
                               int improvement_percent,
                               const std::vector<std::string>& reasons);

    // --- Output Baselines ---
    std::string checkBaseline(const std::string& key, const std::string& output,
                               const std::string& type, int line = 0);
    void recordBaseline(const std::string& key, const std::string& output,
                         const std::string& type);

    // --- Drift Tracking ---
    void writeDriftEvent(const std::string& language, const std::string& task_type,
                         const std::string& code_hash, const std::string& expected,
                         const std::string& got, int line, int consensus, int total,
                         const std::string& file);
    void analyzeDriftTrend(const std::string& language);

    // --- Polyglot Consensus Verification ---
    // Returns error string on hard enforcement failure, empty string otherwise
    std::string verifyPolyglotResult(
        const std::string& language,
        const std::string& code,
        const std::string& result_str,
        int line = 0);
    bool isVerificationEnabled() const;

    // --- Summary & Reporting ---
    const std::vector<CheckResult>& getCheckResults() const { return check_results_; }
    std::string formatSummary() const;
    std::string formatSummaryOneLine() const;
    void printDashboard() const;
    void resetCheckResults() { check_results_.clear(); }

    // Feature 1: Check if any HARD violation was recorded
    bool wasBlocked() const;

    // Feature 2: Quality gate evaluation — returns empty if passed, error msg if failed
    std::string evaluateQualityGate() const;

    // Cumulative risk scoring
    int getCumulativeScore() const { return cumulative_score_; }
    std::string formatScoreBreakdown() const;
    bool verifyScoreIntegrity() const;

    // --- Scoring Calibration (operator-driven weight overrides) ---
    bool calibrateRule(const std::string& rule_name, int weight_override, const std::string& reason);
    bool resetCalibration(const std::string& rule_name);
    std::unordered_map<std::string, ScoringCalibrationEntry> getCalibrationOverrides() const;

    // --- Environment Attestation ---
    std::vector<AttestationResult> runAttestation();

    // --- Contradiction Detection ---
    std::vector<ContradictionResult> detectContradictions();

    // Feature 4: Governance baseline regression detection
    void saveGovernanceBaseline() const;
    std::string checkGovernanceBaseline() const;  // empty = no regression

    // Drift detection: structural regression gate
    struct DriftMetrics {
        int functions = 0;
        int exports = 0;
        int structs = 0;
        int loc = 0;
        bool has_main = false;
        std::vector<std::string> function_names;
        std::vector<std::string> export_names;
        std::map<std::string, int> param_counts;         // Gate 1: per-function param count
        std::vector<std::string> imports;                 // Gate 2: use statement module names
        std::map<std::string, int> complexity_scores;     // Gate 3: per-function complexity
        int comment_lines = 0;                            // Gate 4: comment line count
        int code_lines = 0;                               // Gate 4: code line count
        int polyglot_blocks = 0;                          // Gate 6: polyglot block count
        std::vector<std::string> polyglot_languages;      // Gate 6: unique languages used
        std::map<std::string, std::vector<std::string>> struct_fields;  // Gate 7: per-struct field names
        std::vector<std::string> test_functions;           // Gate 8: test_* function names
        std::map<std::string, std::string> body_hashes;    // Gate 11: SHA-256 of function bodies
        std::map<std::string, double> param_utilization;   // Gate 12: fraction of params used in body
        bool config_present = false;                       // Gate 13: was govern.json found?
        std::string config_hash;                           // Gate 13: SHA-256 of govern.json content
        std::string script_dir;                            // Gate 14: canonical directory of script
        bool signature_present = false;                    // Gate 16: was govern.json.sig found?
        bool baseline_signature_present = false;           // Gate 16b: was drift-baseline.json.sig found?
        std::map<std::string, int> polyglot_loc;           // Gate 17: per-function polyglot line counts
        std::string main_body_hash;                        // Gate 11b: SHA-256 of main{} body
    };
    static DriftMetrics collectDriftMetrics(const ast::Program& program,
                                            const std::string& source,
                                            const std::string& script_path);
    static std::string extractMainBodyPublic(const std::string& source);
    std::string checkDriftDetection(const std::string& filename,
                                    const DriftMetrics& current);
    void saveDriftBaseline(const std::string& filename,
                           const DriftMetrics& metrics) const;
    std::string resolveDriftBaselinePath() const;

    // Integrity: Ed25519 + legacy HMAC signature verification (V-SC-009)
    bool verifyFileSignature(const std::string& file_path) const;
    bool verifyContentSignature(const std::string& file_path,
                                 const std::string& content) const;
    bool verifySignatureImpl(const std::string& file_path,
                              const std::string& content) const;
    static bool signFile(const std::string& file_path);
    bool isBlockedFlag(const std::string& flag) const;
    static std::string getKeyFingerprint();

    // Read private key PEM from NAAB_SIGNING_KEY env var (for CLI key-signing operations).
    static std::string readSigningKeyForCLI();

    // Feature 5: Environment selector
    void applyEnvironment(const std::string& env_name);

    // Phase 8.4: Runtime version pinning
    void checkRuntimeVersions(const std::string& language,
                               const std::string& observed_version);

    // --- Advisory Output Control ---
    void emitAdvisory(const std::string& msg);
    void flushGroupedAdvisories();

    // Report generation
    std::string generateJsonReport() const;
    std::string generateSarifReport() const;
    std::string generateJunitReport() const;
    std::string generateCsvReport() const;
    std::string generateHtmlReport() const;
    void writeReports() const;
    std::string generateExplanation(const std::string& rule_name,
                                    EnforcementLevel level,
                                    bool passed,
                                    const std::string& rationale) const;

    // Agent review: LLM-based governance phase (detection → validation → scoring)
    void runAgentReview(const std::string& source);
    void setSource(const std::string& source);
    void runGovernanceVoice();

    // --- Behavioral Sequence Detection ---
    std::string emitEvent(RuntimeEventType type, const std::string& detail,
                          const std::string& file = "", int line = 0,
                          const std::string& content_fingerprint = "",
                          int output_tokens = 0, int thinking_tokens = 0,
                          const std::unordered_set<std::string>& content_keywords = {},
                          int input_tokens = 0, bool thinking_reported = true);
    void setAgentTurn(int handle_id, int turn);
    void setAgentContext(int handle_id, int turn, const std::string& config_name);
    int getCurrentAgentTurn() const { return current_agent_turn_.load(std::memory_order_relaxed); }
    void setInheritedPressure(int handle_id, double pressure);
    void recoverCoherence(int handle_id);  // F15: coherence recovery at pipeline transitions
    void setPipelineDepth(int handle_id, int depth);  // F3: set pipeline nesting depth
    int consumeRiskBudget(const std::string& config, int cost);   // F8: consume budget, return remaining
    int getRemainingBudget(const std::string& config) const;      // F8: query remaining budget
    std::string checkGovernanceHealth(int turn);    // F4: verify governance instrumentation
    void emitEndOfRunHealthWarnings(FILE* fp, const std::string& timestamp) const;  // Fix 5B: end-of-run safety net
    double computeGovernanceEntropy() const;        // F16: entropy of governance check results
    GovernanceLevel getGovernanceLevel() const;     // F6: current system-wide governance level
    PulseVerdict computePulseVerdict(int turn);     // compute and update pulse health
    PulseVerdict getPulseVerdict() const;            // read current pulse verdict
    GovernancePulse getPulse() const;                // full pulse struct for dashboard/stdlib
    int getGovernanceEpoch() const;                   // monotonic evidence epoch counter
    // De-escalation hysteresis state, for telemetry. The engine knows whether
    // a turn counted as calm and which handle owns the counter; nothing
    // emitted it, so the predicate had to be reconstructed externally -- and
    // shipped WRONG TWICE before matching (first "any analyzed turn", then
    // "turn with no penalty", neither of which is what the engine tests).
    int getDeescalateCalmTurns() const;
    int getDeescalatePressureHandle() const;
    static int verifyTelemetryChain(const std::string& filepath,
        const std::string& hmac_key = "");            // CLI: --verify-telemetry-chain
    int checkDecisionTraceCoherence(const std::string& agent_config);  // F17: contradictions in traces
    std::string checkTemporalCoupling();  // F10: inter-agent timing correlation
    std::string checkAdmission(const std::string& agent_config);
    // The CRITICAL-level suspension alone, without checkAdmission()'s spend
    // projection. agent.commit() applies this: committing makes no API call, so
    // the projection would refuse a free transition, but a CRITICAL suspension
    // must still hold at the moment a proposal becomes real.
    std::string checkCriticalSuspension(const std::string& agent_config);

    // Output admissibility — post-CDD gate on response coherence
    struct OutputAdmissibilityResult {
        bool admissible = true;
        double coherence_score = 1.0;
        double threshold = 0.70;
        std::string action;
    };
    OutputAdmissibilityResult checkOutputAdmissibility(int handle_id, int turn,
        const std::string& agent_config);
    void emitOutputAdmissibilityAttestation(const std::string& agent_config,
        int turn, double coherence_score, double threshold);
    // Per-tool-call gate (gate_tool_calls): throws per the configured OA level.
    [[noreturn]] void enforceOutputAdmissibilityGate(const std::string& agent_config,
        const std::string& tool_name, double coherence_score);

    std::string recordAutonomousAction(const std::string& agent_config);
    int getAutonomousActionCount() const;
    size_t getUniqueAgentCount() const;
    std::string checkBehavioralSequence(const governance::SequenceMatchResult& match);
    std::string checkPreExecution(governance::RuntimeEventType type,
                                   const std::string& detail,
                                   const std::string& file = "",
                                   int line = 0);
    std::string checkContextDrift(int handle_id, int turn,
                                  const std::string& error = "");

    // Agent environment: get drift state for environment dict
    std::optional<governance::DriftState> getDriftState(int handle_id) const;

    // Get minimum coherence across all tracked agents (for multi-agent dashboard/health)
    double getMinAgentCoherence() const;

    // Initialize mandate keywords for semantic mandate alignment signal
    void initializeMandateKeywords(int handle_id, const std::unordered_set<std::string>& keywords);

    // Bind per-agent CDD signal overrides (context_drift_signals) to a handle
    void setSignalOverrides(int handle_id, const std::map<std::string, bool>& overrides);

    // Add instruction keywords from user prompts (for instruction recall signal).
    // visible_turns: turns visible in the context window sent (0 = no windowing).
    void addInstructionKeywords(int handle_id, const std::unordered_set<std::string>& keywords,
                                int visible_turns = 0);

    // Extract plan steps from agent response (for plan drift signal)
    void extractPlanFromResponse(int handle_id, const std::string& response_text);

    // Record tool result keywords (for tool chain integrity signal)
    void recordToolResult(int handle_id, const std::string& tool_name,
                          const std::unordered_set<std::string>& result_keywords);

    // Record tool execution outcome for claim-result reconciliation
    void recordToolOutcome(int handle_id, const std::string& tool_name, bool success);

    // Record external validation outcome (S22) — forwards to the drift analyzer.
    bool recordValidationOutcome(int handle_id, bool passed,
        const std::unordered_set<std::string>& detail_keywords = {},
        int evidence_count = -1);

    // Set per-turn prompt keywords (for prompt compliance signal)
    void setTurnPromptKeywords(int handle_id, const std::unordered_set<std::string>& keywords);

    // Record governance level escalation for effectiveness tracking
    void recordEscalation(int handle_id, int from_level, int to_level);

    // Update quarantine streak: increments on quarantine, resets on admissible.
    int updateQuarantineStreak(int handle_id, bool quarantined);

    // Reset drift state for a specific handle (agent.reset() — fresh conversation).
    void resetDriftState(int handle_id);

    // Reality checkpoint: get pressure data for response dict
    struct CheckpointData {
        bool fired = false;         // true if checkpoint fired this turn (ADVISORY)
        double pressure = 0.0;
        int sustained_turns = 0;
    };
    CheckpointData getCheckpointData(int handle_id, int turn) const;

    // Execution attestation: signed proof that governance checks passed for an action
    void emitAttestation(const std::string& action_type,
        const std::string& agent_config, int turn, double pressure);

    // Refusal attestation: non-binding evidence that governance blocked an action
    void emitRefusalAttestation(const std::string& rule_name,
        EnforcementLevel level, const std::string& enforcement_path,
        const std::string& violation_message);

    // FIX-DX-8: Scope pattern validation
    void validateScopePatterns(const std::vector<std::string>& function_names);

    // --- Audit Trail ---
    void logAuditEvent(const std::string& event_type,
                       const std::string& rule_name,
                       const std::string& message,
                       const std::string& file = "",
                       int line = 0);

    void logPolyglotExecution(const std::string& language,
                              const std::vector<std::string>& bound_vars,
                              int64_t duration_us,
                              const std::string& file = "",
                              int line = 0,
                              const std::string& runtime_version = "");

    void logTaintDecision(const std::string& var_name,
                          const std::string& decision,
                          const std::string& sink,
                          const std::string& file = "",
                          int line = 0);

    void logContractCheck(const std::string& func_name,
                          const std::string& result,
                          const std::string& detail,
                          const std::string& file = "",
                          int line = 0);

    // --- Pass 2: Post-Execution Audit ---
    void addPolyglotExecution(const PolyglotExecutionRecord& record);
    void addTaintFlow(const TaintFlowRecord& flow);
    void addSideEffect(const std::string& type, const std::string& detail,
                       const std::string& file, int line);
    void runPostExecutionAudit();

    // --- Taint Tracking ---
    void markTainted(const std::string& var_name,
                     const std::string& origin_func = "",
                     const std::string& origin_arg = "",
                     const std::string& file = "",
                     int line = 0);
    void clearTaint(const std::string& var_name);
    bool isTainted(const std::string& var_name) const;
    std::optional<TaintMetadata> getTaintLineage(const std::string& var_name) const;
    std::string checkTaintedSink(const std::string& var_name,
                                  const std::string& sink_type,
                                  const std::string& file, int line);
    bool isTaintSource(const std::string& func_name) const;
    bool isSanitizer(const std::string& func_name) const;

    // BUG-O: Save/restore taint state for module loading isolation
    std::unordered_set<std::string> saveTaintState() const;
    void restoreTaintState(const std::unordered_set<std::string>& state);

    // V-GOV-004: counter state snapshot for async task inheritance.
    // Mirrors saveTaintState/restoreTaintState — counters reset to zero in each
    // new Interpreter; async tasks must inherit parent's accumulated counts so
    // that limits like polyglot_blocks are not bypassed by spawning async tasks.
    struct CounterState {
        int polyglot_block_count = 0;
        int total_polyglot_lines = 0;
        int advisory_count       = 0;
        int advisory_suppressed  = 0;
    };
    CounterState saveCounterState() const;
    void restoreCounterState(const CounterState& state);

    // BUG-D: Track if last function return was tainted
    bool lastReturnWasTainted() const;
    void setLastReturnTainted(bool v);
    void setLastReturnTainted(bool v, const std::string& source_func);
    const std::string& lastTaintSource() const { return last_taint_source_; }

    // --- Hooks ---
    void fireHook(const HookConfig& hook,
                  const std::unordered_map<std::string, std::string>& vars);

    // --- Schema Validation ---
    static std::vector<std::string> validateSchema(const std::string& json_path);

private:
    // --- extends / policy distribution ---
    bool loadWithExtends(const std::string& path, int depth,
                         int max_depth, std::set<std::string>& visited,
                         GovernanceRules& child_rules);
    static void mergeRules(const GovernanceRules& base, GovernanceRules& child,
                           const InheritanceConfig& cfg);
    static std::string resolveExtendsPath(const std::string& extends_val,
                                          const std::string& config_dir);

    // C1: thread-local snapshot for data-race-free rules access
    class RulesSnapshot;
    friend class RulesSnapshot;
    const GovernanceRules& rules() const;
    std::shared_ptr<const GovernanceRules> rulesPtr() const;

    std::atomic<bool> active_{false};
    bool override_enabled_ = false;
    std::string override_reason_;
    std::unordered_map<std::string, int> override_counts_;
    mutable std::unordered_map<std::string, ApprovalToken> approval_cache_;
    mutable int64_t approval_mtime_ = 0;
    mutable std::mutex approval_mutex_;
    std::string loaded_path_;
    std::string last_error_;   // "not_found" or empty when loaded successfully
    std::shared_ptr<const GovernanceRules> rules_ptr_;
    std::vector<CheckResult> check_results_;
    mutable std::mutex results_mutex_;  // V-CONC-007: Thread-safe check_results_ access
    static constexpr size_t MAX_CHECK_RESULTS = 10000;  // V-GOV-024: Cap telemetry entries
    GovernancePulse pulse_;  // Governance self-health assessment (writes under results_mutex_)
    std::string agent_id_ = "anonymous";
    std::string run_id_;  // Unique per-execution ID for telemetry run separation
    std::string active_env_;            // Set by applyEnvironment()
    std::string current_check_file_;    // Set by setCheckContext() for report tracking
    int current_check_line_ = 0;        // Set by setCheckContext() for report tracking
    std::unordered_map<std::string, int> emitted_advisories_;  // Advisory occurrence counts (escalation)
    void decayAdvisoryHistory();  // halve occurrence counts on epoch boundary (caller must hold results_mutex_)
    bool preflight_mode_ = false;  // F8: marks results as preflight during preflightIntentCheck
    std::unordered_set<std::string> taint_set_;
    std::unordered_map<std::string, TaintMetadata> taint_lineage_;
    mutable std::mutex taint_mutex_;  // BUG-N: Thread-safe taint operations
    bool last_return_tainted_ = false;  // BUG-D: Track function return taint
    std::string last_taint_source_;      // Lineage: which source function produced the taint

    // Execution-based contracts (v6)
    bool in_contract_test_ = false;  // Re-entrancy guard for execution contracts
    ContractCallFn vm_call_fn_;        // VM function caller (type-erased)
    ContractGlobalsFn vm_globals_fn_;  // VM globals accessor (type-erased)
    interpreter::NaabVal jsonStringToNaabVal(const std::string& json_str);
    interpreter::NaabVal callContractTestFunction(
        const std::string& func_name, const std::vector<interpreter::NaabVal>& args);
    std::string checkMustProduce(const std::string& func_name,
                                  const FunctionContract& contract, int line);
    std::string checkMustVary(const std::string& func_name,
                               const FunctionContract& contract, int line);
    std::string checkMustDifferentiate(const std::string& func_name,
                                       const FunctionContract& contract, int line);
    std::string checkMustHandleCase(const std::string& func_name,
                                     const FunctionContract& contract, int line);
    std::string checkMustSatisfy(const std::string& func_name,
                                  const FunctionContract& contract, int line);

    // Behavioral Sequence Detection
    BehavioralSequenceDetector sequence_detector_;
    ContextDriftAnalyzer drift_analyzer_;
    std::atomic<int> current_agent_turn_{0};
    std::atomic<int> current_agent_handle_{0};
    std::string current_agent_config_;         // agent config name for BSD identity
    mutable std::mutex agent_config_mutex_;     // guards current_agent_config_
    std::atomic<bool> bsd_enabled_{false};

    // Exposure tracking: aggregate autonomous action counters
    std::atomic<int> autonomous_actions_{0};
    std::unordered_set<std::string> unique_agents_;
    // Per-handle pipeline depth — authoritative across worker threads
    std::unordered_map<int, int> pipeline_depths_;
    mutable std::mutex exposure_mutex_;         // guards unique_agents_ + pipeline_depths_
    std::atomic<bool> cdd_enabled_{false};
    std::atomic<bool> agent_governance_active_{false};  // Fix 3B: only count consecutive_passes during agent phase

    // F8: Per-agent risk budget
    std::unordered_map<std::string, int> agent_risk_consumed_;
    mutable std::mutex risk_budget_mutex_;

    // F17: Decision trace coherence
    std::unordered_map<std::string, std::deque<std::string>> agent_decision_traces_;
    mutable std::mutex trace_history_mutex_;

    // F10: Temporal coupling — per-agent event turn tracking
    std::unordered_map<std::string, std::vector<int>> agent_event_turns_;
    mutable std::mutex temporal_mutex_;

    // F6: System-wide governance level
    std::atomic<int> governance_level_{0};  // GovernanceLevel::NORMAL
    // De-escalation hysteresis: consecutive turns where the computed target
    // level sat below governance_level_. Stepping down one level requires
    // circuit_breaker.deescalate_sustained such turns (escalation stays instant).
    std::atomic<int> deescalate_calm_turns_{0};
    // Handle whose pressure most recently raised or held the level. Calm turns
    // count toward de-escalation only when they come from THIS handle — a calm
    // sibling agent says nothing about the degraded one (all handles share the
    // system-wide governance level). 0 = none recorded.
    std::atomic<int> deescalate_pressure_handle_{0};

    // Governance plugins
    std::string govern_json_dir_;           // Directory containing govern.json
    bool plugins_loaded_ = false;           // Lazy loading flag
    bool in_plugin_check_ = false;          // Re-entrancy guard
    std::unordered_set<std::string> warned_plugin_rules_;  // Dedup warnings

    // Advisory dedup tracking
    struct DupCallEntry { std::string function_name; int count; int line; };
    std::unordered_map<std::string, std::vector<DupCallEntry>> dup_call_summary_;
    std::vector<std::pair<std::string, int>> ptc_functions_; // polyglot try/catch: {name, line}
    int advisory_count_ = 0;
    int advisory_suppressed_ = 0;
    int agent_review_count_ = 0;  // confirmed findings from agent review phase
    bool agent_review_voiced_ = false;  // true when voice summary was printed (suppress per-rule list)
    bool governance_voiced_ = false;    // true when governance voice summary was printed
    std::string governance_voice_summary_;  // synthesized remediation guide
    std::string source_;                    // script source (for voice phase)

    // Attestation results (populated by runAttestation())
    bool attestation_passed_ = true;
    std::vector<AttestationResult> attestation_results_;

    // Cumulative risk scoring
    // INVARIANT: cumulative_score_ >= 0 (monotonic — only increases)
    // INVARIANT: cumulative_score_ == sum of enforce()-path ADVISORY weights
    // NOTE: Pass 2 entries (rule_name prefix "pass2.") bypass enforce() and are excluded
    // INVARIANT: cumulative_score_ <= SCORE_SATURATION_LIMIT
    static constexpr int SCORE_SATURATION_LIMIT = 100000;
    int cumulative_score_ = 0;
    std::unordered_map<std::string, int> score_contributions_;

    // Scoring calibration — operator-driven weight overrides
    mutable std::unordered_map<std::string, ScoringCalibrationEntry> scoring_calibration_;
    mutable std::atomic<bool> scoring_calibration_loaded_{false};
    mutable std::mutex calibration_mutex_;  // guards scoring_calibration_ + dirty flag
    bool scoring_calibration_dirty_ = false;
    void loadScoringCalibration() const;
    void saveScoringCalibration() const;

    // Evidence Epoch — monotonic counter incremented on state transitions
    // Prior-epoch evidence is discounted (database MVCC / court jurisdiction analog)
    std::atomic<int> governance_epoch_{0};
    bool score_yellow_warned_ = false;

    // Ed25519 signature warning dedup (per engine instance, not static)
    mutable std::set<std::string> signature_warned_files_;

    // Pass 2: Post-execution audit data
    std::vector<PolyglotExecutionRecord> polyglot_executions_;
    std::vector<TaintFlowRecord> taint_flows_;
    std::vector<SideEffectRecord> side_effects_;
    std::vector<CrossBlockFlow> cross_block_flows_;

    // Pass 2: Sub-audit functions
    void auditPolyglotOutputs();
    void auditTaintFlows();
    void auditDeterminism();
    void auditSemanticCorrectness();
    void auditCrossBlockFlows();
    void auditSideEffects();
    std::string computeCoverageReport() const;
    void printValidationReport();
    std::string checkDeterminism(const std::string& language, const std::string& code, int line);
    std::string checkOutputEntropy(const std::string& output, int line);
    std::string checkErrorDumps(const std::string& output, int line);

    // Rate limiters
    RateLimiter polyglot_rate_;
    RateLimiter stdlib_rate_;
    RateLimiter file_ops_rate_;

    // Execution counters
    int polyglot_block_count_ = 0;
    int total_polyglot_lines_ = 0;

    // Audit trail
    std::string last_audit_hash_;
    mutable std::string last_telemetry_hash_;
    // File-anchored telemetry chain state (all guarded by telemetry_hash_mutex_).
    // The chain links across runs and processes: prev_hash is seeded from the
    // output file's tail, not from the constant genesis, on every write.
    mutable bool run_start_emitted_ = false;
    mutable bool run_end_emitted_ = false;
    mutable long long chained_events_this_run_ = 0;
    // Count declared by the most recent RunEnd. writeReports() is called from
    // ~17 sites and is not once-per-run: a clean execute() writes and seals the
    // run, then main.cpp's contract / quality-gate / baseline exits call it
    // again. Anything chained after a RunEnd must be reconciled by a fresh one.
    mutable long long run_end_declared_ = -1;
    // How many check_results_ have already been dumped to telemetry, and the
    // dedup keys seen across all dumps. check_results_ is never cleared, so
    // without these a second writeReports() re-emits the entire set.
    mutable size_t telemetry_results_dumped_ = 0;
    mutable std::unordered_set<std::string> telemetry_dedup_seen_;
    // Score at the last ScoringSnapshot; -1 = none emitted yet.
    mutable int scoring_snapshot_last_score_ = -1;
    // Returns the prev_hash for the next chained event (file tail → in-memory →
    // genesis) and lazily writes the RunStart anchor on this process's first
    // chained event. Requires telemetry_hash_mutex_ held and fp open+locked.
    std::string chainPrevLocked(FILE* fp) const;
    // Writes the chained RunEnd anchor declaring this run's chained event
    // count. Re-emitted if further chained events followed the previous one —
    // the verifier reads the last RunEnd per run_id, so the newest declaration
    // reconciles the count. Locks telemetry_hash_mutex_ internally.
    void emitRunEnd(FILE* fp, const std::string& timestamp) const;
    mutable std::mutex audit_mutex_;
    std::atomic<int> audit_write_failures_{0};
    mutable std::atomic<int> telemetry_write_failures_{0};

    // Telemetry forwarding (webhook/SIEM)
    mutable std::shared_ptr<TelemetryForwarder> telemetry_forwarder_;
    mutable std::mutex telemetry_fwd_mutex_;  // guards pointer swap during reload/destruction
    mutable std::mutex telemetry_hash_mutex_;  // guards last_telemetry_hash_ across concurrent writes
    mutable std::mutex audit_data_mutex_;  // guards taint_flows_, polyglot_executions_, cross_block_flows_, side_effects_

    // Calibration data (loaded from calibration.json)
    std::map<std::string, std::map<std::string, CalibrationEntry>> calibration_data_;
    bool calibration_loaded_ = false;

    // Baselines data (lazy-loaded from baselines.json)
    // Uses void* to avoid nlohmann/json.hpp in header (kept in .cpp only)
    void* baselines_data_ = nullptr;
    bool baselines_loaded_ = false;
    bool baselines_dirty_ = false;
    std::string baselines_path_;
    int drift_write_count_ = 0;
    void loadBaselines();
    void saveBaselines();

    // --- Mid-run reload state (Governance Under Survivability) ---
    int64_t loaded_mtime_ns_ = 0;                 // govern.json mtime as nanoseconds (fs clock epoch)
    int64_t last_sig_fail_mtime_ = 0;             // mtime of last signature-failed reload (suppress duplicate logs)
    // Atomic because agent worker threads read it via getReloadCount() while
    // reloadIfChanged() writes it under reload_mutex_ — a different lock, so the
    // mutex does not order the read. Mirrors governance_epoch_ above.
    std::atomic<int> reload_count_{0};            // reloads applied this run
    std::vector<std::string> pending_notices_;    // notices from last reload
    mutable std::mutex notices_mutex_;            // guards pending_notices_
    mutable std::mutex reload_mutex_;             // serializes reload attempts

    // --- Decision trace accumulator (storage is thread_local in .cpp) ---
    void addTrace(const std::string& step);
    void clearTrace();
    std::string lookupRationale(const std::string& rule_name) const;

    // --- Core enforcement ---
    std::string enforce(const std::string& rule_name,
                       EnforcementLevel level,
                       const std::string& violation_message);
    void recordPass(const std::string& rule_name, EnforcementLevel level);

    // --- Formatting ---
    static std::string formatError(EnforcementLevel level,
                                   const std::string& what,
                                   const std::string& location,
                                   const std::string& rule,
                                   const std::string& help,
                                   const std::string& bad_example,
                                   const std::string& good_example);
    static std::string levelToString(EnforcementLevel level);
    static std::string levelToTag(EnforcementLevel level);

    // --- Loading helpers ---
    void applyScopes(const std::string& source_file);
    std::string substituteEnvVars(const std::string& value) const;

    // --- Entropy analysis ---
    static double calculateEntropy(const std::string& str);
    static bool looksLikeBase64(const std::string& str);
    static bool looksLikeHex(const std::string& str);

    // --- Schema validation helpers ---
    static size_t levenshteinDistance(const std::string& s1, const std::string& s2);
    static std::string suggestKey(const std::string& key,
                                   const std::vector<std::string>& valid_keys);

    // --- Verification helpers ---
    struct VerificationResult {
        std::string language;
        std::string result;
        int64_t duration_us;
        bool success;
        std::string error;
    };

    std::string generateVerificationCode(const std::string& task_type,
        const std::string& original_code, const std::string& original_result,
        const std::string& source_lang, const std::string& target_lang);
    std::string generateEchoCode(const std::string& target_lang, const std::string& value);
    std::string extractMathExpression(const std::string& code, const std::string& lang);
    std::string escapeStringForVerification(const std::string& s);
    bool compareResults(const std::string& a, const std::string& b, double tolerance);
    bool isNumericString(const std::string& s);
    std::string classifyTaskForVerification(const std::string& code, const std::string& language);

    // --- Audit helpers ---
    std::string computeAuditHash(const std::string& data) const;
    std::string computeHash(const std::string& data, const TamperEvidenceConfig& te) const;
};

} // namespace governance
} // namespace naab
