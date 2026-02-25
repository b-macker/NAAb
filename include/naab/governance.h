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

namespace naab {
namespace governance {

// ============================================================================
// Core Enums
// ============================================================================

enum class EnforcementLevel {
    HARD,       // Block execution. No override possible.
    SOFT,       // Block execution. Override with --governance-override.
    ADVISORY    // Warn only. Execution continues.
};

enum class GovernanceMode {
    ENFORCE,    // Normal enforcement (default)
    AUDIT,      // Dry-run: check everything, block nothing
    OFF         // Disabled
};

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
    std::vector<std::string> allowed_commands;
    std::vector<std::string> blocked_commands;
    bool allow_pipes = true;
    bool allow_redirects = true;
    bool allow_backgrounding = true;
    int max_execution_time = 0;
};

struct EnvVarsCapability {
    bool read = true;
    bool write = true;
    std::vector<std::string> allowed_read;
    std::vector<std::string> blocked_read;
    std::vector<std::string> allowed_write;
    std::vector<std::string> blocked_write;
};

struct ProcessCapability {
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
    std::vector<std::string> allowlist;
    std::vector<std::string> blocklist_extra;
    bool check_chained_calls = false;
    bool check_string_formatting = false;
};

struct ShellInjectionRestriction {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::HARD;
    std::vector<std::string> patterns;
    std::vector<std::string> allowlist;
    bool check_variable_expansion = false;
    bool require_quoting = false;
};

struct ImportsRestriction {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::SOFT;
    std::string mode = "blocklist";
    std::unordered_map<std::string, std::vector<std::string>> blocked;  // lang -> imports
    std::unordered_map<std::string, std::vector<std::string>> allowed;  // lang -> imports
};

struct DataExfiltrationRestriction {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::HARD;
    bool block_base64_encode_secrets = true;
    bool block_hex_encode_secrets = true;
    bool block_url_encode_secrets = true;
    int max_encoded_output_length = 0;
};

struct ResourceAbuseRestriction {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::HARD;
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
    bool block_sudo = true;
    bool block_su = true;
    bool block_chmod_suid = true;
    bool block_setuid = true;
    bool block_capability_changes = true;
};

struct InfoDisclosureRestriction {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::SOFT;
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
    bool block_weak_hashing = true;
    std::vector<std::string> weak_hashes;      // e.g., "md5", "sha1"
    bool block_weak_encryption = true;
    std::vector<std::string> weak_ciphers;     // e.g., "des", "rc4"
    bool block_hardcoded_keys = true;
    bool block_hardcoded_ivs = true;
    bool require_secure_random = false;
    EnforcementLevel secure_random_level = EnforcementLevel::ADVISORY;
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
    std::unordered_map<std::string, PatternWithSeverity> patterns;
    std::vector<PatternWithSeverity> custom_patterns;
    std::vector<std::string> allowlist;
    EntropyCheckConfig entropy_check;
    SuspiciousVariableNames suspicious_variable_names;
};

struct NoPlaceholdersConfig {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::SOFT;
    std::vector<std::string> markers;
    std::vector<std::string> custom_markers;
    bool ignore_in_comments_only = false;
    bool case_sensitive = false;
    int max_violations_before_block = 0;
};

struct NoHardcodedResultsConfig {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::ADVISORY;
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
    std::vector<std::string> patterns;
    std::vector<std::string> custom_patterns;
    bool case_sensitive = false;
};

struct NoSimulationMarkersConfig {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::HARD;
    std::vector<std::string> patterns;
    std::vector<std::string> custom_patterns;
    bool case_sensitive = false;
};

struct NoMockDataConfig {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::ADVISORY;
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
};

struct NoHallucinatedApisConfig {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::ADVISORY;
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
    std::unordered_map<std::string, ApologeticCategory> categories;
    std::vector<std::string> custom_patterns;
    bool scan_comments_only = true;
    bool scan_strings = false;
};

struct NoDeadCodeConfig {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::ADVISORY;
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
    std::vector<std::string> patterns;
    std::vector<std::string> custom_patterns;
    std::vector<std::string> allowlist;
    bool check_polyglot_only = true;
    bool check_naab_code = false;
};

struct NoUnsafeDeserializationConfig {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::HARD;
    std::vector<std::string> patterns;
};

struct NoSqlInjectionConfig {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::HARD;
    std::vector<std::string> patterns;
    std::vector<std::string> allowlist;
};

struct NoPathTraversalConfig {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::HARD;
    std::vector<std::string> patterns;
};

struct NoHardcodedUrlsConfig {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::ADVISORY;
    std::vector<std::string> patterns;
    std::vector<std::string> allowlist;
    bool check_internal_urls = false;
};

struct NoHardcodedIpsConfig {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::ADVISORY;
    std::vector<std::string> patterns;
    std::vector<std::string> allowlist;
};

struct MaxComplexityConfig {
    bool enabled = false;
    EnforcementLevel level = EnforcementLevel::ADVISORY;
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
    bool require_utf8 = true;
    bool block_null_bytes = true;
    bool block_control_characters = true;
    bool block_bom = false;
    bool block_unicode_bidi = true;
    bool block_homoglyph_attacks = true;
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
};

// ============================================================================
// Section 7: Custom Rules
// ============================================================================

struct CustomRule {
    std::string id;
    std::string name;
    std::string description;
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
};

// ============================================================================
// Section 10: Audit Trail & Provenance
// ============================================================================

struct TamperEvidenceConfig {
    bool enabled = false;
    std::string algorithm = "sha256";
    std::string chain_genesis = "NAAB-GOVERNANCE-GENESIS";
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
// Master Rules Structure
// ============================================================================

struct GovernanceRules {
    std::string version;
    GovernanceMode mode = GovernanceMode::ENFORCE;
    std::string extends_path;  // "extends" field
    std::string description;

    LanguagesConfig languages;
    CapabilitiesConfig capabilities;
    LimitsConfig limits;
    RequirementsConfig requirements;
    RestrictionsConfig restrictions;
    CodeQualityConfig code_quality;
    std::vector<CustomRule> custom_rules;
    std::vector<ScopeOverride> scopes;
    OutputConfig output;
    AuditConfig audit;
    MetaConfig meta;
    HooksConfig hooks;
    PolyglotConfig polyglot;

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
};

// ============================================================================
// Check Result Tracking
// ============================================================================

struct CheckResult {
    std::string rule_name;
    EnforcementLevel level;
    bool passed;
    std::string message;
    std::string category;     // for grouping in reports
    std::string severity;     // "critical", "high", "medium", "low"
    int line = 0;
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
    std::string previous_hash;
    std::string current_hash;
};

// ============================================================================
// Pattern Database Types
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

class GovernanceEngine {
public:
    GovernanceEngine() = default;

    // --- Loading ---
    bool loadFromFile(const std::string& path);
    bool discoverAndLoad(const std::string& start_dir);

    // --- State ---
    bool isActive() const { return active_; }
    bool isOverrideEnabled() const { return override_enabled_; }
    void setOverrideEnabled(bool enabled) { override_enabled_ = enabled; }
    const std::string& getLoadedPath() const { return loaded_path_; }
    GovernanceMode getMode() const { return rules_.mode; }
    const GovernanceRules& getRules() const { return rules_; }
    GovernanceRules& getMutableRules() { return rules_; }

    // --- Legacy Getters (backward compatible) ---
    int getTimeoutSeconds() const { return rules_.timeout_seconds; }
    int getMemoryLimitMB() const { return rules_.memory_limit_mb; }
    bool requiresErrorHandling() const { return rules_.require_error_handling; }
    bool requiresMainBlock() const { return rules_.require_main_block; }
    std::string getAuditLevel() const { return rules_.audit_level; }
    bool isTamperEvidenceEnabled() const { return rules_.tamper_evidence; }

    // --- Per-language getters ---
    int getTimeoutForLanguage(const std::string& lang) const;
    int getMaxLinesForLanguage(const std::string& lang) const;
    const LanguageConfig* getLanguageConfig(const std::string& lang) const;

    // --- Original checks (backward compatible) ---
    std::string checkLanguageAllowed(const std::string& language, int line = 0);
    std::string checkNetworkAllowed();
    std::string checkFilesystemAllowed(const std::string& mode);
    std::string checkShellAllowed();
    std::string checkCallDepth(size_t current_depth);
    std::string checkArraySize(size_t size);
    std::string checkPolyglotOutput(const std::string& output);
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
    std::string checkIncompleteLogic(const std::string& code, int line = 0);
    std::string checkHallucinatedApis(const std::string& language,
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

    // Resource limits
    std::string checkLoopIterations(size_t count);
    std::string checkPolyglotBlockCount(size_t count);
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

    // --- Summary & Reporting ---
    const std::vector<CheckResult>& getCheckResults() const { return check_results_; }
    std::string formatSummary() const;
    void resetCheckResults() { check_results_.clear(); }

    // Report generation
    std::string generateJsonReport() const;
    std::string generateSarifReport() const;
    std::string generateJunitReport() const;
    std::string generateCsvReport() const;
    std::string generateHtmlReport() const;
    void writeReports() const;

    // --- Audit Trail ---
    void logAuditEvent(const std::string& event_type,
                       const std::string& rule_name,
                       const std::string& message,
                       const std::string& file = "",
                       int line = 0);

    // --- Hooks ---
    void fireHook(const HookConfig& hook,
                  const std::unordered_map<std::string, std::string>& vars);

    // --- Schema Validation ---
    static std::vector<std::string> validateSchema(const std::string& json_path);

private:
    bool active_ = false;
    bool override_enabled_ = false;
    std::string loaded_path_;
    GovernanceRules rules_;
    std::vector<CheckResult> check_results_;

    // Rate limiters
    RateLimiter polyglot_rate_;
    RateLimiter stdlib_rate_;
    RateLimiter file_ops_rate_;

    // Execution counters
    int polyglot_block_count_ = 0;
    int total_polyglot_lines_ = 0;

    // Audit trail
    std::string last_audit_hash_;
    mutable std::mutex audit_mutex_;

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
    void loadInheritedConfig(const std::string& base_dir, int depth = 0);
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

    // --- Audit helpers ---
    std::string computeAuditHash(const std::string& data) const;
};

} // namespace governance
} // namespace naab
