/*
 * NAAb Governance — Public C API
 * Stable C ABI for embedding NAAb governance scanning in any language via FFI.
 *
 * Thread safety: each naab_gov_engine_t is independent. Do not share a single
 * engine across threads without external synchronization. Create one per thread
 * for concurrent scanning.
 *
 * Memory contract:
 *   - Functions returning char* return malloc'd strings. Caller MUST free
 *     via naab_gov_free_string().
 *   - naab_gov_last_error() returns an internal pointer — caller must NOT free.
 *   - naab_gov_version_string() returns a static pointer — caller must NOT free.
 *
 * Example:
 *   naab_gov_engine_t eng = naab_gov_create();
 *   naab_gov_load_config_string(eng, "{\"version\":\"3.0\",\"mode\":\"enforce\"}");
 *   char* result = naab_gov_scan(eng, "python", "eval('rm -rf /')", "agent.py", 1);
 *   // parse result as JSON: {"blocked": true, "error": "...", "report": {...}}
 *   naab_gov_free_string(result);
 *   naab_gov_destroy(eng);
 */

#ifndef NAAB_GOVERNANCE_H
#define NAAB_GOVERNANCE_H

#ifdef __cplusplus
extern "C" {
#endif

/* --- Visibility --- */
#if defined(_WIN32) || defined(__CYGWIN__)
#  if defined(NAAB_GOV_BUILDING_DLL)
#    define NAAB_GOV_API __declspec(dllexport)
#  elif defined(NAAB_GOV_USING_DLL)
#    define NAAB_GOV_API __declspec(dllimport)
#  else
#    define NAAB_GOV_API
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define NAAB_GOV_API __attribute__((visibility("default")))
#else
#  define NAAB_GOV_API
#endif

/* --- Opaque handle --- */
typedef struct naab_gov_engine_s* naab_gov_engine_t;

/* --- Enums --- */
typedef enum {
    NAAB_GOV_LEVEL_NONE             = 0,
    NAAB_GOV_LEVEL_HARD             = 1,
    NAAB_GOV_LEVEL_APPROVAL_REQUIRED = 2,
    NAAB_GOV_LEVEL_SOFT             = 3,
    NAAB_GOV_LEVEL_ADVISORY         = 4
} naab_gov_level_t;

typedef enum {
    NAAB_GOV_OK           =  0,
    NAAB_GOV_ERR_NULL_ARG = -1,
    NAAB_GOV_ERR_ALLOC    = -2,
    NAAB_GOV_ERR_CONFIG   = -3,
    NAAB_GOV_ERR_INTERNAL = -4
} naab_gov_error_t;

/* --- Lifecycle --- */

/* Create a new governance engine instance. Returns NULL on allocation failure. */
NAAB_GOV_API naab_gov_engine_t naab_gov_create(void);

/* Destroy an engine instance. NULL-safe. */
NAAB_GOV_API void naab_gov_destroy(naab_gov_engine_t engine);

/* --- Configuration --- */

/* Load governance policy from a govern.json file path. */
NAAB_GOV_API naab_gov_error_t naab_gov_load_config(naab_gov_engine_t engine,
                                                    const char* path);

/* Walk up from dir to find govern.json, then load it. */
NAAB_GOV_API naab_gov_error_t naab_gov_discover_config(naab_gov_engine_t engine,
                                                        const char* dir);

/* Load governance policy from a JSON string (no file needed). */
NAAB_GOV_API naab_gov_error_t naab_gov_load_config_string(naab_gov_engine_t engine,
                                                           const char* json_config);

/* Returns 1 if a config is loaded and active, 0 otherwise. */
NAAB_GOV_API int naab_gov_is_active(naab_gov_engine_t engine);

/* --- Scanning --- */

/*
 * Primary entry point: scan a code block.
 *
 * Parameters:
 *   engine      — governance engine (must have config loaded)
 *   language    — language identifier ("python", "javascript", "go", etc.)
 *   code        — the source code to scan
 *   source_file — filename for diagnostics (can be "")
 *   start_line  — line number offset for diagnostics (typically 1)
 *
 * Returns: malloc'd JSON string with structure:
 *   {"blocked": bool, "error": string, "report": {...}}
 *
 * Caller MUST free the result via naab_gov_free_string().
 * Returns NULL on engine error (check naab_gov_last_error()).
 */
NAAB_GOV_API char* naab_gov_scan(naab_gov_engine_t engine,
                                  const char* language,
                                  const char* code,
                                  const char* source_file,
                                  int start_line);

/*
 * Run a single named check against code.
 *
 * check_name: "secrets", "code_injection", "sql_injection",
 *             "dangerous_calls", "shell_injection", "imports",
 *             "obfuscation", "deserialization", "privilege_escalation",
 *             "oversimplification", "incomplete_logic", "encoding"
 *
 * Returns: malloc'd JSON string with check result.
 * Caller MUST free via naab_gov_free_string().
 */
NAAB_GOV_API char* naab_gov_check(naab_gov_engine_t engine,
                                   const char* check_name,
                                   const char* language,
                                   const char* code,
                                   int start_line);

/* --- Results --- */

/* Returns 1 if the last scan had a HARD block, 0 otherwise. */
NAAB_GOV_API int naab_gov_was_blocked(naab_gov_engine_t engine);

/* Full JSON report from the last scan. Caller frees via naab_gov_free_string(). */
NAAB_GOV_API char* naab_gov_json_report(naab_gov_engine_t engine);

/* SARIF report from the last scan. Caller frees via naab_gov_free_string(). */
NAAB_GOV_API char* naab_gov_sarif_report(naab_gov_engine_t engine);

/* Human-readable summary. Caller frees via naab_gov_free_string(). */
NAAB_GOV_API char* naab_gov_summary(naab_gov_engine_t engine);

/* Number of check results from the last scan. */
NAAB_GOV_API int naab_gov_result_count(naab_gov_engine_t engine);

/* Clear results for next scan. */
NAAB_GOV_API void naab_gov_reset(naab_gov_engine_t engine);

/* --- Error handling --- */

/* Returns internal error string. Caller must NOT free. Returns "" if no error. */
NAAB_GOV_API const char* naab_gov_last_error(naab_gov_engine_t engine);

/* --- Memory --- */

/* Free any string returned by naab_gov_scan/check/report functions. NULL-safe. */
NAAB_GOV_API void naab_gov_free_string(char* str);

/* --- Version --- */

/* Returns static version string. Caller must NOT free. */
NAAB_GOV_API const char* naab_gov_version_string(void);

#ifdef __cplusplus
}
#endif

#endif /* NAAB_GOVERNANCE_H */
