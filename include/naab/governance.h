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
};

// ============================================================================
// Section 12: Hooks
// ============================================================================

struct HookConfig {
    std::string command;
    std::vector<std::string> args;
    int timeout = 5;
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
    // and must lose half its coherence before enforcement fires. Empirically, well-behaved agents
    // rarely drop below 0.7; adversarial agents cross 0.5 within 5-8 turns.
    double coherence_threshold = 0.5;
    int max_contradictions = 5;
    int check_interval_turns = 3;
    int fingerprint_window = 20;
    bool rate_normalized = false;  // F5: scale penalties by rate (count/turns) instead of flat weight
    // Rationale: 0.2 = partial recovery (20% of full coherence). Prevents full reset after a single
    // correct action, requiring sustained good behavior to fully recover. Analogous to credit
    // restoration: recent good conduct helps but doesn't erase history immediately.
    double coherence_recovery_amount = 0.2;
    double coherence_natural_healing = 0.0;  // F15: per-turn recovery when no signals fire (0 = disabled)
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
    struct Signals {
        bool repeated_failures = true;
        bool circular_actions = true;
        bool scope_creep = true;
        bool intent_contradictions = true;
        bool vocabulary_contraction = true;
        bool coherence_velocity = true;       // F1: fire when coherence decaying rapidly
        bool capability_underutilization = false; // F9: fire on sudden late capability use
        bool semantic_stability = false;       // F19: fire on content topic shift (expensive)
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
        double coherence_velocity = 0.12;     // F1: moderate — velocity drop is a derivative signal
        double capability_underutilization = 0.1; // F9: low — may be legitimate deferred usage
        double semantic_stability = 0.1;       // F19: low — topic shifts are common in exploration
    } weights;

    // Signal detection thresholds — tune sensitivity of individual CDD signals.
    // Rationale for each default documented inline.
    struct Thresholds {
        // -0.15: a 15% coherence drop in one turn. Normal turns show ≤5% variation;
        // -0.15 is 3x normal noise, filtering jitter while catching real drops.
        double velocity_drop = -0.15;
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
    } thresholds;

    // Reality Checkpoint: composite operational pressure detection
    struct RealityCheckpoint {
        bool enabled = false;
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
    // 0.3 = 30% keyword overlap: the agent's challenge response must contain at least
    // 30% of the system_prompt's key terms. Low enough to allow paraphrasing, high
    // enough to catch completely unrelated responses.
    double step_up_keyword_threshold = 0.3;
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
    int consecutive_passes = 0;       // reset on any block/enforcement
    int advisory_count = 0;           // advisory findings emitted (did not block execution)
    int refusal_count = 0;            // enforcement refusals attested (all blocking paths)

    // Per-subsystem health
    bool bsd_connected = true;
    bool cdd_connected = true;
    bool telemetry_connected = true;
    double entropy = -1.0;            // -1 = not yet computed

    // Hysteresis (sustained degradation required before transition)
    int consecutive_degraded = 0;
    int last_transition_turn = -100;  // cooldown between transitions

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

    // --- Forwarding (webhook/SIEM) ---
    std::string webhook_url;            // POST endpoint for JSON event batches
    std::string webhook_auth_header;    // full header value, e.g. "Bearer xxx"
    std::string webhook_auth_env;       // env var name for auth header value
    int forward_batch_size = 10;        // events per POST request
    int forward_timeout_ms = 5000;      // per-request timeout
    int forward_retry_count = 2;        // retries on transient failure
    int forward_buffer_max = 1000;      // max queued events before drop
    int forward_shutdown_drain_ms = 5000; // M2: max shutdown drain time
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
    bool shell_allowed = true;
    bool shell_allowed_set = false;
    // Per-agent network capability override (same pattern as shell_allowed)
    bool network_allowed = true;
    bool network_allowed_set = false;
    // Fine-grained action matrix — empty = all allowed
    // Actions: "SHELL_EXEC", "NET_CONNECT", "FS_READ", "FS_WRITE", "AGENT_SEND", "TOOL_EXEC", "CODEGEN_EXEC"
    std::vector<std::string> allowed_actions;

    // --- LLM config (populated from "agents" key in govern.json) ---
    std::string provider = "anthropic";
    std::string model;                          // primary model (first in chain)
    std::vector<std::string> model_chain;       // fallback models [model, fallback1, ...]
    std::string api_key_env = "ANTHROPIC_API_KEY";  // primary key env var
    std::vector<std::string> api_key_envs;      // rotation keys [primary, alt1, alt2, ...]
    int max_tokens = 4096;
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
    double temperature = 1.0;
    std::string stop_reason_action = "end";
    bool stream = false;
    int timeout_seconds = 30;  // F11: per-call LLM API timeout
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

    // Output Contract — validation schema for LLM responses (Phase 7)
    struct OutputContract {
        std::string format;  // "json", "text", etc. (empty = no format enforcement)
        std::vector<std::string> required_fields;  // for JSON: list of keys that must be present
        std::unordered_map<std::string, std::string> field_types;  // field → "string", "number", "boolean", etc.
        std::unordered_map<std::string, std::string> regex_checks;  // field → regex pattern to match
    };
    OutputContract output_contract;  // empty = no contract enforcement
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
    std::vector<AgentConfig> agents;  // was: agent_roles (migrated to unified config)
    std::vector<ScorerConfig> scorers; // named scorer configs from "scorers" section

    // --- Agent dispatch (parallel agent execution config) ---
    struct AgentDispatchConfig {
        int max_concurrent = 6;    // max concurrent agent API calls
        int pool_size = 6;         // thread pool worker count (I/O-bound, not CPU)
        int pool_queue_max = 50;   // max queued tasks before rejection
        int max_retries_per_run = 0; // 0 = unlimited

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
    int getReloadCount() const { return reload_count_; }

    // --- State ---
    bool isActive() const { return active_; }
    bool isOverrideEnabled() const { return override_enabled_; }
    void setOverrideEnabled(bool enabled) { override_enabled_ = enabled; }
    void setOverrideReason(const std::string& reason) { override_reason_ = reason; }
    const std::string& getOverrideReason() const { return override_reason_; }
    bool hasValidApproval(const std::string& rule_name, std::string& approver_id_out) const;
    const std::string& getLoadedPath() const { return loaded_path_; }
    const std::string& getLastError() const { return last_error_; }
    GovernanceMode getMode() const;
    const std::string& getGovernDir() const { return govern_json_dir_; }
    const GovernanceRules& getRules() const;
    std::shared_ptr<const GovernanceRules> getRulesPtr() const;
    GovernanceRules& getMutableRules();  // init-only — UB if called during concurrent execution

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
    using ContractCallFn = std::function<interpreter::NaabVal(interpreter::NaabVal, const std::vector<interpreter::NaabVal>&)>;
    using ContractGlobalsFn = std::function<const std::unordered_map<std::string, interpreter::NaabVal>&()>;
    void setVMCallbacks(ContractCallFn call_fn, ContractGlobalsFn globals_fn) {
        vm_call_fn_ = std::move(call_fn);
        vm_globals_fn_ = std::move(globals_fn);
    }
    void clearVMCallbacks() { vm_call_fn_ = nullptr; vm_globals_fn_ = nullptr; }

    // Call a NaabVal function via the VM callback (used by tool execution loop)
    interpreter::NaabVal callVMFunction(const interpreter::NaabVal& fn,
                                         const std::vector<interpreter::NaabVal>& args) {
        if (!vm_call_fn_) {
            throw std::runtime_error("Agent error: no VM callback available for tool execution");
        }
        return vm_call_fn_(fn, args);
    }
    bool hasVMCallbacks() const { return vm_call_fn_ != nullptr; }

    // --- Path access control ---
    std::string checkPathAccess(const std::string& filepath, const std::string& mode);

    // --- Telemetry ---
    void writeTelemetry() const;
    void writeAgentTelemetry(const std::string& event_type,
        const std::unordered_map<std::string, std::string>& fields);

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
                                    const std::string& code, int line = 0);
    std::string checkPrivilegeEscalation(const std::string& code, int line = 0);
    std::string checkDataExfiltration(const std::string& code, int line = 0);
    std::string checkResourceAbuse(const std::string& code, int line = 0);
    std::string checkInfoDisclosure(const std::string& language,
                                     const std::string& code, int line = 0);
    std::string checkCryptoWeakness(const std::string& code, int line = 0);
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
                          const std::string& file = "", int line = 0);
    void setAgentTurn(int handle_id, int turn);
    void setAgentContext(int handle_id, int turn, const std::string& config_name);
    void setInheritedPressure(int handle_id, double pressure);
    void recoverCoherence(int handle_id);  // F15: coherence recovery at pipeline transitions
    void setPipelineDepth(int handle_id, int depth);  // F3: set pipeline nesting depth
    int consumeRiskBudget(const std::string& config, int cost);   // F8: consume budget, return remaining
    int getRemainingBudget(const std::string& config) const;      // F8: query remaining budget
    std::string checkGovernanceHealth(int turn);    // F4: verify governance instrumentation
    double computeGovernanceEntropy() const;        // F16: entropy of governance check results
    GovernanceLevel getGovernanceLevel() const;     // F6: current system-wide governance level
    PulseVerdict computePulseVerdict(int turn);     // compute and update pulse health
    PulseVerdict getPulseVerdict() const;            // read current pulse verdict
    GovernancePulse getPulse() const;                // full pulse struct for dashboard/stdlib
    int getGovernanceEpoch() const;                   // monotonic evidence epoch counter
    int checkDecisionTraceCoherence(const std::string& agent_config);  // F17: contradictions in traces
    std::string checkTemporalCoupling();  // F10: inter-agent timing correlation
    std::string checkAdmission(const std::string& agent_config);
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
    const TaintMetadata* getTaintLineage(const std::string& var_name) const;
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
    mutable std::mutex exposure_mutex_;         // guards unique_agents_
    std::atomic<bool> cdd_enabled_{false};

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
    mutable std::mutex audit_mutex_;

    // Telemetry forwarding (webhook/SIEM)
    mutable std::shared_ptr<TelemetryForwarder> telemetry_forwarder_;
    mutable std::mutex telemetry_fwd_mutex_;  // guards pointer swap during reload/destruction
    mutable std::mutex telemetry_hash_mutex_;  // guards last_telemetry_hash_ across concurrent writes

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
    int reload_count_ = 0;                        // reloads applied this run
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
